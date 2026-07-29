foreach(_kbe_required_variable IN ITEMS
        KBE_PWSH KBE_SCRIPT KBE_ASSETS KBE_OUTPUT KBE_OWNED_ROOT
        KBE_KBCMD KBE_MSBUILD KBE_GODOT KBE_TYPESCRIPT)
    if(NOT DEFINED ${_kbe_required_variable} OR "${${_kbe_required_variable}}" STREQUAL "")
        message(FATAL_ERROR "Missing required Windows SDK candidate variable: ${_kbe_required_variable}")
    endif()
endforeach()

# 递归清理前验证严格子目录关系，避免错误缓存值把删除范围扩展到 Testing 根或调用方发布目录。
# Validate a strict child relationship before recursive cleanup so a bad cache value cannot widen deletion to the Testing root or caller release paths.
cmake_path(ABSOLUTE_PATH KBE_OWNED_ROOT NORMALIZE OUTPUT_VARIABLE _kbe_owned_root)
cmake_path(ABSOLUTE_PATH KBE_OUTPUT NORMALIZE OUTPUT_VARIABLE _kbe_output)
cmake_path(IS_PREFIX _kbe_owned_root "${_kbe_output}" NORMALIZE _kbe_is_owned)
if(NOT _kbe_is_owned OR _kbe_output STREQUAL _kbe_owned_root)
    message(FATAL_ERROR
        "Refusing to clear SDK candidate output outside the owned Testing subtree: ${_kbe_output}")
endif()

# 候选生成器拒绝非空目录；这里只删除 CMake Testing 下由本测试独占的目录，绝不触碰调用方发布目录。
# The candidate generator rejects non-empty directories; remove only this test-owned directory under CMake Testing and never a caller release directory.
file(REMOVE_RECURSE "${_kbe_output}")
execute_process(
    COMMAND "${KBE_PWSH}"
        -NoLogo
        -NoProfile
        -NonInteractive
        -File "${KBE_SCRIPT}"
        -AssetsPath "${KBE_ASSETS}"
        -OutputPath "${_kbe_output}"
        -KbcmdPath "${KBE_KBCMD}"
        -MSBuildPath "${KBE_MSBUILD}"
        -GodotPath "${KBE_GODOT}"
        -TypeScriptPath "${KBE_TYPESCRIPT}"
    RESULT_VARIABLE _kbe_result
    COMMAND_ECHO STDOUT
)
if(NOT _kbe_result EQUAL 0)
    message(FATAL_ERROR "Windows SDK release-candidate validation failed with exit code ${_kbe_result}")
endif()
