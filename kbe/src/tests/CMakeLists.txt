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
option(KBE_TEST_SERVER_CLUSTER "Enable the server cluster integration test" OFF)
set(KBE_TEST_DATABASE_HOST "127.0.0.1" CACHE STRING "Database host used by CMake integration tests")
set(KBE_TEST_DATABASE_USERNAME "" CACHE STRING "Database username used by CMake integration tests")
set(KBE_TEST_DATABASE_PASSWORD "" CACHE STRING "Database password used by CMake integration tests")
set(KBE_TEST_DATABASES "" CACHE STRING
    "Semicolon-separated database integration tests to enable: mysql, mongodb, postgresql")
if((KBE_TEST_SERVER_CLUSTER OR KBE_TEST_DATABASES) AND NOT KBE_TEST_ASSETS)
    message(FATAL_ERROR
        "KBE_TEST_ASSETS is required when server cluster or database integration tests are enabled")
endif()
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

    if(KBE_TEST_SERVER_CLUSTER OR KBE_TEST_DATABASES)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
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
    endif()

    if(KBE_TEST_SERVER_CLUSTER)
        # 普通集群会使用 assets 自身的数据库配置，因此必须显式启用，不能由路径存在间接触发。
        # The regular cluster uses the assets database configuration and therefore requires explicit opt-in rather than path-based activation.
        add_test(NAME integration.server_cluster
            COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/python/test_server_cluster.py"
                --components "${_kbe_cluster_components_arg}"
                --repository-root "${CMAKE_SOURCE_DIR}/../.."
                --assets-root "${_kbe_test_assets}"
                --binary-root "${CMAKE_BINARY_DIR}"
                --run-root "${CMAKE_BINARY_DIR}/Testing/server-cluster-$<CONFIG>"
        )
        set_tests_properties(integration.server_cluster PROPERTIES
            LABELS "integration;portable;runtime;server"
            RUN_SERIAL TRUE
            TIMEOUT 180
        )
    endif()

    if(KBE_TEST_DATABASES)
        if(NOT KBE_TEST_DATABASE_USERNAME)
            message(FATAL_ERROR
                "KBE_TEST_DATABASE_USERNAME is required when KBE_TEST_DATABASES is not empty")
        endif()

        # 数据库测试必须显式选择；仅配置凭据不会让未启动的后端意外阻断常规 CTest。
        # Database tests require explicit selection; credentials alone cannot let inactive backends unexpectedly block regular CTest runs.
        set(_kbe_supported_databases mysql mongodb postgresql)
        foreach(_kbe_database_type IN LISTS KBE_TEST_DATABASES)
            if(NOT _kbe_database_type IN_LIST _kbe_supported_databases)
                message(FATAL_ERROR
                    "Unsupported KBE_TEST_DATABASES entry '${_kbe_database_type}'. "
                    "Expected mysql, mongodb, or postgresql.")
            endif()

            if(_kbe_database_type STREQUAL "mysql")
                set(_kbe_database_port 3306)
                set(_kbe_database_name kbe_cmake_mysql_test)
            elseif(_kbe_database_type STREQUAL "mongodb")
                set(_kbe_database_port 27017)
                set(_kbe_database_name kbe_cmake_mongodb_test)
            else()
                set(_kbe_database_port 5432)
                set(_kbe_database_name kbe_cmake_postgresql_test)
            endif()

            # 每个后端使用独立数据库和独立日志目录，避免测试改动项目正在使用的数据。
            # Each backend uses an isolated database and log directory so tests cannot modify the project's active data.
            add_test(NAME "integration.database_${_kbe_database_type}"
                COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/python/test_server_cluster.py"
                    --components "${_kbe_cluster_components_arg}"
                    --repository-root "${CMAKE_SOURCE_DIR}/../.."
                    --assets-root "${_kbe_test_assets}"
                    --binary-root "${CMAKE_BINARY_DIR}"
                    --run-root "${CMAKE_BINARY_DIR}/Testing/database-${_kbe_database_type}-$<CONFIG>"
                    --database-type "${_kbe_database_type}"
                    --database-host "${KBE_TEST_DATABASE_HOST}"
                    --database-port "${_kbe_database_port}"
                    --database-name "${_kbe_database_name}"
                    --database-username "${KBE_TEST_DATABASE_USERNAME}"
                    --database-password "${KBE_TEST_DATABASE_PASSWORD}"
            )
            set_tests_properties("integration.database_${_kbe_database_type}" PROPERTIES
                LABELS "database;integration;portable;runtime;server;${_kbe_database_type}"
                RUN_SERIAL TRUE
                SKIP_RETURN_CODE 77
                TIMEOUT 240
            )
        endforeach()
    endif()
endif()

# SDK 回归属于源码测试树；外部项目验收和发布流程不应成为引擎 CTest 的隐式依赖。
# SDK regressions belong to the source test tree; external project acceptance and release workflows must not become implicit engine CTest dependencies.
add_subdirectory(sdk)
add_subdirectory(network)
add_subdirectory(common)
