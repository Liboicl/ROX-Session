/*
 * $Id$
 *
 * ROX-Session, a very simple session manager
 * By Thomas Leonard, <tal197@users.sourceforge.net>.
 */

#ifndef _SESSION_H
#define _SESSION_H

#include "xsettings-manager.h"

#include <sys/types.h>
#include <unistd.h>

extern XSettingsManager *manager;
extern gboolean call_child_died;

/* Prototypes */
void session_init(void);
void show_main_window(void);
void run_login_script(void);
void child_died_callback(void);
void show_session_options(void);

#endif /* _SESSION_H */
