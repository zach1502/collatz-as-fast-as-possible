# Registers the bvcache fixture and the script-driven CPU tests.

# "bvcache" fixture: warm the shared test-only bv cache before its users run.
#
# test_fixture_bvcache_warm_tw{390,6,0} run the CLI at N=24, B=24 with all
# hardware threads (support/warm_bvcache.cmake), building every 2^24 dip table
# the tests load (tags 0, 6, 390). CTest runs a FIXTURES_SETUP entry before
# every entry that FIXTURES_REQUIRED it, and adds it automatically to any
# `-L`/`-R` selection that includes one of those, so a bare `ctest -L ... -j N`
# gets a warm cache and each table is built once instead of by every user racing
# on a cold cache. The warm step is best-effort and never fails, so a missing or
# broken CLI only means each user builds its own cold table.
if(TARGET collatz)
    collatz_scaled_timeout(300 _warm_timeout)
    # One setup entry per dip tag so the three builds run concurrently (a
    # fixture may have several setup tests; users wait for all of them).
    foreach(_tag 390 6 0)
        add_test(NAME test_fixture_bvcache_warm_tw${_tag}
            COMMAND ${CMAKE_COMMAND}
                -DCOLLATZ_BIN=$<TARGET_FILE:collatz>
                -DCACHE_DIR=${CMAKE_CURRENT_BINARY_DIR}/.bvcache
                -DTAG=${_tag}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/support/warm_bvcache.cmake)
        set_tests_properties(test_fixture_bvcache_warm_tw${_tag} PROPERTIES
            FIXTURES_SETUP bvcache
            LABELS fixture
            TIMEOUT ${_warm_timeout})
    endforeach()
endif()

# Pause/resume end-to-end through the real binary (verify --checkpoint, a
# resume from the files a killed run leaves, mismatch refusal, --slice-range +
# `collatz merge`). A CMake script, so it needs no shell or Python and runs the
# same on MSYS2. N=30 with A=0 (no bitvector cache): a few seconds; TIMEOUT 120
# leaves room for a loaded machine.
if(TARGET collatz)
    collatz_scaled_timeout(120 _ckpt_e2e_timeout)
    add_test(NAME test_integration_checkpoint_e2e
        COMMAND ${CMAKE_COMMAND} -DCOLLATZ=$<TARGET_FILE:collatz>
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/checkpoint_e2e
            -P ${CMAKE_CURRENT_SOURCE_DIR}/integration/checkpoint_e2e.cmake)
    set_tests_properties(test_integration_checkpoint_e2e PROPERTIES
        LABELS unit TIMEOUT ${_ckpt_e2e_timeout})
    # The verify and merge command lines around --checkpoint and --json (help,
    # empty paths, the A/N hint, atomic --json, foreign DIR, PAUSE, forged slices).
    add_test(NAME test_integration_checkpoint_cli_e2e
        COMMAND ${CMAKE_COMMAND} -DCOLLATZ=$<TARGET_FILE:collatz>
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/checkpoint_cli_e2e
            -P ${CMAKE_CURRENT_SOURCE_DIR}/integration/checkpoint_cli_e2e.cmake)
    set_tests_properties(test_integration_checkpoint_cli_e2e PROPERTIES
        LABELS unit TIMEOUT ${_ckpt_e2e_timeout})
endif()

# Work-unit system end to end (server/test/e2e_worker.mjs): real `collatz
# worker` processes with the owner's worker token against the server under
# `wrangler dev --local`, at K=32 (M=22, B=24, so it needs the bvcache fixture).
# Node runs the driver; it exits 77 (skipped) when wrangler is not installed in
# server/.
find_program(NODE_EXECUTABLE NAMES node)
if(TARGET collatz AND NODE_EXECUTABLE)
    collatz_scaled_timeout(300 _worker_e2e_timeout)
    add_test(NAME test_integration_worker_e2e
        COMMAND ${NODE_EXECUTABLE} ${PROJECT_SOURCE_DIR}/server/test/e2e_worker.mjs
            --collatz $<TARGET_FILE:collatz>
            --work ${CMAKE_CURRENT_BINARY_DIR}/worker_e2e)
    set_tests_properties(test_integration_worker_e2e PROPERTIES
        LABELS unit
        SKIP_RETURN_CODE 77
        TIMEOUT ${_worker_e2e_timeout}
        FIXTURES_REQUIRED bvcache
        ENVIRONMENT "COLLATZ_CACHE_DIR=${CMAKE_CURRENT_BINARY_DIR}/.bvcache")
endif()

# `collatz worker` on command lines it refuses before it starts curl
# (integration/worker_cli.cmake): empty --token-file and --server values, and a
# --curl that is no program. No unit runs, so it needs no bvcache fixture.
if(TARGET collatz)
    collatz_scaled_timeout(300 _worker_cli_timeout)
    add_test(NAME test_integration_worker_cli
        COMMAND ${CMAKE_COMMAND} -DCOLLATZ=$<TARGET_FILE:collatz>
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/worker_cli
            -P ${CMAKE_CURRENT_SOURCE_DIR}/integration/worker_cli.cmake)
    set_tests_properties(test_integration_worker_cli PROPERTIES
        LABELS unit
        TIMEOUT ${_worker_cli_timeout})
endif()
