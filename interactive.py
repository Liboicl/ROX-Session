#!/usr/bin/env python
import sys

import findrox; findrox.version(1, 9, 6)
import rox
from rox import g
import os.path

__builtins__._ = rox.i18n.translation(os.path.join(rox.app_dir, 'Messages'))

try:
	import dbus
except ImportError:
	rox.croak("Failed to import dbus module. You probably need "
		"to install a package with a name like 'python2.3-dbus'.\n"
		"D-BUS can also be downloaded from http://freedesktop.org\n"
		"(be sure to compile the python bindings)")

try:
	bus = dbus.Bus(dbus.Bus.TYPE_SESSION)
	dbus_service = bus.get_service('org.freedesktop.DBus')
	dbus_object = dbus_service.get_object('/org/freedesktop/DBus',
						   'org.freedesktop.DBus')
	rox_session_running = 'net.sf.rox.Session' in dbus_object.ListServices()
except:
	rox_session_running = False

try:
	if rox_session_running:
		import logout
		rox_session = bus.get_service('net.sf.rox.Session')
		logout.show_logout_box(rox_session)
	else:
		import setup
		setup.setup_with_confirm()
except SystemExit:
	pass
except:
	rox.report_exception()