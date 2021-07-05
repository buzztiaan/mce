/**
 * @file mce-lib.h
 * Headers for various helper functions
 * for the Mode Control Entity
 * <p>
 * Copyright © 2004-2010 Nokia Corporation and/or its subsidiary(-ies).
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
#ifndef _MCE_LIB_H_
#define _MCE_LIB_H_

#include <glib.h>

/** translation structure */
typedef struct {
	const gint number;		/**< Number representation */
	const gchar *const string;	/**< String representation */
} mce_translation_t;

const gchar *bin_to_string(guint bin);

const gchar *mce_translate_int_to_string_with_default(const mce_translation_t translation[], gint number, const gchar *default_string);
const gchar *mce_translate_int_to_string(const mce_translation_t translation[],
					 gint number);

gint mce_translate_string_to_int_with_default(const mce_translation_t translation[], const gchar *const string, gint default_number);
gint mce_translate_string_to_int(const mce_translation_t translation[],
				 const gchar *const string);

gchar *strstr_delim(const gchar *const haystack, const char *needle,
		    const char *const delimiter);


#endif /* _MCE_LIB_H_ */
