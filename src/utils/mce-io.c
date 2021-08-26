/**
 * @file mce-io.c
 * Generic I/O functionality for the Mode Control Entity
 * <p>
 * Copyright © 2006-2010 Nokia Corporation and/or its subsidiary(-ies).
 * <p>
 * @author David Weinehall <david.weinehall@nokia.com>
 * @author Jonathan Wilson <jfwfreo@tpgi.com.au>
 *
 * mce is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * mce is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mce.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <glob.h>
#include <glib.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include "mce.h"
#include "mce-io.h"
#include "mce-log.h"

/** List of all file monitors */
static GSList *file_monitors = NULL;

/** I/O monitor type */
typedef enum {
	IOMON_UNSET = -1,			/**< I/O monitor type unset */
	IOMON_STRING = 0,			/**< String I/O monitor */
	IOMON_CHUNK = 1				/**< Chunk I/O monitor */
} iomon_type;

/** I/O monitor structure */
typedef struct {
	gchar *file;				/**< Monitored file */
	GIOChannel *iochan;			/**< I/O channel */
	iomon_cb callback;			/**< Callback */
	iomon_error_cb remdev_callback;		/**< Error callback */
	gpointer remdev_data;
	gulong chunk_size;			/**< Read-chunk size */
	guint data_source_id;			/**< GSource ID for data */
	guint error_source_id;			/**< GSource ID for errors */
	gint fd;				/**< File Descriptor */
	iomon_type type;			/**< Monitor type */
	error_policy_t error_policy;		/**< Error policy */
	GIOCondition latest_io_condition;	/**< Latest I/O condition */
	gboolean rewind;			/**< Rewind policy */
	gboolean suspended;			/**< Is the I/O monitor
						 *   suspended? */
} iomon_struct;

/**
 * Read a string from a file
 *
 * @param file Path to the file
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_read_string_from_file(const gchar *const file, gchar **string)
{
	GError *error = NULL;
	gboolean status = FALSE;

	if (file == NULL) {
		mce_log(LL_CRIT,
			"mce_read_string_from_file() "
			"called with file == NULL!");
		goto EXIT;
	}

	if (g_file_get_contents(file, string, NULL, &error) == FALSE) {
		mce_log(LL_ERR,
			"Cannot open `%s' for reading; %s",
			file, error->message);
		goto EXIT;
	}

	status = TRUE;

EXIT:
	g_clear_error(&error);

	return status;
}

/**
 * Read a number representation of a string from a file
 *
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_read_number_string_from_file(const gchar *const file,
					  gulong *number)
{
	gchar *string;
	gboolean status;

	if ((status = mce_read_string_from_file(file, &string)) == TRUE) {
		gulong num = strtoul(string, NULL, 10);

		g_free(string);

		/* Error check for the results of strtoul() */
		if ((errno == EINVAL) ||
		    ((num == ULONG_MAX) && (errno == ERANGE))) {
			status = FALSE;

			/* Reset errno,
			 * to avoid false positives down the line
			 */
			errno = 0;
		} else {
			*number = num;
		}
	}

	return status;
}

/**
 * Write a string to a file matching glob pattern
 *
 * @param file Glob pattern that should resolve to the file, if multiple files
 * are matches, writes to all.
 * @param string The string to write
 * @return TRUE iff all writes succeed, FALSE on failure
 */
gboolean mce_write_string_to_glob(const gchar *const pattern,
				  const gchar *const string)
{

    glob_t glob_result;
    gboolean all_writes_ok = TRUE;

    if (glob(pattern, GLOB_NOMATCH, NULL, &glob_result)) {
        return FALSE;
    }

    for (size_t i = 0; i < glob_result.gl_pathc; i++) {
        all_writes_ok &= mce_write_string_to_file(glob_result.gl_pathv[i], string);
    }

    globfree(&glob_result);

    return all_writes_ok;
}

/**
 * Write a string to a file
 *
 * @param file Path to the file
 * @param string The string to write
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_write_string_to_file(const gchar *const file,
				  const gchar *const string)
{
	GIOChannel *iochan = NULL;
	GIOStatus iostatus;
	GError *error = NULL;
	gboolean status = TRUE;

	if (file == NULL) {
		mce_log(LL_CRIT,
			"mce_write_string_to_file() "
			"called with file == NULL!");
		status = FALSE;
		goto EXIT;
	}

	if (string == NULL) {
		mce_log(LL_CRIT,
			"mce_write_string_to_file() "
			"called with string == NULL!");
		status = FALSE;
		goto EXIT;
	}

	if ((iochan = g_io_channel_new_file(file, "w", &error)) == NULL) {
		mce_log(LL_ERR,
			"Cannot open `%s' for writing; %s",
			file, error->message);
		status = FALSE;
		goto EXIT;
	}

	iostatus = g_io_channel_write_chars(iochan, string,
					    -1, NULL, &error);

	if (iostatus != G_IO_STATUS_NORMAL) {
		mce_log(LL_ERR,
			"Cannot modify `%s'; %s",
			file, error->message);
		status = FALSE;
		g_clear_error(&error);
	}

	iostatus = g_io_channel_shutdown(iochan, TRUE, &error);

	if (iostatus != G_IO_STATUS_NORMAL) {
		mce_log(LL_ERR,
			"Cannot close `%s'; %s",
			file, error->message);
		status = FALSE;
	}

	g_io_channel_unref(iochan);

EXIT:
	g_clear_error(&error);

	return status;
}

/**
 * Write a string representation of a number to files matting the glob pattern.
 *
 * @param number The number to write
 * @return TRUE iff all writes succeed, FALSE on failure
 */
gboolean mce_write_number_string_to_glob(const gchar *const pattern,
					 const gulong number)
{
	gchar *string;
	gboolean status;

	string = g_strdup_printf("%lu", number);
	status = mce_write_string_to_glob(pattern, string);
	g_free(string);

	return status;
}

/**
 * Write a string representation of a number to a file
 *
 * @param number The number to write
 * @return TRUE on success, FALSE on failure
 */
gboolean mce_write_number_string_to_file(const gchar *const file,
					 const gulong number)
{
	gchar *string;
	gboolean status;

	string = g_strdup_printf("%lu", number);
	status = mce_write_string_to_file(file, string);
	g_free(string);

	return status;
}

/**
 * Callback for successful string I/O
 *
 * @param source The source of the activity
 * @param condition The I/O condition
 * @param data The iomon structure
 * @return Depending on error policy this function either exits
 *         or returns TRUE
 */
static gboolean io_string_cb(GIOChannel *source,
			     GIOCondition condition,
			     gpointer data)
{
	iomon_struct *iomon = data;
	gchar *str = NULL;
	gsize bytes_read;
	GError *error = NULL;
	gboolean status = TRUE;

	/* Silence warnings */
	(void)condition;

	if (iomon == NULL) {
		mce_log(LL_CRIT,
			"io_string_cb() "
			"called with iomon == NULL!");
		status = FALSE;
		goto EXIT;
	}

	iomon->latest_io_condition = 0;

	/* Seek to the beginning of the file before reading if needed */
	if (iomon->rewind == TRUE) {
		g_io_channel_seek_position(source, 0, G_SEEK_SET, &error);
		g_clear_error(&error);
	}

	g_io_channel_read_line(source, &str, &bytes_read, NULL, &error);

	/* Errors and empty reads are nasty */
	if (error != NULL) {
		mce_log(LL_ERR,
			"Error when reading from %s: %s",
			iomon->file, error->message);

		iomon->remdev_callback(iomon->remdev_data, iomon->file, iomon, error);
		g_clear_error(&error);
		return FALSE;

	} else if ((bytes_read == 0) || (str == NULL) || (strlen(str) == 0)) {
		mce_log(LL_ERR,
			"Empty read from %s",
			iomon->file);
	} else {
		iomon->callback(str, bytes_read);
		g_free(str);
	}

	g_clear_error(&error);

EXIT:
	if ((status == FALSE) &&
	    (iomon != NULL) &&
	    (iomon->error_policy == MCE_IO_ERROR_POLICY_EXIT)) {
		g_main_loop_quit(mainloop);
		exit(EXIT_FAILURE);
	}

	return TRUE;
}

/**
 * Callback for successful chunk I/O
 *
 * @param source The source of the activity
 * @param condition The I/O condition
 * @param data The iomon structure
 * @return Depending on error policy this function either exits
 *         or returns TRUE
 */
static gboolean io_chunk_cb(GIOChannel *source,
			    GIOCondition condition,
			    gpointer data)
{
	iomon_struct *iomon = data;
	gchar *chunk = NULL;
	gsize bytes_read;
	GIOStatus io_status;
	GError *error = NULL;
	gboolean status = TRUE;

	/* Silence warnings */
	(void)condition;

	if (iomon == NULL) {
		mce_log(LL_CRIT,
			"io_chunk_cb() "
			"called with iomon == NULL!");
		status = FALSE;
		goto EXIT;
	}

	iomon->latest_io_condition = 0;

	/* Seek to the beginning of the file before reading if needed */
	if (iomon->rewind == TRUE) {
		g_io_channel_seek_position(source, 0, G_SEEK_SET, &error);
		g_clear_error(&error);
	}

	chunk = g_malloc(iomon->chunk_size);

	do {
		io_status = g_io_channel_read_chars(source, chunk,
						    iomon->chunk_size,
						    &bytes_read, &error);
	} while ((io_status == G_IO_STATUS_AGAIN) && (error != NULL));

	/* Errors and empty reads are nasty */
	if (error != NULL) {
		mce_log(LL_ERR,
			"Error when reading from %s: %s",
			iomon->file, error->message);

		if ((error->code == G_IO_CHANNEL_ERROR_FAILED) &&
		    (errno == ENODEV)) {
			mcs_io_monitor_seek_to_end(iomon);

			iomon->remdev_callback(iomon->remdev_data, iomon->file, iomon, error);
			g_clear_error(&error);
			return FALSE;
		}

		/* Reset errno,
		 * to avoid false positives down the line
		 */
		errno = 0;
	} else if (bytes_read == 0) {
		mce_log(LL_ERR,
			"Empty read from %s",
			iomon->file);
	} else {
		iomon->callback(chunk, bytes_read);
	}

	g_free(chunk);

	g_clear_error(&error);

EXIT:
	if ((status == FALSE) &&
	    (iomon != NULL) &&
	    (iomon->error_policy == MCE_IO_ERROR_POLICY_EXIT)) {
		g_main_loop_quit(mainloop);
		exit(EXIT_FAILURE);
	}

	return TRUE;
}


static loglevel_t io_mon_get_log_level(iomon_struct *iomon)
{
	switch (iomon->error_policy) {
	case MCE_IO_ERROR_POLICY_EXIT:
		return LL_CRIT;

	case MCE_IO_ERROR_POLICY_WARN:
		return LL_WARN;

	case MCE_IO_ERROR_POLICY_IGNORE:
	default:
		/* No log message when ignoring errors */
		return LL_NONE;
	}
}

/**
 * Callback for I/O errors
 *
 * @param source Unused
 * @param condition The GIOCondition for the error
 * @param data The iomon structure
 * @return Depending on error policy this function either exits
 *         or returns TRUE
 */
static gboolean io_error_cb(GIOChannel *source,
			    GIOCondition condition,
			    gpointer data)
{
	iomon_struct *iomon = data;
	gboolean exit_on_error = FALSE;
	loglevel_t loglevel;

	/* Silence warnings */
	(void)source;

	if (iomon == NULL) {
		mce_log(LL_CRIT,
			"io_error_cb() "
			"called with iomon == NULL!");
		goto EXIT;
	}

	loglevel = io_mon_get_log_level(iomon);
	if(loglevel == LL_CRIT)
		exit_on_error = TRUE;

	/* We just got an I/O condition we've already reported
	 * since the last successful read; don't log
	 */
	if ((exit_on_error == FALSE) &&
	    ((iomon->latest_io_condition & condition) == condition)) {
		loglevel = LL_NONE;
	} else {
		iomon->latest_io_condition |= condition;
	}

	if (loglevel != LL_NONE) {
		mce_log(loglevel,
			"Error accessing %s (condition: %d). %s",
			iomon->file, condition,
			(exit_on_error == TRUE) ? "Exiting" : "Ignoring");
	}

EXIT:
	if ((iomon != NULL) && (exit_on_error == TRUE)) {
		g_main_loop_quit(mainloop);
		exit(EXIT_FAILURE);
	}

	return TRUE;
}

/**
 * Suspend an I/O monitor
 *
 * @param io_monitor A pointer to the I/O monitor to suspend
 */
void mce_suspend_io_monitor(gconstpointer io_monitor)
{
	iomon_struct *iomon = (iomon_struct *)io_monitor;

	if (iomon == NULL) {
		mce_log(LL_CRIT,
			"mce_suspend_io_monitor() "
			"called with iomon == NULL!");
		goto EXIT;
	}

	if (iomon->suspended == TRUE)
		goto EXIT;

	/* Remove I/O watches */
	g_source_remove(iomon->data_source_id);
	g_source_remove(iomon->error_source_id);

	iomon->suspended = TRUE;

EXIT:
	return;
}

/**
 * Resume an I/O monitor
 *
 * @param io_monitor A pointer to the I/O monitor to resume
 */
void mce_resume_io_monitor(gconstpointer io_monitor)
{
	iomon_struct *iomon = (iomon_struct *)io_monitor;
	GIOFunc callback = NULL;

	if (iomon == NULL) {
		mce_log(LL_CRIT,
			"mce_resume_io_monitor() "
			"called with iomon == NULL!");
		goto EXIT;
	}

	if (iomon->suspended == FALSE)
		goto EXIT;

	switch (iomon->type) {
	case IOMON_STRING:
		callback = io_string_cb;
		break;

	case IOMON_CHUNK:
		callback = io_chunk_cb;
		break;

	case IOMON_UNSET:
	default:
		break;
	}

	if (callback != NULL) {

		/* Seek to the end of the file; unless we use the rewind policy */
		if (iomon->rewind == FALSE)
		    mcs_io_monitor_seek_to_end(io_monitor);

		iomon->error_source_id = g_io_add_watch(iomon->iochan,
							G_IO_HUP | G_IO_NVAL,
							io_error_cb, iomon);
		iomon->data_source_id = g_io_add_watch(iomon->iochan,
						       G_IO_IN | G_IO_PRI |
						       G_IO_ERR,
						       callback, iomon);
		iomon->suspended = FALSE;

	} else {
		mce_log(LL_ERR,
			"Failed to resume `%s'; invalid callback",
			iomon->file);
	}

EXIT:
	return;
}

/**
 * Register an I/O monitor; reads and returns data
 *
 * @param fd File Descriptor; this takes priority over file; -1 if not used
 * @param file Path to the file
 * @param error_policy MCE_IO_ERROR_POLICY_EXIT to exit on error,
 *                     MCE_IO_ERROR_POLICY_WARN to warn about errors
 *                                              but ignore them,
 *                     MCE_IO_ERROR_POLICY_IGNORE to silently ignore errors
 * @param callback Function to call with result
 * @return An I/O monitor pointer on success, NULL on failure
 */
static iomon_struct *mce_register_io_monitor(const gint fd,
					     const gchar *const file,
					     error_policy_t error_policy,
					     iomon_cb callback,
					     iomon_error_cb remdev_callback,
					     gpointer remdev_data)
{
	iomon_struct *iomon = NULL;
	GIOChannel *iochan = NULL;
	GError *error = NULL;

	if (file == NULL) {
		mce_log(LL_CRIT,
			"mce_register_io_monitor() "
			"called with file == NULL!");
		goto EXIT;
	}

	if (callback == NULL) {
		mce_log(LL_CRIT,
			"mce_register_io_monitor() "
			"called with callback == NULL!");
		goto EXIT;
	}

	if ((iomon = g_slice_new(iomon_struct)) == NULL) {
		mce_log(LL_CRIT,
			"Failed to allocate memory for iomon_struct");
		goto EXIT;
	}

	if (fd != -1) {
		if ((iochan = g_io_channel_unix_new(fd)) == NULL) {
			/* XXX: this is probably not good either;
			 * we should only ignore non-existing files
			 */
			if (error_policy != MCE_IO_ERROR_POLICY_IGNORE)
				mce_log(LL_ERR,
					"Failed to open `%s'", file);

			g_slice_free(iomon_struct, iomon);
			iomon = NULL;
			goto EXIT;
		}
	} else {
		if ((iochan = g_io_channel_new_file(file, "r",
						   &error)) == NULL) {
			/* XXX: this is probably not good either;
			 * we should only ignore non-existing files
			 */
			if (error_policy != MCE_IO_ERROR_POLICY_IGNORE)
				mce_log(LL_ERR,
					"Failed to open `%s'; %s",
					file, error->message);

			g_slice_free(iomon_struct, iomon);
			iomon = NULL;
			goto EXIT;
		}
	}

	iomon->fd = fd;
	iomon->file = g_strdup(file);
	iomon->iochan = iochan;
	iomon->callback = callback;
	iomon->remdev_callback = remdev_callback;
	iomon->remdev_data = remdev_data;
	iomon->error_policy = error_policy;
	iomon->rewind = FALSE;
	iomon->chunk_size = 0;

	file_monitors = g_slist_prepend(file_monitors, iomon);

	iomon->suspended = TRUE;

EXIT:
	g_clear_error(&error);

	return iomon;
}

/**
 * Register an I/O monitor; reads and returns a string
 *
 * @param fd File Descriptor; this takes priority over file; -1 if not used
 * @param file Path to the file
 * @param error_policy MCE_IO_ERROR_POLICY_EXIT to exit on error,
 *                     MCE_IO_ERROR_POLICY_WARN to warn about errors
 *                                              but ignore them,
 *                     MCE_IO_ERROR_POLICY_IGNORE to silently ignore errors
 * @param rewind_policy TRUE to seek to the beginning,
 *                      FALSE to stay at current position
 * @param callback Function to call with result
 * @return An I/O monitor cookie on success, NULL on failure
 */
gconstpointer mce_register_io_monitor_string(const gint fd,
					     const gchar *const file,
					     error_policy_t error_policy,
					     gboolean rewind_policy,
					     iomon_cb callback,
					     iomon_error_cb remdev_callback,
					     gpointer remdev_data)
{
	iomon_struct *iomon = NULL;

	iomon = mce_register_io_monitor(fd, file, error_policy, callback,
			remdev_callback, remdev_data);

	if (iomon == NULL)
		goto EXIT;

	/* Verify that the rewind policy is sane */
	if ((g_io_channel_get_flags(iomon->iochan) &
	     G_IO_FLAG_IS_SEEKABLE) == G_IO_FLAG_IS_SEEKABLE) {
		/* Set the rewind policy */
		iomon->rewind = rewind_policy;
	} else if (rewind_policy == TRUE) {
		mce_log(LL_ERR,
			"Attempting to set rewind policy to TRUE "
			"on non-seekable I/O channel `%s'",
			file);
		iomon->rewind = FALSE;
	}

	/* Set the I/O monitor type and call resume to add an I/O watch */
	iomon->type = IOMON_STRING;
	mce_resume_io_monitor(iomon);

EXIT:
	return iomon;
}

/**
 * Register an I/O monitor; reads and returns a chunk of specified size
 *
 * @param fd File Descriptor; this takes priority over file; -1 if not used
 * @param file Path to the file
 * @param error_policy MCE_IO_ERROR_POLICY_EXIT to exit on error,
 *                     MCE_IO_ERROR_POLICY_WARN to warn about errors
 *                                              but ignore them,
 *                     MCE_IO_ERROR_POLICY_IGNORE to silently ignore errors
 * @param rewind_policy TRUE to seek to the beginning,
 *                      FALSE to stay at current position
 * @param callback Function to call with result
 * @param chunk_size The number of bytes to read in each chunk
 * @return An I/O monitor cookie on success, NULL on failure
 */
gconstpointer mce_register_io_monitor_chunk(const gint fd,
					    const gchar *const file,
					    error_policy_t error_policy,
					    gboolean rewind_policy,
					    iomon_cb callback,
					    gulong chunk_size,
					    iomon_error_cb remdev_callback,
					    gpointer remdev_data)
{
	iomon_struct *iomon = NULL;
	GError *error = NULL;

	iomon = mce_register_io_monitor(fd, file, error_policy, callback,
			remdev_callback, remdev_data);

	if (iomon == NULL)
		goto EXIT;

	/* Set the read chunk size */
	iomon->chunk_size = chunk_size;

	/* Verify that the rewind policy is sane */
	if ((g_io_channel_get_flags(iomon->iochan) &
	     G_IO_FLAG_IS_SEEKABLE) == G_IO_FLAG_IS_SEEKABLE) {
		/* Set the rewind policy */
		iomon->rewind = rewind_policy;
	} else if (rewind_policy == TRUE) {
		mce_log(LL_ERR,
			"Attempting to set rewind policy to TRUE "
			"on non-seekable I/O channel `%s'",
			file);
		iomon->rewind = FALSE;
	}

	/* We only read this file in binary form */
	(void)g_io_channel_set_encoding(iomon->iochan, NULL, &error);

	g_clear_error(&error);

	/* Don't block */
	(void)g_io_channel_set_flags(iomon->iochan, G_IO_FLAG_NONBLOCK, &error);

	g_clear_error(&error);

	/* Set the I/O monitor type and call resume to add an I/O watch */
	iomon->type = IOMON_CHUNK;
	mce_resume_io_monitor(iomon);

EXIT:
	return iomon;
}

/**
 * Unregister an I/O monitor
 * Note: This does NOT shutdown I/O channels created from file descriptors
 *
 * @param io_monitor A pointer to the I/O monitor to unregister
 */
void mce_unregister_io_monitor(gconstpointer io_monitor)
{
	iomon_struct *iomon = (iomon_struct *)io_monitor;
	guint oldlen;

	if (iomon == NULL) {
		mce_log(LL_DEBUG,
			"mce_unregister_io_monitor called "
			"with NULL argument");
		goto EXIT;
	}

	oldlen = g_slist_length(file_monitors);

	if (file_monitors != NULL)
		file_monitors = g_slist_remove(file_monitors, iomon);

	/* Did we remove any entry? */
	if (oldlen == g_slist_length(file_monitors)) {
		mce_log(LL_WARN,
			"Trying to unregister non-existing file monitor");
	}

	/* Remove I/O watches */
	mce_suspend_io_monitor(iomon);

	/* We can close this I/O channel, since it's not an external fd */
	if (iomon->fd == -1) {
		GIOStatus iostatus;
		GError *error = NULL;

		iostatus = g_io_channel_shutdown(iomon->iochan, TRUE, &error);

		if (iostatus != G_IO_STATUS_NORMAL) {
			loglevel_t loglevel = LL_ERR;

			/* If we get ENODEV, only log a debug message,
			 * since this happens for hotpluggable
			 * /dev/input files
			 */
			if ((error->code == G_IO_CHANNEL_ERROR_FAILED) &&
			    (errno == ENODEV))
				loglevel = LL_DEBUG;

			mce_log(loglevel,
				"Cannot close `%s'; %s",
				iomon->file, error->message);
		}

		g_clear_error(&error);
	}

	g_io_channel_unref(iomon->iochan);
	if(iomon->fd != -1 && close(iomon->fd) < 0) {
		mce_log(LL_ERR, "mce-io: Can not close %i errno: %s", iomon->fd, strerror(errno));
	}
	g_free(iomon->file);
	g_slice_free(iomon_struct, iomon);

EXIT:
	return;
}

gboolean mcs_io_monitor_seek_to_end(gconstpointer io_monitor)
{
	iomon_struct *iomon = (iomon_struct *)io_monitor;
	GError *error = NULL;
	gboolean seek_success = FALSE;

	if ((g_io_channel_get_flags(iomon->iochan) & G_IO_FLAG_IS_SEEKABLE) == G_IO_FLAG_IS_SEEKABLE) {
		g_io_channel_seek_position(iomon->iochan, 0, G_SEEK_END, &error);
		if (!error)
			seek_success = TRUE;
		g_clear_error(&error);
	}

	error = NULL;

	if (!seek_success) {
		gsize bytes_read;
		char buffer[1024];
		do {
			g_io_channel_read_chars(iomon->iochan, buffer,
						sizeof(buffer),
						&bytes_read, &error);
		} while (bytes_read > 0 && error == NULL);
	}

	return TRUE;
}

/**
 * Return the name of the monitored file
 *
 * @param io_monitor An opaque pointer to the I/O monitor structure
 * @return The name of the monitored file
 */
const gchar *mce_get_io_monitor_name(gconstpointer io_monitor)
{
	iomon_struct *iomon = (iomon_struct *)io_monitor;

	return iomon->file;
}

/**
 * Return the file descriptor of the monitored file;
 * if the file being monitored was opened from a path
 * rather than a file descriptor, -1 is returned
 *
 * @param io_monitor An opaque pointer to the I/O monitor structure
 * @return The file descriptor of the monitored file
 */
int mce_get_io_monitor_fd(gconstpointer io_monitor)
{
	iomon_struct *iomon = (iomon_struct *)io_monitor;

	return iomon->fd;
}
