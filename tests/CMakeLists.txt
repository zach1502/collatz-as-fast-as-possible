include(FetchContent)

# doctest 2.4.11 (commit ae7a13539fb71f270b87eb2e874fbac80bc8dda2), pinned by the
# SHA-256 of its release archive. An installed doctest 2.4.11 or newer 2.x that
# find_package() finds is used instead (FIND_PACKAGE_ARGS), so only a machine
# without one needs the network to configure;
# -DFETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER forces the pinned archive, and
# -DCOLLATZ_BUILD_TESTS=OFF skips doctest. SYSTEM makes doctest's headers system
# includes, so the strict -Werror flags of the test targets (newer clang warns
# about doctest's __COUNTER__ under -Wpedantic) stay on first-party code.
FetchContent_Declare(
    doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
    URL_HASH SHA256=632ed2c05a7f53fa961381497bf8069093f0d6628c5f26286161fbd32a560186
    SYSTEM
    FIND_PACKAGE_ARGS 2.4.11)
# doctest v2.4.11 declares a cmake_minimum_required older than CMake 4 accepts;
# this setting lets the subproject configure.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
# `cmake --install` installs the collatz executable only, not doctest. A cache
# entry, because doctest's option() would drop a normal variable under the old
# policy version it declares.
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(doctest)

# Optional from-scratch reference implementation (ref/) for property and
# differential tests. Built only when ref/*.cpp exist, so the tree can ship
# without it; headers live under ref/include/. When present it is linked into
# every test target.
file(GLOB REF_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/ref/*.cpp")
if(REF_SOURCES)
    add_library(collatz_ref ${REF_SOURCES})
    target_include_directories(collatz_ref
        PUBLIC "${CMAKE_SOURCE_DIR}/ref/include")
    target_link_libraries(collatz_ref PUBLIC collatz_core)
    collatz_apply_strict_flags(collatz_ref)
endif()

# Layout (CONTRIBUTING.md): tests/<dir>/test_<name>.cpp is the unit test of
# src/<dir>/<name>.cpp (or of a header-only include/collatz/<name>.hpp, or of a
# piece or topic of a split module), and tests/integration/test_<scenario>.cpp
# holds the tests that span several files. tests/support/ holds the shared
# helpers. Each test file is its own executable, test_<dir>_<name>, linking
# doctest::doctest and collatz_core (and collatz_ref when present), with main
# provided per TU by DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN; an executable with more
# cases gets further source files with target_sources() where it is registered.

# scale.hpp picks full or reduced sweep sizes from COLLATZ_SANITIZED, which is
# derived from the compile flags (`-fsanitize=...` in CMAKE_CXX_FLAGS), so tests
# shrink heavy sweeps under sanitizers with no per-preset setting. scale.hpp also
# falls back to the compilers' sanitizer macros.
set(COLLATZ_TESTS_SANITIZED OFF)
if(CMAKE_CXX_FLAGS MATCHES "fsanitize")
    set(COLLATZ_TESTS_SANITIZED ON)
endif()

# Every TIMEOUT in this file and the register_*.cmake files is a budget for the
# windows-release preset. An -O0 build with ASan and UBSan runs the CPU-bound
# dip and certificate builds about 11 times slower.
# COLLATZ_TEST_TIMEOUT_SCALE multiplies every TIMEOUT: 1 in release, 4 under
# sanitizers. Budgets stay bounded (at most 450 * 4 = 1800 s) so a deadlock in a
# sanitizer build still fails the test. collatz_scaled_timeout() applies the
# scale in one place.
set(COLLATZ_TEST_TIMEOUT_SCALE 1)
if(COLLATZ_TESTS_SANITIZED)
    set(COLLATZ_TEST_TIMEOUT_SCALE 4)
endif()

# Set <base> seconds * COLLATZ_TEST_TIMEOUT_SCALE as ${out_var} in the caller.
function(collatz_scaled_timeout base out_var)
    math(EXPR _scaled "${base} * ${COLLATZ_TEST_TIMEOUT_SCALE}")
    set(${out_var} ${_scaled} PARENT_SCOPE)
endfunction()

# collatz_add_test(<file> LABEL <label> [TIMEOUT <s>] [PROCESSORS <n>] [GPU] [BVCACHE]
#                  [OPENCL] [SHARDS <suffix>=<pattern>...])
#
# Builds tests/<file> as the executable test_<dir>_<name> (core/test_step2.cpp is
# test_core_step2) and registers it under the CTest label LABEL.
#
# The budget is the label's (unit 10 s, property 60 s) unless TIMEOUT
# is given, scaled by COLLATZ_TEST_TIMEOUT_SCALE.
# A test that blows its budget is either too heavy for its tier or hung.
# GPU takes the "gpu" resource lock, BVCACHE requires the bvcache fixture
# (register_integration_cpu_b.cmake), PROCESSORS is the `ctest -j` hint for
# tests that fan out to hardware_threads(), and OPENCL links the GPU runtime
# (collatz_gpu), so only a build with that backend registers such a test.
#
# SHARDS: `ctest -j` wall is bounded by the longest single ctest entry, and a
# doctest executable runs its TEST_CASEs serially, so a heavy file can be
# registered as one entry per doctest `--test-case=<pattern>` (doctest wildcards
# `*` and `?`; a comma separates patterns) plus <target>.rest, which excludes
# every pattern, so a TEST_CASE added later (or one no pattern matches) still
# runs and the bare binary still runs all of them. A shard also runs in its own
# process, which isolates the cases that set environment knobs. Entry names are
# <target>.<suffix>; `ctest -R <target>` selects them all.
function(collatz_add_test file)
    cmake_parse_arguments(T "GPU;BVCACHE;OPENCL"
        "LABEL;TIMEOUT;PROCESSORS" "SHARDS" ${ARGN})
    get_filename_component(_dir ${file} DIRECTORY)
    get_filename_component(_stem ${file} NAME_WE)
    string(REGEX REPLACE "^test_" "test_${_dir}_" _target ${_stem})
    add_executable(${_target} ${file})
    target_link_libraries(${_target} PRIVATE collatz_core doctest::doctest)
    if(TARGET collatz_ref)
        target_link_libraries(${_target} PRIVATE collatz_ref)
    endif()
    target_include_directories(${_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support")
    if(_dir STREQUAL "gpu" OR _dir STREQUAL "integration")
        # The GPU tests include src/gpu headers (host_pipeline.hpp, tail_defaults.hpp)
        # that need no OpenCL.
        target_include_directories(${_target} PRIVATE "${PROJECT_SOURCE_DIR}/src/gpu")
    endif()
    collatz_apply_strict_flags(${_target})
    if(COLLATZ_TESTS_SANITIZED)
        target_compile_definitions(${_target} PRIVATE COLLATZ_SANITIZED=1)
    endif()
    if(T_OPENCL)
        target_link_libraries(${_target} PRIVATE collatz_gpu)
    endif()

    if(NOT T_SHARDS)
        add_test(NAME ${_target} COMMAND ${_target})
        set(_entries ${_target})
    else()
        set(_entries)
        set(_patterns)
        foreach(_spec ${T_SHARDS})
            string(FIND "${_spec}" "=" _eq)
            string(SUBSTRING "${_spec}" 0 ${_eq} _suffix)
            math(EXPR _pstart "${_eq} + 1")
            string(SUBSTRING "${_spec}" ${_pstart} -1 _pattern)
            add_test(NAME ${_target}.${_suffix} COMMAND ${_target} "--test-case=${_pattern}")
            list(APPEND _entries ${_target}.${_suffix})
            list(APPEND _patterns "${_pattern}")
        endforeach()
        list(JOIN _patterns "," _excl)
        add_test(NAME ${_target}.rest COMMAND ${_target} "--test-case-exclude=${_excl}")
        list(APPEND _entries ${_target}.rest)
    endif()

    if(DEFINED T_TIMEOUT)
        set(_timeout ${T_TIMEOUT})
    elseif(T_LABEL STREQUAL "property")
        set(_timeout 60)
    else()
        set(_timeout 10)
    endif()
    collatz_scaled_timeout(${_timeout} _timeout)
    # One shared on-disk bv cache dir for the whole test run, separate from the
    # developer's %LOCALAPPDATA%\collatz. The 2^B dip table is keyed by B and the
    # window-term tag only, so once one test (or the bvcache fixture of
    # register_integration_cpu_b.cmake) builds dip_B24, every other test loads
    # it instead of re-walking 2^24. Cache writes are temp-file + atomic rename
    # (step2_cache_write.cpp), so sharing the dir across `ctest -j` is race-safe.
    set_tests_properties(${_entries} PROPERTIES
        LABELS ${T_LABEL}
        TIMEOUT ${_timeout}
        ENVIRONMENT "COLLATZ_CACHE_DIR=${CMAKE_CURRENT_BINARY_DIR}/.bvcache")
    # GPU tests share one physical device; two running at once oversubscribe it
    # and can collide. RESOURCE_LOCK "gpu" makes CTest never schedule any two of
    # them concurrently, while still overlapping them with the CPU-only tests
    # (unlike RUN_SERIAL, which would block the whole run). A GPU test exits 77
    # when a case could not run on a device (tests/support/gpu_test_main.hpp),
    # and CTest reports that entry as skipped rather than passed.
    if(T_GPU)
        set_tests_properties(${_entries} PROPERTIES RESOURCE_LOCK gpu SKIP_RETURN_CODE 77)
    endif()
    if(T_BVCACHE)
        set_tests_properties(${_entries} PROPERTIES FIXTURES_REQUIRED bvcache)
    endif()
    # The value (>= half the machine) caps how many heavy tests co-schedule; light
    # single-threaded tests still fill the gaps.
    if(DEFINED T_PROCESSORS)
        set_tests_properties(${_entries} PROPERTIES PROCESSORS ${T_PROCESSORS})
    endif()
endfunction()

# The tests are registered in one file per group. The include order is the
# registration order and so fixes the test numbers that `ctest -N` shows.
include(${CMAKE_CURRENT_SOURCE_DIR}/register_unit.cmake)

# Integration tests, one file per scenario, each registered with its label.
include(${CMAKE_CURRENT_SOURCE_DIR}/register_integration_cpu_a.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/register_integration_gpu.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/register_integration_cpu_b.cmake)

# The git commit stamp (cmake/GitCommitStamp.cmake), as CMake scripts:
# test_integration_git_commit_stamp checks the value WriteGitCommitStamp.cmake writes
# for scratch repositories, worktrees and tarballs, with and without git, and
# test_integration_git_commit_stamp_build builds a small project, which must show
# each edit, checkout and commit at its next build without a reconfigure. Both are
# skipped without git.
collatz_scaled_timeout(60 _stamp_timeout)
add_test(NAME test_integration_git_commit_stamp
    COMMAND ${CMAKE_COMMAND} "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DSCRIPT=${PROJECT_SOURCE_DIR}/cmake/WriteGitCommitStamp.cmake"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/git_commit_stamp"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/integration/git_commit_stamp.cmake")
add_test(NAME test_integration_git_commit_stamp_build
    COMMAND ${CMAKE_COMMAND} "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DMODULE_DIR=${PROJECT_SOURCE_DIR}/cmake"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/git_commit_stamp_build"
        "-DGENERATOR=${CMAKE_GENERATOR}" "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DCXX=${CMAKE_CXX_COMPILER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/integration/git_commit_stamp_build.cmake")
set_tests_properties(test_integration_git_commit_stamp test_integration_git_commit_stamp_build
    PROPERTIES LABELS unit TIMEOUT ${_stamp_timeout}
        SKIP_REGULAR_EXPRESSION "git is not available; skipped")

# `cmake --install` of this build tree into a scratch prefix installs exactly the
# collatz executable (integration/install_layout.cmake).
add_test(NAME test_integration_install_layout
    COMMAND ${CMAKE_COMMAND} "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
        "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/install_layout"
        "-DEXPECTED=bin/collatz${CMAKE_EXECUTABLE_SUFFIX}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/integration/install_layout.cmake")
set_tests_properties(test_integration_install_layout PROPERTIES LABELS unit TIMEOUT ${_stamp_timeout})

# `quick` label: the inner-loop subset of the unit and property tests
# (`ctest -L quick`). It is added on top of the unit/property label, so a
# selection by those labels still runs every test. `quick` is every
# unit|property entry except GPU tests (name matches "gpu": serialized on one
# device and timing-sensitive under load) and the heaviest CPU tests, including
# their shards. It is computed from the registered tests, so a new test file
# joins it automatically. This block must stay last so it sees every add_test()
# above.
set(_quick_excluded_re
    "^(test_integration_step2_wide_a|test_integration_oracle|test_integration_worker_e2e)(\\.|$)")
get_property(_all_tests DIRECTORY PROPERTY TESTS)
foreach(t ${_all_tests})
    if(t MATCHES "gpu" OR t MATCHES "${_quick_excluded_re}")
        continue()
    endif()
    get_test_property(${t} LABELS _labels)
    if("unit" IN_LIST _labels OR "property" IN_LIST _labels)
        set_tests_properties(${t} PROPERTIES LABELS "${_labels};quick")
    endif()
endforeach()

# Style checks (label lint), defined in tools/lint/CMakeLists.txt.
add_subdirectory(${PROJECT_SOURCE_DIR}/tools/lint ${CMAKE_CURRENT_BINARY_DIR}/lint)
