include_guard(GLOBAL)

# 所有目标通过接口库继承同一组 ABI、字符集和公共头文件设置，避免子目录各自漂移。
# Every target inherits one ABI, character-set, and public-include policy through an interface library so subdirectories cannot drift independently.
add_library(kbe_build_options INTERFACE)
add_library(KBE::BuildOptions ALIAS kbe_build_options)

target_include_directories(kbe_build_options INTERFACE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/lib"
    "${CMAKE_SOURCE_DIR}/server"
    "${CMAKE_SOURCE_DIR}/lib/dependencies"
    "${CMAKE_SOURCE_DIR}/lib/dependencies/g3dlite"
)

target_compile_definitions(kbe_build_options INTERFACE
    ENABLE_WATCHERS
    KBE_USE_ASSERTS
    LOG4CXX_STATIC
    USE_OPENSSL
    CODE_INLINE
    $<$<PLATFORM_ID:Windows>:WIN32>
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
)

if(MSVC)
    # 与既有 VS 工程保持 /MT、/MTd ABI，并显式使用 UTF-8 源码和执行字符集。
    # Match the existing VS projects' /MT and /MTd ABI while explicitly using UTF-8 source and execution character sets.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "MSVC runtime" FORCE)
    target_compile_definitions(kbe_build_options INTERFACE UNICODE _UNICODE)
    # /FS 让同一目标的并行编译通过 mspdbsrv 串行写 PDB，避免首次完整构建出现 C1041。
    # /FS serializes parallel PDB writes through mspdbsrv, preventing C1041 during the first complete build.
    target_compile_options(kbe_build_options INTERFACE /utf-8 /W3 /FS $<$<COMPILE_LANGUAGE:CXX>:/permissive->)
else()
    # 分配器选择发生在 common 编译期，头文件、宏和库必须通过公共 ABI 接口同时传播。
    # Allocator selection happens while compiling common, so its header, macro, and library must travel together through the shared ABI interface.
    find_path(KBE_JEMALLOC_INCLUDE_DIR NAMES jemalloc/jemalloc.h REQUIRED)
    find_library(KBE_JEMALLOC_LIBRARY NAMES jemalloc REQUIRED)
    target_include_directories(kbe_build_options INTERFACE "${KBE_JEMALLOC_INCLUDE_DIR}")
    target_compile_definitions(kbe_build_options INTERFACE USE_JEMALLOC)
    target_link_libraries(kbe_build_options INTERFACE "${KBE_JEMALLOC_LIBRARY}")
    target_compile_options(kbe_build_options INTERFACE -Wall)
endif()

function(kbe_configure_target target_name)
    target_link_libraries(${target_name} PUBLIC KBE::BuildOptions)
    set_target_properties(${target_name} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${KBE_CMAKE_OUTPUT_ROOT}/lib"
        LIBRARY_OUTPUT_DIRECTORY "${KBE_CMAKE_OUTPUT_ROOT}/lib"
        RUNTIME_OUTPUT_DIRECTORY "${KBE_CMAKE_OUTPUT_ROOT}/bin"
        DEBUG_POSTFIX "_d"
    )
endfunction()
