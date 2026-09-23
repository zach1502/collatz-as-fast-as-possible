# End-to-end checks of the verify and merge command lines around --checkpoint
# and --json, through the real binary (portable: a CMake script, no shell or
# Python). Invoked by ctest as
#   cmake -DCOLLATZ=<path to collatz> -DWORK=<scratch dir> -P checkpoint_cli_e2e.cmake
#
#  1. -h and --help print the usage on stdout and exit 0 (verify and merge);
#     the verify usage gives the A and B defaults by N, as the README does.
#  2. An empty path for --json, --checkpoint or --progress-file, and an empty
#     merge --json, exit 2 instead of turning the option off.
#  3. N - A above 76 suggests only an --A that verify accepts (at most 14), or
#     a lower --N.
#  4. merge stamps its JSON with the slices' commit, not its own.
#  5. --json goes through PATH.tmp and a rename: a symlink at PATH is replaced
#     and its target kept, and a directory at PATH leaves no PATH.tmp behind.
#  6. A non-empty DIR without run.json is refused (exit 2) unless
#     --checkpoint-force, which keeps the user's files, *.tmp included; a
#     non-empty directory named PAUSE is an error, not a pause.
#  7. Slices of another --mod3t, copied into DIR with DIR's own checkpoint
#     block (fingerprint), are redone by the resume: its Result equals the
#     clean run, and --json and DIR/merged.json carry the same digest.

cmake_minimum_required(VERSION 3.25)
if(NOT COLLATZ OR NOT WORK)
    message(FATAL_ERROR "need -DCOLLATZ=... -DWORK=...")
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
# A = 0: no Step-2 tables, so no bitvector cache is needed.
set(ARGS verify --N 22 --A 0 --threads 2)

function(run_cmd out_var rc_var)
    execute_process(COMMAND ${ARGN}
        OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
    set(${out_var} "${_out}" PARENT_SCOPE)
    set(${rc_var} "${_rc}" PARENT_SCOPE)
    set(LAST_ERR "${_err}" PARENT_SCOPE)
endfunction()

# Stops unless the last command exited with `want` and printed `pattern` on stderr.
macro(expect what want pattern)
    if(NOT rc EQUAL ${want} OR NOT LAST_ERR MATCHES "${pattern}")
        message(FATAL_ERROR "${what}: exit ${rc}, want ${want} and '${pattern}' in:\n${LAST_ERR}")
    endif()
endmacro()

function(digest_of file out_var)
    file(READ "${file}" _t)
    if(NOT _t MATCHES "\"result_digest\": \"([0-9a-f]+)\"")
        message(FATAL_ERROR "no result_digest in ${file}")
    endif()
    set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

# 1. Help.
foreach(_h -h --help)
    run_cmd(o rc ${COLLATZ} verify ${_h})
    if(NOT rc EQUAL 0 OR NOT o MATCHES "usage: collatz verify" OR
            NOT o MATCHES "default by N: 10, 9 from N=44 on the CPU" OR
            NOT o MATCHES "default by N: 24 below N=40, 30 for")
        message(FATAL_ERROR "verify ${_h}: exit ${rc}, stdout:\n${o}\nstderr:\n${LAST_ERR}")
    endif()
    run_cmd(o rc ${COLLATZ} merge ${_h})
    if(NOT rc EQUAL 0 OR NOT o MATCHES "usage: collatz merge")
        message(FATAL_ERROR "merge ${_h}: exit ${rc}, stdout:\n${o}\nstderr:\n${LAST_ERR}")
    endif()
endforeach()

# A finished two-slice run for the merge checks.
set(CM "${WORK}/cm")
run_cmd(o rc ${COLLATZ} ${ARGS} --checkpoint "${CM}" --slices 2)
expect("checkpointed run" 0 "all slices done")

# 2. Empty paths. A quoted "" reaches the program as an empty argument.
foreach(_opt --json --checkpoint --progress-file)
    execute_process(COMMAND "${COLLATZ}" verify --N 20 --A 0 ${_opt} ""
        OUTPUT_QUIET ERROR_VARIABLE LAST_ERR RESULT_VARIABLE rc)
    expect("verify ${_opt} ''" 2 "${_opt} needs a non-empty path")
endforeach()
execute_process(COMMAND "${COLLATZ}" merge "${CM}" --json ""
    OUTPUT_QUIET ERROR_VARIABLE LAST_ERR RESULT_VARIABLE rc)
expect("merge --json ''" 2 "--json needs a non-empty path")

# 3. The A/N suggestion.
run_cmd(o rc ${COLLATZ} verify --N 96 --A 14)
expect("verify --N 96 --A 14" 2 "lower --N to at most 90")
run_cmd(o rc ${COLLATZ} verify --N 86 --A 0)
expect("verify --N 86 --A 0" 2 "e[.]g[.] --A 10[)]")
foreach(_n 96 86)
    run_cmd(o rc ${COLLATZ} verify --N ${_n} --A 0)
    string(REGEX MATCHALL "--A [0-9]+" _sugg "${LAST_ERR}")
    foreach(_s ${_sugg})
        string(REGEX REPLACE "--A " "" _a "${_s}")
        if(_a GREATER 14)
            message(FATAL_ERROR "N=${_n} suggests ${_s}, above the largest A (14):\n${LAST_ERR}")
        endif()
    endforeach()
endforeach()

# 4. merge stamps the slices' commit.
foreach(_c 0 1)
    file(READ "${CM}/slice_${_c}_of_2.json" _t)
    string(REGEX REPLACE "\"commit\": \"[^\"]*\"" "\"commit\": \"feedface\"" _t "${_t}")
    file(WRITE "${CM}/slice_${_c}_of_2.json" "${_t}")
endforeach()
run_cmd(o rc ${COLLATZ} merge "${CM}" --json "${WORK}/cm.json")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "merge of the re-stamped slices failed (${rc}): ${LAST_ERR}")
endif()
file(READ "${WORK}/cm.json" _j)
if(NOT _j MATCHES "\"commit\": \"feedface\"")
    message(FATAL_ERROR "merge did not stamp the slices' commit:\n${_j}")
endif()

# 5. --json through a temp file and a rename.
file(MAKE_DIRECTORY "${WORK}/outdir")
file(WRITE "${WORK}/outdir/x" "x")
run_cmd(o rc ${COLLATZ} verify --N 20 --A 0 --json "${WORK}/outdir")
expect("verify --json DIR" 2 "failed to write JSON")
run_cmd(o rc ${COLLATZ} merge "${CM}" --json "${WORK}/outdir")
expect("merge --json DIR" 2 "failed to write JSON")
if(EXISTS "${WORK}/outdir.tmp")
    message(FATAL_ERROR "a failed rename left ${WORK}/outdir.tmp")
endif()
file(WRITE "${WORK}/target.txt" "old\n")
file(CREATE_LINK "target.txt" "${WORK}/link.json" RESULT _lrc SYMBOLIC)
if(_lrc EQUAL 0) # symlinks may be unavailable (Windows without the privilege)
    run_cmd(o rc ${COLLATZ} verify --N 20 --A 0 --json "${WORK}/link.json")
    file(READ "${WORK}/target.txt" _t)
    if(NOT rc EQUAL 0 OR IS_SYMLINK "${WORK}/link.json" OR NOT _t STREQUAL "old\n")
        message(FATAL_ERROR "verify --json wrote through the symlink instead of replacing it "
            "(exit ${rc}): ${LAST_ERR}")
    endif()
endif()

# 6. A DIR that verify --checkpoint did not make, and a PAUSE that stays.
set(FD "${WORK}/fd")
file(MAKE_DIRECTORY "${FD}")
file(WRITE "${FD}/notes.txt" "mine\n")
file(WRITE "${FD}/report.docx.tmp" "mine\n")
run_cmd(o rc ${COLLATZ} ${ARGS} --checkpoint "${FD}" --slices 2)
expect("foreign DIR" 2 "--checkpoint-force")
if(EXISTS "${FD}/run.json")
    message(FATAL_ERROR "the refused run wrote ${FD}/run.json")
endif()
run_cmd(o rc ${COLLATZ} ${ARGS} --checkpoint "${FD}" --slices 2 --checkpoint-force)
expect("foreign DIR with --checkpoint-force" 0 "all slices done")
if(NOT EXISTS "${FD}/notes.txt" OR NOT EXISTS "${FD}/report.docx.tmp")
    message(FATAL_ERROR "--checkpoint-force removed a file this program did not write")
endif()
file(REMOVE "${FD}/slice_1_of_2.json")
file(MAKE_DIRECTORY "${FD}/PAUSE")
file(WRITE "${FD}/PAUSE/x" "x")
run_cmd(o rc ${COLLATZ} ${ARGS} --checkpoint "${FD}")
expect("non-empty PAUSE directory" 2 "cannot remove the old pause request")

# 7. Forged slices of another --mod3t.
function(scalars text out_var)
    string(REGEX MATCHALL
        "(candidates_checked|step2_masked_out|step3_steps_total|coverage_checksum|pow_checksum|blocks_total|max_excursion|max_excursion_n|max_exceeds_u128|overflow_fallbacks|step1_survivors) += [^\n]*"
        _m "${text}")
    set(${out_var} "${_m}" PARENT_SCOPE)
endfunction()
run_cmd(ref rc ${COLLATZ} ${ARGS})
scalars("${ref}" ref_s)
run_cmd(o rc ${COLLATZ} ${ARGS} --checkpoint "${WORK}/fa" --slices 2)
run_cmd(o rc ${COLLATZ} ${ARGS} --mod3t 0 --checkpoint "${WORK}/fb" --slices 2)
set(_key "\"checkpoint\": {")
foreach(_c 0 1)
    file(READ "${WORK}/fa/slice_${_c}_of_2.json" _ours)
    file(READ "${WORK}/fb/slice_${_c}_of_2.json" _theirs)
    string(FIND "${_ours}" "${_key}" _at_o)
    string(FIND "${_theirs}" "${_key}" _at_t)
    if(_at_o EQUAL -1 OR _at_t EQUAL -1)
        message(FATAL_ERROR "no checkpoint block in slice ${_c}")
    endif()
    string(SUBSTRING "${_ours}" ${_at_o} -1 _block)
    string(SUBSTRING "${_theirs}" 0 ${_at_t} _head)
    file(WRITE "${WORK}/fa/slice_${_c}_of_2.json" "${_head}${_block}")
endforeach()
file(REMOVE "${WORK}/fa/merged.json")
run_cmd(o rc ${COLLATZ} ${ARGS} --checkpoint "${WORK}/fa" --json "${WORK}/fa.json")
expect("resume with forged slices" 0 "does not match this run's parameters")
scalars("${o}" o_s)
if(NOT o_s STREQUAL ref_s)
    message(FATAL_ERROR "resumed Result differs from the clean run:\n${o_s}\nvs\n${ref_s}")
endif()
digest_of("${WORK}/fa.json" _d1)
digest_of("${WORK}/fa/merged.json" _d2)
if(NOT _d1 STREQUAL _d2)
    message(FATAL_ERROR "--json digest ${_d1} differs from merged.json digest ${_d2}")
endif()
file(REMOVE_RECURSE "${WORK}")
message(STATUS "checkpoint CLI e2e: OK")
