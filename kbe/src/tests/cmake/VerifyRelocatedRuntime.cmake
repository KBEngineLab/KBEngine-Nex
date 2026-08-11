if(NOT DEFINED KBE_EXECUTABLE OR NOT DEFINED KBE_PYTHON_RUNTIME_FILES OR
   NOT DEFINED KBE_RELOCATED_DIR OR NOT DEFINED KBE_ROOT OR
   NOT DEFINED KBE_RES_PATH)
    message(FATAL_ERROR "Relocated runtime verification arguments are incomplete")
endif()

file(REMOVE_RECURSE "${KBE_RELOCATED_DIR}")
file(MAKE_DIRECTORY "${KBE_RELOCATED_DIR}")

cmake_path(GET KBE_EXECUTABLE FILENAME _kbe_executable_name)
file(COPY_FILE "${KBE_EXECUTABLE}"
    "${KBE_RELOCATED_DIR}/${_kbe_executable_name}" ONLY_IF_DIFFERENT)

string(REPLACE "|" ";" _kbe_python_runtime_files "${KBE_PYTHON_RUNTIME_FILES}")
foreach(_kbe_python_runtime_file IN LISTS _kbe_python_runtime_files)
    cmake_path(GET _kbe_python_runtime_file FILENAME _kbe_runtime_name)
    file(COPY_FILE "${_kbe_python_runtime_file}"
        "${KBE_RELOCATED_DIR}/${_kbe_runtime_name}" ONLY_IF_DIFFERENT)
endforeach()

# 资源目录保持显式可控，只改变二进制位置，以便单独验证动态 Python 不依赖构建树绝对路径。
# Keep resources explicit and move only binaries so this test isolates dynamic Python from build-tree absolute paths.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "KBE_ROOT=${KBE_ROOT}"
        "KBE_RES_PATH=${KBE_RES_PATH}"
        "KBE_BIN_PATH=${KBE_RELOCATED_DIR}"
        "${KBE_RELOCATED_DIR}/${_kbe_executable_name}" --getuid
    WORKING_DIRECTORY "${KBE_RELOCATED_DIR}"
    RESULT_VARIABLE _kbe_result
    OUTPUT_VARIABLE _kbe_stdout
    ERROR_VARIABLE _kbe_stderr
)
if(NOT _kbe_result EQUAL 0 OR NOT _kbe_stdout MATCHES "[0-9]+")
    message(FATAL_ERROR
        "Relocated kbcmd failed with exit code ${_kbe_result}.\n"
        "stdout:\n${_kbe_stdout}\n"
        "stderr:\n${_kbe_stderr}"
    )
endif()

message(STATUS "Verified relocated kbcmd with Python runtime libraries beside the executable")
