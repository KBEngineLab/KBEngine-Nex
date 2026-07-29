set(_kbe_gdscript_work_root "${CMAKE_BINARY_DIR}/Testing/sdk-gdscript-$<CONFIG>")

add_test(NAME sdk.gdscript.websocket
    COMMAND "${CMAKE_COMMAND}"
        "-DKBE_GODOT=${KBE_SDK_GODOT_EXECUTABLE}"
        "-DKBE_TEMPLATE_ROOT=${_kbe_sdk_template_root}/gdscript"
        "-DKBE_FIXTURE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DKBE_WORK_ROOT=${_kbe_gdscript_work_root}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/RunGdscriptWebSocket.cmake"
)
set_tests_properties(sdk.gdscript.websocket PROPERTIES
    LABELS "sdk;gdscript;unit;portable"
    PASS_REGULAR_EXPRESSION "GDSCRIPT_WEBSOCKET_FRAME_TEST_PASS"
    TIMEOUT 180
)
