cmake_minimum_required(VERSION 3.25)

project(collatz
    VERSION 0.1.0
    DESCRIPTION "Fast, correctness-first Collatz verifier"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "collatz builds on Windows only: configure it in an MSYS2 UCRT64 shell with cmake --preset windows-release (README.md, Build).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Always emit compile_commands.json for clang-tidy / editors.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(COLLATZ_NATIVE "Enable native-arch codegen (-mcpu/-march native)" ON)
option(COLLATZ_LTO "Enable thin LTO in Release when supported" ON)
option(COLLATZ_BUILD_TESTS "Build the test suite" ON)

include(CompilerFlags)
include(EmbedKernelSource)
include(GitCommitStamp)

find_package(GMP REQUIRED)

find_package(OpenCL QUIET)
set(_collatz_opencl_default OFF)
if(OpenCL_FOUND)
    set(_collatz_opencl_default ON)
endif()
option(COLLATZ_OPENCL "Build the portable OpenCL 1.2 GPU backend" ${_collatz_opencl_default})
if(COLLATZ_OPENCL AND NOT OpenCL_FOUND)
    message(WARNING "COLLATZ_OPENCL is ON but find_package(OpenCL) failed; disabling GPU backend.")
    set(COLLATZ_OPENCL OFF)
endif()
if(COLLATZ_OPENCL)
    message(STATUS "COLLATZ_OPENCL: ON (${OpenCL_LIBRARIES})")
else()
    message(STATUS "COLLATZ_OPENCL: OFF")
endif()

# collatz_core library. 
# Sources are globbed from src/core/
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp")
add_library(collatz_core ${CORE_SOURCES})
target_include_directories(collatz_core
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(collatz_core
    PUBLIC GMP::GMP)
target_link_libraries(collatz_core PUBLIC bcrypt advapi32)
collatz_apply_strict_flags(collatz_core)

# collatz CLI executable: main.cpp plus every subcommand under src/cli/ (globbed)
file(GLOB CLI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cli/*.cpp")
add_executable(collatz src/main.cpp ${CLI_SOURCES})
target_link_libraries(collatz PRIVATE collatz_core)
collatz_apply_strict_flags(collatz)

# `cmake --install` installs the executable and nothing else: the libraries are
# static and linked into it, and tests/CMakeLists.txt keeps doctest out.
include(GNUInstallDirs)
install(TARGETS collatz RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The git commit of this tree, which `verify --json` records, the worker sends and
# --checkpoint fingerprints, as the string literal COLLATZ_GIT_COMMIT in every
# source of the executable: <sha>, <sha>-dirty, <sha>-nogit or unknown
# (cmake/WriteGitCommitStamp.cmake says when). It is computed on every build, so an
# edit, a commit or a pull shows at the next build, and a build whose value did not
# change recompiles nothing.
collatz_git_commit_stamp(collatz)

# OpenCL GPU backend (optional; src/gpu). Kernels in src/gpu/kernels/*.cl and
# the shared limb header are embedded into a generated C++ header at configure
# time (file(READ) + configure_file), so the binary carries its kernels with no
# runtime file I/O. The generated header lives in the build tree, not the source
# tree. The limb header and survivor_cpi.cl include their part files from
# u128_limbs/ and survivor_cpi/ next to them; the embed replaces each such
# include with the part's bytes, so the OpenCL compiler gets one text and host
# C++ includes the same limb parts. Editing a source or a part re-embeds it.
if(COLLATZ_OPENCL)
    set(_gpu_gen_dir "${CMAKE_BINARY_DIR}/gpu_generated")
    collatz_embed_source_expr("${CMAKE_CURRENT_SOURCE_DIR}/include/collatz/gpu/u128_limbs.h"
        COLLATZ_KERNEL_U128_LIMBS_EXPR
        PART_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include/collatz/gpu/u128_limbs"
        PART_PREFIX "collatz/gpu/u128_limbs/")
    collatz_embed_source_expr("${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/kernels/survivor_tail16.cl"
        COLLATZ_KERNEL_SURVIVOR_TAIL16_EXPR)
    collatz_embed_source_expr("${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/kernels/survivor_cpi.cl"
        COLLATZ_KERNEL_SURVIVOR_CPI_EXPR
        PART_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/kernels/survivor_cpi"
        PART_PREFIX "survivor_cpi/")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/kernel_sources.hpp.in"
        "${_gpu_gen_dir}/collatz/gpu/kernel_sources.hpp"
        @ONLY)

    file(GLOB_RECURSE GPU_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/*.cpp")
    add_library(collatz_gpu ${GPU_SOURCES})
    target_include_directories(collatz_gpu PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu"
        "${_gpu_gen_dir}")
    # The tail16 runtime uses only header-only (constexpr/inline) parts of
    # collatz_core, but the verify_gpu orchestration (src/gpu/verify_gpu.cpp)
    # calls Step 1/2/3 symbols, so collatz_gpu links collatz_core PRIVATE. Every
    # consumer (CLI exe, GPU tests) also links collatz_core. OpenCL stays PUBLIC
    # so consumers inherit the OpenCL link.
    target_link_libraries(collatz_gpu
        PUBLIC OpenCL::OpenCL
        PRIVATE collatz_core)

    target_compile_definitions(collatz_gpu PUBLIC
        CL_TARGET_OPENCL_VERSION=120
        COLLATZ_HAS_OPENCL=1)
    collatz_apply_strict_flags(collatz_gpu)

    target_link_libraries(collatz PRIVATE collatz_gpu)
endif()

if(COLLATZ_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()
