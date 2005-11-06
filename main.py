import os
import logging
import children

import rox
from rox import basedir, g
import constants

import session, wm, settings
import session_dbus
import dbus
import log

def manage_session(test_mode):
	log.init()

	set_up_environment()
	session.init()
	children.init()
	session_dbus.init()
	settings.init()

	service = dbus.Service(constants.session_service, bus = session_dbus.session_bus)
	SessionObject(service)

	try:
		if test_mode:
			print "Test mode!"
			print "Started", os.system("ls >&2 &")
			print "OK"
		else:
			wm.start()

		g.main()
	finally:
		settings.destroy()

def set_up_environment():
	if 'CHOICESPATH' not in os.environ:
		os.environ['CHOICESPATH'] = os.path.expanduser("~/Choices:/usr/local/share/Choices:/usr/share/Choices")

	if 'BROWSER' not in os.environ:
		os.environ['BROWSER'] = os.path.join(rox.app_dir, 'browser')

	# TODO: CHOICESPATH/ROX-Session/Environment

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

class SessionObject(dbus.Object):
	def __init__(self, service):
		dbus.Object.__init__(self, "/Session", service, [
			self.LogoutWithoutConfirm,
			self.ShowOptions,
			self.ShowMessages])

	def LogoutWithoutConfirm(self, message):
		g.main_quit()
	
	def ShowOptions(self, message):
		rox.edit_options()
	
	def ShowMessages(self, message):
		raise Exception('ShowMessages')
