# Strict warnings-as-errors flags, applied only to first-party targets through
# collatz_apply_strict_flags(<target>). FetchContent dependencies (doctest) do
# not get them, and doctest's headers are SYSTEM includes (tests/CMakeLists.txt),
# so its warnings cannot break the build, even where a test includes them.

include(CheckCXXCompilerFlag)
include(CheckIPOSupported)

# GCC and Clang accept every flag in this set.
set(COLLATZ_STRICT_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wcast-qual
    -Wformat=2
    -Wundef
    -Wdouble-promotion
    -Wimplicit-fallthrough
    -Wvla
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Werror)

# Native architecture flag, chosen by target architecture rather than by
# compiler and never naming a CPU. ARM and AArch64 use -mcpu=native. x86 uses
# -march=native (native ISA and tuning), because GCC deprecates -mcpu on x86,
# warns, and reduces it to tuning only. A compiler that rejects the flag builds
# without it, and an unknown architecture gets no native flag.
set(COLLATZ_NATIVE_FLAGS "")
if(COLLATZ_NATIVE)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _collatz_arch)
    if(_collatz_arch MATCHES "^(arm64|aarch64)$")
        set(_collatz_native_candidate "-mcpu=native")
    elseif(_collatz_arch MATCHES "^(x86_64|amd64|i686)$")
        set(_collatz_native_candidate "-march=native")
    else()
        set(_collatz_native_candidate "")
    endif()
    if(_collatz_native_candidate)
        check_cxx_compiler_flag("${_collatz_native_candidate}" COLLATZ_HAS_NATIVE_FLAG)
        if(COLLATZ_HAS_NATIVE_FLAG)
            set(COLLATZ_NATIVE_FLAGS "${_collatz_native_candidate}")
        endif()
    endif()
endif()

set(COLLATZ_LTO_SUPPORTED OFF)
if(COLLATZ_LTO)
    check_ipo_supported(RESULT COLLATZ_LTO_SUPPORTED OUTPUT _collatz_lto_msg)
    if(NOT COLLATZ_LTO_SUPPORTED)
        message(STATUS "COLLATZ_LTO requested but unsupported: ${_collatz_lto_msg}")
    endif()
endif()

# Thin-LTO cache. Every first-party executable LTO-links collatz_core, so
# without a cache an edit to one file in src/core/ re-optimizes all of
# collatz_core's modules in every link. Clang's thin-LTO backend caches each
# module's object under a key of module hash, imports and export information
# and reuses it while the key is unchanged; the linked binaries are
# byte-identical with or without the cache.
option(COLLATZ_LTO_CACHE "Cache thin-LTO backend objects across links (Clang)" ON)
set(COLLATZ_LTO_CACHE_LINK_OPTIONS "")
if(COLLATZ_LTO AND COLLATZ_LTO_SUPPORTED AND COLLATZ_LTO_CACHE
        AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    include(CheckLinkerFlag)
    set(_collatz_lto_cache_dir "${PROJECT_BINARY_DIR}/lto-cache")
    check_linker_flag(CXX "-Wl,--thinlto-cache-dir=${_collatz_lto_cache_dir}"
        COLLATZ_HAS_LLD_LTO_CACHE)
    if(COLLATZ_HAS_LLD_LTO_CACHE)
        set(COLLATZ_LTO_CACHE_LINK_OPTIONS
            "-Wl,--thinlto-cache-dir=${_collatz_lto_cache_dir}"
            "-Wl,--thinlto-cache-policy=prune_after=168h:cache_size_bytes=2g")
    endif()
    if(COLLATZ_LTO_CACHE_LINK_OPTIONS)
        message(STATUS "COLLATZ_LTO_CACHE: ON (${_collatz_lto_cache_dir})")
    else()
        message(STATUS "COLLATZ_LTO_CACHE: linker has no thin-LTO cache flag; OFF")
    endif()
endif()

# Adds the strict warnings, the native architecture flag and LTO to a
# first-party target.
function(collatz_apply_strict_flags target)
    target_compile_options(${target} PRIVATE ${COLLATZ_STRICT_WARNINGS})
    if(COLLATZ_NATIVE_FLAGS)
        target_compile_options(${target} PRIVATE ${COLLATZ_NATIVE_FLAGS})
    endif()
    if(COLLATZ_LTO AND COLLATZ_LTO_SUPPORTED)
        set_property(TARGET ${target}
            PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        # Only the link steps of executables run the LTO backend, so only
        # executables get the cache flags. $<CONFIG:Release> matches the IPO
        # property above, so other configurations never see them.
        get_target_property(_collatz_type ${target} TYPE)
        if(COLLATZ_LTO_CACHE_LINK_OPTIONS AND _collatz_type STREQUAL "EXECUTABLE")
            target_link_options(${target} PRIVATE
                "$<$<CONFIG:Release>:${COLLATZ_LTO_CACHE_LINK_OPTIONS}>")
        endif()
    endif()
endfunction()
