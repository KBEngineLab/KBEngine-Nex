ifndef KBE_ROOT
export KBE_ROOT := $(subst /kbe/src/lib,,$(CURDIR))
endif

ifndef KBE_CONFIG
	KBE_CONFIG=Hybrid
	ifeq ($(shell uname -m),x86_64)
		 KBE_CONFIG=Hybrid64
	endif
endif

# This directory must match the directory in src/build/common.mak
LIBDIR=$(KBE_ROOT)/kbe/src/lib/bin

all install clean full:
	test -d $(LIBDIR) || mkdir $(LIBDIR)
	cd dependencies/g3dlite && $(MAKE) $@
	cd dependencies/tinyxml && $(MAKE) $@
	cd dependencies/sigar/linux && $(MAKE) $@
	cd dependencies/tmxparser && $(MAKE) $@
	cd dependencies/jwsmtp/jwsmtp/jwsmtp && $(MAKE) $@
	cd python && $(MAKE) $@ && cp -f "$(KBE_ROOT)/kbe/src/lib/python/libpython3.7m.a" "$(KBE_ROOT)/kbe/src/libs/libpython.a" && mkdir -p "$(KBE_ROOT)/kbe/res/scripts/common/lib-dynload/" && cp -rf $(KBE_ROOT)/kbe/src/lib/python/build/lib.*/* "$(KBE_ROOT)/kbe/res/scripts/common/lib-dynload/" && rm -rf "$(KBE_ROOT)/kbe/res/scripts/common/Lib/*" && cp -rf "$(KBE_ROOT)/kbe/src/lib/python/Lib" "$(KBE_ROOT)/kbe/res/scripts/common/Lib"
	cd client_lib && $(MAKE) $@
	cd common && $(MAKE) $@
	cd db_redis && $(MAKE) $@
	cd db_mysql && $(MAKE) $@
	cd db_interface && $(MAKE) $@
	cd entitydef && $(MAKE) $@
	cd math && $(MAKE) $@
	cd resmgr && $(MAKE) $@
	cd pyscript && $(MAKE) $@
	cd server && $(MAKE) $@
	cd navigation && $(MAKE) $@
	cd network && $(MAKE) $@
	cd helper && $(MAKE) $@
	cd thread && $(MAKE) $@
	cd xml && $(MAKE) $@

