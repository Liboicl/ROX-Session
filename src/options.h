/*
 * $Id$
 *
 * ROX-Filer, filer for the ROX desktop project
 * By Thomas Leonard, <tal197@users.sourceforge.net>.
 */

#ifndef _OPTIONS_H
#define _OPTIONS_H

typedef struct _Option Option;

#include <gtk/gtk.h>
#include <libxml/parser.h>

typedef void OptionNotify(void);
typedef GList * (*OptionBuildFn)(Option *option, xmlNode *node, guchar *label);

struct _Option {
	guchar		*value;
	long		int_value;
	gboolean	has_changed;

	guchar		*backup;	/* Copy of value to Revert to */

	GtkWidget	*widget;		/* NULL => No UI yet */
	void		(*update_widget)(Option *option);
	guchar *	(*read_widget)(Option *option);
};

/* Prototypes */

void options_init(void);

void option_register_widget(char *name, OptionBuildFn builder);
void option_check_widget(Option *option);

void option_add_int(Option *option, guchar *key, int value);
void option_add_string(Option *option, guchar *key, guchar *value);

void options_notify(void);
void option_add_notify(OptionNotify *callback);
void option_add_saver(OptionNotify *callback);

void options_show(void);

#endif /* _OPTIONS_H */
