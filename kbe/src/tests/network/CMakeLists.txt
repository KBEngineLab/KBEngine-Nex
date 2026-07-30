add_executable(kbe_test_network_completion_rearm
    completion_rearm_queue.cpp
    ${CMAKE_SOURCE_DIR}/lib/network/completion_rearm_queue.cpp
)

add_executable(kbe_test_network_completion_send_queue
    completion_tcp_send_queue.cpp
    ${CMAKE_SOURCE_DIR}/lib/network/completion_tcp_send_queue.cpp
)

add_executable(kbe_test_network_completion_context_pool
    completion_context_pool.cpp
)

add_executable(kbe_test_network_kcp_update_queue
    kcp_update_queue.cpp
    ${CMAKE_SOURCE_DIR}/lib/network/kcp_update_queue.cpp
)
target_include_directories(kbe_test_network_completion_send_queue SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/lib)
set_target_properties(kbe_test_network_completion_send_queue PROPERTIES FOLDER "Tests/Network")
target_include_directories(kbe_test_network_completion_context_pool SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/lib)
set_target_properties(kbe_test_network_completion_context_pool PROPERTIES FOLDER "Tests/Network")
target_include_directories(kbe_test_network_kcp_update_queue SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/lib)
set_target_properties(kbe_test_network_kcp_update_queue PROPERTIES FOLDER "Tests/Network")
target_include_directories(kbe_test_network_completion_rearm SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/lib)
set_target_properties(kbe_test_network_completion_rearm PROPERTIES FOLDER "Tests/Network")

if(MSVC)
    target_compile_options(kbe_test_network_completion_rearm PRIVATE /W4 /WX /utf-8)
    target_compile_options(kbe_test_network_completion_send_queue PRIVATE /W4 /WX /utf-8)
    target_compile_options(kbe_test_network_completion_context_pool PRIVATE /W4 /WX /utf-8)
    target_compile_options(kbe_test_network_kcp_update_queue PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(kbe_test_network_completion_rearm PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    target_compile_options(kbe_test_network_completion_send_queue PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    target_compile_options(kbe_test_network_completion_context_pool PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    target_compile_options(kbe_test_network_kcp_update_queue PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
endif()

add_test(NAME network.completion_rearm_queue COMMAND kbe_test_network_completion_rearm)
set_tests_properties(network.completion_rearm_queue PROPERTIES
    LABELS "network;cxx;unit;portable"
    PASS_REGULAR_EXPRESSION "COMPLETION_REARM_QUEUE_TEST_PASS"
)

add_test(NAME network.completion_tcp_send_queue COMMAND kbe_test_network_completion_send_queue)
set_tests_properties(network.completion_tcp_send_queue PROPERTIES
    LABELS "network;cxx;unit;portable;performance"
    PASS_REGULAR_EXPRESSION "COMPLETION_TCP_SEND_QUEUE_TEST_PASS"
)

add_test(NAME network.completion_context_pool COMMAND kbe_test_network_completion_context_pool)
set_tests_properties(network.completion_context_pool PROPERTIES
    LABELS "network;cxx;unit;portable;performance"
    PASS_REGULAR_EXPRESSION "COMPLETION_CONTEXT_POOL_TEST_PASS"
)

add_test(NAME network.kcp_update_queue COMMAND kbe_test_network_kcp_update_queue)
set_tests_properties(network.kcp_update_queue PROPERTIES
    LABELS "network;cxx;unit;portable;performance;kcp"
    PASS_REGULAR_EXPRESSION "KCP_UPDATE_QUEUE_TEST_PASS"
)

add_custom_target(kbe_network_tests DEPENDS
    kbe_test_network_completion_rearm
    kbe_test_network_completion_send_queue
    kbe_test_network_completion_context_pool
    kbe_test_network_kcp_update_queue
)
set_target_properties(kbe_network_tests PROPERTIES FOLDER "Tests/Network")
