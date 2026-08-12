if(NOT DEFINED KBE_CELLAPP_SOURCE OR NOT EXISTS "${KBE_CELLAPP_SOURCE}")
    message(FATAL_ERROR "KBE_CELLAPP_SOURCE must identify cellapp.cpp")
endif()

file(READ "${KBE_CELLAPP_SOURCE}" _kbe_cellapp_source)

string(FIND "${_kbe_cellapp_source}"
    "PyObject* Cellapp::__py_createEntity(PyObject* self, PyObject* args)"
    _kbe_function_start)
string(FIND "${_kbe_cellapp_source}"
    "PyObject* Cellapp::__py_executeRawDatabaseCommand"
    _kbe_function_end)

if(_kbe_function_start LESS 0 OR _kbe_function_end LESS 0 OR
        _kbe_function_end LESS_EQUAL _kbe_function_start)
    message(FATAL_ERROR "Could not isolate Cellapp::__py_createEntity")
endif()

math(EXPR _kbe_function_length "${_kbe_function_end} - ${_kbe_function_start}")
string(SUBSTRING "${_kbe_cellapp_source}" ${_kbe_function_start}
    ${_kbe_function_length} _kbe_create_entity)

# Python C API failures must preserve an exception when returning NULL.
# Python C API失败返回NULL时必须保留异常，不能先打印并清除。
string(FIND "${_kbe_create_entity}" "PyErr_PrintEx" _kbe_print_error)
if(NOT _kbe_print_error EQUAL -1)
    message(FATAL_ERROR
        "Cellapp::__py_createEntity must not clear Python exceptions with PyErr_PrintEx")
endif()

foreach(_kbe_required_text IN ITEMS
        "sIOO|O:createEntity"
        "PyExc_RuntimeError"
        "cellapp is shutting down"
        "else if(!PyErr_Occurred())")
    string(FIND "${_kbe_create_entity}" "${_kbe_required_text}" _kbe_required_pos)
    if(_kbe_required_pos EQUAL -1)
        message(FATAL_ERROR
            "Cellapp::__py_createEntity is missing Python error contract: ${_kbe_required_text}")
    endif()
endforeach()

message(STATUS "Cellapp createEntity Python exception contract verified")
