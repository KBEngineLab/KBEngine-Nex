add_executable(kbe_test_network_completion_rearm
    completion_rearm_queue.cpp
    ${CMAKE_SOURCE_DIR}/lib/network/completion_rearm_queue.cpp
)
target_include_directories(kbe_test_network_completion_rearm SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/lib)
set_target_properties(kbe_test_network_completion_rearm PROPERTIES FOLDER "Tests/Network")

if(MSVC)
    target_compile_options(kbe_test_network_completion_rearm PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(kbe_test_network_completion_rearm PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
endif()

add_test(NAME network.completion_rearm_queue COMMAND kbe_test_network_completion_rearm)
set_tests_properties(network.completion_rearm_queue PROPERTIES
    LABELS "network;cxx;unit;portable"
    PASS_REGULAR_EXPRESSION "COMPLETION_REARM_QUEUE_TEST_PASS"
)

add_custom_target(kbe_network_tests DEPENDS kbe_test_network_completion_rearm)
set_target_properties(kbe_network_tests PROPERTIES FOLDER "Tests/Network")
