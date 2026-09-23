# `collatz worker` through the real binary on command lines it refuses before
# it starts curl, so no server and no curl are needed. Invoked by ctest as
#   cmake -DCOLLATZ=<path to collatz> -DWORK=<scratch dir> -P worker_cli.cmake
#
#  1. empty --token-file and --server values are refused (exit 2), not read as
#     "not given" (which fell back to COLLATZ_WORKER_TOKEN or the saved server).
#  2. a --curl that is no program stops the worker at once (exit 2).

cmake_minimum_required(VERSION 3.25)
if(NOT COLLATZ OR NOT WORK)
    message(FATAL_ERROR "need -DCOLLATZ=... -DWORK=...")
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(TOKEN "cli-test-token-0123456789abcdef0123456789abcdef")
set(TOKEN_FILE "${WORK}/token.txt")
file(WRITE "${TOKEN_FILE}" "${TOKEN}\n")
# A curl that does not exist, for every run below: scenario 2 checks that the
# worker refuses it, and in scenario 1 it keeps a run that wrongly gets past
# the empty value from starting a real curl.
set(NO_CURL "${WORK}/no-such-curl")

# Runs the worker with the token file; sets RC, OUT and ERR.
function(worker)
    cmake_parse_arguments(W "" "TIMEOUT" "" ${ARGN})
    if(NOT W_TIMEOUT)
        set(W_TIMEOUT 120)
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E env COLLATZ_WORKER_TOKEN=
            COLLATZ_WORKER_BACKOFF_MS=1 "${COLLATZ}" worker --server http://127.0.0.1:9
            --token-file "${TOKEN_FILE}" --plain --threads 2 --block-bits 32
            --audit-rate 0 ${W_UNPARSED_ARGUMENTS}
        OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc TIMEOUT ${W_TIMEOUT})
    set(RC "${rc}" PARENT_SCOPE)
    set(OUT "${out}" PARENT_SCOPE)
    set(ERR "${err}" PARENT_SCOPE)
endfunction()

# expect(<what> [RC <code>] [HAS <text>...]): checks the last run's exit code
# and that its stdout and stderr together hold each <text>.
function(expect what)
    cmake_parse_arguments(E "" "RC" "HAS" ${ARGN})
    set(all "${OUT}${ERR}")
    set(bad "")
    if(DEFINED E_RC AND NOT "${RC}" STREQUAL "${E_RC}")
        string(APPEND bad "exit '${RC}', not ${E_RC}; ")
    endif()
    foreach(t IN LISTS E_HAS)
        string(FIND "${all}" "${t}" at)
        if(at EQUAL -1)
            string(APPEND bad "no '${t}'; ")
        endif()
    endforeach()
    if(bad)
        message(SEND_ERROR "FAIL ${what}: ${bad}\n--- stdout:\n${OUT}\n--- stderr:\n${ERR}")
    else()
        message(STATUS "ok   ${what}")
    endif()
endfunction()

# 1. Empty values (called directly: a list expansion would drop the "").
execute_process(COMMAND ${CMAKE_COMMAND} -E env "COLLATZ_WORKER_TOKEN=${TOKEN}"
        "${COLLATZ}" worker --server http://127.0.0.1:9 --curl "${NO_CURL}"
        --config "${WORK}/e1/worker.json" --token-file "" --once --plain
    OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR RESULT_VARIABLE RC TIMEOUT 60)
expect("--token-file '' is refused" RC 2 HAS "--token-file needs a non-empty value")
file(WRITE "${WORK}/e2/worker.json" [[{"server": "http://127.0.0.1:9"}]])
execute_process(COMMAND "${COLLATZ}" worker
        --config "${WORK}/e2/worker.json" --server "" --curl "${NO_CURL}"
        --token-file "${TOKEN_FILE}" --once --plain
    OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR RESULT_VARIABLE RC TIMEOUT 60)
expect("--server '' is refused" RC 2 HAS "--server needs a non-empty value")

# 2. An unusable curl.
worker(--config "${WORK}/c1/worker.json" --once --curl "${NO_CURL}" TIMEOUT 20)
expect("a --curl that is no program" RC 2 HAS "cannot run curl")
