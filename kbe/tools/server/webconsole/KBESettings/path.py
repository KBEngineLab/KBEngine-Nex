# -*- coding: utf-8 -*-

"""
init extra path
"""
import os, sys

def initExtraRootPath():
	"""
	初始化扩展的目录，以加载其它的脚本
	"""
	print('init extra root path')
	parent_dir = os.path.dirname( os.path.abspath( __file__ ) )
	parent_dir = os.path.dirname( parent_dir )
	parent_dir = os.path.dirname( parent_dir )
	if parent_dir not in sys.path:
		sys.path.append( parent_dir )
