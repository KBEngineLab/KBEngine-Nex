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

add_executable(kbe_test_network_completion_udp_send_queue
    completion_udp_send_queue.cpp
	${CMAKE_SOURCE_DIR}/lib/network/completion_udp_send_budget.cpp
)

add_executable(kbe_test_network_kcp_update_queue
    kcp_update_queue.cpp
    ${CMAKE_SOURCE_DIR}/lib/network/kcp_update_queue.cpp
)

add_executable(kbe_test_network_kcp_send_state kcp_send_state.cpp)

add_executable(kbe_test_network_kcp_protocol_metrics
    kcp_protocol_metrics.cpp
    ${CMAKE_SOURCE_DIR}/lib/network/ikcp.c
)

add_executable(kbe_test_network_udp_send_backpressure
    udp_send_backpressure.cpp
    ${CMAKE_SOURCE_DIR}/lib/network/udp_send_backpressure.cpp
)

# These focused fixtures compile selected network implementation files directly.
# common.h publicly includes fmt headers, so inherit fmt usage requirements without
# linking the full common library and its unrelated runtime initialization.
# 这些轻量夹具直接编译指定网络实现；common.h 公开包含 fmt，因此只继承 fmt 的
# 使用要求，避免为了头文件再引入整个 common 库及无关运行时初始化。
set(_kbe_network_test_targets
    kbe_test_network_completion_rearm
    kbe_test_network_completion_send_queue
    kbe_test_network_completion_context_pool
    kbe_test_network_completion_udp_send_queue
    kbe_test_network_kcp_update_queue
    kbe_test_network_kcp_send_state
    kbe_test_network_kcp_protocol_metrics
    kbe_test_network_udp_send_backpressure
)
foreach(_kbe_network_test_target IN LISTS _kbe_network_test_targets)
    target_link_libraries(${_kbe_network_test_target} PRIVATE fmt::fmt)
endforeach()

target_include_directories(kbe_test_network_completion_send_queue SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/lib)
set_target_properties(kbe_test_network_completion_send_queue PROPERTIES FOLDER "Tests/Network")
target_include_directories(kbe_test_network_completion_context_pool SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/lib)
set_target_properties(kbe_test_network_completion_context_pool PROPERTIES FOLDER "Tests/Network")
target_include_directories(kbe_test_network_completion_udp_send_queue SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/lib)
set_target_properties(kbe_test_network_completion_udp_send_queue PROPERTIES FOLDER "Tests/Network")
target_include_directories(kbe_test_network_kcp_update_queue SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/lib)
set_target_properties(kbe_test_network_kcp_update_queue PROPERTIES FOLDER "Tests/Network")
target_include_directories(kbe_test_network_kcp_send_state SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/lib)
set_target_properties(kbe_test_network_kcp_send_state PROPERTIES FOLDER "Tests/Network")
target_include_directories(kbe_test_network_kcp_protocol_metrics SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/lib)
set_target_properties(kbe_test_network_kcp_protocol_metrics PROPERTIES FOLDER "Tests/Network")
target_include_directories(kbe_test_network_udp_send_backpressure SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/lib)
set_target_properties(kbe_test_network_udp_send_backpressure PROPERTIES FOLDER "Tests/Network")
target_include_directories(kbe_test_network_completion_rearm SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/lib)
set_target_properties(kbe_test_network_completion_rearm PROPERTIES FOLDER "Tests/Network")

if(MSVC)
    target_compile_options(kbe_test_network_completion_rearm PRIVATE /W4 /WX /utf-8)
    target_compile_options(kbe_test_network_completion_send_queue PRIVATE /W4 /WX /utf-8)
    target_compile_options(kbe_test_network_completion_context_pool PRIVATE /W4 /WX /utf-8)
    target_compile_options(kbe_test_network_completion_udp_send_queue PRIVATE /W4 /WX /utf-8)
    target_compile_options(kbe_test_network_kcp_update_queue PRIVATE /W4 /WX /utf-8)
    target_compile_options(kbe_test_network_kcp_send_state PRIVATE /W4 /WX /utf-8)
    target_compile_options(kbe_test_network_kcp_protocol_metrics PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/W4;/WX;/utf-8>
        $<$<COMPILE_LANGUAGE:C>:/W3;/utf-8>
    )
    target_compile_options(kbe_test_network_udp_send_backpressure PRIVATE /W4 /WX /utf-8)
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
    target_compile_options(kbe_test_network_completion_udp_send_queue PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    target_compile_options(kbe_test_network_kcp_update_queue PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    target_compile_options(kbe_test_network_kcp_send_state PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
    target_compile_options(kbe_test_network_kcp_protocol_metrics PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Werror>
    )
    target_compile_options(kbe_test_network_udp_send_backpressure PRIVATE
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

add_test(NAME network.completion_udp_send_queue COMMAND kbe_test_network_completion_udp_send_queue)
set_tests_properties(network.completion_udp_send_queue PROPERTIES
    LABELS "network;cxx;unit;portable;performance;kcp"
    PASS_REGULAR_EXPRESSION "COMPLETION_UDP_SEND_QUEUE_TEST_PASS"
)

add_test(NAME network.kcp_update_queue COMMAND kbe_test_network_kcp_update_queue)
set_tests_properties(network.kcp_update_queue PROPERTIES
    LABELS "network;cxx;unit;portable;performance;kcp"
    PASS_REGULAR_EXPRESSION "KCP_UPDATE_QUEUE_TEST_PASS"
)

add_test(NAME network.kcp_send_state COMMAND kbe_test_network_kcp_send_state)
set_tests_properties(network.kcp_send_state PROPERTIES
	LABELS "network;cxx;unit;portable;performance;kcp"
	PASS_REGULAR_EXPRESSION "KCP_SEND_STATE_TEST_PASS"
)

add_test(NAME network.kcp_protocol_metrics COMMAND kbe_test_network_kcp_protocol_metrics)
set_tests_properties(network.kcp_protocol_metrics PROPERTIES
	LABELS "network;cxx;unit;portable;performance;kcp"
	PASS_REGULAR_EXPRESSION "KCP_PROTOCOL_METRICS_TEST_PASS"
)

add_test(NAME network.udp_send_backpressure COMMAND kbe_test_network_udp_send_backpressure)
set_tests_properties(network.udp_send_backpressure PROPERTIES
    LABELS "network;cxx;unit;portable;kcp"
    PASS_REGULAR_EXPRESSION "UDP_SEND_BACKPRESSURE_TEST_PASS"
)

add_custom_target(kbe_network_tests DEPENDS
    kbe_test_network_completion_rearm
    kbe_test_network_completion_send_queue
    kbe_test_network_completion_context_pool
    kbe_test_network_completion_udp_send_queue
    kbe_test_network_kcp_update_queue
    kbe_test_network_kcp_send_state
    kbe_test_network_kcp_protocol_metrics
    kbe_test_network_udp_send_backpressure
)
set_target_properties(kbe_network_tests PROPERTIES FOLDER "Tests/Network")
