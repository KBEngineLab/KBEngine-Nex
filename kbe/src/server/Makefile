ifndef KBE_ROOT
export KBE_ROOT := $(subst /kbe/src/server,,$(CURDIR))
endif

# 定义所有需要构建的子目录
SUBDIRS := ../lib baseapp baseappmgr cellapp cellappmgr dbmgr loginapp machine tools

all clean realclean all_config install::

# 检查KBE_ROOT环境变量（保持不变）
# Check with and without the trailing slash
ifneq ($(CURDIR),$(KBE_ROOT)/kbe/src/server)
ifneq ($(CURDIR),$(KBE_ROOT)kbe/src/server)
	@echo 'Error: KBE_ROOT=$(KBE_ROOT)'
	@echo '       is not the root of $(CURDIR)'
	@echo 'Remove the KBE_ROOT environment variable.'
	@false
endif
endif

# 并行构建所有子目录
	@$(MAKE)  $(SUBDIRS) MAKE_TARGET=$@

# 构建完成提示（保持不变）
ifdef KBE_CONFIG
	@echo completed $@ \(KBE_CONFIG = $(KBE_CONFIG)\)
else
	@echo completed $@
endif
	@$(MAKE)  done

# 子目录构建规则
.PHONY: $(SUBDIRS)
$(SUBDIRS):
	$(MAKE) -C $@ $(MAKE_TARGET)

# 完成提示规则（保持不变）
done:
ifdef DO_NOT_BELL
else
	@echo -n 
endif

server:
	$(MAKE) all