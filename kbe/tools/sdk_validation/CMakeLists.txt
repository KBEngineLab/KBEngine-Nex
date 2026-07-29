cmake_minimum_required(VERSION 3.25)

set(_kbe_sdk_validation_root "${CMAKE_CURRENT_LIST_DIR}")
set(_kbe_sdk_validation_tests "${_kbe_sdk_validation_root}/tests")
set(_kbe_sdk_validation_output "${CMAKE_BINARY_DIR}/Testing/sdk-validation-$<CONFIG>")

# 外部工具全部通过缓存变量暴露，自动发现只提供本机默认值，不把开发机绝对路径写入项目。
# Every external tool is exposed through a cache variable; discovery only supplies local defaults and never stores developer-specific paths in source.
function(kbe_sdk_import_environment_path variable_name description)
    if((NOT DEFINED ${variable_name}
        OR "${${variable_name}}" STREQUAL ""
        OR "${${variable_name}}" MATCHES "-NOTFOUND$")
       AND DEFINED ENV{${variable_name}}
       AND NOT "$ENV{${variable_name}}" STREQUAL "")
        set(${variable_name} "$ENV{${variable_name}}" CACHE FILEPATH "${description}" FORCE)
    endif()
endfunction()

kbe_sdk_import_environment_path(KBE_SDK_PWSH_EXECUTABLE "PowerShell 7 executable used by SDK validation")
kbe_sdk_import_environment_path(KBE_SDK_DOTNET_EXECUTABLE ".NET executable used by C# SDK validation")
kbe_sdk_import_environment_path(KBE_SDK_NODE_EXECUTABLE "Node.js executable used by TypeScript SDK validation")
kbe_sdk_import_environment_path(KBE_SDK_TYPESCRIPT_EXECUTABLE "TypeScript compiler used by SDK validation")
kbe_sdk_import_environment_path(KBE_SDK_GODOT_EXECUTABLE "Godot console executable used by GDScript SDK validation")
kbe_sdk_import_environment_path(KBE_SDK_MSBUILD_EXECUTABLE "MSBuild executable used by SDK validation fixtures")

find_program(KBE_SDK_PWSH_EXECUTABLE NAMES pwsh)
find_program(KBE_SDK_DOTNET_EXECUTABLE NAMES dotnet)
find_program(KBE_SDK_NODE_EXECUTABLE NAMES node)
find_program(KBE_SDK_TYPESCRIPT_EXECUTABLE NAMES tsc tsc.cmd)
find_program(KBE_SDK_GODOT_EXECUTABLE NAMES godot4 godot Godot_v4.7-stable_win64_console)

if(NOT KBE_SDK_MSBUILD_EXECUTABLE AND CMAKE_VS_MSBUILD_COMMAND)
    set(KBE_SDK_MSBUILD_EXECUTABLE "${CMAKE_VS_MSBUILD_COMMAND}" CACHE FILEPATH
        "MSBuild executable used by SDK validation fixtures" FORCE)
endif()
set(KBE_SDK_VALIDATION_MANIFEST "" CACHE FILEPATH
    "Project-owned SDK validation manifest used by the online CTest gate")
if(KBE_SDK_VALIDATION_MANIFEST STREQUAL ""
   AND DEFINED ENV{KBE_SDK_VALIDATION_MANIFEST}
   AND NOT "$ENV{KBE_SDK_VALIDATION_MANIFEST}" STREQUAL "")
    set(KBE_SDK_VALIDATION_MANIFEST "$ENV{KBE_SDK_VALIDATION_MANIFEST}" CACHE FILEPATH
        "Project-owned SDK validation manifest used by the online CTest gate" FORCE)
endif()

# 游戏项目通常把 TypeScript 固定在 node_modules 中；复用该版本可避免全局 tsc 与项目锁定版本不一致。
# Game projects commonly pin TypeScript under node_modules; reusing that compiler avoids drift from a globally installed tsc.
if(NOT KBE_SDK_TYPESCRIPT_EXECUTABLE AND KBE_TEST_ASSETS)
    set(_kbe_assets_typescript
        "${KBE_TEST_ASSETS}/kbeclient/typescript-component-e2e/node_modules/.bin/tsc.cmd")
    if(EXISTS "${_kbe_assets_typescript}")
        set(KBE_SDK_TYPESCRIPT_EXECUTABLE "${_kbe_assets_typescript}" CACHE FILEPATH
            "TypeScript compiler used by SDK validation" FORCE)
    endif()
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)

function(kbe_set_sdk_test_properties test_name timeout labels pass_expression)
    set_tests_properties("${test_name}" PROPERTIES
        LABELS "${labels}"
        TIMEOUT "${timeout}"
    )
    if(NOT "${pass_expression}" STREQUAL "")
        set_tests_properties("${test_name}" PROPERTIES
            PASS_REGULAR_EXPRESSION "${pass_expression}"
        )
    endif()
endfunction()

if(KBE_SDK_PWSH_EXECUTABLE)
    add_test(NAME sdk.validation.runner_contract
        COMMAND "${KBE_SDK_PWSH_EXECUTABLE}"
            -NoLogo
            -NoProfile
            -NonInteractive
            -File "${_kbe_sdk_validation_tests}/Test-SdkValidation.ps1"
    )
    kbe_set_sdk_test_properties(
        sdk.validation.runner_contract 120 "sdk;unit;offline"
        "SDK_VALIDATION_TEST_PASS"
    )

    # 发布产物测试使用 CMake 生成的真实 kbcmd，但只读取仓库自带模板，不依赖业务服务或数据库。
    # The release-artifact test uses the real CMake-built kbcmd while reading only repository templates, without game services or databases.
    add_test(NAME sdk.release.artifacts
        COMMAND "${KBE_SDK_PWSH_EXECUTABLE}"
            -NoLogo
            -NoProfile
            -NonInteractive
            -File "${_kbe_sdk_validation_tests}/Test-SdkReleaseArtifacts.ps1"
            -KbcmdPath "$<TARGET_FILE:kbcmd>"
    )
    kbe_set_sdk_test_properties(
        sdk.release.artifacts 300 "sdk;release;offline"
        "SDK_RELEASE_ARTIFACT_TEST_PASS"
    )

    if(KBE_SDK_DOTNET_EXECUTABLE)
        add_test(NAME sdk.csharp.warning_gate
            COMMAND "${KBE_SDK_PWSH_EXECUTABLE}"
                -NoLogo
                -NoProfile
                -NonInteractive
                -File "${_kbe_sdk_validation_tests}/Test-CsharpSdkWarnings.ps1"
                -KbcmdPath "$<TARGET_FILE:kbcmd>"
                -DotnetPath "${KBE_SDK_DOTNET_EXECUTABLE}"
        )
        kbe_set_sdk_test_properties(
            sdk.csharp.warning_gate 240 "sdk;csharp;release;offline"
            "CSHARP_SDK_WARNING_TEST_PASS"
        )

        foreach(_kbe_csharp_fixture IN ITEMS heartbeat tcp_send)
            if(_kbe_csharp_fixture STREQUAL "heartbeat")
                set(_kbe_csharp_project "${_kbe_sdk_validation_tests}/csharp_heartbeat/CsharpHeartbeatStateTest.csproj")
                set(_kbe_csharp_marker "CSHARP_NETWORK_STATE_TEST_PASS")
            else()
                set(_kbe_csharp_project "${_kbe_sdk_validation_tests}/csharp_tcp_send/CsharpTcpSendQueueTest.csproj")
                set(_kbe_csharp_marker "CSHARP_SEND_BACKPRESSURE_TEST_PASS")
            endif()

            set(_kbe_csharp_test "sdk.csharp.${_kbe_csharp_fixture}")
            set(_kbe_csharp_output "${_kbe_sdk_validation_output}/csharp-${_kbe_csharp_fixture}/")
            add_test(NAME "${_kbe_csharp_test}"
                COMMAND "${KBE_SDK_DOTNET_EXECUTABLE}" run
                    --project "${_kbe_csharp_project}"
                    --configuration Release
                    --nologo
                    "--property:BaseOutputPath=${_kbe_csharp_output}bin/"
                    "--property:BaseIntermediateOutputPath=${_kbe_csharp_output}obj/"
            )
            kbe_set_sdk_test_properties(
                "${_kbe_csharp_test}" 180 "sdk;csharp;unit;offline"
                "${_kbe_csharp_marker}"
            )
        endforeach()
    endif()

    if(WIN32 AND EXISTS "${KBE_SDK_MSBUILD_EXECUTABLE}")
        add_test(NAME sdk.cxx.numeric_boundary
            COMMAND "${KBE_SDK_PWSH_EXECUTABLE}"
                -NoLogo
                -NoProfile
                -NonInteractive
                -File "${_kbe_sdk_validation_tests}/Test-CxxSdkBoundaries.ps1"
                -KbcmdPath "$<TARGET_FILE:kbcmd>"
                -MSBuildPath "${KBE_SDK_MSBUILD_EXECUTABLE}"
        )
        kbe_set_sdk_test_properties(
            sdk.cxx.numeric_boundary 300 "sdk;cxx;release;offline"
            "CXX_SDK_BOUNDARY_TEST_PASS"
        )

        set(_kbe_cxx_tcp_project "${_kbe_sdk_validation_tests}/cxx_tcp_receive/CxxTcpReceiveQueueTest.vcxproj")
        set(_kbe_cxx_tcp_output "${_kbe_sdk_validation_output}/cxx-tcp/")
        set(_kbe_cxx_tcp_executable "${_kbe_cxx_tcp_output}CxxTcpReceiveQueueTest.exe")
        add_test(NAME sdk.cxx.tcp_receive_build
            COMMAND "${KBE_SDK_MSBUILD_EXECUTABLE}"
                "${_kbe_cxx_tcp_project}"
                /m:1
                /nr:false
                /t:Rebuild
                /p:Configuration=Release
                /p:Platform=x64
                /p:PlatformToolset=v143
                "/p:OutDir=${_kbe_cxx_tcp_output}"
                "/p:IntDir=${_kbe_cxx_tcp_output}obj/"
                /v:minimal
        )
        kbe_set_sdk_test_properties(
            sdk.cxx.tcp_receive_build 180 "sdk;cxx;build;offline" ""
        )

        add_test(NAME sdk.cxx.tcp_receive
            COMMAND "${_kbe_cxx_tcp_executable}"
        )
        set_tests_properties(sdk.cxx.tcp_receive PROPERTIES
            DEPENDS sdk.cxx.tcp_receive_build
        )
        kbe_set_sdk_test_properties(
            sdk.cxx.tcp_receive 60 "sdk;cxx;unit;offline"
            "CXX_TCP_RECEIVE_BACKPRESSURE_TEST_PASS"
        )
    endif()

    # 在线 E2E 清单属于具体游戏项目；只有调用方显式配置时才注册，避免默认测试读取任意本地 assets。
    # The online E2E manifest belongs to a game project and is registered only when explicitly configured, preventing default tests from reading arbitrary local assets.
    if(NOT KBE_SDK_VALIDATION_MANIFEST STREQUAL "")
        if(NOT EXISTS "${KBE_SDK_VALIDATION_MANIFEST}")
            message(FATAL_ERROR
                "KBE_SDK_VALIDATION_MANIFEST does not exist: ${KBE_SDK_VALIDATION_MANIFEST}")
        endif()

        add_test(NAME sdk.e2e.manifest
            COMMAND "${CMAKE_COMMAND}"
                "-DKBE_PWSH=${KBE_SDK_PWSH_EXECUTABLE}"
                "-DKBE_RUNNER=${_kbe_sdk_validation_root}/Invoke-SdkValidation.ps1"
                "-DKBE_MANIFEST=${KBE_SDK_VALIDATION_MANIFEST}"
                "-DKBE_RESULTS=${_kbe_sdk_validation_output}/e2e"
                "-DKBE_OWNED_ROOT=${CMAKE_BINARY_DIR}/Testing"
                -P "${_kbe_sdk_validation_root}/cmake/RunSdkManifest.cmake"
        )
        kbe_set_sdk_test_properties(
            sdk.e2e.manifest 1800 "sdk;e2e;online" ""
        )
        set_tests_properties(sdk.e2e.manifest PROPERTIES RUN_SERIAL TRUE)
    endif()

    # Windows 候选门禁需要完整 assets、MSBuild、Godot 和 TypeScript；条件不完整时保持可见的配置提示而不注册残缺测试。
    # The Windows candidate gate requires complete assets, MSBuild, Godot, and TypeScript; incomplete conditions remain a visible configure note instead of registering a broken test.
    if(WIN32 AND KBE_TEST_ASSETS AND EXISTS "${KBE_SDK_MSBUILD_EXECUTABLE}"
       AND KBE_SDK_GODOT_EXECUTABLE AND KBE_SDK_TYPESCRIPT_EXECUTABLE)
        set(_kbe_windows_candidate_output "${_kbe_sdk_validation_output}/windows-candidate")
        add_test(NAME sdk.release.windows_candidate
            COMMAND "${CMAKE_COMMAND}"
                "-DKBE_PWSH=${KBE_SDK_PWSH_EXECUTABLE}"
                "-DKBE_SCRIPT=${_kbe_sdk_validation_root}/Test-WindowsReleaseCandidate.ps1"
                "-DKBE_ASSETS=${KBE_TEST_ASSETS}"
                "-DKBE_OUTPUT=${_kbe_windows_candidate_output}"
                "-DKBE_OWNED_ROOT=${CMAKE_BINARY_DIR}/Testing"
                "-DKBE_KBCMD=$<TARGET_FILE:kbcmd>"
                "-DKBE_MSBUILD=${KBE_SDK_MSBUILD_EXECUTABLE}"
                "-DKBE_GODOT=${KBE_SDK_GODOT_EXECUTABLE}"
                "-DKBE_TYPESCRIPT=${KBE_SDK_TYPESCRIPT_EXECUTABLE}"
                -P "${_kbe_sdk_validation_root}/cmake/RunWindowsReleaseCandidate.cmake"
        )
        kbe_set_sdk_test_properties(
            sdk.release.windows_candidate 1200 "sdk;release;windows;offline"
            "WINDOWS_RELEASE_CANDIDATE_PASS"
        )
        set_tests_properties(sdk.release.windows_candidate PROPERTIES
            RUN_SERIAL TRUE
            FIXTURES_SETUP sdk_windows_candidate
        )

        add_test(NAME sdk.gdscript.websocket
            COMMAND "${KBE_SDK_PWSH_EXECUTABLE}"
                -NoLogo
                -NoProfile
                -NonInteractive
                -File "${_kbe_sdk_validation_tests}/gdscript_websocket/Invoke-GdscriptWebSocketTest.ps1"
                -SdkPath "${_kbe_windows_candidate_output}/gdscript"
                -GodotPath "${KBE_SDK_GODOT_EXECUTABLE}"
        )
        kbe_set_sdk_test_properties(
            sdk.gdscript.websocket 180 "sdk;gdscript;unit;offline"
            "GDSCRIPT_WEBSOCKET_FRAME_TEST_PASS"
        )
        set_tests_properties(sdk.gdscript.websocket PROPERTIES
            FIXTURES_REQUIRED sdk_windows_candidate
        )
    else()
        message(STATUS
            "Full Windows SDK candidate CTest is disabled. Configure KBE_TEST_ASSETS, "
            "KBE_SDK_GODOT_EXECUTABLE, and KBE_SDK_TYPESCRIPT_EXECUTABLE to enable it.")
    endif()
else()
    message(STATUS "SDK PowerShell validation tests are disabled because pwsh was not found.")
endif()

if(Python3_Interpreter_FOUND)
    add_test(NAME sdk.validation.resource_release
        COMMAND "${Python3_EXECUTABLE}" -B
            "${_kbe_sdk_validation_tests}/Test-SdkResourceRelease.py"
            -v
    )
    kbe_set_sdk_test_properties(
        sdk.validation.resource_release 60 "sdk;python;unit;offline" ""
    )
endif()

if(KBE_SDK_TYPESCRIPT_EXECUTABLE AND KBE_SDK_NODE_EXECUTABLE)
    set(_kbe_typescript_output "${_kbe_sdk_validation_output}/typescript-websocket")
    add_test(NAME sdk.typescript.websocket_build
        COMMAND "${KBE_SDK_TYPESCRIPT_EXECUTABLE}"
            -p "${_kbe_sdk_validation_tests}/typescript_websocket/tsconfig.json"
            --outDir "${_kbe_typescript_output}"
    )
    kbe_set_sdk_test_properties(
        sdk.typescript.websocket_build 180 "sdk;typescript;build;offline" ""
    )

    add_test(NAME sdk.typescript.websocket
        COMMAND "${KBE_SDK_NODE_EXECUTABLE}"
            "${_kbe_typescript_output}/tools/sdk_validation/tests/typescript_websocket/Program.js"
    )
    set_tests_properties(sdk.typescript.websocket PROPERTIES
        DEPENDS sdk.typescript.websocket_build
    )
    kbe_set_sdk_test_properties(
        sdk.typescript.websocket 60 "sdk;typescript;unit;offline"
        "TYPESCRIPT_WEBSOCKET_FRAME_TEST_PASS"
    )
endif()
