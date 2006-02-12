import os, sys
from logging import info, warn
from xml.dom import Node, minidom

import rox
from rox import g, basedir
import constants

def get_n_buttons():
	stream = os.popen('xmodmap -pp', 'r')
	mapping = stream.read()
	stream.close()
	for line in mapping.split('\n'):
		for word in line.split(' '):
			if word[0].isdigit():
				return int(word)
	warn("No numbers in output from xmodmap -pp!")
	return 5
		
try:
	n_buttons = get_n_buttons()
except Exception, ex:
	warn("Failed to read number of buttons", ex)
	n_buttons = 5

def manager_check_running(screen):
	return _get_manager(screen) is not None

def _property_name(screen):
	return "_XSETTINGS_S%d" % screen

def _get_manager(screen):
	return g.gdk.selection_owner_get(g.gdk.atom_intern(_property_name(screen)))

class XSetting:
	def __init__(self, value, serial = 0):
		self.value = value
		self.serial = serial

	def serialise(self, name):
		return (self.type + '\0' +
			intToLittleEndian16(len(name)) +
			name + padding(len(name)) +
			intToLittleEndian32(self.serial) +
			self.serialise_body())
	
	def __repr__(self):
		return "<%s>" % self.value

class IntXSetting(XSetting):
	type = '\0' 	# XSETTINGS_TYPE_INT 
	str_type = 'int'

	def serialise_body(self):
		return intToLittleEndian32(self.value)

class StrXSetting(XSetting):
	type = '\1' 	# XSETTINGS_TYPE_STRING
	str_type = 'string'

	def serialise_body(self):
		return (intToLittleEndian32(len(self.value)) + 
			self.value + padding(len(self.value)))

class Manager:
	timestamp = None
	option_group = None

	def __init__(self, screen):
		self.screen = screen

		self.selection_atom = g.gdk.atom_intern(_property_name(screen), False)
		self.xsettings_atom = g.gdk.atom_intern('_XSETTINGS_SETTINGS', False)
		self.manager_atom = g.gdk.atom_intern('MANAGER', False)

		self.serial = 0

		self.window = g.Invisible()
		self.window.add_events(g.gdk.PROPERTY_CHANGE_MASK)
		self.window.connect('property-notify-event', self.property_notify)

		if manager_check_running(0):
			print >>sys.stderr, _("An XSETTINGS manager is already running. "
					"Not taking control of XSETTINGS...")
			return
		else:
			g.gdk.selection_owner_set(self.window.window, self.selection_atom,
					self.get_server_time(), False)

			if _get_manager(screen) != self.window.window:
				info('Failed to acquire XSettings manager selection')
				self.terminate()
				return

		# Can't see how to do this with PyGTK. But, since nothing else is
		# running at this point, we're probably OK.

		# XSendEvent (display, RootWindow (display, screen),
		#		False, StructureNotifyMask, (XEvent *)&xev);
		info('Acquired XSettings selection successfully - window %s', self.window.window)

		# Load settings
		try:
			path = basedir.load_first_config(constants.site, 'ROX-Session', 'Settings.xml')
			if path:
				self.load_settings(path)
		except:
			rox.report_exception()

		self.notify()
	
	def load_settings(self, path):
		doc = minidom.parse(path)
		
		root = doc.documentElement
		assert root.localName == 'Settings'
		for o in root.childNodes:
			if o.nodeType != Node.ELEMENT_NODE:
				continue
			if o.localName == 'Setting':
				name = str(o.getAttribute('name'))
				value = o.getAttribute('value')
				type = o.getAttribute('type')
				if type == 'string':
					self.set(name, str(value))
				elif type == 'int':
					self.set(name, int(value))
				else:
					raise Exception('Unknown type "%s"' % type)
			else:
				print "Warning: Non Option element", o

	def get_server_time(self):
		assert self.timestamp is None

		atom = g.gdk.atom_intern('_TIMESTAMP_PROP', False)
		self.window.window.property_change(atom, atom,
						8, g.gdk.PROP_MODE_REPLACE, 'a')
		while self.timestamp is None:
			info("Waiting for timestamp...")
			g.main_iteration()
		time = self.timestamp
		self.timestamp = None
		return long(time)

	def property_notify(self, window, event):
		self.timestamp = event.time
	
	def terminate(self):
		info("Terminate")
		return
	
	def set(self, name, value):
		self._set(name, value)
		if name == 'ROX/WindowManager':
			import wm
			wm.offer_restart()

	def _set(self, name, value):
		old = self._settings.get(name, None)
		if old is not None:
			if old.value == value: return	# No change
		if isinstance(value, str):
			new = StrXSetting(value, self.serial)
		elif isinstance(value, int):
			new = IntXSetting(int(value), self.serial)
		else:
			raise Exception('Unknown type for "%s"' % `value`)
		self._settings[name] = new
	
	def get(self, name, default):
		setting = self._settings.get(name, None)
		#print "Value for", name, "is", setting
		if setting is None:
			return default
		return setting.value
	
	def notify(self):
		"""Push settings to other apps"""
		# Note: we always use little-endian values
		buffer = ''
		n_xsettings = 0
		for s in self._settings:
			if s.startswith('ROX/'): continue
			setting = self._settings[s]
			buffer += setting.serialise(s)
			n_xsettings += 1

		buffer = ('\0\0\0\0' +
			 intToLittleEndian32(self.serial) +
			 intToLittleEndian32(n_xsettings) +
			 buffer)

		self.serial += 1

		#print "Set\t", `buffer`
			 
		self.window.window.property_change(self.xsettings_atom,
						   self.xsettings_atom, 8,
						   g.gdk.PROP_MODE_REPLACE,
						   buffer)

		# ROX/ settings are not sent via XSettings
		def intv(name): return str(self._settings[name].value)

		try:
			if os.spawnlp(os.P_WAIT, 'xset', 'xset',
				      'm', intv('ROX/AccelFactor') + '/10',
				      intv('ROX/AccelThreshold')):
				warn('xset failed')
		except OSError, exc:
			warn('xset failed: %s' % exc)

		buttons = range(1, n_buttons + 1)
		if self._settings['ROX/LeftHanded'].value:
			right_button = min(3, n_buttons)
			buttons[0] = right_button
			buttons[right_button - 1] = 1
		buttons = ' '.join(map(str, buttons))

		try:
			if os.spawnlp(os.P_WAIT, 'xmodmap', 'xmodmap', '-e',
				      'pointer = ' + buttons):
				warn('xmodmap failed')
		except OSError, exc:
			warn('xmodmap failed: %s' % exc)
				

		cursor_theme = self._settings['ROX/CursorTheme'].value
		xrdb = os.popen('xrdb -merge', 'w')
  		xrdb.write('Xcursor.theme: %s\n'
			   'Xcursor.theme_core: true\n'
			   'Xcursor.size: %d\n' %
			   (cursor_theme, self._settings['ROX/CursorSize'].value))
		result = xrdb.close()
		if result:
			warn('xrdb exited with %d' % result)

		layout = self._settings['ROX/KeyTable'].value.split(';')
		if len(layout) > 2:
			args = ['-layout', layout[1], '-model', layout[2]]
			if len(layout) > 3 and layout[3]:
				args += ['-varient', layout[3]]
			if len(layout) > 4 and layout[4]:
				args += ['-option', layout[4]]
			if os.spawnlp(os.P_WAIT, 'setxkbmap', 'setxkbmap', *args):
				warn('setxkbmap failed')

	def save(self):
		doc = minidom.parseString("<Settings/>")
		root = doc.documentElement
		for x in self._settings:
			elem = doc.createElement('Setting')
			root.appendChild(elem)
			elem.setAttribute('name', x)
			setting = self._settings[x]
			elem.setAttribute('value', str(setting.value))
			elem.setAttribute('type', str(setting.str_type))
		save_dir = basedir.save_config_path(constants.site, 'ROX-Session')
		settings_file = os.path.join(save_dir, 'Settings.xml')
		stream = file(settings_file + '.new', 'w')
		doc.writexml(stream, addindent = ' ', newl = '\n')
		stream.close()
		os.rename(settings_file + '.new', settings_file)

	_settings = {
		'Gtk/FontName': StrXSetting('Sans 10'),
		'Net/ThemeName': StrXSetting('Default'),
		'Net/CursorBlinkTime': IntXSetting(1200),
		'Xft/Antialias': IntXSetting(1),

		'ROX/DPMSEnable': IntXSetting(1),
		'ROX/DPMSStandby': IntXSetting(15 * 60),
		'ROX/DPMSSuspend': IntXSetting(20 * 60),
		'ROX/DPMSOff': IntXSetting(30 * 60),

		'ROX/AccelFactor': IntXSetting(20),
		'ROX/AccelThreshold': IntXSetting(10),
		'ROX/LeftHanded': IntXSetting(0),

		'ROX/CursorTheme': StrXSetting(''),
		'ROX/CursorSize': IntXSetting(18),

		'ROX/NumLock': IntXSetting(0),
		'ROX/CapsLock': IntXSetting(0),
		'ROX/KeyTable': StrXSetting(''),
		'ROX/KbdRepeat': IntXSetting(1),
		'ROX/KbdDelay': IntXSetting(500),
		'ROX/KbdInterval': IntXSetting(30),

		'ROX/ManageScreensaver': IntXSetting(0),
		'ROX/BlankTime': IntXSetting(10 * 60),
	}

def padding(length):
	needed = (length + 3) & ~3
	return '\0' * (needed - length)

def intToLittleEndian16(i):
	return chr(i & 0xff) + chr((i >> 8) & 0xff)

def intToLittleEndian32(i):
	return chr(i & 0xff) + chr((i >> 8) & 0xff) + chr((i >> 16) & 0xff) + chr((i >> 24) & 0xff)
