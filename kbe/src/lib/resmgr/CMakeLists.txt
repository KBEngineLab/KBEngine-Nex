# ---------------- 源文件 ----------------
set(SRCS
	resmgr.cpp
	resourceobject.cpp
	plugins/plugin_manifest.cpp
	plugins/plugin_manager.cpp
)
# ---------------- 创建静态库 ----------------
add_library(resmgr STATIC ${SRCS})


target_link_libraries(resmgr PRIVATE
	xml
	fmt::fmt
	nlohmann_json::nlohmann_json
)

