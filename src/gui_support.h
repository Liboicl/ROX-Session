/*
 * $Id$
 *
 * ROX-Session, a very simple session manager
 * By Thomas Leonard, <tal197@users.sourceforge.net>.
 */

#ifndef _GUI_SUPPORT_H
#define _GUI_SUPPORT_H

#include <gtk/gtk.h>

GtkWidget *button_new_mixed(char *stock, char *label);
void report_error(char *message, ...);
int get_choice(char *title,
	       char *message,
	       int number_of_buttons, ...);

#endif /* _GUI_SUPPORT_H */
