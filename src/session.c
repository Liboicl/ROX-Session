/* vi: set cindent:
 * $Id$
 *
 * ROX-Session, a very simple session manager
 * Copyright (C) 2002, Thomas Leonard, <tal197@users.sourceforge.net>.
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

#include "config.h"

#include <errno.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>
#include <sys/param.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>

#include <gdk/gdkx.h>
#include <gtk/gtk.h>

#include "global.h"

#include "session.h"
#include "gui_support.h"
#include "options.h"
#include "choices.h"
#include "main.h"
#include "wm.h"


/* The pid of the Login script. -1 if terminated. If this process
 * exits with a non-zero exit status then we assume the session went
 * wrong and try to fix it.
 */
static pid_t	login_child = -1;

/* The pid of the ROX-Filer process managing the pinboard and panel.
 * ROX-Session will offer to restart this if it dies.
 */
static pid_t	rox_pid = -1;

/* If log.c gets any data and this is TRUE, it calls child_died_callback() */
gboolean call_child_died = FALSE;

/* See http://www.freedesktop.org/standards/xsettings/xsettings.html */
XSettingsManager *manager = NULL;

typedef struct _Setting Setting;

struct _Setting {
	gchar *name;
	gchar *default_value;
	Option option;
};

static Setting settings[] = {
	{"Net/DoubleClickTime", "250"},
	{"Net/DndDragThreshold", "8"},
#if 0
	{ "Gtk/ColorPalette", "gtk-color-palette" }, */
	{ "Gtk/IconSizes", "gtk-icon-sizes" }
	{ "Gtk/KeyThemeName", NULL },
	{ "Gtk/ToolbarStyle", "gtk-toolbar-style" },
	{ "Gtk/ToolbarIconSize", "gtk-toolbar-icon-size" },
	{ "Gtk/IMPreeditStyle", "gtk-im-preedit-style" },
	{ "Gtk/IMStatusStyle", "gtk-im-status-style" },
#endif
	{"Net/ThemeName", "Default"},

	{"Net/CursorBlink", "1"},
	{"Net/CursorBlinkTime", "1200"},

	{"Gtk/CanChangeAccels", "1"},
	{"Gtk/FontName", "Sans 10"},

#if 0
	{"Xft/Antialias", "0"},
#endif
};

static Option mouse_accel_threshold;
static Option mouse_accel_factor;

static Option halt_command, reboot_command, suspend_command;

#define N_SETTINGS (sizeof(settings) / sizeof(*settings))

static const char * bad_xpm[] = {
"12 12 3 1",
" 	c #000000000000",
".	c #FFFF00000000",
"x	c #FFFFFFFFFFFF",
"            ",
" ..xxxxxx.. ",
" ...xxxx... ",
" x...xx...x ",
" xx......xx ",
" xxx....xxx ",
" xxx....xxx ",
" xx......xx ",
" x...xx...x ",
" ...xxxx... ",
" ..xxxxxx.. ",
"            "};

#define ROX_STOCK_HALT "rox-halt"
#define ROX_STOCK_SUSPEND "rox-suspend"

static const char *stocks[] = {
	ROX_STOCK_HALT,
	ROX_STOCK_SUSPEND,
};

/* Static prototypes */
static GList *build_mouse_tester(Option *option, xmlNode *node, guchar *label);
static GList *build_gtk_theme(Option *option, xmlNode *node, guchar *label);
static void child_died(int signum);
static void terminate_xsettings(void *data);
static void xsettings_changed(void);
static void show_session_options(void);
static GtkWidget *op_button(const char *text, const char *stock,
			    Option *command, const char *message);
static char *pathdup(const char *path);
static void rox_process_died(void);
static void run_rox_process(void);

/****************************************************************
 *			EXTERNAL INTERFACE			*
 ****************************************************************/

void session_init(void)
{
	GtkIconFactory *factory;
	struct sigaction	act;
	int i;

	/* Let child processes die */
	act.sa_handler = child_died;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_NOCLDSTOP;
	sigaction(SIGCHLD, &act, NULL);

	if (xsettings_manager_check_running(gdk_display,
					    DefaultScreen(gdk_display)))
	{
		g_printerr("An XSETTINGS manager is already running. "
				"Not taking control of XSETTINGS...\n");
	}
	else
		manager = xsettings_manager_new(gdk_display,
						DefaultScreen(gdk_display),
					        terminate_xsettings, NULL);

	for (i = 0; i < N_SETTINGS; i++)
	{
		option_add_string(&settings[i].option,
				  settings[i].name,
				  settings[i].default_value);
	}

	option_add_int(&mouse_accel_threshold, "accel_threshold", 10);
	option_add_int(&mouse_accel_factor, "accel_factor", 20);

	option_add_string(&halt_command, "halt_command", "halt");
	option_add_string(&reboot_command, "reboot_command", "reboot");
	option_add_string(&suspend_command, "suspend_command",
			  "xset dpms force off");

	option_register_widget("mouse-tester", build_mouse_tester);
	option_register_widget("gtk-theme", build_gtk_theme);

	option_add_notify(xsettings_changed);

	xsettings_changed();

	factory = gtk_icon_factory_new();
	for (i = 0; i < G_N_ELEMENTS(stocks); i++)
	{
		GdkPixbuf *pixbuf;
		GError *error = NULL;
		gchar *path;
		GtkIconSet *iset;
		const gchar *name = stocks[i];

		path = g_strconcat(app_dir, "/images/", name, ".png", NULL);
		pixbuf = gdk_pixbuf_new_from_file(path, &error);
		if (!pixbuf)
		{
			g_warning("%s", error->message);
			g_error_free(error);
			pixbuf = gdk_pixbuf_new_from_xpm_data(bad_xpm);
		}
		g_free(path);

		iset = gtk_icon_set_new_from_pixbuf(pixbuf);
		g_object_unref(G_OBJECT(pixbuf));
		gtk_icon_factory_add(factory, name, iset);
		gtk_icon_set_unref(iset);
	}
	gtk_icon_factory_add_default(factory);
}

/* Called from the mainloop sometime after child_died executes */
void child_died_callback(void)
{
	pid_t	child;
	int	status;

	call_child_died = FALSE;

	/* Find out which children exited and allow them to die */
	while (1)
	{
		child = waitpid(-1, &status, WNOHANG);

		if (child == 0 || child == -1)
			break;

		if (child == wm_pid)
		{
			wm_pid = -1;
			wm_process_died();
		}

		if (child == rox_pid)
		{
			rox_pid = -1;
			rox_process_died();
		}

		if (child == login_child &&
			(WIFEXITED(status) == 0 || WEXITSTATUS(status) != 0))
		{
			login_child = -1;
			login_failure(WIFEXITED(status) == 0
					? -1	/* Signal death */
					: WEXITSTATUS(status));
		}
	}
}

void show_main_window(void)
{
	static GtkWidget *window = NULL;
	GtkWidget *button, *hbox, *vbox;

	if (window)
	{
		gtk_window_present(GTK_WINDOW(window));
		return;
	}
	
	window = gtk_dialog_new();
	gtk_dialog_set_has_separator(GTK_DIALOG(window), FALSE);
	vbox = GTK_DIALOG(window)->vbox;

	hbox = gtk_hbutton_box_new();
	gtk_container_set_border_width(GTK_CONTAINER(hbox), 2);
	gtk_button_box_set_layout(GTK_BUTTON_BOX(hbox), GTK_BUTTONBOX_END);
	gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, TRUE, 0);

	button = op_button(_("_Halt"), ROX_STOCK_HALT, &halt_command,
			_("Attempting to halt the system..."));
	GTK_WIDGET_UNSET_FLAGS(button, GTK_CAN_FOCUS);
	gtk_box_pack_end(GTK_BOX(hbox), button, FALSE, TRUE, 0);
	button = op_button(_("_Reboot"), GTK_STOCK_REFRESH, &reboot_command,
			_("Attempting to restart the system..."));
	GTK_WIDGET_UNSET_FLAGS(button, GTK_CAN_FOCUS);
	gtk_box_pack_end(GTK_BOX(hbox), button, FALSE, TRUE, 0);
	button = op_button(_("_Sleep"), ROX_STOCK_SUSPEND, &suspend_command,
			_("Attempting to enter suspend mode..."));
	GTK_WIDGET_UNSET_FLAGS(button, GTK_CAN_FOCUS);
	gtk_box_pack_end(GTK_BOX(hbox), button, FALSE, TRUE, 0);

	gtk_box_pack_start(GTK_BOX(vbox),
		gtk_label_new(_("Really logout?\n(unsaved data will be lost)")),
		TRUE, TRUE, 20);

	gtk_widget_show_all(vbox);

	gtk_window_set_title(GTK_WINDOW(window), PROJECT);

	gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);

	button = button_new_mixed(GTK_STOCK_PREFERENCES,
				_("Session Settings"));
	gtk_widget_show(button);
	gtk_dialog_add_action_widget(GTK_DIALOG(window), button, 1);

	gtk_dialog_add_button(GTK_DIALOG(window),
			GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL);

	button = button_new_mixed(GTK_STOCK_QUIT, _("Logout"));
	GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);
	gtk_widget_show(button);
	gtk_dialog_add_action_widget(GTK_DIALOG(window),
			button, GTK_RESPONSE_YES);

	gtk_dialog_set_default_response(GTK_DIALOG(window), GTK_RESPONSE_YES);

	switch (gtk_dialog_run(GTK_DIALOG(window)))
	{
		case GTK_RESPONSE_YES:
			gtk_main_quit();
			break;
		case 1:
			show_session_options();
			break;
		default:
			break;
	}

	gtk_widget_destroy(window);
	window = NULL;
}

void run_login_script(void)
{
	static gboolean logged_in = FALSE;
	GError	*error = NULL;
	gchar	*argv[2];
	gint	pid;

	if (logged_in || test_mode)
		return;
	logged_in = TRUE;

	/* Run ROX-Filer */
	run_rox_process();

	/* Run Login script */

	argv[0] = choices_find_path_load("Login", "ROX-Session");
	if (!argv[0])
		argv[0] = g_strconcat(app_dir, "/Login", NULL);

	argv[1] = NULL;
	g_spawn_async(NULL, argv, NULL,
			G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL,
			NULL, NULL, &pid, &error);
	g_free(argv[0]);

	if (error)
	{
		login_child = -1;
	
		report_error(_("Failed to run login script:\n%s"),
				error->message);
		
		g_error_free(error);
	}
	else
		login_child = pid;
}

/****************************************************************
 *			INTERNAL FUNCTIONS			*
 ****************************************************************/

/* Also called for mouse settings... */
static void xsettings_changed(void)
{
	int i;

	XChangePointerControl(GDK_DISPLAY(), True, True,
				mouse_accel_factor.int_value, 10,
				mouse_accel_threshold.int_value);

	if (!manager)
		return;

	for (i = 0; i < N_SETTINGS; i++)
	{
		if (g_ascii_isdigit(settings[i].default_value[0]))
			xsettings_manager_set_int(manager,
					settings[i].name,
					settings[i].option.int_value);
		else
			xsettings_manager_set_string(manager,
					settings[i].name,
					settings[i].option.value);
	}

	/* This one is hard-coded. Despite the name, it actually enables
	 * the standard Unix keybinding of Ctrl-U. It only adds shortcuts,
	 * without changing any existing ones, so we always want this.
	 */
	xsettings_manager_set_string(manager, "Gtk/KeyThemeName", "Emacs");

	xsettings_manager_notify(manager);
}

static void terminate_xsettings(void *data)
{
	g_warning("ROX-Session is no longer the XSETTINGS manager!");
}

/* This is called as a signal handler.
 * Don't do the waitpid here, because this might get called before
 * the assignment in child = fork() completes!
 */
static void child_died(int signum)
{
	fcntl(STDERR_FILENO, O_NONBLOCK, TRUE);
	write(STDERR_FILENO, "\n", 1);
	call_child_died = TRUE;
}

static gboolean tester_clicked(GtkBin *button, GdkEventButton *event)
{
	GtkLabel *label = GTK_LABEL(button->child);

	if (event->type == GDK_BUTTON_PRESS)
		gtk_label_set_text(label, _("Single click!"));
	else if (event->type == GDK_2BUTTON_PRESS)
		gtk_label_set_text(label, _("Double click!!"));
	else if (event->type == GDK_3BUTTON_PRESS)
		gtk_label_set_text(label, _("Triple click!!!"));
	
	return 1;
}

static gboolean reset_tester(GtkBin *button)
{
	GtkLabel *label = GTK_LABEL(button->child);

	gtk_label_set_text(label, _("Double-click here to test mouse"));
	return 0;
}

static GList *build_mouse_tester(Option *option, xmlNode *node, guchar *label)
{
	GtkWidget *widget;

	g_return_val_if_fail(option == NULL, NULL);
	g_return_val_if_fail(label == NULL, NULL);

	widget = gtk_button_new_with_label("");
	g_signal_connect(widget, "button-press-event",
			 G_CALLBACK(tester_clicked), NULL);
	g_signal_connect(widget, "leave-notify-event",
			 G_CALLBACK(reset_tester), NULL);
	reset_tester(GTK_BIN(widget));

	return g_list_prepend(NULL, widget);
}

static guchar *read_theme(Option *option)
{
	GtkOptionMenu *om = GTK_OPTION_MENU(option->widget);
	GtkLabel *item;

	item = GTK_LABEL(GTK_BIN(om)->child);

	g_return_val_if_fail(item != NULL, g_strdup("Default"));

	return g_strdup(gtk_label_get_text(item));
}

static void update_theme(Option *option)
{
	GtkOptionMenu *om = GTK_OPTION_MENU(option->widget);
	GtkWidget *menu;
	GList *kids, *next;
	int i = 0;

	menu = gtk_option_menu_get_menu(om);

	kids = gtk_container_get_children(GTK_CONTAINER(menu));
	for (next = kids; next; next = next->next, i++)
	{
		GtkLabel *item = GTK_LABEL(GTK_BIN(next->data)->child);
		const gchar *label;

		/* The label actually moves from the menu!! */
		if (!item)
			item = GTK_LABEL(GTK_BIN(om)->child);

		label = gtk_label_get_text(item);

		g_return_if_fail(label != NULL);

		if (strcmp(label, option->value) == 0)
			break;
	}
	g_list_free(kids);
	
	if (next)
		gtk_option_menu_set_history(om, i);
	else
		g_warning("Theme '%s' not found", option->value);
}

static void add_themes(GHashTable *themes, const guchar *path)
{
	DIR *dir;
	struct dirent *ent;

	dir = opendir(path);
	if (!dir)
		return;

	while ((ent = readdir(dir)))
	{
		char *gtk2;
		if (ent->d_name[0] == '.')
			continue;

		gtk2 = g_build_filename(path, ent->d_name, "gtk-2.0", NULL);
		if (access(gtk2, F_OK) == 0)
			g_hash_table_insert(themes,
					g_strdup(ent->d_name), NULL);
		g_free(gtk2);
	}
	closedir(dir);
}

static void hash_to_array(gpointer key, gpointer value, gpointer array)
{
	g_ptr_array_add((GPtrArray *) array, key);
}

static gint sort_by_name(gconstpointer a, gconstpointer b)
{
	return strcmp(*(char **) a, *(char **) b);
}

static GList *build_gtk_theme(Option *option, xmlNode *node, guchar *label)
{
	GtkWidget *button, *menu, *hbox;
	GHashTable *themes;
	GPtrArray *names;
	guchar *user_dir;
	int i;

	g_return_val_if_fail(option != NULL, NULL);
	g_return_val_if_fail(label != NULL, NULL);

	hbox = gtk_hbox_new(FALSE, 4);

	gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new(_(label)),
				FALSE, TRUE, 0);

	button = gtk_option_menu_new();
	gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);

	menu = gtk_menu_new();
	gtk_option_menu_set_menu(GTK_OPTION_MENU(button), menu);

	themes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	user_dir = g_build_filename(g_get_home_dir(), ".themes", NULL);
	add_themes(themes, user_dir);
	g_free(user_dir);
	add_themes(themes, gtk_rc_get_theme_dir());

	names = g_ptr_array_new();
	g_hash_table_foreach(themes, hash_to_array, names);
	g_ptr_array_sort(names, sort_by_name);

	for (i = 0; i < names->len; i++)
	{
		GtkWidget *item;
		const char *name = names->pdata[i];

		item = gtk_menu_item_new_with_label(name);
		gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
		gtk_widget_show_all(item);
	}

	g_ptr_array_free(names, TRUE);
	g_hash_table_destroy(themes);

	option->update_widget = update_theme;
	option->read_widget = read_theme;
	option->widget = button;

	gtk_signal_connect_object(GTK_OBJECT(button), "changed",
			GTK_SIGNAL_FUNC(option_check_widget),
			(GtkObject *) option);

	return g_list_append(NULL, hbox);
}

static void show_session_options(void)
{
	if (!manager)
		report_error(_("ROX-Session not managing XSettings, so changes "
				"will have no immediate effect..."));
	options_show();
}

static void op_clicked(GtkButton *button, Option *command)
{
	pid_t child;
	const char *message;
	GtkWidget *dialog;

	dialog = gtk_widget_get_toplevel(GTK_WIDGET(button));
	gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);

	message = g_object_get_data(G_OBJECT(button), "rox-message");

	child = fork();
	if (child == -1)
	{
		report_error("fork() failed: %s", g_strerror(errno));
		return;
	}
	else if (child)
		return;	/* Parent */

	dup2(STDERR_FILENO, STDOUT_FILENO);
	close(STDIN_FILENO);

	g_print("ROX-Session: %s\n", message);
	
	_exit(system(command->value));
}

static GtkWidget *op_button(const char *text, const char *stock,
			    Option *command, const char *message)
{
	GtkWidget *button, *image, *hbox, *label;
	gchar *tmp;

	button = gtk_button_new();

	hbox = gtk_hbox_new(FALSE, 0);
	gtk_container_add(GTK_CONTAINER(button), hbox);

	image = gtk_image_new_from_stock(stock, GTK_ICON_SIZE_BUTTON);
	gtk_box_pack_start(GTK_BOX(hbox), image, FALSE, TRUE, 4);

	label = gtk_label_new_with_mnemonic(text);
	gtk_misc_set_alignment(GTK_MISC(label), 0, 0.5);
	gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 0);

	/* No need for clickable buttons if no command is set */
	tmp = g_strstrip(g_strdup(command->value));
	if (*tmp)
	{
		g_object_set_data_full(G_OBJECT(button), "rox-message",
				g_strdup(message), g_free);
		g_signal_connect(button, "clicked", G_CALLBACK(op_clicked),
				command);
	}
	else
		gtk_widget_set_sensitive(button, FALSE);

	g_free (tmp);

	return button;
}

/* Like g_strdup, but does realpath() too (if possible) */
static char *pathdup(const char *path)
{
	char real[MAXPATHLEN];

	g_return_val_if_fail(path != NULL, NULL);

	if (realpath(path, real))
		return g_strdup(real);

	return g_strdup(path);
}

static void rox_process_died(void)
{
	int r;

	rox_pid = -1;

	r = get_choice(_("ROX-Filer has terminated (crashed?)."
			 "You should probably try to restart it."), 3,
			GTK_STOCK_NO, _("Do nothing"),
			GTK_STOCK_EXECUTE, _("Run Xterm"),
			GTK_STOCK_REFRESH, _("_Restart"));

	if (r == 2)
		run_rox_process();
	else if (r == 1)
		system("xterm &");
}

static void run_rox_process(void)
{
	GError	*error = NULL;
	gchar	*argv[3];
	gint	pid;

	argv[0] = choices_find_path_load("RunROX", "ROX-Session");
	if (!argv[0])
		argv[0] = g_strconcat(app_dir, "/RunROX", NULL);
	argv[1] = pathdup(app_dir);
	argv[2] = NULL;
	g_spawn_async(NULL, argv, NULL,
			G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL,
			NULL, NULL, &pid, &error);

	if (error)
	{
		rox_pid = -1;
		report_error(_("Failed to run '%s':\n%s"),
				argv[0], error->message);
		g_error_free(error);
		rox_process_died();
	}
	else
		rox_pid = pid;

	g_free(argv[0]);
	g_free(argv[1]);
}
