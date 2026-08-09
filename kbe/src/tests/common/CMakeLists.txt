add_executable(kbe_test_common_pooling
    debug_helper_stubs.cpp
    pooling.cpp
)
target_link_libraries(kbe_test_common_pooling PRIVATE common)
set_target_properties(kbe_test_common_pooling PROPERTIES FOLDER "Tests/Common")

if(MSVC)
    target_compile_options(kbe_test_common_pooling PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(kbe_test_common_pooling PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
endif()

add_test(NAME common.pooling COMMAND kbe_test_common_pooling)
set_tests_properties(common.pooling PROPERTIES
    LABELS "common;cxx;unit;portable;performance"
    PASS_REGULAR_EXPRESSION "COMMON_POOLING_TEST_PASS"
)

add_executable(kbe_test_blowfish_compat
	debug_helper_stubs.cpp
	blowfish_compat.cpp
)
target_include_directories(kbe_test_blowfish_compat PRIVATE
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/lib"
)
target_link_libraries(kbe_test_blowfish_compat PRIVATE common)
set_target_properties(kbe_test_blowfish_compat PROPERTIES FOLDER "Tests/Common")

if(MSVC)
	target_compile_options(kbe_test_blowfish_compat PRIVATE /W4 /WX /utf-8)
else()
	target_compile_options(kbe_test_blowfish_compat PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME common.blowfish_compat COMMAND kbe_test_blowfish_compat)
set_tests_properties(common.blowfish_compat PROPERTIES
	LABELS "common;cxx;unit;portable;security"
	PASS_REGULAR_EXPRESSION "BLOWFISH_COMPAT_TEST_PASS"
)

add_executable(kbe_test_timer_fairness
	debug_helper_stubs.cpp
	timer_fairness.cpp
)
target_link_libraries(kbe_test_timer_fairness PRIVATE common)
set_target_properties(kbe_test_timer_fairness PROPERTIES FOLDER "Tests/Common")

add_executable(kbe_test_receive_window_policy
	debug_helper_stubs.cpp
	receive_window_policy.cpp
)
target_include_directories(kbe_test_receive_window_policy PRIVATE
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/lib"
)
target_link_libraries(kbe_test_receive_window_policy PRIVATE common)
set_target_properties(kbe_test_receive_window_policy PROPERTIES FOLDER "Tests/Common")

if(MSVC)
	target_compile_options(kbe_test_timer_fairness PRIVATE /W4 /WX /utf-8)
	target_compile_options(kbe_test_receive_window_policy PRIVATE /W4 /WX /utf-8)
else()
	target_compile_options(kbe_test_timer_fairness PRIVATE -Wall -Wextra -Wpedantic -Werror)
	target_compile_options(kbe_test_receive_window_policy PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME common.timer_fairness COMMAND kbe_test_timer_fairness)
set_tests_properties(common.timer_fairness PROPERTIES
	LABELS "common;cxx;unit;portable;performance"
	PASS_REGULAR_EXPRESSION "TIMER_FAIRNESS_TEST_PASS"
)

add_test(NAME security.receive_window_policy COMMAND kbe_test_receive_window_policy)
set_tests_properties(security.receive_window_policy PROPERTIES
	LABELS "network;cxx;unit;portable;security;performance"
	PASS_REGULAR_EXPRESSION "RECEIVE_WINDOW_POLICY_TEST_PASS"
)

add_executable(kbe_test_profile_latency profile_latency.cpp)
target_link_libraries(kbe_test_profile_latency PRIVATE helper)
set_target_properties(kbe_test_profile_latency PROPERTIES FOLDER "Tests/Common")

if(MSVC)
	target_compile_options(kbe_test_profile_latency PRIVATE /W4 /WX /utf-8)
else()
	target_compile_options(kbe_test_profile_latency PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME performance.profile_latency COMMAND kbe_test_profile_latency)
set_tests_properties(performance.profile_latency PROPERTIES
	LABELS "common;cxx;unit;portable;performance"
	PASS_REGULAR_EXPRESSION "PROFILE_LATENCY_TEST_PASS"
)

add_executable(kbe_test_profile_warning_limiter profile_warning_limiter.cpp)
target_include_directories(kbe_test_profile_warning_limiter PRIVATE
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/lib"
)
set_target_properties(kbe_test_profile_warning_limiter PROPERTIES FOLDER "Tests/Common")

if(MSVC)
	target_compile_options(kbe_test_profile_warning_limiter PRIVATE /W4 /WX /utf-8)
else()
	target_compile_options(kbe_test_profile_warning_limiter PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME performance.profile_warning_limiter COMMAND kbe_test_profile_warning_limiter)
set_tests_properties(performance.profile_warning_limiter PROPERTIES
	LABELS "common;cxx;unit;portable;performance"
	PASS_REGULAR_EXPRESSION "PROFILE_WARNING_LIMITER_TEST_PASS"
)

add_executable(kbe_test_message_processing_metrics message_processing_metrics.cpp)
target_include_directories(kbe_test_message_processing_metrics PRIVATE
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/lib"
)
set_target_properties(kbe_test_message_processing_metrics PROPERTIES FOLDER "Tests/Common")

if(MSVC)
	target_compile_options(kbe_test_message_processing_metrics PRIVATE /W4 /WX /utf-8)
else()
	target_compile_options(kbe_test_message_processing_metrics PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME performance.message_processing_metrics COMMAND kbe_test_message_processing_metrics)
set_tests_properties(performance.message_processing_metrics PROPERTIES
	LABELS "network;cxx;unit;portable;performance"
	PASS_REGULAR_EXPRESSION "MESSAGE_PROCESSING_METRICS_TEST_PASS"
)

add_executable(kbe_test_script_stage_metrics script_stage_metrics.cpp)
target_include_directories(kbe_test_script_stage_metrics PRIVATE
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/lib"
)
set_target_properties(kbe_test_script_stage_metrics PROPERTIES FOLDER "Tests/Common")

if(MSVC)
	target_compile_options(kbe_test_script_stage_metrics PRIVATE /W4 /WX /utf-8)
else()
	target_compile_options(kbe_test_script_stage_metrics PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME performance.script_stage_metrics COMMAND kbe_test_script_stage_metrics)
set_tests_properties(performance.script_stage_metrics PROPERTIES
	LABELS "baseapp;cellapp;cxx;unit;portable;performance"
	PASS_REGULAR_EXPRESSION "SCRIPT_STAGE_METRICS_TEST_PASS"
)

add_executable(kbe_test_witness_dirty_queue witness_dirty_queue.cpp)
target_include_directories(kbe_test_witness_dirty_queue PRIVATE
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/lib"
)
set_target_properties(kbe_test_witness_dirty_queue PROPERTIES FOLDER "Tests/Common")

if(MSVC)
	target_compile_options(kbe_test_witness_dirty_queue PRIVATE /W4 /WX /utf-8)
else()
	target_compile_options(kbe_test_witness_dirty_queue PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME cellapp.witness_dirty_queue COMMAND kbe_test_witness_dirty_queue)
set_tests_properties(cellapp.witness_dirty_queue PROPERTIES
	LABELS "cellapp;cxx;unit;portable;performance"
	PASS_REGULAR_EXPRESSION "WITNESS_DIRTY_QUEUE_TEST_PASS"
)

add_executable(kbe_test_witness_load_metrics witness_load_metrics.cpp)
target_include_directories(kbe_test_witness_load_metrics PRIVATE
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/lib"
)
set_target_properties(kbe_test_witness_load_metrics PROPERTIES FOLDER "Tests/Common")

if(MSVC)
	target_compile_options(kbe_test_witness_load_metrics PRIVATE /W4 /WX /utf-8)
else()
	target_compile_options(kbe_test_witness_load_metrics PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME cellapp.witness_load_metrics COMMAND kbe_test_witness_load_metrics)
set_tests_properties(cellapp.witness_load_metrics PROPERTIES
	LABELS "cellapp;cxx;unit;portable;performance"
	PASS_REGULAR_EXPRESSION "WITNESS_LOAD_METRICS_TEST_PASS"
)

add_executable(kbe_test_witness_volatile_budget witness_volatile_budget.cpp)
target_include_directories(kbe_test_witness_volatile_budget PRIVATE
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/lib"
)
set_target_properties(kbe_test_witness_volatile_budget PROPERTIES FOLDER "Tests/Common")

if(MSVC)
	target_compile_options(kbe_test_witness_volatile_budget PRIVATE /W4 /WX /utf-8)
else()
	target_compile_options(kbe_test_witness_volatile_budget PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME cellapp.witness_volatile_budget COMMAND kbe_test_witness_volatile_budget)
set_tests_properties(cellapp.witness_volatile_budget PROPERTIES
	LABELS "cellapp;cxx;unit;portable;performance"
	PASS_REGULAR_EXPRESSION "WITNESS_VOLATILE_BUDGET_TEST_PASS"
)

add_executable(kbe_test_cellapp_placement cellapp_placement.cpp)
target_include_directories(kbe_test_cellapp_placement PRIVATE
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/lib"
)
set_target_properties(kbe_test_cellapp_placement PROPERTIES FOLDER "Tests/Common")

if(MSVC)
	target_compile_options(kbe_test_cellapp_placement PRIVATE /W4 /WX /utf-8)
else()
	target_compile_options(kbe_test_cellapp_placement PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME cellapp.placement COMMAND kbe_test_cellapp_placement)
set_tests_properties(cellapp.placement PROPERTIES
	LABELS "cellappmgr;cxx;unit;portable;performance"
	PASS_REGULAR_EXPRESSION "CELLAPP_PLACEMENT_TEST_PASS"
)

add_executable(kbe_test_baseapp_placement baseapp_placement.cpp)
target_include_directories(kbe_test_baseapp_placement PRIVATE
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/lib"
)
set_target_properties(kbe_test_baseapp_placement PROPERTIES FOLDER "Tests/Common")

if(MSVC)
	target_compile_options(kbe_test_baseapp_placement PRIVATE /W4 /WX /utf-8)
else()
	target_compile_options(kbe_test_baseapp_placement PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_test(NAME baseapp.placement COMMAND kbe_test_baseapp_placement)
set_tests_properties(baseapp.placement PROPERTIES
	LABELS "baseappmgr;cxx;unit;portable;performance"
	PASS_REGULAR_EXPRESSION "BASEAPP_PLACEMENT_TEST_PASS"
)

add_executable(kbe_test_coordinate_system_near_insert
	debug_helper_stubs.cpp
	coordinate_system_near_insert.cpp
	${CMAKE_SOURCE_DIR}/server/cellapp/coordinate_node.cpp
	${CMAKE_SOURCE_DIR}/server/cellapp/coordinate_system.cpp
)
target_include_directories(kbe_test_coordinate_system_near_insert PRIVATE
	"${CMAKE_SOURCE_DIR}"
	"${CMAKE_SOURCE_DIR}/lib"
	"${CMAKE_SOURCE_DIR}/server/cellapp"
)
target_compile_definitions(kbe_test_coordinate_system_near_insert PRIVATE
	KBE_COORDINATE_SYSTEM_DISABLE_PROFILE
)
target_link_libraries(kbe_test_coordinate_system_near_insert PRIVATE common)
set_target_properties(kbe_test_coordinate_system_near_insert PROPERTIES FOLDER "Tests/Common")

if(MSVC)
	target_compile_options(kbe_test_coordinate_system_near_insert PRIVATE /W4 /WX /utf-8)
else()
	target_compile_options(kbe_test_coordinate_system_near_insert PRIVATE
		-Wall -Wextra -Wpedantic -Werror
	)
endif()

add_test(NAME cellapp.coordinate_system_near_insert
	COMMAND kbe_test_coordinate_system_near_insert
)
set_tests_properties(cellapp.coordinate_system_near_insert PROPERTIES
	LABELS "cellapp;cxx;unit;portable;performance"
	PASS_REGULAR_EXPRESSION "COORDINATE_SYSTEM_NEAR_INSERT_TEST_PASS"
)

add_custom_target(kbe_common_tests DEPENDS
	kbe_test_common_pooling
	kbe_test_blowfish_compat
	kbe_test_timer_fairness
	kbe_test_receive_window_policy
	kbe_test_profile_latency
	kbe_test_profile_warning_limiter
	kbe_test_message_processing_metrics
	kbe_test_script_stage_metrics
	kbe_test_witness_dirty_queue
	kbe_test_witness_load_metrics
	kbe_test_witness_volatile_budget
	kbe_test_cellapp_placement
	kbe_test_baseapp_placement
	kbe_test_coordinate_system_near_insert
)
set_target_properties(kbe_common_tests PROPERTIES FOLDER "Tests/Common")
