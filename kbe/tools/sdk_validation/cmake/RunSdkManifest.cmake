foreach(_kbe_required_variable IN ITEMS
        KBE_PWSH KBE_RUNNER KBE_MANIFEST KBE_RESULTS KBE_OWNED_ROOT)
    if(NOT DEFINED ${_kbe_required_variable} OR "${${_kbe_required_variable}}" STREQUAL "")
        message(FATAL_ERROR "Missing required SDK manifest runner variable: ${_kbe_required_variable}")
    endif()
endforeach()

# 递归清理前验证严格子目录关系，避免错误缓存值把删除范围扩展到 Testing 根或调用方目录。
# Validate a strict child relationship before recursive cleanup so a bad cache value cannot widen deletion to the Testing root or caller-owned paths.
cmake_path(ABSOLUTE_PATH KBE_OWNED_ROOT NORMALIZE OUTPUT_VARIABLE _kbe_owned_root)
cmake_path(ABSOLUTE_PATH KBE_RESULTS NORMALIZE OUTPUT_VARIABLE _kbe_results)
cmake_path(IS_PREFIX _kbe_owned_root "${_kbe_results}" NORMALIZE _kbe_is_owned)
if(NOT _kbe_is_owned OR _kbe_results STREQUAL _kbe_owned_root)
    message(FATAL_ERROR
        "Refusing to clear SDK results outside the owned Testing subtree: ${_kbe_results}")
endif()

# 每次执行只清理本测试拥有的结果目录，防止上一次日志混入新的验收摘要。
# Each run clears only the result directory owned by this test so stale logs cannot enter the new validation summary.
file(REMOVE_RECURSE "${_kbe_results}")
execute_process(
    COMMAND "${KBE_PWSH}"
        -NoLogo
        -NoProfile
        -NonInteractive
        -File "${KBE_RUNNER}"
        -ManifestPath "${KBE_MANIFEST}"
        -ResultsPath "${_kbe_results}"
    RESULT_VARIABLE _kbe_result
    COMMAND_ECHO STDOUT
)
if(NOT _kbe_result EQUAL 0)
    message(FATAL_ERROR "SDK manifest validation failed with exit code ${_kbe_result}")
endif()
