/*
 * $Id$
 *
 * ROX-Filer, filer for the ROX desktop project
 * Copyright (C) 2002, the ROX-Filer team.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* This is a reimplementation of the GNU gettext function, because not everyone
 * has GNU gettext.
 */

#include "config.h"

#include <string.h>

#include "gui_support.h"

static GHashTable *translate = NULL;

/* Returns the 32 bit number at 'addr'.
 * Reverse the byte order of 'number' iff 'swap' is TRUE.
 */
#define WORD(addr) word(addr, swap)

/* Static prototypes */
static guint32 word(char *addr, gboolean swap);


/****************************************************************
 *			EXTERNAL INTERFACE			*
 ****************************************************************/

/* Returns a pointer to a static, nul-terminated, string which
 * is the translation of 'from' (or 'from' itself if there is
 * no translation).
 */
const char *rox_gettext(const char *from)
{
	char	*retval;
	
	if (!translate || !from || !*from)
		return from;

	retval = g_hash_table_lookup(translate, from);

	return retval ? retval : from;
}

void rox_clear_translation(void)
{
	if (translate)
	{
		g_hash_table_destroy(translate);
		translate = NULL;
	}
}

/* Read in this .gmo format file; all translations found override
 * any existing translations for future calls to rox_gettext().
 */
void rox_add_translations(const char *path)
{
	guint32		magic;
	char		*data, *from_base, *to_base;
	gsize		size;
	gboolean	swap;		/* TRUE => reverse byte-order of ints */
	int		n, n_total;
	GError		*error = NULL;

	if (!g_file_get_contents(path, &data, &size, &error))
	{
		report_error("%s", error ? error->message : path);
		g_error_free(error);
		return;
	}
	
	if (size < 20)
	{
		report_error(_("Invalid .gmo translation file "
					"(too short): %s"), path);
		goto out;
	}

	magic = *((guint *) data);

	if (magic == 0x950412de)
		swap = FALSE;
	else if (magic == 0xde120495)
		swap = TRUE;
	else
	{
		report_error(_("Invalid .gmo translation file "
					"(GNU magic number not found): %s"),
				  path);
		goto out;
	}

	if (WORD(data + 4) != 0)
		g_warning("rox_add_translations: expected format revision 0");

	if (!translate)
	{
		translate = g_hash_table_new_full(g_str_hash, g_str_equal,
						  g_free, g_free);
	}

	n_total = WORD(data + 8);
	from_base = data + WORD(data + 12);
	to_base = data + WORD(data + 16);

	for (n = 0; n < n_total; n++)
	{
		char	*from = data + WORD(from_base + (n << 3) + 4);
		char	*to   = data + WORD(to_base + (n << 3) + 4);

		g_hash_table_insert(translate, g_strdup(from), g_strdup(to));
	}

out:
	g_free(data);
}

/****************************************************************
 *			INTERNAL FUNCTIONS			*
 ****************************************************************/

static guint32 word(char *addr, gboolean swap)
{
	guint32	val = *((guint32 *) addr);

	if (swap)
		return GUINT32_SWAP_LE_BE(val);
	else
		return val;
}
