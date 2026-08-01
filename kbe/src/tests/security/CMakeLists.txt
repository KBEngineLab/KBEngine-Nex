add_executable(kbe_test_security_component_routing_guard
	component_routing_guard.cpp
)
target_link_libraries(kbe_test_security_component_routing_guard PRIVATE common)
set_target_properties(kbe_test_security_component_routing_guard PROPERTIES
	FOLDER "Tests/Security"
)

if(MSVC)
	target_compile_options(kbe_test_security_component_routing_guard PRIVATE /W4 /WX /utf-8)
else()
	target_compile_options(kbe_test_security_component_routing_guard PRIVATE
		-Wall -Wextra -Wpedantic -Werror
	)
endif()

add_test(NAME security.component_routing_guard
	COMMAND kbe_test_security_component_routing_guard
)
set_tests_properties(security.component_routing_guard PROPERTIES
	LABELS "security;cxx;unit;portable"
	PASS_REGULAR_EXPRESSION "SECURITY_COMPONENT_ROUTING_GUARD_TEST_PASS"
)

add_test(NAME security.source_contract
	COMMAND "${CMAKE_COMMAND}"
		"-DKBE_SOURCE_ROOT=${CMAKE_SOURCE_DIR}"
		-P "${CMAKE_SOURCE_DIR}/tests/cmake/VerifySecuritySourceContract.cmake"
)
set_tests_properties(security.source_contract PROPERTIES
	LABELS "security;source;portable"
)

add_executable(kbe_test_security_client_request_guard
	client_request_guard.cpp
)
target_link_libraries(kbe_test_security_client_request_guard PRIVATE common)
set_target_properties(kbe_test_security_client_request_guard PROPERTIES
	FOLDER "Tests/Security"
)

if(MSVC)
	target_compile_options(kbe_test_security_client_request_guard PRIVATE /W4 /WX /utf-8)
else()
	target_compile_options(kbe_test_security_client_request_guard PRIVATE
		-Wall -Wextra -Wpedantic -Werror
	)
endif()

add_test(NAME security.client_request_guard
	COMMAND kbe_test_security_client_request_guard
)
set_tests_properties(security.client_request_guard PROPERTIES
	LABELS "security;cxx;unit;portable"
	PASS_REGULAR_EXPRESSION "SECURITY_CLIENT_REQUEST_GUARD_TEST_PASS"
)

add_custom_target(kbe_security_tests DEPENDS
	kbe_test_security_component_routing_guard
	kbe_test_security_client_request_guard
)
set_target_properties(kbe_security_tests PROPERTIES FOLDER "Tests/Security")

if(KBE_TEST_SERVER_CLUSTER)
	add_test(NAME security.multiprocess_ingress
		COMMAND "${Python3_EXECUTABLE}" -B
			"${CMAKE_SOURCE_DIR}/tests/python/test_security_ingress.py"
			--cluster-script "${CMAKE_SOURCE_DIR}/tests/python/test_server_cluster.py"
			--components "${_kbe_cluster_components_arg}"
			--repository-root "${CMAKE_SOURCE_DIR}/../.."
			--assets-root "${_kbe_test_assets}"
			--binary-root "${CMAKE_BINARY_DIR}"
			--run-root "${CMAKE_BINARY_DIR}/Testing/security-ingress-$<CONFIG>"
	)
	set_tests_properties(security.multiprocess_ingress PROPERTIES
		LABELS "security;integration;network;runtime;server"
		PASS_REGULAR_EXPRESSION "SECURITY_MULTIPROCESS_INGRESS_PASS probes=44"
		RUN_SERIAL TRUE
		TIMEOUT 240
	)

	if(KBE_SDK_NODE_EXECUTABLE)
		add_test(NAME security.authenticated_client_ingress
			COMMAND "${Python3_EXECUTABLE}" -B
				"${CMAKE_SOURCE_DIR}/tests/python/test_authenticated_client_ingress.py"
				--cluster-script "${CMAKE_SOURCE_DIR}/tests/python/test_server_cluster.py"
				--probe-script "${CMAKE_CURRENT_SOURCE_DIR}/authenticated_client_probe.js"
				--node-executable "${KBE_SDK_NODE_EXECUTABLE}"
				--components "${_kbe_cluster_components_arg}"
				--repository-root "${CMAKE_SOURCE_DIR}/../.."
				--assets-root "${_kbe_test_assets}"
				--binary-root "${CMAKE_BINARY_DIR}"
				--run-root "${CMAKE_BINARY_DIR}/Testing/security-authenticated-client-$<CONFIG>"
		)
		set_tests_properties(security.authenticated_client_ingress PROPERTIES
			LABELS "security;integration;network;runtime;server;authenticated"
			PASS_REGULAR_EXPRESSION "SECURITY_AUTHENTICATED_CLIENT_INGRESS_PASS"
			RUN_SERIAL TRUE
			TIMEOUT 240
		)

		add_test(NAME security.authenticated_cell_rpc_relationship
			COMMAND "${Python3_EXECUTABLE}" -B
				"${CMAKE_SOURCE_DIR}/tests/python/test_authenticated_cell_rpc.py"
				--cluster-script "${CMAKE_SOURCE_DIR}/tests/python/test_server_cluster.py"
				--probe-script "${CMAKE_CURRENT_SOURCE_DIR}/authenticated_cell_rpc_probe.js"
				--resource-overlay "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cell-rpc"
				--node-executable "${KBE_SDK_NODE_EXECUTABLE}"
				--components "${_kbe_cluster_components_arg}"
				--repository-root "${CMAKE_SOURCE_DIR}/../.."
				--assets-root "${_kbe_test_assets}"
				--binary-root "${CMAKE_BINARY_DIR}"
				--run-root "${CMAKE_BINARY_DIR}/Testing/security-authenticated-cell-rpc-$<CONFIG>"
		)
		set_tests_properties(security.authenticated_cell_rpc_relationship PROPERTIES
			LABELS "security;integration;network;runtime;server;authenticated;cell-rpc"
			PASS_REGULAR_EXPRESSION "SECURITY_AUTHENTICATED_CELL_RPC_PASS"
			RUN_SERIAL TRUE
			TIMEOUT 300
		)
	endif()
endif()
