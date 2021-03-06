import os, sys
import logging
import children
import imp

import rox
from rox import basedir, g
import constants

import xxmlrpc
import rox.OptionsBox

import session, wm, settings
import session_dbus
import mydbus
if mydbus.dbus_version_ok:
	try:
		import dbus
		import dbus.service
		import dbus.glib
	except:
		mydbus.dbus_version_ok = False
import log

problem_msg=_('''Interprocess communication is not possible, session
management will not work.  Your python dbus is too old %s,
and your GTK+ version is too new %s.  Please update
your python dbus to at least %s.''') % (mydbus.version, g.gtk_version,
                                        mydbus.min_version)

def manage_session(test_mode):
	log.init()

	if not mydbus.dbus_version_ok and not xxmlrpc.working:
		rox.alert(problem_msg)
		
	set_up_environment()
	session.init()
	children.init()
	session_dbus.init()  # Start even if DBus is too old, for session bus
	xml_settings = settings.init()

	if mydbus.dbus_version_ok:
		service = dbus.service.BusName(constants.session_service,
					   bus = session_dbus.get_session_bus())
		SessionObject3x(service)

	# This is like the D-BUS service, except using XML-RPC-over-X
	xml_service = xxmlrpc.XXMLRPCServer(constants.session_service)
	xml_service.add_object('/Session', XMLSessionObject())
	xml_service.add_object('/Settings', xml_settings)

	try:
		if test_mode:
			print "Test mode!"
			print "Started", os.system("(/bin/echo hi >&2; sleep 4; date >&2)&")
			print "OK"
		else:
			try:
				wm.start()
			except:
				rox.report_exception()

		g.main()
	finally:
		session_dbus.destroy()

def set_up_environment():
	if 'BROWSER' not in os.environ:
		os.environ['BROWSER'] = os.path.join(rox.app_dir, 'browser')

	# TODO: CHOICESPATH/ROX-Session/Environment
	env_loaded=False
	for d in basedir.load_config_paths('rox.sourceforge.net',
					   'ROX-Session'):
		try:
			fl, pathname, descr=imp.find_module('Environment', [d])
		except ImportError:
			pass
		else:
			# Make sure it is not world writable
			st=os.stat(pathname)
			if st.st_mode & os.path.stat.S_IWOTH:
				fl.close()
				continue

			try:
				mod=imp.load_module('Environment', fl,
						    pathname, descr)
				env_loaded=True
			except:
				exc=sys.exc_info()[1]
				log.log.log('Failed to process %s: %s' % (pathname,
								      str(exc)))
			fl.close()

	if env_loaded:
		# XDG variables may have changed
		reload(basedir)

	# Set a variable to indicate we are in the ROX desktop.  Helps
	# cross-desktop scripts to identify us.
	os.environ['ROX_SESSION']='1'

	# Close stdin. We don't need it, and it can cause problems if
	# a child process wants a password, etc...
	fd = os.open('/dev/null', os.O_RDONLY)
	if fd > 0:
		os.close(0)
		os.dup2(fd, 0)
		os.close(fd)

	for style in basedir.load_config_paths(constants.site, 'ROX-Session', 'Styles'):
		break
	else:
		style = os.path.join(rox.app_dir, 'Styles')
	logging.info("Loading styles from '%s'", style)
	g.rc_parse(style)

def build_i18n_message(box, node, label):
	widget=g.Label(_("""Note that you must save your choices, 
log out and log back in for the new language
setting to take full effect."""))
	widget.set_alignment(0, 0.5)
	widget.set_justify(g.JUSTIFY_LEFT)
	widget.set_line_wrap(True)

	hbox=g.HBox(False, 4)
	image=g.image_new_from_stock(g.STOCK_DIALOG_INFO,
				     g.ICON_SIZE_BUTTON)
	align=g.Alignment(0, 0, 0, 0)

	align.add(image)
	hbox.pack_start(align, False, True, 0)
	hbox.pack_start(widget, False, True, 0)

	return [hbox]
	
rox.OptionsBox.widget_registry['i18n-message'] = build_i18n_message

def edit_options():
	rox.edit_options()

if mydbus.dbus_version_ok:
	class SessionObject3x(dbus.service.Object):
		def __init__(self, service):
			dbus.service.Object.__init__(self, service, "/Session")

		# Prefered syntax, but requires python 2.4 or later
		#@dbus.service.method(constants.control_interface)
		def LogoutWithoutConfirm(self):
			g.main_quit()
		ShowMessages=dbus.service.method(constants.control_interface)(LogoutWithoutConfirm)

	
		def ShowOptions(self):
			edit_options()
		ShowOptions=dbus.service.method(constants.control_interface)(ShowOptions)

	
		def ShowMessages(self):
			log.log.show_log_window()
		ShowMessages=dbus.service.method(constants.control_interface)(ShowMessages)

		
class XMLSessionObject:
	allowed_methods = ('LogoutWithoutConfirm', 'ShowOptions', 'ShowMessages')
	def LogoutWithoutConfirm(self):
		g.main_quit()

	def ShowOptions(self):
		edit_options()

	def ShowMessages(self):
		log.log.show_log_window()
