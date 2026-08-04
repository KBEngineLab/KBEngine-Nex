if(NOT DEFINED KBE_EXECUTABLES OR NOT DEFINED KBE_PYTHON_STDLIB OR
   NOT DEFINED KBE_PYTHON_EXTENSION OR NOT DEFINED KBE_RUNTIME_DIR OR
   NOT DEFINED KBE_LOG4J_CONFIG)
    message(FATAL_ERROR "Runtime verification arguments are incomplete")
endif()

string(REPLACE "|" ";" _kbe_executables "${KBE_EXECUTABLES}")
foreach(_kbe_executable IN LISTS _kbe_executables)
    if(NOT EXISTS "${_kbe_executable}")
        message(FATAL_ERROR "Server executable is missing: ${_kbe_executable}")
    endif()
    cmake_path(GET _kbe_executable PARENT_PATH _kbe_executable_dir)
    cmake_path(NORMAL_PATH _kbe_executable_dir)
    cmake_path(NORMAL_PATH KBE_RUNTIME_DIR OUTPUT_VARIABLE _kbe_runtime_dir)
    if(NOT _kbe_executable_dir STREQUAL _kbe_runtime_dir)
        message(FATAL_ERROR
            "Server executable escaped the stable runtime directory: ${_kbe_executable}")
    endif()
    cmake_path(GET _kbe_executable STEM _kbe_executable_stem)
    if(_kbe_executable_stem MATCHES "_d$")
        message(FATAL_ERROR "Server executable has a Debug postfix: ${_kbe_executable}")
    endif()
endforeach()

if(NOT EXISTS "${KBE_LOG4J_CONFIG}")
    message(FATAL_ERROR "Runtime log4j configuration is missing: ${KBE_LOG4J_CONFIG}")
endif()

if(NOT EXISTS "${KBE_PYTHON_STDLIB}")
    message(FATAL_ERROR "Python standard library is missing: ${KBE_PYTHON_STDLIB}")
endif()

file(GLOB _kbe_python_extensions "${KBE_PYTHON_EXTENSION}")
if(NOT _kbe_python_extensions)
    message(FATAL_ERROR "Python extension probe is missing: ${KBE_PYTHON_EXTENSION}")
endif()

if(WIN32)
    foreach(_kbe_python_dll IN ITEMS python3.dll "python${KBE_PYTHON_ABI}.dll")
        if(NOT EXISTS "${KBE_PYTHON_DLL_DIR}/${_kbe_python_dll}")
            message(FATAL_ERROR "Python runtime DLL is missing: ${KBE_PYTHON_DLL_DIR}/${_kbe_python_dll}")
        endif()
    endforeach()
endif()

list(LENGTH _kbe_executables _kbe_executable_count)
message(STATUS "Verified ${_kbe_executable_count} server executables and the embedded Python runtime")
