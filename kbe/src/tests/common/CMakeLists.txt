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

add_custom_target(kbe_common_tests DEPENDS kbe_test_common_pooling)
set_target_properties(kbe_common_tests PROPERTIES FOLDER "Tests/Common")
