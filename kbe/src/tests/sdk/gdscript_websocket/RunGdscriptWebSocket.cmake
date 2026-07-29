foreach(_kbe_required_variable IN ITEMS
    KBE_GODOT
    KBE_TEMPLATE_ROOT
    KBE_FIXTURE_ROOT
    KBE_WORK_ROOT
)
    if(NOT DEFINED ${_kbe_required_variable} OR "${${_kbe_required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${_kbe_required_variable} is required")
    endif()
endforeach()

# 工作目录完全位于 CMake 构建树中；每次重建可防止 Godot 类缓存掩盖 SDK 源码变化。
# The work directory lives entirely in the CMake build tree; rebuilding it prevents Godot's class cache from hiding SDK source changes.
file(REMOVE_RECURSE "${KBE_WORK_ROOT}")
file(MAKE_DIRECTORY "${KBE_WORK_ROOT}")

foreach(_kbe_template_file IN ITEMS
    MemoryStream.gd
    MessageReaderBase.gd
    MessageReaderWS.gd
)
    file(COPY_FILE
        "${KBE_TEMPLATE_ROOT}/${_kbe_template_file}"
        "${KBE_WORK_ROOT}/${_kbe_template_file}"
        ONLY_IF_DIFFERENT
    )
endforeach()

foreach(_kbe_fixture_file IN ITEMS
    KBEngine.gd
    Messages.gd
    Program.gd
    RecyclableObject.gd
    project.godot
)
    file(COPY_FILE
        "${KBE_FIXTURE_ROOT}/${_kbe_fixture_file}"
        "${KBE_WORK_ROOT}/${_kbe_fixture_file}"
        ONLY_IF_DIFFERENT
    )
endforeach()

# 先让 Godot 建立全局 class_name 缓存，再执行测试脚本；两步都使用同一隔离项目。
# Import the project first to build Godot's global class_name cache, then run the test script in the same isolated project.
execute_process(
    COMMAND "${KBE_GODOT}" --headless --path "${KBE_WORK_ROOT}" --editor --quit
    RESULT_VARIABLE _kbe_import_result
    OUTPUT_VARIABLE _kbe_import_output
    ERROR_VARIABLE _kbe_import_error
)
if(NOT _kbe_import_result EQUAL 0)
    message(FATAL_ERROR
        "Godot SDK fixture import failed with ${_kbe_import_result}:\n"
        "${_kbe_import_output}\n${_kbe_import_error}")
endif()

execute_process(
    COMMAND "${KBE_GODOT}" --headless --path "${KBE_WORK_ROOT}" --script "${KBE_WORK_ROOT}/Program.gd"
    RESULT_VARIABLE _kbe_test_result
    OUTPUT_VARIABLE _kbe_test_output
    ERROR_VARIABLE _kbe_test_error
)
if(NOT _kbe_test_result EQUAL 0)
    message(FATAL_ERROR
        "GDScript WebSocket fixture failed with ${_kbe_test_result}:\n"
        "${_kbe_test_output}\n${_kbe_test_error}")
endif()

message("${_kbe_test_output}")
if(NOT "${_kbe_test_output}" MATCHES "GDSCRIPT_WEBSOCKET_FRAME_TEST_PASS")
    message(FATAL_ERROR "GDScript WebSocket fixture did not emit its pass marker")
endif()
