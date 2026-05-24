# -*- coding: utf-8 -*-

class PluginEntry:
	"""
	Optional base class for plugin lifecycle modules.

	The engine calls module-level functions on plugin_entry.py. This class is
	only a convenience for plugin authors who prefer to delegate to an object.
	"""

	def onInit(self, isReload):
		pass

	def onComponentReady(self, isBootstrap):
		pass

	def onFini(self):
		pass
