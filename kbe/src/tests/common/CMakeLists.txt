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

add_custom_target(kbe_common_tests DEPENDS
	kbe_test_common_pooling
	kbe_test_profile_latency
	kbe_test_witness_dirty_queue
	kbe_test_witness_load_metrics
	kbe_test_witness_volatile_budget
	kbe_test_cellapp_placement
	kbe_test_baseapp_placement
)
set_target_properties(kbe_common_tests PROPERTIES FOLDER "Tests/Common")
