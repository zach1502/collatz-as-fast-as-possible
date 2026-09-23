# Registers the unit tests: test_*.cpp in core/, cli/, gpu/ and ref/.

# Unit test files that need more than the unit defaults are listed here; the glob
# below registers every other file in core/, cli/, gpu/ and ref/, so a new unit
# test file needs no CMake edit.
collatz_add_test(core/test_checkpoint_dir.cpp LABEL unit TIMEOUT 60)
collatz_add_test(core/test_checkpoint_fingerprint.cpp LABEL unit TIMEOUT 60)
# The env-knob classification test scans src/ and include/ for COLLATZ_* names.
target_compile_definitions(test_core_checkpoint_fingerprint
    PRIVATE COLLATZ_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
collatz_add_test(core/test_checkpoint_json.cpp LABEL unit TIMEOUT 60)
collatz_add_test(core/test_checkpoint_merge.cpp LABEL unit TIMEOUT 60)
collatz_add_test(core/test_checkpoint_params.cpp LABEL unit TIMEOUT 60)
collatz_add_test(core/test_checkpoint_run.cpp LABEL unit TIMEOUT 60)
collatz_add_test(core/test_frontier.cpp LABEL unit TIMEOUT 60)
collatz_add_test(core/test_step1_dfs.cpp LABEL unit PROCESSORS 4)
collatz_add_test(core/test_step1_mod3t.cpp LABEL unit PROCESSORS 4)
collatz_add_test(core/test_step1_sieve.cpp LABEL unit PROCESSORS 4)
# Step 2 builds B=24 filters (the 2^24 dip from the bvcache fixture, or a fresh
# in-process build that fans out to hardware_threads()), and the builder groups
# (class-major cold build, two-build cache files) each make several fresh B<=20
# builds and point COLLATZ_CACHE_DIR at a private directory, so each group runs in
# its own process: each group is one file, and the B=24 mask and soundness cases
# are shards of their files.
collatz_add_test(core/test_step2.cpp LABEL unit BVCACHE TIMEOUT 120 PROCESSORS 4
    SHARDS "monotone=B-window mask is monotone*")
collatz_add_test(core/test_step2_builders.cpp LABEL unit BVCACHE TIMEOUT 120 PROCESSORS 4)
collatz_add_test(core/test_step2_candidates.cpp LABEL unit BVCACHE TIMEOUT 120 PROCESSORS 4)
collatz_add_test(core/test_step2_coldbv.cpp LABEL unit BVCACHE TIMEOUT 120 PROCESSORS 4)
collatz_add_test(core/test_step2_dip.cpp LABEL unit BVCACHE TIMEOUT 120 PROCESSORS 4)
collatz_add_test(core/test_step2_soundness.cpp LABEL unit BVCACHE TIMEOUT 120 PROCESSORS 4
    SHARDS "sound=Theorem 4.4 soundness*")
# The GMP block replays are slow under sanitizers.
collatz_add_test(core/test_step3_blocks.cpp LABEL unit TIMEOUT 120)
collatz_add_test(core/test_step3_defer.cpp LABEL unit TIMEOUT 120)
collatz_add_test(core/test_step3_gate_fold.cpp LABEL unit TIMEOUT 120)
collatz_add_test(core/test_step3_lanes.cpp LABEL unit TIMEOUT 120)
collatz_add_test(core/test_step3_persist.cpp LABEL unit TIMEOUT 120)
collatz_add_test(core/test_step3_u64lanes.cpp LABEL unit TIMEOUT 120)
# verify::run at B=24 with use_bv_cache on loads the shared .bvcache; on a cold
# cache (a fresh build tree) the dip build fans out to hardware_threads(), and
# several at once stall a 4-core machine past the 10 s unit default.
collatz_add_test(core/test_verify_block.cpp LABEL unit BVCACHE TIMEOUT 120)
collatz_add_test(core/test_verify_bounds.cpp LABEL unit BVCACHE TIMEOUT 120)
collatz_add_test(core/test_verify_defaults.cpp LABEL unit BVCACHE TIMEOUT 120)
# A few hundred small blocks, each walked member by member.
collatz_add_test(core/test_verify_excursion.cpp LABEL unit TIMEOUT 60)
# Tests the record fold's internal headers in src/core.
collatz_add_test(core/test_verify_fold.cpp LABEL unit)
target_include_directories(test_core_verify_fold PRIVATE "${PROJECT_SOURCE_DIR}/src/core")
collatz_add_test(core/test_verify_run.cpp LABEL unit BVCACHE TIMEOUT 120)
collatz_add_test(core/test_worker.cpp LABEL unit)
# The digest test reads the server's fixtures.
target_compile_definitions(test_core_worker PRIVATE COLLATZ_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# The GPU tests are bound by device latency, so under contention from other
# `--gpu` processes the 10 s unit default times out on small, correct work; 60 s
# still fails a hung queue within a minute.
collatz_add_test(gpu/test_u128_limbs.cpp LABEL unit GPU TIMEOUT 60)
# Its cases are spread over these files of the same executable; they are not
# test_*.cpp files, so the glob below does not register them on their own.
target_sources(test_gpu_u128_limbs PRIVATE
    gpu/test_u128_limbs/u128_limbs_02_u96_tail.cpp
    gpu/test_u128_limbs/u128_limbs_03_slot_loop.cpp
    gpu/test_u128_limbs/u128_limbs_04_folds_digests.cpp
    gpu/test_u128_limbs/u128_limbs_05_knob_helpers.cpp
    gpu/test_u128_limbs/u128_limbs_06_lane_n86.cpp)

file(GLOB _unit_tests CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/test_*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/cli/test_*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/gpu/test_*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ref/test_*.cpp")
foreach(_file ${_unit_tests})
    get_filename_component(_dir ${_file} DIRECTORY)
    get_filename_component(_stem ${_file} NAME_WE)
    string(REGEX REPLACE "^test_" "test_${_dir}_" _target ${_stem})
    if(NOT TARGET ${_target})
        collatz_add_test(${_file} LABEL unit)
    endif()
endforeach()
