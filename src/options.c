/*
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

/* options.c - code for handling user choices */

/* How it works:
 *
 * On startup:
 *
 * - The <Choices>/PROJECT/Options file is read in. Each line
 *   is a name/value pair, and these are stored in the 'loading' hash table.
 *
 * - Each part of the filer then calls option_add_int(), or a related function,
 *   supplying the name for each option and a default value. Once an option is
 *   registered, it is removed from the loading table.
 *
 * - If things need to happen when values change, modules register with
 *   option_add_notify().
 *
 * - option_register_widget() can be used during initialisation (any time
 *   before the Options box is displayed) to tell the system how to render a
 *   particular type of option.
 *
 * - Finally, all notify callbacks are called. Use the Option->has_changed
 *   field to work out what has changed from the defaults.
 *
 * When the user opens the Options box:
 *
 * - The Options.xml file is read and used to create the Options dialog box.
 *   Each element in the file has a key corresponding to an option named
 *   above.
 *
 * - For each widget in the box, the current value of the option is used to
 *   set the widget's state.
 *
 * - All current values are saved for a possible Revert later.
 *
 * When the user changes an option or clicks on Revert:
 *
 * - The option values are updated.
 *
 * - All notify callbacks are called. Use the Option->has_changed field
 *   to see what changed.
 *
 * When Save is clicked:
 *
 * - All the options are written to the filesystem and the saver_callbacks are
 *   called.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <gtk/gtk.h>
#include <libxml/parser.h>

#include "choices.h"
#include "options.h"
#include "main.h"
#include "gui_support.h"

/* Add all option tooltips to this group */
static GtkTooltips *option_tooltips = NULL;
#define OPTION_TIP(widget, tip)	\
	gtk_tooltips_set_tip(option_tooltips, widget, tip, NULL)

/* The Options window. NULL if not yet created. */
static GtkWidget *window = NULL;

/* "filer_unique" -> (Option *) */
static GHashTable *option_hash = NULL;

/* A mapping (name -> value) for options which have been loaded by not
 * yet registered. The options in this table cannot be used until
 * option_add_*() is called to move them into option_hash.
 */
static GHashTable *loading = NULL;

/* A mapping (XML name -> OptionBuildFn). When reading the Options.xml
 * file, this table gives the function used to create the widgets.
 */
static GHashTable *widget_builder = NULL;

/* List of functions to call after all option values are updated */
static GList *notify_callbacks = NULL;

/* List of functions to call after all options are saved */
static GList *saver_callbacks = NULL;

static int updating_widgets = 0;	/* Ignore change signals when set */

/* Static prototypes */
static void save_options(GtkWidget *widget, gpointer data);
static void revert_options(GtkWidget *widget, gpointer data);
static void build_options_window(void);
static GtkWidget *build_frame(void);
static void update_option_widgets(void);
static void button_patch_set_colour(GtkWidget *button, GdkColor *color);
static void option_add(Option *option, guchar *key);
static void set_not_changed(gpointer key, gpointer value, gpointer data);
static void load_options(xmlDoc *doc);

static GList *build_toggle(Option *option, xmlNode *node, guchar *label);
static GList *build_slider(Option *option, xmlNode *node, guchar *label);
static GList *build_entry(Option *option, xmlNode *node, guchar *label);
static GList *build_radio_group(Option *option, xmlNode *node, guchar *label);
static GList *build_colour(Option *option, xmlNode *node, guchar *label);
static GList *build_menu(Option *option, xmlNode *node, guchar *label);
static GList *build_font(Option *option, xmlNode *node, guchar *label);


/****************************************************************
 *			EXTERNAL INTERFACE			*
 ****************************************************************/

void options_init(void)
{
	char	*path;
	xmlDoc	*doc;

	loading = g_hash_table_new(g_str_hash, g_str_equal);
	option_hash = g_hash_table_new(g_str_hash, g_str_equal);
	widget_builder = g_hash_table_new(g_str_hash, g_str_equal);

	path = choices_find_path_load("Options", PROJECT);
	if (path)
	{
		/* Load in all the options set in the filer, storing them
		 * temporarily in the loading hash table.
		 * They get moved to option_hash when they're registered.
		 */
		doc = xmlParseFile(path);
		load_options(doc);
		xmlFreeDoc(doc);

		g_free(path);
	}

	option_register_widget("toggle", build_toggle);
	option_register_widget("slider", build_slider);
	option_register_widget("entry", build_entry);
	option_register_widget("radio-group", build_radio_group);
	option_register_widget("colour", build_colour);
	option_register_widget("menu", build_menu);
	option_register_widget("font", build_font);
}

/* When parsing the XML file, process an element named 'name' by
 * calling 'builder(option, xml_node, label)'.
 * builder returns the new widgets to add to the options box.
 * 'name' should be a static string. Call 'option_check_widget' when
 * the widget's value is modified.
 *
 * Functions to set or get the widget's state can be stored in 'option'.
 * If the option doesn't have a name attribute in Options.xml then
 * ui will be NULL on entry (this is used for buttons).
 */
void option_register_widget(char *name, OptionBuildFn builder)
{
	g_hash_table_insert(widget_builder, name, builder);
}

/* This is called when the widget's value is modified by the user.
 * Reads the new value of the widget into the option and calls
 * the notify callbacks.
 */
void option_check_widget(Option *option)
{
	guchar		*new = NULL;

	if (updating_widgets)
		return;		/* Not caused by the user... */

	g_return_if_fail(option->read_widget != NULL);

	new = option->read_widget(option);

	g_return_if_fail(new != NULL);

	g_hash_table_foreach(option_hash, set_not_changed, NULL);

	option->has_changed = strcmp(option->value, new) != 0;

	if (!option->has_changed)
	{
		g_free(new);
		return;
	}

	g_free(option->value);
	option->value = new;
	option->int_value = atoi(new);

	options_notify();
}

/* Call all the notify callbacks. This should happen after any options
 * have their values changed. Set each has_changed before calling.
 */
void options_notify(void)
{
	GList	*next;

	for (next = notify_callbacks; next; next = next->next)
	{
		OptionNotify *cb = (OptionNotify *) next->data;

		cb();
	}
}

/* Store values used by Revert */
static void store_backup(gpointer key, gpointer value, gpointer data)
{
	Option *option = (Option *) value;

	g_free(option->backup);
	option->backup = g_strdup(option->value);
}

/* Allow the user to edit the options. Returns the window widget (you don't
 * normally need this). NULL if already open.
 */
GtkWidget *options_show(void)
{
	if (!option_tooltips)
		option_tooltips = gtk_tooltips_new();

	if (g_hash_table_size(loading) != 0)
	{
		g_printerr(PROJECT ": Some options loaded but not used:\n");
		g_hash_table_foreach(loading, (GHFunc) puts, NULL);
	}

	if (window)
	{
		gtk_window_present(GTK_WINDOW(window));
		return NULL;
	}

	g_hash_table_foreach(option_hash, store_backup, NULL);
			
	build_options_window();

	update_option_widgets();
	
	gtk_widget_show_all(window);

	return window;
}

/* Initialise and register a new integer option */
void option_add_int(Option *option, guchar *key, int value)
{
	option->value = g_strdup_printf("%d", value);
	option->int_value = value;
	option_add(option, key);
}

void option_add_string(Option *option, guchar *key, guchar *value)
{
	option->value = g_strdup(value);
	option->int_value = atoi(value);
	option_add(option, key);
}

/* Add a callback which will be called after any options have changed their
 * values. If serveral options change at once, this is called after all
 * changes.
 */
void option_add_notify(OptionNotify *callback)
{
	g_return_if_fail(callback != NULL);

	notify_callbacks = g_list_append(notify_callbacks, callback);
}

/* Call 'callback' after all the options have been saved */
void option_add_saver(OptionNotify *callback)
{
	g_return_if_fail(callback != NULL);

	saver_callbacks = g_list_append(saver_callbacks, callback);
}

/****************************************************************
 *                      INTERNAL FUNCTIONS                      *
 ****************************************************************/

/* Option should contain the default value.
 * It must never be destroyed after being registered (Options are typically
 * statically allocated).
 * The key corresponds to the option's name in Options.xml, and to the key
 * in the saved options file.
 *
 * On exit, the value will have been updated to the loaded value, if
 * different to the default.
 */
static void option_add(Option *option, guchar *key)
{
	gpointer okey, value;

	g_return_if_fail(option_hash != NULL);
	g_return_if_fail(g_hash_table_lookup(option_hash, key) == NULL);
	g_return_if_fail(option->value != NULL);
	
	option->has_changed = FALSE;

	option->widget = NULL;
	option->update_widget = NULL;
	option->read_widget = NULL;
	option->backup = NULL;

	g_hash_table_insert(option_hash, key, option);

	/* Use the value loaded from the file, if any */
	if (g_hash_table_lookup_extended(loading, key, &okey, &value))
	{
		option->has_changed = strcmp(option->value, value) != 0;
			
		g_free(option->value);
		option->value = value;
		option->int_value = atoi(value);
		g_hash_table_remove(loading, key);
		g_free(okey);
	}
}

static GtkColorSelectionDialog *current_csel_box = NULL;
static GtkFontSelectionDialog *current_fontsel_box = NULL;

static void get_new_colour(GtkWidget *ok, Option *option)
{
	GtkWidget	*csel;
	gdouble		c[4];
	GdkColor	colour;

	g_return_if_fail(current_csel_box != NULL);

	csel = current_csel_box->colorsel;

	gtk_color_selection_get_color(GTK_COLOR_SELECTION(csel), c);
	colour.red = c[0] * 0xffff;
	colour.green = c[1] * 0xffff;
	colour.blue = c[2] * 0xffff;

	button_patch_set_colour(option->widget, &colour);
	
	gtk_widget_destroy(GTK_WIDGET(current_csel_box));

	option_check_widget(option);
}

static void set_to_null(gpointer *data)
{
	*data = NULL;
}

static void open_coloursel(GtkWidget *button, Option *option)
{
	GtkColorSelectionDialog	*csel;
	GtkWidget		*dialog, *patch;
	gdouble			c[4];

	if (current_csel_box)
		gtk_widget_destroy(GTK_WIDGET(current_csel_box));

	dialog = gtk_color_selection_dialog_new(NULL);
	csel = GTK_COLOR_SELECTION_DIALOG(dialog);
	current_csel_box = csel;
	gtk_window_set_position(GTK_WINDOW(csel), GTK_WIN_POS_MOUSE);

	gtk_signal_connect_object(GTK_OBJECT(dialog), "destroy",
			GTK_SIGNAL_FUNC(set_to_null),
			(GtkObject *) &current_csel_box);
	gtk_widget_hide(csel->help_button);
	gtk_signal_connect_object(GTK_OBJECT(csel->cancel_button), "clicked",
			GTK_SIGNAL_FUNC(gtk_widget_destroy),
			GTK_OBJECT(dialog));
	gtk_signal_connect(GTK_OBJECT(csel->ok_button), "clicked",
			GTK_SIGNAL_FUNC(get_new_colour), option);

	patch = GTK_BIN(button)->child;

	c[0] = ((gdouble) patch->style->bg[GTK_STATE_NORMAL].red) / 0xffff;
	c[1] = ((gdouble) patch->style->bg[GTK_STATE_NORMAL].green) / 0xffff;
	c[2] = ((gdouble) patch->style->bg[GTK_STATE_NORMAL].blue) / 0xffff;
	gtk_color_selection_set_color(GTK_COLOR_SELECTION(csel->colorsel), c);

	gtk_widget_show(dialog);
}

static void font_chosen(GtkWidget *dialog, gint response, Option *option)
{
	gchar *font;

	if (response != GTK_RESPONSE_OK)
		goto out;

	font = gtk_font_selection_dialog_get_font_name(
					GTK_FONT_SELECTION_DIALOG(dialog));

	gtk_label_set_text(GTK_LABEL(option->widget), font);

	g_free(font);

	option_check_widget(option);

out:
	gtk_widget_destroy(dialog);

}

static void open_fontsel(GtkWidget *button, Option *option)
{
	if (current_fontsel_box)
		gtk_widget_destroy(GTK_WIDGET(current_fontsel_box));

	current_fontsel_box = GTK_FONT_SELECTION_DIALOG(
				gtk_font_selection_dialog_new(PROJECT));

	gtk_window_set_position(GTK_WINDOW(current_fontsel_box),
				GTK_WIN_POS_MOUSE);

	gtk_signal_connect_object(GTK_OBJECT(current_fontsel_box), "destroy",
			GTK_SIGNAL_FUNC(set_to_null),
			(GtkObject *) &current_fontsel_box);

	gtk_font_selection_dialog_set_font_name(current_fontsel_box,
						option->value);

	gtk_signal_connect(GTK_OBJECT(current_fontsel_box), "response",
			GTK_SIGNAL_FUNC(font_chosen), option);

	gtk_widget_show(GTK_WIDGET(current_fontsel_box));
}

/* These are used during parsing... */
static xmlDocPtr options_doc = NULL;

#define DATA(node) (xmlNodeListGetString(options_doc, node->xmlChildrenNode, 1))

static void may_add_tip(GtkWidget *widget, xmlNode *element)
{
	guchar	*data, *tip;

	data = DATA(element);
	if (!data)
		return;

	tip = g_strstrip(g_strdup(data));
	g_free(data);
	if (*tip)
		OPTION_TIP(widget, _(tip));
	g_free(tip);
}

static int get_int(xmlNode *node, guchar *attr)
{
	guchar *txt;
	int	retval;

	txt = xmlGetProp(node, attr);
	if (!txt)
		return 0;

	retval = atoi(txt);
	g_free(txt);

	return retval;
}

static GtkWidget *build_radio(xmlNode *radio, GtkWidget *prev)
{
	GtkWidget	*button;
	GtkRadioButton	*prev_button = (GtkRadioButton *) prev;
	guchar		*label;

	label = xmlGetProp(radio, "label");

	button = gtk_radio_button_new_with_label(
			prev_button ? gtk_radio_button_group(prev_button)
				    : NULL,
			_(label));
	g_free(label);

	may_add_tip(button, radio);

	gtk_object_set_data(GTK_OBJECT(button), "value",
			xmlGetProp(radio, "value"));

	return button;
}

static void build_menu_item(xmlNode *node, GtkWidget *option_menu)
{
	GtkWidget	*item, *menu;
	guchar		*label;

	g_return_if_fail(strcmp(node->name, "item") == 0);

	label = xmlGetProp(node, "label");
	item = gtk_menu_item_new_with_label(_(label));
	g_free(label);

	menu = gtk_option_menu_get_menu(GTK_OPTION_MENU(option_menu));
	gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
	gtk_widget_show_all(menu);

	gtk_object_set_data(GTK_OBJECT(item),
				"value", xmlGetProp(node, "value"));
}

static void build_widget(xmlNode *widget, GtkWidget *box)
{
	const char *name = widget->name;
	OptionBuildFn builder;
	guchar	*oname;
	Option	*option;
	guchar	*label;

	if (strcmp(name, "label") == 0)
	{
		GtkWidget *label;
		guchar *text;

		text = DATA(widget);
		label = gtk_label_new(_(text));
		g_free(text);

		gtk_misc_set_alignment(GTK_MISC(label), 0, 1);
		gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_LEFT);
		gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
		gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
		return;
	}
	else if (strcmp(name, "spacer") == 0)
	{
		GtkWidget *eb;

		eb = gtk_event_box_new();
		gtk_widget_set_usize(eb, 12, 12);
		gtk_box_pack_start(GTK_BOX(box), eb, FALSE, TRUE, 0);
		return;
	}

	label = xmlGetProp(widget, "label");

	if (strcmp(name, "hbox") == 0 || strcmp(name, "vbox") == 0)
	{
		GtkWidget *nbox;
		xmlNode	  *hw;

		if (name[0] == 'h')
			nbox = gtk_hbox_new(FALSE, 4);
		else
			nbox = gtk_vbox_new(FALSE, 4);

		if (label)
			gtk_box_pack_start(GTK_BOX(nbox),
				gtk_label_new(_(label)), FALSE, TRUE, 4);
		gtk_box_pack_start(GTK_BOX(box), nbox, FALSE, TRUE, 0);

		for (hw = widget->xmlChildrenNode; hw; hw = hw->next)
		{
			if (hw->type == XML_ELEMENT_NODE)
				build_widget(hw, nbox);
		}

		g_free(label);
		return;
	}

	oname = xmlGetProp(widget, "name");

	if (oname)
	{
		option = g_hash_table_lookup(option_hash, oname);

		if (!option)
		{
			g_warning("No Option for '%s'!\n", oname);
			g_free(oname);
			return;
		}

		g_free(oname);
	}
	else
		option = NULL;

	builder = g_hash_table_lookup(widget_builder, name);
	if (builder)
	{
		GList *widgets, *next;

		if (option && option->widget)
			g_warning("Widget for option already exists!");

		widgets = builder(option, widget, label);

		for (next = widgets; next; next = next->next)
		{
			GtkWidget *w = (GtkWidget *) next->data;
			gtk_box_pack_start(GTK_BOX(box), w, FALSE, TRUE, 0);
		}
		g_list_free(widgets);
	}
	else
		g_warning("Unknown option type '%s'\n", name);

	g_free(label);
}

static void build_sections(xmlNode *options, GtkWidget *sections_box)
{
	xmlNode	*section = options->xmlChildrenNode;

	g_return_if_fail(strcmp(options->name, "options") == 0);

	for (; section; section = section->next)
	{
		guchar 		*title;
		GtkWidget   	*page, *scrolled_area;
		xmlNode		*widget;

		if (section->type != XML_ELEMENT_NODE)
			continue;

		title = xmlGetProp(section, "title");
		page = gtk_vbox_new(FALSE, 0);
		gtk_container_set_border_width(GTK_CONTAINER(page), 4);

		scrolled_area = gtk_scrolled_window_new(NULL, NULL);
		gtk_scrolled_window_set_policy(
					GTK_SCROLLED_WINDOW(scrolled_area),
					GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

		gtk_scrolled_window_add_with_viewport(
					GTK_SCROLLED_WINDOW(scrolled_area),
					page);

		gtk_notebook_append_page(GTK_NOTEBOOK(sections_box),
					scrolled_area,
					gtk_label_new(_(title)));

		widget = section->xmlChildrenNode;
		for (; widget; widget = widget->next)
		{
			if (widget->type == XML_ELEMENT_NODE)
				build_widget(widget, page);
		}

		g_free(title);
	}
}

/* Parse <app_dir>/Options.xml to create the options window.
 * Sets the global 'window' variable.
 */
static void build_options_window(void)
{
	GtkWidget *sections_box;
	xmlDocPtr options_doc;
	gchar	  *path;

	sections_box = build_frame();
	
	path = g_strconcat(app_dir, "/Options.xml", NULL);
	options_doc = xmlParseFile(path);

	if (!options_doc)
	{
		report_error("Internal error: %s unreadable", path);
		g_free(path);
		return;
	}

	g_free(path);

	build_sections(xmlDocGetRootElement(options_doc), sections_box);

	xmlFreeDoc(options_doc);
	options_doc = NULL;
}

static void null_widget(gpointer key, gpointer value, gpointer data)
{
	Option	*option = (Option *) value;

	g_return_if_fail(option->widget != NULL);

	option->widget = NULL;
}

static void options_destroyed(GtkWidget *widget, gpointer data)
{
	if (current_csel_box)
		gtk_widget_destroy(GTK_WIDGET(current_csel_box));
	if (current_fontsel_box)
		gtk_widget_destroy(GTK_WIDGET(current_fontsel_box));
	
	if (widget == window)
	{
		window = NULL;

		g_hash_table_foreach(option_hash, null_widget, NULL);
	}
}

/* Creates the window and adds the various buttons to it.
 * Returns the vbox to add sections to and sets the global
 * 'window'.
 */
static GtkWidget *build_frame(void)
{
	GtkWidget	*sections_box;
	GtkWidget	*tl_vbox;
	GtkWidget	*actions, *button;
	char		*string, *save_path;

	window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

	gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
	gtk_window_set_title(GTK_WINDOW(window), _("Options"));
	gtk_signal_connect(GTK_OBJECT(window), "destroy",
			GTK_SIGNAL_FUNC(options_destroyed), NULL);
	gtk_container_set_border_width(GTK_CONTAINER(window), 4);
	gtk_window_set_default_size(GTK_WINDOW(window), -1, 300);

	tl_vbox = gtk_vbox_new(FALSE, 4);
	gtk_container_add(GTK_CONTAINER(window), tl_vbox);

	sections_box = gtk_notebook_new();
	gtk_notebook_set_scrollable(GTK_NOTEBOOK(sections_box), TRUE);
	gtk_notebook_set_tab_pos(GTK_NOTEBOOK(sections_box), GTK_POS_LEFT);
	gtk_box_pack_start(GTK_BOX(tl_vbox), sections_box, TRUE, TRUE, 0);
	
	actions = gtk_hbutton_box_new();
	gtk_button_box_set_layout(GTK_BUTTON_BOX(actions),
				  GTK_BUTTONBOX_END);
	gtk_button_box_set_spacing(GTK_BUTTON_BOX(actions), 10);

	save_path = choices_find_path_save("...", PROJECT, FALSE);
	if (save_path)
		gtk_box_pack_start(GTK_BOX(tl_vbox), actions, FALSE, TRUE, 0);
	else
	{
		GtkWidget *hbox;

		hbox = gtk_hbox_new(FALSE, 0);
		gtk_box_pack_start(GTK_BOX(tl_vbox), hbox, FALSE, TRUE, 0);
		gtk_box_pack_start(GTK_BOX(hbox),
			gtk_label_new(_("(saving disabled by CHOICESPATH)")),
			FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(hbox), actions, TRUE, TRUE, 0);
	}
	
	button = button_new_mixed(GTK_STOCK_UNDO, "_Revert");
	GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);
	gtk_box_pack_start(GTK_BOX(actions), button, FALSE, TRUE, 0);
	gtk_signal_connect(GTK_OBJECT(button), "clicked",
			GTK_SIGNAL_FUNC(revert_options), NULL);
	gtk_tooltips_set_tip(option_tooltips, button,
			_("Restore all choices to how they were when the "
			  "Options box was opened."), NULL);

	button = button_new_mixed(GTK_STOCK_APPLY, "_OK");
	GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);
	gtk_box_pack_start(GTK_BOX(actions), button, FALSE, TRUE, 0);
	gtk_signal_connect_object(GTK_OBJECT(button), "clicked",
		GTK_SIGNAL_FUNC(gtk_widget_destroy), GTK_OBJECT(window));

	if (save_path)
	{
		button = gtk_button_new_from_stock(GTK_STOCK_SAVE);
		gtk_box_pack_start(GTK_BOX(actions), button, FALSE, TRUE, 0);
		gtk_signal_connect(GTK_OBJECT(button), "clicked",
			GTK_SIGNAL_FUNC(save_options), NULL);

		string = g_strdup_printf(_("Choices will be saved as:\n%s"),
					save_path);
		gtk_tooltips_set_tip(option_tooltips, button, string, NULL);
		g_free(save_path);
		g_free(string);
		GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);
		gtk_widget_grab_default(button);
		gtk_widget_grab_focus(button);
	}

	return sections_box;
}

/* Given the last radio button in the group, select whichever
 * radio button matches the given value.
 */
static void radio_group_set_value(GtkRadioButton *last, guchar *value)
{	
	GSList	*next;

	next = gtk_radio_button_group(last);
	while (next)
	{
		GtkToggleButton *button = (GtkToggleButton *) next->data;
		guchar	*val;

		val = gtk_object_get_data(GTK_OBJECT(button), "value");
		g_return_if_fail(val != NULL);

		if (strcmp(val, value) == 0)
		{
			gtk_toggle_button_set_active(button, TRUE);
			return;
		}
		
		next = next->next;
	}

	g_warning("Can't find radio button with value %s\n", value);
}

/* Given the last radio button in the group, return a copy of the
 * value for the selected radio item.
 */
static guchar *radio_group_get_value(GtkRadioButton *last)
{
	GSList	*next;

	next = gtk_radio_button_group(last);
	while (next)
	{
		GtkToggleButton *button = (GtkToggleButton *) next->data;

		if (gtk_toggle_button_get_active(button))
		{
			guchar	*val;

			val = gtk_object_get_data(GTK_OBJECT(button), "value");
			g_return_val_if_fail(val != NULL, NULL);

			return g_strdup(val);
		}
		
		next = next->next;
	}

	return NULL;
}

/* Select this item with this value */
static void option_menu_set(GtkOptionMenu *om, guchar *value)
{
	GtkWidget *menu;
	GList	  *list, *next;
	int	  i = 0;

	menu = gtk_option_menu_get_menu(om);
	list = gtk_container_children(GTK_CONTAINER(menu));

	for (next = list; next; next = next->next)
	{
		GtkObject	*item = (GtkObject *) next->data;
		guchar		*data;

		data = gtk_object_get_data(item, "value");
		g_return_if_fail(data != NULL);

		if (strcmp(data, value) == 0)
		{
			gtk_option_menu_set_history(om, i);
			break;
		}
		
		i++;
	}

	g_list_free(list);
}

/* Get the value (static) of the selected item */
static guchar *option_menu_get(GtkOptionMenu *om)
{
	GtkWidget *menu, *item;

	menu = gtk_option_menu_get_menu(om);
	item = gtk_menu_get_active(GTK_MENU(menu));

	return gtk_object_get_data(GTK_OBJECT(item), "value");
}

static void restore_backup(gpointer key, gpointer value, gpointer data)
{
	Option *option = (Option *) value;

	g_return_if_fail(option->backup != NULL);

	option->has_changed = strcmp(option->value, option->backup) != 0;
	if (!option->has_changed)
		return;

	g_free(option->value);
	option->value = g_strdup(option->backup);
	option->int_value = atoi(option->value);
}

static void revert_options(GtkWidget *widget, gpointer data)
{
	g_hash_table_foreach(option_hash, restore_backup, NULL);
	options_notify();
	update_option_widgets();
}

static void write_option(gpointer key, gpointer value, gpointer data)
{
	xmlNodePtr doc = (xmlNodePtr) data;
	Option *option = (Option *) value;
	xmlNodePtr tree;

	tree = xmlNewTextChild(doc, NULL, "Option", option->value);
	xmlSetProp(tree, "name", (gchar *) key);
}

/* Save doc as XML as filename, 0 on success or -1 on failure */
static int save_xml_file(xmlDocPtr doc, gchar *filename)
{
#if LIBXML_VERSION > 20400
	if (xmlSaveFormatFileEnc(filename, doc, NULL, 1) < 0)
		return 1;
#else
	FILE *out;
	
	out = fopen(filename, "w");
	if (!out)
		return 1;

	xmlDocDump(out, doc);  /* Some versions return void */

	if (fclose(out))
		return 1;
#endif

	return 0;
}

static void save_options(GtkWidget *widget, gpointer data)
{
	xmlDoc	*doc;
	GList	*next;
	guchar	*save, *save_new;

	save = choices_find_path_save("Options", PROJECT, TRUE);
	if (!save)
	{
		report_error(_("Could not save options: %s"),
				_("Choices saving is disabled by "
					"CHOICESPATH variable"));
		return;
	}

	save_new = g_strconcat(save, ".new", NULL);

	doc = xmlNewDoc("1.0");
	xmlDocSetRootElement(doc, xmlNewDocNode(doc, NULL, "Options", NULL));

	g_hash_table_foreach(option_hash, write_option,
				xmlDocGetRootElement(doc));

	if (save_xml_file(doc, save_new) || rename(save_new, save))
		report_error(_("Error saving %s: %s"), save, g_strerror(errno));

	g_free(save_new);
	g_free(save);
	xmlFreeDoc(doc);

	for (next = saver_callbacks; next; next = next->next)
	{
		OptionNotify *cb = (OptionNotify *) next->data;
		cb();
	}

	gtk_widget_destroy(window);
}

/* Make the widget reflect the current value of the option */
static void update_cb(gpointer key, gpointer value, gpointer data)
{
	Option *option = (Option *) value;

	g_return_if_fail(option != NULL);
	g_return_if_fail(option->widget != NULL);

	updating_widgets++;
	
	if (option->update_widget)
		option->update_widget(option);

	updating_widgets--;
}

/* Reflect the values in the Option structures by changing the widgets
 * in the Options window.
 */
static void update_option_widgets(void)
{
	g_hash_table_foreach(option_hash, update_cb, NULL);
}

/* Each of the following update the widget to make it show the current
 * value of the option.
 */

static void update_toggle(Option *option)
{
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(option->widget),
			option->int_value);
}

static void update_entry(Option *option)
{
	gtk_entry_set_text(GTK_ENTRY(option->widget), option->value);
}

static void update_radio_group(Option *option)
{
	radio_group_set_value(GTK_RADIO_BUTTON(option->widget), option->value);
}

static void update_slider(Option *option)
{
	gtk_adjustment_set_value(
			gtk_range_get_adjustment(GTK_RANGE(option->widget)),
			option->int_value);
}

static void update_menu(Option *option)
{
	option_menu_set(GTK_OPTION_MENU(option->widget), option->value);
}

static void update_font(Option *option)
{
	gtk_label_set_text(GTK_LABEL(option->widget), option->value);
}

static void update_colour(Option *option)
{
	GdkColor colour;

	gdk_color_parse(option->value, &colour);
	button_patch_set_colour(option->widget, &colour);
}

/* Each of these read_* calls get the new (string) value of an option
 * from the widget.
 */

static guchar *read_toggle(Option *option)
{
	GtkToggleButton *toggle = GTK_TOGGLE_BUTTON(option->widget);

	return g_strdup_printf("%d", gtk_toggle_button_get_active(toggle));
}

static guchar *read_entry(Option *option)
{
	return gtk_editable_get_chars(GTK_EDITABLE(option->widget), 0, -1);
}

static guchar *read_slider(Option *option)
{
	return g_strdup_printf("%f",
		gtk_range_get_adjustment(GTK_RANGE(option->widget))->value);
}

static guchar *read_radio_group(Option *option)
{
	return radio_group_get_value(GTK_RADIO_BUTTON(option->widget));
}

static guchar *read_menu(Option *option)
{
	return g_strdup(option_menu_get(GTK_OPTION_MENU(option->widget)));
}

static guchar *read_font(Option *option)
{
	return g_strdup(gtk_label_get_text(GTK_LABEL(option->widget)));
}

static guchar *read_colour(Option *option)
{
	GtkStyle *style = GTK_BIN(option->widget)->child->style;

	return g_strdup_printf("#%04x%04x%04x",
			style->bg[GTK_STATE_NORMAL].red,
			style->bg[GTK_STATE_NORMAL].green,
			style->bg[GTK_STATE_NORMAL].blue);
}

static void set_not_changed(gpointer key, gpointer value, gpointer data)
{
	Option	*option = (Option *) value;

	option->has_changed = FALSE;
}

/* These create new widgets in the options window and set the appropriate
 * callbacks.
 */

static GList *build_toggle(Option *option, xmlNode *node, guchar *label)
{
	GtkWidget	*toggle;

	g_return_val_if_fail(option != NULL, NULL);

	toggle = gtk_check_button_new_with_label(_(label));

	may_add_tip(toggle, node);

	option->update_widget = update_toggle;
	option->read_widget = read_toggle;
	option->widget = toggle;

	gtk_signal_connect_object(GTK_OBJECT(toggle), "toggled",
			GTK_SIGNAL_FUNC(option_check_widget),
			(GtkObject *) option);

	return g_list_append(NULL, toggle);
}

static GList *build_slider(Option *option, xmlNode *node, guchar *label)
{
	GtkAdjustment *adj;
	GtkWidget *hbox, *slide;
	int	min, max;
	int	fixed;
	int	showvalue;
	guchar	*end;

	g_return_val_if_fail(option != NULL, NULL);

	min = get_int(node, "min");
	max = get_int(node, "max");
	fixed = get_int(node, "fixed");
	showvalue = get_int(node, "showvalue");

	adj = GTK_ADJUSTMENT(gtk_adjustment_new(0,
				min, max, 1, 10, 0));

	hbox = gtk_hbox_new(FALSE, 4);
	gtk_box_pack_start(GTK_BOX(hbox),
			gtk_label_new(_(label)),
			FALSE, TRUE, 0);

	end = xmlGetProp(node, "end");
	if (end)
	{
		gtk_box_pack_end(GTK_BOX(hbox), gtk_label_new(_(end)),
				 FALSE, TRUE, 0);
		g_free(end);
	}

	slide = gtk_hscale_new(adj);

	if (fixed)
		gtk_widget_set_usize(slide, adj->upper, 24);
	if (showvalue)
	{
		gtk_scale_set_draw_value(GTK_SCALE(slide), TRUE);
		gtk_scale_set_value_pos(GTK_SCALE(slide),
				GTK_POS_LEFT);
		gtk_scale_set_digits(GTK_SCALE(slide), 0);
	}
	else 
		gtk_scale_set_draw_value(GTK_SCALE(slide), FALSE);
	GTK_WIDGET_UNSET_FLAGS(slide, GTK_CAN_FOCUS);

	may_add_tip(slide, node);

	gtk_box_pack_start(GTK_BOX(hbox), slide, !fixed, TRUE, 0);

	option->update_widget = update_slider;
	option->read_widget = read_slider;
	option->widget = slide;

	gtk_signal_connect_object(GTK_OBJECT(adj), "value-changed",
			GTK_SIGNAL_FUNC(option_check_widget),
			(GtkObject *) option);

	return g_list_append(NULL, hbox);
}

static GList *build_entry(Option *option, xmlNode *node, guchar *label)
{
	GtkWidget	*hbox;
	GtkWidget	*entry;

	g_return_val_if_fail(option != NULL, NULL);

	hbox = gtk_hbox_new(FALSE, 4);

	gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new(_(label)),
				FALSE, TRUE, 0);

	entry = gtk_entry_new();
	gtk_box_pack_start(GTK_BOX(hbox), entry, TRUE, TRUE, 0);
	may_add_tip(entry, node);

	option->update_widget = update_entry;
	option->read_widget = read_entry;
	option->widget = entry;

	gtk_signal_connect_object_after(GTK_OBJECT(entry), "changed",
			GTK_SIGNAL_FUNC(option_check_widget),
			(GtkObject *) option);

	return g_list_append(NULL, hbox);
}

static GList *build_radio_group(Option *option, xmlNode *node, guchar *label)
{
	GList		*list = NULL;
	GtkWidget	*button = NULL;
	xmlNode		*rn;

	g_return_val_if_fail(option != NULL, NULL);

	for (rn = node->xmlChildrenNode; rn; rn = rn->next)
	{
		if (rn->type == XML_ELEMENT_NODE)
		{
			button = build_radio(rn, button);
			gtk_signal_connect_object(GTK_OBJECT(button), "toggled",
				GTK_SIGNAL_FUNC(option_check_widget),
				(GtkObject *) option);
			list = g_list_append(list, button);
		}
	}

	option->update_widget = update_radio_group;
	option->read_widget = read_radio_group;
	option->widget = button;

	return list;
}

static GList *build_colour(Option *option, xmlNode *node, guchar *label)
{
	GtkWidget	*hbox, *da, *button;
	int		lpos;
	
	g_return_val_if_fail(option != NULL, NULL);

	/* lpos gives the position for the label 
	 * 0: label comes before the button
	 * non-zero: label comes after the button
	 */
	lpos = get_int(node, "lpos");

	hbox = gtk_hbox_new(FALSE, 4);
	if (lpos == 0)
		gtk_box_pack_start(GTK_BOX(hbox),
			gtk_label_new(_(label)),
			FALSE, TRUE, 0);

	button = gtk_button_new();
	da = gtk_drawing_area_new();
	gtk_drawing_area_size(GTK_DRAWING_AREA(da), 64, 12);
	gtk_container_add(GTK_CONTAINER(button), da);
	gtk_signal_connect(GTK_OBJECT(button), "clicked",
			GTK_SIGNAL_FUNC(open_coloursel), option);

	may_add_tip(button, node);
	
	gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, TRUE, 0);
	if (lpos)
		gtk_box_pack_start(GTK_BOX(hbox),
			gtk_label_new(_(label)),
			FALSE, TRUE, 0);

	option->update_widget = update_colour;
	option->read_widget = read_colour;
	option->widget = button;

	return g_list_append(NULL, hbox);
}

static GList *build_menu(Option *option, xmlNode *node, guchar *label)
{
	GtkWidget	*hbox, *om, *option_menu;
	xmlNode		*item;
	GtkWidget	*menu;
	GList		*list, *kids;
	int		min_w = 4, min_h = 4;

	g_return_val_if_fail(option != NULL, NULL);

	hbox = gtk_hbox_new(FALSE, 4);

	gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new(_(label)),
			FALSE, TRUE, 0);

	option_menu = gtk_option_menu_new();
	gtk_box_pack_start(GTK_BOX(hbox), option_menu, FALSE, TRUE, 0);

	om = gtk_menu_new();
	gtk_option_menu_set_menu(GTK_OPTION_MENU(option_menu), om);

	for (item = node->xmlChildrenNode; item; item = item->next)
	{
		if (item->type == XML_ELEMENT_NODE)
			build_menu_item(item, option_menu);
	}

	menu = gtk_option_menu_get_menu(GTK_OPTION_MENU(option_menu));
	list = kids = gtk_container_children(GTK_CONTAINER(menu));

	while (kids)
	{
		GtkWidget	*item = (GtkWidget *) kids->data;
		GtkRequisition	req;

		gtk_widget_size_request(item, &req);
		if (req.width > min_w)
			min_w = req.width;
		if (req.height > min_h)
			min_h = req.height;
		
		kids = kids->next;
	}

	g_list_free(list);

	gtk_widget_set_usize(option_menu,
			min_w + 50,	/* Else node doesn't work! */
			min_h + 4);

	option->update_widget = update_menu;
	option->read_widget = read_menu;
	option->widget = option_menu;

	gtk_signal_connect_object_after(GTK_OBJECT(option_menu), "changed",
			GTK_SIGNAL_FUNC(option_check_widget),
			(GtkObject *) option);

	return g_list_append(NULL, hbox);
}

static GList *build_font(Option *option, xmlNode *node, guchar *label)
{
	GtkWidget	*hbox, *button;

	g_return_val_if_fail(option != NULL, NULL);

	hbox = gtk_hbox_new(FALSE, 4);

	gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new(_(label)),
			FALSE, TRUE, 0);

	button = gtk_button_new_with_label("");
	gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, TRUE, 0);

	option->update_widget = update_font;
	option->read_widget = read_font;
	option->widget = GTK_BIN(button)->child;

	gtk_signal_connect(GTK_OBJECT(button), "clicked",
			GTK_SIGNAL_FUNC(open_fontsel), (GtkObject *) option);

	return g_list_append(NULL, hbox);
}

static void button_patch_set_colour(GtkWidget *button, GdkColor *colour)
{
	GtkStyle   	*style;
	GtkWidget	*patch;

	patch = GTK_BIN(button)->child;

	style = gtk_style_copy(GTK_WIDGET(patch)->style);
	style->bg[GTK_STATE_NORMAL].red = colour->red;
	style->bg[GTK_STATE_NORMAL].green = colour->green;
	style->bg[GTK_STATE_NORMAL].blue = colour->blue;
	gtk_widget_set_style(patch, style);
	gtk_style_unref(style);

	if (GTK_WIDGET_REALIZED(patch))
		gdk_window_clear(patch->window);
}

static void load_options(xmlDoc *doc)
{
	xmlNode *root, *node;

	root = xmlDocGetRootElement(doc);
	
	g_return_if_fail(strcmp(root->name, "Options") == 0);

	for (node = root->xmlChildrenNode; node; node = node->next)
	{
		gchar *value, *name;

		if (node->type != XML_ELEMENT_NODE)
			continue;
		if (strcmp(node->name, "Option") != 0)
			continue;
		name = xmlGetProp(node, "name");
		if (!name)
			continue;

		value = xmlNodeGetContent(node);

		if (g_hash_table_lookup(loading, name))
			g_warning("Duplicate option found!");

		g_hash_table_insert(loading, name, value);

		/* (don't need to free name or value) */
	}
}
