# ---------------- 源文件 ----------------
set(SRCS
	fixedarray.cpp
	common.cpp
	datatype.cpp
	datatypes.cpp
	detaillevel.cpp
	entity_component.cpp
	entity_component_call.cpp
	entity_call.cpp
	entitydef.cpp
	py_entitydef.cpp
	entitycallabstract.cpp
	fixeddict.cpp
	method.cpp
	property.cpp
	remote_entity_method.cpp
	scriptdef_module.cpp
	volatileinfo.cpp
)
# ---------------- 创建静态库 ----------------
add_library(entitydef STATIC ${SRCS})


target_link_libraries(entitydef PUBLIC 
	fmt::fmt
	xml
	python_interface
)
target_link_libraries(entitydef PRIVATE resmgr)



