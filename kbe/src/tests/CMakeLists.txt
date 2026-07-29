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

add_test(NAME source.macos_sigar_contract
    COMMAND "${CMAKE_COMMAND}"
        "-DKBE_SIGAR_ROOT=${CMAKE_SOURCE_DIR}/lib/dependencies/sigar"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyMacOSSigar.cmake"
)
set_tests_properties(source.macos_sigar_contract PROPERTIES LABELS "macos;source")

add_test(NAME source.python_triplet_matrix
    COMMAND "${CMAKE_COMMAND}"
        "-DKBE_CMAKE_ROOT=${CMAKE_SOURCE_DIR}/cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyPythonTriplets.cmake"
)
set_tests_properties(source.python_triplet_matrix PROPERTIES LABELS "python;source")

add_test(NAME source.python_triplet_rejects_universal
    COMMAND "${CMAKE_COMMAND}"
        "-DKBE_CMAKE_ROOT=${CMAKE_SOURCE_DIR}/cmake"
        -DKBE_TEST_UNSUPPORTED_UNIVERSAL=ON
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyPythonTriplets.cmake"
)
set_tests_properties(source.python_triplet_rejects_universal PROPERTIES
    LABELS "macos;python;source"
)

set(KBE_TEST_ASSETS "" CACHE PATH "Assets directory used by CMake integration tests")
set(KBE_TEST_DATABASE_HOST "127.0.0.1" CACHE STRING "Database host used by CMake integration tests")
set(KBE_TEST_DATABASE_USERNAME "" CACHE STRING "Database username used by CMake integration tests")
set(KBE_TEST_DATABASE_PASSWORD "" CACHE STRING "Database password used by CMake integration tests")
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

        if(KBE_TEST_DATABASE_USERNAME)
            # 每个后端使用独立数据库和独立日志目录，避免测试改动课程项目正在使用的数据。
            # Each backend uses an isolated database and log directory so tests cannot modify the course project's active data.
            foreach(_kbe_database_spec IN ITEMS
                "mysql::3306::kbe_cmake_mysql_test"
                "mongodb::27017::kbe_cmake_mongodb_test"
                "postgresql::5432::kbe_cmake_postgresql_test"
            )
                string(REPLACE "::" ";" _kbe_database_fields "${_kbe_database_spec}")
                list(GET _kbe_database_fields 0 _kbe_database_type)
                list(GET _kbe_database_fields 1 _kbe_database_port)
                list(GET _kbe_database_fields 2 _kbe_database_name)

                add_test(NAME "integration.database_${_kbe_database_type}"
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
                        -RunRoot "${CMAKE_BINARY_DIR}/Testing/database-${_kbe_database_type}-$<CONFIG>"
                        -DatabaseType "${_kbe_database_type}"
                        -DatabaseHost "${KBE_TEST_DATABASE_HOST}"
                        -DatabasePort "${_kbe_database_port}"
                        -DatabaseName "${_kbe_database_name}"
                        -DatabaseUsername "${KBE_TEST_DATABASE_USERNAME}"
                        -DatabasePassword "${KBE_TEST_DATABASE_PASSWORD}"
                )
                set_tests_properties("integration.database_${_kbe_database_type}" PROPERTIES
                    LABELS "database;integration;runtime;server;${_kbe_database_type}"
                    RUN_SERIAL TRUE
                    TIMEOUT 240
                )
            endforeach()
        endif()
    endif()
endif()

# SDK 验证工具维护自己的测试和外部工具条件，主测试树只负责挂载，避免两处清单随 SDK 演进而漂移。
# The SDK validation tool owns its tests and external-tool conditions; the main test tree only mounts it to avoid duplicated manifests drifting as SDKs evolve.
add_subdirectory(
    "${CMAKE_SOURCE_DIR}/../tools/sdk_validation"
    "${CMAKE_BINARY_DIR}/tools/sdk_validation"
)
