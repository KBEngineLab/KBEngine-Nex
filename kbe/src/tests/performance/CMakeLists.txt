find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(NAME performance.metrics_contract
    COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/test_metrics_contract.py"
)
set_tests_properties(performance.metrics_contract PROPERTIES
    LABELS "performance;portable;python;unit"
    ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}"
    PASS_REGULAR_EXPRESSION "PERFORMANCE_METRICS_CONTRACT_TEST_PASS"
)
