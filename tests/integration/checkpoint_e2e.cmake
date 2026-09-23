# End-to-end test of `collatz verify --checkpoint` / `collatz merge` through the
# real binary (portable: a CMake script, no shell or Python). Invoked by ctest as
#   cmake -DCOLLATZ=<path to collatz> -DWORK=<scratch dir> -P checkpoint_e2e.cmake
#
#  1. reference: an unsharded run.
#  2. a --checkpoint --slices 8 run; deleting slices 3..7 and merged.json and
#     adding a partial *.tmp file leaves what a kill after 3 slices leaves.
#  3. rerunning the same command (no --slices: adopted from DIR/run.json) skips
#     the 3 finished slices and prints the same Result scalars as step 1.
#  4. a different --mod3t into the same DIR is refused (exit 2).
#  5. two --slice-range halves in separate DIRs merge (`collatz merge`) to the
#     same Result; one half alone reports the missing slices (exit 3).
#  6. slice files verify did not write are refused by `collatz merge` (exit 2):
#     a case_k of 2^62, a B of 2^32 + 24, and a counter changed after the
#     slice was written (its result_digest no longer matches); a resume redoes
#     the changed slice and prints the reference Result.
#  7. --threads above the limit (4 per hardware thread, at least 256) and
#     --bitvectors above 32 are refused (exit 2) by verify, certify and worker.

cmake_minimum_required(VERSION 3.25)
if(NOT COLLATZ OR NOT WORK)
    message(FATAL_ERROR "need -DCOLLATZ=... -DWORK=...")
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(ARGS verify --N 30 --A 0 --threads 2)

function(run_cmd out_var rc_var)
    execute_process(COMMAND ${ARGN}
        OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
    set(${out_var} "${_out}" PARENT_SCOPE)
    set(${rc_var} "${_rc}" PARENT_SCOPE)
    set(LAST_ERR "${_err}" PARENT_SCOPE)
endfunction()

# Keep only the mergeable Result scalar lines (prefix_walks/timing excluded).
function(scalars text out_var)
    string(REGEX MATCHALL
        "(candidates_checked|step2_masked_out|step3_steps_total|coverage_checksum|pow_checksum|blocks_total|max_excursion|max_excursion_n|max_exceeds_u128|overflow_fallbacks|step1_survivors) += [^\n]*"
        _m "${text}")
    set(${out_var} "${_m}" PARENT_SCOPE)
endfunction()

run_cmd(ref rc ${COLLATZ} ${ARGS})
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "reference run failed (${rc}): ${LAST_ERR}")
endif()
scalars("${ref}" ref_s)
list(LENGTH ref_s n_ref)
if(n_ref LESS 11)
    message(FATAL_ERROR "could not parse reference output:\n${ref}")
endif()

set(D "${WORK}/ck")
run_cmd(o1 rc ${COLLATZ} ${ARGS} --checkpoint "${D}" --slices 8)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "checkpointed run failed (${rc}): ${LAST_ERR}")
endif()
foreach(_c 3 4 5 6 7)
    file(REMOVE "${D}/slice_${_c}_of_8.json")
endforeach()
file(REMOVE "${D}/merged.json")
file(WRITE "${D}/slice_3_of_8.json.tmp" "{\"schema\": \"collatz-verify-json/1\", \"res")
file(GLOB done1 "${D}/slice_*_of_8.json")
list(LENGTH done1 n1)
if(NOT n1 EQUAL 3)
    message(FATAL_ERROR "expected 3 slices after removing slices 3..7, got ${n1}")
endif()

run_cmd(o2 rc ${COLLATZ} ${ARGS} --checkpoint "${D}")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "resume failed (${rc}): ${LAST_ERR}")
endif()
if(NOT LAST_ERR MATCHES "3 done, 5 to run")
    message(FATAL_ERROR "resume did not skip the 3 finished slices:\n${LAST_ERR}")
endif()
scalars("${o2}" o2_s)
if(NOT o2_s STREQUAL ref_s)
    message(FATAL_ERROR "checkpointed Result differs from unsharded:\n${o2_s}\nvs\n${ref_s}")
endif()
if(NOT EXISTS "${D}/merged.json")
    message(FATAL_ERROR "merged.json missing")
endif()

run_cmd(o3 rc ${COLLATZ} ${ARGS} --mod3t 0 --checkpoint "${D}")
if(NOT rc EQUAL 2 OR NOT LAST_ERR MATCHES "different parameters")
    message(FATAL_ERROR "mismatched rerun was not refused (${rc}): ${LAST_ERR}")
endif()

run_cmd(oa rc ${COLLATZ} ${ARGS} --checkpoint "${WORK}/m1" --slices 8 --slice-range 0:5)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "range 0:5 failed: ${LAST_ERR}")
endif()
run_cmd(om rc ${COLLATZ} merge "${WORK}/m1")
if(NOT rc EQUAL 3 OR NOT LAST_ERR MATCHES "missing slice\\(s\\): 5\\.\\.7")
    message(FATAL_ERROR "incomplete merge not reported (${rc}): ${LAST_ERR}")
endif()
run_cmd(ob rc ${COLLATZ} ${ARGS} --checkpoint "${WORK}/m2" --slices 8 --slice-range 5:8)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "range 5:8 failed: ${LAST_ERR}")
endif()
run_cmd(om rc ${COLLATZ} merge "${WORK}/m1" "${WORK}/m2")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "merge failed (${rc}): ${LAST_ERR}")
endif()
scalars("${om}" om_s)
if(NOT om_s STREQUAL ref_s)
    message(FATAL_ERROR "merged Result differs from unsharded:\n${om_s}\nvs\n${ref_s}")
endif()

set(H "${WORK}/h")
run_cmd(oh rc ${COLLATZ} ${ARGS} --checkpoint "${H}" --slices 2)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "run for the changed-file checks failed (${rc}): ${LAST_ERR}")
endif()
file(READ "${H}/slice_1_of_2.json" good)

# Copies DIR H to WORK/<name> with the one `from` in slice 1 replaced by `to`
# (and slice 0 removed with ONLY), and expects `collatz merge` to exit 2 with
# `want` in its message.
function(merge_refuses name from to want)
    string(FIND "${good}" "${from}" at)
    string(FIND "${good}" "${from}" last REVERSE)
    if(at EQUAL -1 OR NOT at EQUAL last)
        message(FATAL_ERROR "${name}: '${from}' is not in slice 1 exactly once")
    endif()
    string(REPLACE "${from}" "${to}" bad "${good}")
    file(REMOVE_RECURSE "${WORK}/${name}")
    file(COPY "${H}/" DESTINATION "${WORK}/${name}")
    file(WRITE "${WORK}/${name}/slice_1_of_2.json" "${bad}")
    if("ONLY" IN_LIST ARGN)
        file(REMOVE "${WORK}/${name}/slice_0_of_2.json")
    endif()
    run_cmd(o rc ${COLLATZ} merge "${WORK}/${name}")
    if(NOT rc EQUAL 2 OR NOT LAST_ERR MATCHES "${want}")
        message(FATAL_ERROR "${name}: merge was not refused with '${want}' (${rc}): ${LAST_ERR}")
    endif()
endfunction()
# One slice of 2^62 (the fuzzed file): merge sized its coverage scan by it.
merge_refuses(case_k "\"case_k\": 2\n" "\"case_k\": 4611686018427387904\n"
    "case_k is 4611686018427387904" ONLY)
merge_refuses(param_b "\"B\": 24," "\"B\": 4294967320," "B is 4294967320, above 2\\^32-1")
string(REGEX MATCH "\"candidates_checked\": ([0-9]+)" cand "${good}")
math(EXPR cand_plus "${CMAKE_MATCH_1} + 1")
merge_refuses(counter "${cand}" "\"candidates_checked\": ${cand_plus}"
    "result_digest [0-9a-f]+\\.\\.\\. does not match")
run_cmd(oc rc ${COLLATZ} ${ARGS} --checkpoint "${WORK}/counter")
if(NOT rc EQUAL 0 OR NOT LAST_ERR MATCHES "slice_1_of_2.json is unusable \\(result.result_digest")
    message(FATAL_ERROR "resume did not redo the changed slice (${rc}): ${LAST_ERR}")
endif()
scalars("${oc}" oc_s)
if(NOT oc_s STREQUAL ref_s)
    message(FATAL_ERROR "resumed Result differs from unsharded:\n${oc_s}\nvs\n${ref_s}")
endif()

# A binary without the --threads check would size its per-thread buffers by
# 2^32-1 in the commands after this one, so it stops here: without --N, this
# one cannot start a run.
run_cmd(o rc ${COLLATZ} verify --threads 4294967295)
if(NOT rc EQUAL 2 OR NOT LAST_ERR MATCHES "--threads must be at most")
    message(FATAL_ERROR "verify --threads 4294967295 was not refused (${rc}): ${LAST_ERR}")
endif()
foreach(_cmd
        "verify;--N;30;--A;0;--threads;4294967295"
        "certify;--N;20;--threads;4294967295"
        "worker;--server;https://example.invalid;--threads;4294967295"
        "verify;--N;30;--bitvectors;33"
        "certify;--N;20;--bitvectors;33")
    run_cmd(o rc ${COLLATZ} ${_cmd})
    if(NOT rc EQUAL 2 OR NOT LAST_ERR MATCHES "(--threads|--bitvectors) must be at most")
        message(FATAL_ERROR "'${_cmd}' was not refused (${rc}): ${LAST_ERR}")
    endif()
endforeach()
file(REMOVE_RECURSE "${WORK}")
message(STATUS "checkpoint e2e: OK")
