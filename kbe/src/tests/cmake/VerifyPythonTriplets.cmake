if(NOT DEFINED KBE_CMAKE_ROOT OR NOT EXISTS "${KBE_CMAKE_ROOT}/KbePythonTriplet.cmake")
    message(FATAL_ERROR "KBE_CMAKE_ROOT must contain KbePythonTriplet.cmake")
endif()

include("${KBE_CMAKE_ROOT}/KbePythonTriplet.cmake")

if(KBE_TEST_UNSUPPORTED_UNIVERSAL_CHILD)
    # 一份动态 Python 不能同时承载 x64 与 arm64 扩展，预期此调用明确失败。
    # One dynamic Python runtime cannot host both x64 and arm64 extensions, so this call is expected to fail explicitly.
    kbe_resolve_python_triplet(_kbe_universal_triplet "Darwin" "arm64" "" "arm64;x86_64")
    message(FATAL_ERROR "Universal macOS Python triplet was unexpectedly accepted: ${_kbe_universal_triplet}")
endif()

if(KBE_TEST_UNSUPPORTED_UNIVERSAL)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DKBE_CMAKE_ROOT=${KBE_CMAKE_ROOT}"
            -DKBE_TEST_UNSUPPORTED_UNIVERSAL_CHILD=ON
            -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE _kbe_universal_result
        OUTPUT_VARIABLE _kbe_universal_stdout
        ERROR_VARIABLE _kbe_universal_stderr
    )
    set(_kbe_universal_output "${_kbe_universal_stdout}\n${_kbe_universal_stderr}")
    if(_kbe_universal_result EQUAL 0 OR
       NOT _kbe_universal_output MATCHES "Embedded Python requires one macOS architecture per build")
        message(FATAL_ERROR
            "Universal macOS rejection did not fail with the required diagnostic.\n${_kbe_universal_output}"
        )
    endif()
    message(STATUS "Verified that universal macOS Python builds are rejected with an actionable diagnostic")
    return()
endif()

function(kbe_assert_python_triplet expected system_name system_processor vs_platform osx_architectures)
    kbe_resolve_python_triplet(
        _kbe_actual
        "${system_name}"
        "${system_processor}"
        "${vs_platform}"
        "${osx_architectures}"
    )
    if(NOT _kbe_actual STREQUAL expected)
        message(FATAL_ERROR
            "Python triplet mismatch for ${system_name}/${system_processor}/${vs_platform}/${osx_architectures}: "
            "expected ${expected}, received ${_kbe_actual}"
        )
    endif()
endfunction()

kbe_assert_python_triplet("x64-windows" "Windows" "AMD64" "x64" "")
kbe_assert_python_triplet("arm64-windows" "Windows" "AMD64" "ARM64" "")
kbe_assert_python_triplet("x64-linux" "Linux" "x86_64" "" "")
kbe_assert_python_triplet("arm64-linux" "Linux" "aarch64" "" "")
kbe_assert_python_triplet("x64-osx" "Darwin" "arm64" "" "x86_64")
kbe_assert_python_triplet("arm64-osx" "Darwin" "x86_64" "" "arm64")

message(STATUS "Verified Python vcpkg triplet selection for Windows, Linux, and macOS x64/arm64")
