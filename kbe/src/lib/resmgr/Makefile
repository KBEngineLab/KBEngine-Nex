LIB =	resmgr

SRCS =				\
	resmgr			\
	resourceobject		\
	plugins/plugin_manifest \
	plugins/plugin_manager

ifndef KBE_ROOT
export KBE_ROOT := $(subst /kbe/src/lib/$(LIB),,$(CURDIR))
endif

include $(KBE_ROOT)/kbe/src/build/common.mak
