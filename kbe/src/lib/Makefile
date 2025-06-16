ifndef KBE_ROOT
export KBE_ROOT := $(subst /kbe/src/lib,,$(CURDIR))
endif

ifndef KBE_CONFIG
	KBE_CONFIG=Hybrid
	ifeq ($(shell uname -m),x86_64)
		 KBE_CONFIG=Hybrid64
	endif
endif

ifneq (,$(findstring 64, $(KBE_CONFIG)))
	OPENSSL_CONFIG+="x86_64=1"
else
	OPENSSL_CONFIG+=
endif

ifneq ("$(wildcard /usr/lib/x86_64-linux-gnu/libssl.a)", "")
    $(info "use system openssl.")
    USE_SELF_OPENSSL=0
else
    $(info "use self openssl.")
    USE_SELF_OPENSSL=1
endif

# 定义关键目录
LIBDIR=$(KBE_ROOT)/kbe/src/lib/bin
INCLUDE_DIR=$(KBE_ROOT)/kbe/src/include
LIB_DIR=$(KBE_ROOT)/kbe/src/libs

# 确保目录存在
$(shell mkdir -p $(LIBDIR) $(INCLUDE_DIR) $(LIB_DIR))


# 并行构建目标定义
all install clean full: mkdir_target stage1  python_build stage2

# install clean full:
# 	$(MAKE) -j1 $@  # 非并行执行特殊目标

# 特殊处理目标（非并行）
mkdir_target:
	@test -d $(LIBDIR) || mkdir -p $(LIBDIR)


# 阶段1: 基础依赖库
stage1: mkdir_target zlib_build fmt_build jemalloc_build openssl_build curl_build \
        hiredis_build g3dlite_build tinyxml_build sigar_build tmxparser_build \
        jwsmtp_build apr_build expat_build apr_util_build log4cxx_build
	@true

# 阶段2: 主项目组件
stage2: client_lib_build common_build db_redis_build db_mysql_build \
        db_interface_build entitydef_build math_build resmgr_build \
        pyscript_build server_build navigation_build network_build \
        helper_build thread_build xml_build
	@true

# 依赖库构建规则

stage2: stage1


zlib_build:
	cd dependencies/zlib && $(MAKE) all  && if ! test -e $(LIB_DIR)/libz.a; then cp -f "libz.a" "$(LIB_DIR)"; fi

fmt_build:
	cd dependencies/fmt/src && $(MAKE) all 

jemalloc_build:
	cd dependencies/jemalloc && $(MAKE) all  && \
	if ! test -e $(LIB_DIR)/libjemalloc.a; then cp -f "./lib/libjemalloc.a" "$(LIB_DIR)"; fi 

openssl_build: zlib_build
	@if [ $(USE_SELF_OPENSSL) -eq 1 ] && [ ! -e dependencies/openssl ]; then \
		ln -sf $(KBE_ROOT)/kbe/src/lib/dependencies/vsopenssl dependencies/openssl; \
	fi
	if test -e dependencies/openssl/include/openssl/ssl.h; then \
		cd dependencies/openssl && $(MAKE) all  && \
		if ! test -e $(LIB_DIR)/libssl.a; then cp -f "libssl.a" "$(LIB_DIR)"; fi && \
		if ! test -e $(LIB_DIR)/libcrypto.a; then cp -f "libcrypto.a" "$(LIB_DIR)"; fi; \
	fi

curl_build: openssl_build
	cd dependencies/curl && $(MAKE) all 

hiredis_build:
	cd dependencies/hiredis && $(MAKE) all 

g3dlite_build:
	cd dependencies/g3dlite && $(MAKE) all 

tinyxml_build: jemalloc_build
	cd dependencies/tinyxml && $(MAKE) all 

sigar_build: jemalloc_build
	cd dependencies/sigar/linux && $(MAKE) all 

tmxparser_build: jemalloc_build
	cd dependencies/tmxparser && $(MAKE) all 

jwsmtp_build: jemalloc_build
	cd dependencies/jwsmtp/jwsmtp/jwsmtp && $(MAKE) all 

apr_build: jemalloc_build
	cd dependencies/apr && $(MAKE) all  && if ! test -e $(LIB_DIR)/libapr-1.a; then cp -f ".libs/libapr-1.a" "$(LIB_DIR)"; fi

expat_build: jemalloc_build
	cd dependencies/expat && $(MAKE) all  && if ! test -e $(LIB_DIR)/libexpat.a; then cp -f "./.libs/libexpat.a" "$(LIB_DIR)"; fi

apr_util_build: apr_build expat_build
	cd dependencies/apr-util && $(MAKE) all  && if ! test -e $(LIB_DIR)/libaprutil-1.a; then cp -f ".libs/libaprutil-1.a" "$(LIB_DIR)"; fi

log4cxx_build: apr_build apr_util_build
	cd dependencies/log4cxx && $(MAKE) all  -j1   && if ! test -e $(LIB_DIR)/liblog4cxx.a; then cp -f "src/main/cpp/.libs/liblog4cxx.a" "$(LIB_DIR)"; fi

# Python特殊处理
python_build: 
	cd python && $(MAKE) all  && \
	cp -f "$(KBE_ROOT)/kbe/src/lib/python/libpython3.13.a" "$(LIB_DIR)/libpython.a" && \
	mkdir -p "$(KBE_ROOT)/kbe/res/scripts/common/lib-dynload/" && \
	cp -rfL  $(KBE_ROOT)/kbe/src/lib/python/build/lib.*/* "$(KBE_ROOT)/kbe/res/scripts/common/lib-dynload/" && \
	rm -rf "$(KBE_ROOT)/kbe/res/scripts/common/Lib" && \
	cp -rf "$(KBE_ROOT)/kbe/src/lib/python/Lib" "$(KBE_ROOT)/kbe/res/scripts/common/Lib"

# 显式定义common_build的依赖关系
common_build: python_build jemalloc_build
	cd common && $(MAKE) all 

# 其他组件构建规则
client_lib_build: python_build log4cxx_build
	cd client_lib && $(MAKE) all 

db_redis_build: python_build 
	cd db_redis && $(MAKE) all 

db_mysql_build: python_build 
	cd db_mysql && $(MAKE) all 

db_interface_build: python_build math_build 
	cd db_interface && $(MAKE) all 

entitydef_build: python_build log4cxx_build
	cd entitydef && $(MAKE) all 

math_build: python_build  
	cd math && $(MAKE) all 

resmgr_build:  jemalloc_build log4cxx_build
	cd resmgr && $(MAKE) all 

pyscript_build: python_build jemalloc_build  
	cd pyscript && $(MAKE) all 

server_build: python_build jemalloc_build log4cxx_build
	cd server && $(MAKE) all 

navigation_build: math_build jemalloc_build common_build
	cd navigation && $(MAKE) all 

network_build: jemalloc_build common_build
	cd network && $(MAKE) all 

helper_build: jemalloc_build log4cxx_build math_build common_build
	cd helper && $(MAKE) all  -j1  

thread_build: jemalloc_build common_build
	cd thread && $(MAKE) all 

xml_build: jemalloc_build 
	cd xml && $(MAKE) all 

# 非并行目标处理
.PHONY: all install clean full mkdir_target stage1 stage2 python_build \
        zlib_build fmt_build jemalloc_build openssl_build curl_build \
        hiredis_build g3dlite_build tinyxml_build sigar_build tmxparser_build \
        jwsmtp_build apr_build expat_build apr_util_build log4cxx_build \
        client_lib_build common_build db_redis_build db_mysql_build \
        db_interface_build entitydef_build math_build resmgr_build \
        pyscript_build server_build navigation_build network_build \
        helper_build thread_build xml_build