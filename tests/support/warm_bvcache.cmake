# ctest FIXTURES_SETUP for the shared, test-only bv cache (fixture "bvcache",
# registered in tests/register_integration_cpu_b.cmake). Invoked as
#   cmake -DCOLLATZ_BIN=<collatz> -DCACHE_DIR=<dir> -DTAG=0|6|390 -P warm_bvcache.cmake
#
# Builds one of the 2^24 `dip` tables the tests load, once and with every
# hardware thread, before any dependent test starts, so on a cold cache (a fresh
# build tree) several tests do not rebuild the same table concurrently. The
# three dip tags (tag = t_w + 32*D):
#   * 390 = t_w 6, D 12 -- the CLI default;
#   * 6   = t_w 6, D 0  -- tests that set window_chains = 6 without templates;
#   * 0   = t_w 0, D 0  -- the verify::Options and step2::Params defaults.
# ctest registers one setup entry per TAG so the three builds run concurrently.
# Each call is cheap once the cache is warm. A failure here is reported but
# never fails the fixture; dependent tests then build what they need
# themselves.
if(NOT COLLATZ_BIN OR NOT CACHE_DIR OR NOT DEFINED TAG)
    message(FATAL_ERROR "usage: cmake -DCOLLATZ_BIN=... -DCACHE_DIR=... -DTAG=... -P warm_bvcache.cmake")
endif()
set(_args_390 "")
set(_args_6 "--window-templates-D;0")
set(_args_0 "--window-chains;0;--window-templates-D;0")
if(NOT DEFINED _args_${TAG})
    message(FATAL_ERROR "warm_bvcache: unknown TAG '${TAG}' (expected 0, 6 or 390)")
endif()
file(MAKE_DIRECTORY "${CACHE_DIR}")
set(ENV{COLLATZ_CACHE_DIR} "${CACHE_DIR}")
execute_process(
    COMMAND "${COLLATZ_BIN}" verify --N 24 --B 24 --threads 0 ${_args_${TAG}}
    RESULT_VARIABLE _rc
    OUTPUT_QUIET
    ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(WARNING "bv-cache warm step failed (tag ${TAG}): rc=${_rc} ${_err}")
endif()
file(GLOB _files "${CACHE_DIR}/dip_*")
message(STATUS "bv-cache warm: ${_files}")
