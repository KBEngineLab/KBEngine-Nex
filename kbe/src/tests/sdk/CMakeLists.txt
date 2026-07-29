set(_kbe_sdk_template_root "${CMAKE_SOURCE_DIR}/../res/sdk_templates/client")
set(_kbe_sdk_test_output "${CMAKE_BINARY_DIR}/Testing/sdk-$<CONFIG>")

# SDK 源码 fixture 必须由当前生成器编译，使 MSVC、Clang、GCC、Visual Studio 和 Ninja 走同一测试定义。
# SDK source fixtures must use the active generator so MSVC, Clang, GCC, Visual Studio, and Ninja share one test definition.
add_executable(kbe_test_sdk_cxx_numeric_boundary
    cxx_numeric_boundary/Program.cpp
)
target_include_directories(kbe_test_sdk_cxx_numeric_boundary PRIVATE
    "${_kbe_sdk_template_root}/cxx"
)

add_executable(kbe_test_sdk_cxx_tcp_receive
    cxx_tcp_receive/Program.cpp
)
target_include_directories(kbe_test_sdk_cxx_tcp_receive PRIVATE
    "${_kbe_sdk_template_root}/cxx"
)
target_link_libraries(kbe_test_sdk_cxx_tcp_receive PRIVATE Threads::Threads)

foreach(_kbe_sdk_cxx_target IN ITEMS
    kbe_test_sdk_cxx_numeric_boundary
    kbe_test_sdk_cxx_tcp_receive
)
    set_target_properties("${_kbe_sdk_cxx_target}" PROPERTIES FOLDER "Tests/SDK")
    if(MSVC)
        target_compile_options("${_kbe_sdk_cxx_target}" PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options("${_kbe_sdk_cxx_target}" PRIVATE
            -Wall -Wextra -Wpedantic -Werror
        )
    endif()
endforeach()

add_test(NAME sdk.cxx.numeric_boundary COMMAND kbe_test_sdk_cxx_numeric_boundary)
set_tests_properties(sdk.cxx.numeric_boundary PROPERTIES
    LABELS "sdk;cxx;unit;portable"
    PASS_REGULAR_EXPRESSION "CXX_NUMERIC_BOUNDARY_TEST_PASS"
)

add_test(NAME sdk.cxx.tcp_receive COMMAND kbe_test_sdk_cxx_tcp_receive)
set_tests_properties(sdk.cxx.tcp_receive PROPERTIES
    LABELS "sdk;cxx;unit;portable"
    PASS_REGULAR_EXPRESSION "CXX_TCP_RECEIVE_BACKPRESSURE_TEST_PASS"
    TIMEOUT 60
)

# 可选 SDK 工具只影响对应测试是否注册，不影响 C++ 基础回归和常规服务端构建。
# Optional SDK tools only control registration of their tests and never block the C++ baseline or regular server builds.
function(kbe_sdk_import_environment_path variable_name description)
    if((NOT DEFINED ${variable_name}
        OR "${${variable_name}}" STREQUAL ""
        OR "${${variable_name}}" MATCHES "-NOTFOUND$")
       AND DEFINED ENV{${variable_name}} AND NOT "$ENV{${variable_name}}" STREQUAL "")
        set(${variable_name} "$ENV{${variable_name}}" CACHE FILEPATH "${description}" FORCE)
    endif()
endfunction()

kbe_sdk_import_environment_path(KBE_SDK_DOTNET_EXECUTABLE ".NET executable used by C# SDK tests")
kbe_sdk_import_environment_path(KBE_SDK_NODE_EXECUTABLE "Node.js executable used by TypeScript SDK tests")
kbe_sdk_import_environment_path(KBE_SDK_TYPESCRIPT_EXECUTABLE "TypeScript compiler used by SDK tests")
kbe_sdk_import_environment_path(KBE_SDK_GODOT_EXECUTABLE "Godot executable used by GDScript SDK tests")

find_program(KBE_SDK_DOTNET_EXECUTABLE NAMES dotnet)
find_program(KBE_SDK_NODE_EXECUTABLE NAMES node)
find_program(KBE_SDK_TYPESCRIPT_EXECUTABLE NAMES tsc tsc.cmd)
find_program(KBE_SDK_GODOT_EXECUTABLE NAMES godot4 godot Godot_v4.7-stable_win64_console)

if(KBE_SDK_DOTNET_EXECUTABLE)
    foreach(_kbe_csharp_fixture IN ITEMS heartbeat tcp_send)
        if(_kbe_csharp_fixture STREQUAL "heartbeat")
            set(_kbe_csharp_project "${CMAKE_CURRENT_SOURCE_DIR}/csharp_heartbeat/CsharpHeartbeatStateTest.csproj")
            set(_kbe_csharp_marker "CSHARP_NETWORK_STATE_TEST_PASS")
        else()
            set(_kbe_csharp_project "${CMAKE_CURRENT_SOURCE_DIR}/csharp_tcp_send/CsharpTcpSendQueueTest.csproj")
            set(_kbe_csharp_marker "CSHARP_SEND_BACKPRESSURE_TEST_PASS")
        endif()

        add_test(NAME "sdk.csharp.${_kbe_csharp_fixture}"
            COMMAND "${KBE_SDK_DOTNET_EXECUTABLE}" run
                --project "${_kbe_csharp_project}"
                --configuration Release
                --nologo
                "--property:KbeSdkTemplateRoot=${_kbe_sdk_template_root}/csharp"
                "--property:BaseOutputPath=${_kbe_sdk_test_output}/csharp-${_kbe_csharp_fixture}/bin/"
                "--property:BaseIntermediateOutputPath=${_kbe_sdk_test_output}/csharp-${_kbe_csharp_fixture}/obj/"
        )
        set_tests_properties("sdk.csharp.${_kbe_csharp_fixture}" PROPERTIES
            LABELS "sdk;csharp;unit;portable"
            PASS_REGULAR_EXPRESSION "${_kbe_csharp_marker}"
            TIMEOUT 180
        )
    endforeach()
endif()

if(KBE_SDK_TYPESCRIPT_EXECUTABLE AND KBE_SDK_NODE_EXECUTABLE)
    set(_kbe_typescript_output "${_kbe_sdk_test_output}/typescript-websocket")
    add_test(NAME sdk.typescript.websocket_build
        COMMAND "${KBE_SDK_TYPESCRIPT_EXECUTABLE}"
            -p "${CMAKE_CURRENT_SOURCE_DIR}/typescript_websocket/tsconfig.json"
            --outDir "${_kbe_typescript_output}"
    )
    set_tests_properties(sdk.typescript.websocket_build PROPERTIES
        LABELS "sdk;typescript;build;portable"
        TIMEOUT 180
    )

    add_test(NAME sdk.typescript.websocket
        COMMAND "${KBE_SDK_NODE_EXECUTABLE}"
            "${_kbe_typescript_output}/src/tests/sdk/typescript_websocket/Program.js"
    )
    set_tests_properties(sdk.typescript.websocket PROPERTIES
        DEPENDS sdk.typescript.websocket_build
        LABELS "sdk;typescript;unit;portable"
        PASS_REGULAR_EXPRESSION "TYPESCRIPT_WEBSOCKET_FRAME_TEST_PASS"
        TIMEOUT 60
    )
endif()

if(KBE_SDK_GODOT_EXECUTABLE)
    add_subdirectory(gdscript_websocket)
endif()

add_custom_target(kbe_sdk_tests DEPENDS
    kbe_test_sdk_cxx_numeric_boundary
    kbe_test_sdk_cxx_tcp_receive
)
set_target_properties(kbe_sdk_tests PROPERTIES FOLDER "Tests/SDK")
