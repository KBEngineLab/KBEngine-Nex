set(_kbe_runtime_executables
    "$<TARGET_FILE:machine>"
    "$<TARGET_FILE:baseappmgr>"
    "$<TARGET_FILE:cellappmgr>"
    "$<TARGET_FILE:dbmgr>"
    "$<TARGET_FILE:loginapp>"
    "$<TARGET_FILE:baseapp>"
    "$<TARGET_FILE:cellapp>"
    "$<TARGET_FILE:bots>"
    "$<TARGET_FILE:logger>"
    "$<TARGET_FILE:interfaces>"
    "$<TARGET_FILE:kbcmd>"
)
list(JOIN _kbe_runtime_executables "|" _kbe_runtime_executables_arg)

add_test(NAME runtime.layout
    COMMAND "${CMAKE_COMMAND}"
        "-DKBE_EXECUTABLES=${_kbe_runtime_executables_arg}"
        "-DKBE_PYTHON_STDLIB=${KBE_RUNTIME_PYTHON_STDLIB}"
        "-DKBE_PYTHON_EXTENSION=${KBE_RUNTIME_PYTHON_EXTENSION}"
        "-DKBE_PYTHON_DLL_DIR=$<TARGET_FILE_DIR:kbcmd>"
        "-DKBE_PYTHON_ABI=${_kbe_python_abi}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyRuntime.cmake"
)
set_tests_properties(runtime.layout PROPERTIES LABELS "smoke;runtime")

add_test(NAME runtime.kbcmd_getuid
    COMMAND "$<TARGET_FILE:kbcmd>" --getuid
)
set_tests_properties(runtime.kbcmd_getuid PROPERTIES
    LABELS "smoke;runtime"
    PASS_REGULAR_EXPRESSION "[0-9]+"
    FAIL_REGULAR_EXPRESSION "\\[ERROR\\];Traceback"
    ENVIRONMENT "KBE_ROOT=${CMAKE_SOURCE_DIR}/../..;KBE_RES_PATH=${CMAKE_SOURCE_DIR}/../res;KBE_BIN_PATH=$<TARGET_FILE_DIR:kbcmd>"
)

set(KBE_TEST_ASSETS "" CACHE PATH "Assets directory used by CMake integration tests")
if(KBE_TEST_ASSETS)
    cmake_path(ABSOLUTE_PATH KBE_TEST_ASSETS NORMALIZE OUTPUT_VARIABLE _kbe_test_assets)
    if(NOT EXISTS "${_kbe_test_assets}/res/server/kbengine.xml" AND NOT EXISTS "${_kbe_test_assets}/kbengine.xml")
        message(FATAL_ERROR "KBE_TEST_ASSETS does not contain kbengine.xml: ${_kbe_test_assets}")
    endif()
    if(NOT EXISTS "${_kbe_test_assets}/scripts/entities.xml" AND NOT EXISTS "${_kbe_test_assets}/entities.xml")
        message(FATAL_ERROR "KBE_TEST_ASSETS does not contain entities.xml: ${_kbe_test_assets}")
    endif()

    add_test(NAME integration.kbcmd_typescript_sdk
        COMMAND "${CMAKE_COMMAND}"
            "-DKBE_EXECUTABLE=$<TARGET_FILE:kbcmd>"
            "-DKBE_REPOSITORY_ROOT=${CMAKE_SOURCE_DIR}/../.."
            "-DKBE_ASSETS_ROOT=${_kbe_test_assets}"
            "-DKBE_OUTPUT_ROOT=${CMAKE_BINARY_DIR}/Testing/kbcmd-typescript-$<CONFIG>"
            "-DKBE_BINARY_ROOT=${CMAKE_BINARY_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunKbcmdSdkSmoke.cmake"
    )
    set_tests_properties(integration.kbcmd_typescript_sdk PROPERTIES
        LABELS "integration;runtime;sdk"
        TIMEOUT 180
    )

    if(WIN32)
        set(_kbe_cluster_components
            "machine::$<TARGET_FILE:machine>::1000::1"
            "logger::$<TARGET_FILE:logger>::2000::2"
            "interfaces::$<TARGET_FILE:interfaces>::3000::3"
            "dbmgr::$<TARGET_FILE:dbmgr>::4000::4"
            "baseappmgr::$<TARGET_FILE:baseappmgr>::5000::5"
            "cellappmgr::$<TARGET_FILE:cellappmgr>::6000::6"
            "baseapp::$<TARGET_FILE:baseapp>::7001::7"
            "cellapp::$<TARGET_FILE:cellapp>::8001::9"
            "loginapp::$<TARGET_FILE:loginapp>::9000::11"
        )
        list(JOIN _kbe_cluster_components "|" _kbe_cluster_components_arg)

        add_test(NAME integration.server_cluster
            COMMAND powershell.exe
                -NoLogo
                -NoProfile
                -NonInteractive
                -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/powershell/Test-ServerCluster.ps1"
                -Components "${_kbe_cluster_components_arg}"
                -RepositoryRoot "${CMAKE_SOURCE_DIR}/../.."
                -AssetsRoot "${_kbe_test_assets}"
                -BinaryRoot "${CMAKE_BINARY_DIR}"
                -RunRoot "${CMAKE_BINARY_DIR}/Testing/server-cluster-$<CONFIG>"
        )
        set_tests_properties(integration.server_cluster PROPERTIES
            LABELS "integration;runtime;server"
            RUN_SERIAL TRUE
            TIMEOUT 180
        )
    endif()
endif()
