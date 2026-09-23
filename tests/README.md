# Test suite

The tests use doctest and CTest. CONTRIBUTING.md ("Test layout") gives the rules for where a test
goes; this file describes how the suite is registered and run.

## Running

```sh
cmake --preset windows-release && cmake --build --preset windows-release
ctest --preset windows-release                                        # every test, 8 at a time
ctest --test-dir build/windows-release -L unit -j8                    # one label
ctest --test-dir build/windows-release -R test_core_step2_soundness   # one executable's shards
build/windows-release/tests/test_core_step2 --test-case='*monotone*'  # one case, without ctest
```

The labels are `unit`, `property`, `lint`, `quick` (the `unit` and `property` entries without
the GPU tests and the slowest CPU tests) and `fixture` (the cache-warming entries below). `ctest
-L` takes a regular expression, so `-L 'unit|property'` selects both. Every entry has a budget
(`TIMEOUT`): 10 s for `unit` and 60 s for `property` unless its registration line sets another,
4 times as long in a sanitizer build.

## Layout and registration

`core/`, `gpu/` and `ref/` hold one unit-test file per source file or topic, `integration/` one
file per scenario and `support/` the shared helpers. `collatz_add_test()` in
`tests/CMakeLists.txt` builds and registers a test; the calls are in `register_unit.cmake` (the
unit tests, most of them through a glob), `register_integration_cpu_a.cmake`,
`register_integration_gpu.cmake` and `register_integration_cpu_b.cmake`. The tests that link
the OpenCL backend are registered only when it is built. No file has more than 300 lines, so an
executable whose cases need more has a main file and further files in `<dir>/test_<name>/`, added
with `target_sources()`.

Not every entry is a doctest executable. `integration/checkpoint_e2e.cmake` runs pause, resume,
refusal and `collatz merge` through the binary as a CMake script,
`integration/checkpoint_cli_e2e.cmake` checks the verify and merge command lines around
`--checkpoint` and `--json` the same way, `integration/worker_cli.cmake` runs `collatz worker`
on command lines it refuses before it starts curl, `integration/install_layout.cmake` checks
that `cmake --install` installs only `bin/collatz`, and the worker end-to-end test runs
`server/test/e2e_worker.mjs`, which starts the server under `wrangler dev --local` and two
`collatz worker` processes, and exits 77 (skipped) when wrangler is not installed in `server/`.
The Step-2 cache tests make their cache dirs under the temp root and remove them when the test
process exits (`support/step2_cache_dir.hpp`).

## Parallel `ctest -j` layout

- Wall time is the critical path. A doctest binary runs its `TEST_CASE`s serially, so heavy
  multi-case files are split into several ctest entries on the same binary (the `SHARDS` of
  `collatz_add_test()`, given in the `register_*.cmake` files): `<test>.<suffix>` entries select
  cases with `--test-case=`, and `<test>.rest` excludes all of them, so a newly added case still
  runs. `ctest -R <test>` selects every entry.
- Shared bv cache fixture: `test_fixture_bvcache_warm_tw{390,6,0}` build the `2^24` dip tables
  into `build/<preset>/tests/.bvcache` (all cores, concurrently) before any test that
  `FIXTURES_REQUIRED bvcache`; ctest adds them to any `-L`/`-R` selection automatically. Tests
  reuse that cache only when `COLLATZ_CACHE_DIR` is set (`collatz::test::scoped_bv_cache()`),
  so a bare binary run never touches `%LOCALAPPDATA%\collatz`.
- GPU entries share `RESOURCE_LOCK gpu` and run one at a time; with other processes on the same
  device they are latency-bound, which is why their unit budget is 60 s rather than 10 s.

## `scale.hpp`

`tests/support/scale.hpp` holds the full and the reduced sweep sizes, so tests need not
hard-code sizes under `#ifdef NDEBUG`:

- `sanitized()`: true under ASan or UBSan, detected through the
  `COLLATZ_SANITIZED` define that `tests/CMakeLists.txt` adds when the compile flags contain
  `-fsanitize` (with a compiler-macro fallback). Tests also use it to choose whole size lists.
- `scaled(full, small)`: returns `full` in optimized builds and `small` under sanitizers. Used
  for loop bounds, sample counts, `N`, `M`, `B`.

No assertion is dropped: under sanitizers the sweeps only shrink, and the release run keeps full
strength.

## Randomized comparison with `ref/`

`tests/integration/test_ref_random.cpp` (label `property`, 16 configurations) draws seeded
random `(N, A, B, bitvectors, partial OEE, mod3t, t_w, D, threads)` and
compares every result scalar of `verify::run` (candidates, coverage, pow, blocks, max, argmax,
exceeds, `max_exact`) against `ref::verify_ref`. Each configuration runs with the bitvector cache
off, written cold into a fresh private `COLLATZ_CACHE_DIR`, loaded warm (it must hit) and from a
directory shared by the whole sweep. Scope and gaps are in `tests/support/ref_random_audit.hpp`.
More configurations, and a rerun of one:

```sh
COLLATZ_AUDIT_SEED=1000 COLLATZ_AUDIT_CONFIGS=50 COLLATZ_AUDIT_VERBOSE=1 \
  build/windows-release/tests/test_integration_ref_random
COLLATZ_AUDIT_SEED=<seed> COLLATZ_AUDIT_ONLY=<index> build/windows-release/tests/test_integration_ref_random
```
