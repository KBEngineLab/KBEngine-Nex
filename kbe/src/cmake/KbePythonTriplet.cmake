include_guard(GLOBAL)

# VS 目标平台与单一 macOS 目标架构优先于主机处理器，保证交叉构建选择正确的扩展 ABI。
# The VS target platform and a single macOS target architecture override the host processor so cross-builds select the correct extension ABI.
function(kbe_resolve_python_triplet output_variable system_name system_processor vs_platform osx_architectures)
    set(_kbe_effective_processor "${system_processor}")

    if(system_name STREQUAL "Darwin" AND osx_architectures)
        set(_kbe_osx_architectures ${osx_architectures})
        list(LENGTH _kbe_osx_architectures _kbe_osx_architecture_count)
        if(NOT _kbe_osx_architecture_count EQUAL 1)
            message(FATAL_ERROR
                "Embedded Python requires one macOS architecture per build; "
                "universal architectures '${osx_architectures}' need separate x64 and arm64 build trees."
            )
        endif()
        list(GET _kbe_osx_architectures 0 _kbe_effective_processor)
    elseif(system_name STREQUAL "Windows" AND vs_platform)
        set(_kbe_effective_processor "${vs_platform}")
    endif()

    string(TOLOWER "${_kbe_effective_processor}" _kbe_python_processor)
    if(_kbe_python_processor MATCHES "^(arm64|aarch64)$")
        set(_kbe_python_arch "arm64")
    elseif(_kbe_python_processor MATCHES "^(amd64|x86_64|x64)$")
        set(_kbe_python_arch "x64")
    else()
        message(FATAL_ERROR
            "Unsupported Python architecture '${_kbe_effective_processor}' "
            "for ${system_name}; set KBE_PYTHON_TRIPLET explicitly only when a matching vcpkg triplet exists."
        )
    endif()

    if(system_name STREQUAL "Windows")
        set(_kbe_python_platform "windows")
    elseif(system_name STREQUAL "Darwin")
        set(_kbe_python_platform "osx")
    elseif(system_name STREQUAL "Linux")
        set(_kbe_python_platform "linux")
    else()
        message(FATAL_ERROR "Unsupported Python platform: ${system_name}")
    endif()

    set(${output_variable} "${_kbe_python_arch}-${_kbe_python_platform}" PARENT_SCOPE)
endfunction()
