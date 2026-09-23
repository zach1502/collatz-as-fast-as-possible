# Contributing

How the repository is laid out, how to build and test it, where new code goes, how a change is
accepted and which style checks it must pass.

## Layout

| Path | Contents |
|------|----------|
| `src/core/` | the library `collatz_core`: Steps 1 to 3, block mode, checkpoints and merges, `certify`, the linear verifier and the worker's protocol |
| `src/gpu/` | the OpenCL backend `collatz_gpu`: the host pipeline, and the kernels in `src/gpu/kernels/` |
| `src/cli/`, `src/main.cpp` | the `collatz` executable, one `cmd_<name>.cpp` per subcommand |
| `include/collatz/` | public headers; `gpu/u128_limbs.h` is shared by the host code and the kernels |
| `ref/` | `collatz_ref`, an independent implementation that the tests compare against |
| `tests/` | the doctest and CTest suite, see [tests/README.md](tests/README.md) |
| `tools/lint/` | the style checker, `lint_all.py` and the settings of the other tools |
| `cmake/` | compiler flags, `FindGMP.cmake` and the kernel embedding |
| `scripts/` | `start-worker.sh`, and the Windows setup and start scripts (`windows/`) |
| `server/` | the work server: the Worker (`src/`), the D1 schema (`schema/`, and `migrations/` generated from it), the status page (`public/`), the campaign, admin, ledger and migration tools (`scripts/`), the path records (`data/`) and its tests (`test/`) |

## Build and test

```sh
cmake --preset windows-release && cmake --build --preset windows-release
ctest --preset windows-release                                       # every test, 8 at a time
ctest --test-dir build/windows-release -L 'unit|property' -j8        # selected labels
ctest --test-dir build/windows-release -R test_core_step2_soundness  # one executable's shards
python3 tools/lint/lint_all.py                                       # every installed style tool
cd server && npm install && npm test                                 # the server: tsc, ESLint, tests
```

The `windows-release` preset is the release build with GCC on Windows (MSYS2 UCRT64).

## Adding code without touching CMake

The build picks up sources by globbing (with `CONFIGURE_DEPENDS`, so CMake re-globs when the
file set changes). Adding a source or unit-test file needs no edit to `CMakeLists.txt`; an
integration test needs one line, which gives its label, in a `tests/register_integration_*.cmake`
file.

### Where files go

| Add a...                     | Put it in                        | Result |
|------------------------------|----------------------------------|--------|
| core library source          | `src/core/*.cpp`                 | compiled into `collatz_core` |
| GPU host source              | `src/gpu/*.cpp`                  | compiled into `collatz_gpu` when the OpenCL backend is built |
| public header                | `include/collatz/*.hpp`          | available to all targets |
| CLI subcommand               | `src/cli/cmd_<name>.cpp`         | linked into the `collatz` executable |
| unit test of a source file   | `tests/<dir>/test_<name>.cpp`    | the `unit` executable `test_<dir>_<name>` |
| test that spans files        | `tests/integration/test_<scenario>.cpp` | its own executable, label set in `tests/register_integration_*.cmake` |
| test helper                  | `tests/support/`                 | on the include path of every test |
| reference implementation     | `ref/*.cpp` + `ref/include/`     | compiled into `collatz_ref`, linked into tests |

`collatz_ref` is built only when `ref/*.cpp` exist; tests link it automatically when present. A
new OpenCL program file is the one exception to the globbing: `CMakeLists.txt` embeds each
kernel file by name, and `cmake/kernel_sources.hpp.in` declares it.

### Files of at most 300 lines

No source, script or build file has more than 300 lines, comments and blank lines included; the
`file-length` style check enforces this ("Style checks" lists the file types). A file that would
pass the limit is split by topic, and each piece is named after what it holds:

- A source file `src/<dir>/<name>.cpp` keeps its name and gets siblings `<name>_<topic>.cpp` in
  the same directory (`step2.cpp`, `step2_dip.cpp`, `step2_pack.cpp`). `src/core` and `src/gpu`
  are globbed recursively and `src/cli` is not, so CLI files stay flat.
- Declarations the pieces share go in an internal header next to them, `<name>_internal.hpp`
  (`src/gpu/<name>/internal.hpp` in `src/gpu`), with a `// Defined in <file>.` line over each
  group. A function that two pieces call is a named function in a `detail` namespace declared
  there (`collatz::verify::detail`, `collatz::gpu::verify_gpu_detail`), not one in an anonymous
  namespace. A hot loop and its small callees stay in one file or become `inline` functions in
  a header.
- A public header `include/collatz/<name>.hpp` stays as an umbrella that keeps the overview
  comment and includes its pieces from `include/collatz/<name>/` (`step2.hpp` includes
  `step2/params.hpp`, `step2/filter.hpp` and `step2/candidate_mask.hpp`), so no `#include`
  elsewhere changes.
- The OpenCL program `src/gpu/kernels/survivor_cpi.cl` and the limb header
  `include/collatz/gpu/u128_limbs.h` include numbered part files from `survivor_cpi/` and
  `u128_limbs/`. When `cmake/EmbedKernelSource.cmake` embeds the kernels it replaces each of
  those `#include` lines with the part, so the OpenCL compiler sees one text;
  `test_integration_kernel_embed` checks the embedded text against the files.
- A server module `server/src/<name>.ts` re-exports from `server/src/<name>/` (`result.ts` and
  `result/*.ts`) or gets siblings `<name>_<topic>.ts` (`admin_units.ts`), so imports elsewhere
  do not change. Server tests share their setup through `server/test/helpers-*.mjs`.
- A script keeps its entry file and sources (shell, PowerShell) or imports (Python) its pieces.

A split moves code and its comments and changes no behavior, so the pins in the README
("Correctness evidence") stay the same.

### Test layout

Unit tests map onto source files. The unit tests of `src/<dir>/<name>.cpp` (`<dir>` is `core`
or `gpu`) or `ref/<name>.cpp` are in `tests/<dir>/test_<name>.cpp`. A module that is split into
several files has one test file per piece or topic, named after it: `test_step2_dip.cpp` tests
`src/core/step2_dip.cpp`, and `test_step3_persist.cpp` the persistent lanes of Step 3. A header
and the `.cpp` of the same name are one subject; a header that holds logic of its own has its
own file (`include/collatz/u128.hpp` is tested in `tests/core/test_u128.cpp`,
`include/collatz/gpu/u128_limbs.h` and `src/gpu/host_pipeline.hpp` in `tests/gpu/`). A test's
subject is the code it checks; the expected value may come from GMP, a brute force or another
module.

Tests whose subject is the whole pipeline go to `tests/integration/`, one file per scenario:
the property and oracle sweeps against GMP and `ref/`, regression pins, the GPU verifier against
the CPU verifier and runs of the `collatz` binary. `src/cli` is compiled into the `collatz`
executable only, so its commands are tested through the binary there. Helpers used by more than
one test file live in `tests/support/`.

Each test file is one executable, `test_<dir>_<name>` (`tests/core/test_step2.cpp` builds
`test_core_step2`), that links `doctest::doctest` and `collatz_core` and uses
`DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`. When one executable's cases need more than 300 lines, its
main file keeps `main` and the other cases go to files
`tests/<dir>/test_<name>/<name>_NN_<topic>.cpp`. They are not `test_*.cpp`, so the unit-test
glob does not build them alone; `target_sources(test_<dir>_<name> PRIVATE ...)` after the
executable's `collatz_add_test()` line adds them (`test_gpu_u128_limbs`,
`test_integration_gpu_selfcheck`). Two integration executables keep their second file next to
the first (`test_u128_domain_kernels.cpp`, `test_exclusion_certificates_partial_oee.cpp`). An
executable's shards select from the cases of all its files.

`collatz_add_test()` in `tests/CMakeLists.txt` builds a test file and registers it with its
label and options (budget, GPU lock, bv cache fixture, shards). The calls are in four files,
which `tests/CMakeLists.txt` includes in this order; the order fixes the test numbers that
`ctest -N` shows:

| File | Registers |
|------|-----------|
| `tests/register_unit.cmake` | the unit tests in `tests/core`, `tests/gpu` and `tests/ref` |
| `tests/register_integration_cpu_a.cmake` | CPU pins, oracles, sieves, `certify`, `ref/` and block mode |
| `tests/register_integration_gpu.cmake` | the GPU tests and the kernel embed check (only when the OpenCL backend is built), and the `--progress-file` test |
| `tests/register_integration_cpu_b.cmake` | the bv cache fixture and the script-driven tests |

`register_unit.cmake` globs the `test_*.cpp` files of the unit-test directories and registers
each with the label `unit` and the unit defaults, so such a file needs no CMake edit. A unit test
that needs more (a longer budget, the GPU lock, the fixture, shards or extra sources) gets its
own `collatz_add_test()` line above the glob. An integration test gets one line, with its label,
in the `register_integration_*.cmake` file of its group.

| Label      | Runs                                                                 |
|------------|----------------------------------------------------------------------|
| `unit`     | all of `tests/<dir>/` and the integration tests that take seconds; budget 10 s unless the test sets one |
| `property` | the property and oracle sweeps, the pins, `certify` and block mode; budget 60 s unless set |
| `lint`     | the style checks of `tools/lint/CMakeLists.txt`                      |
| `quick`    | added to the `unit` and `property` entries except the GPU tests and the slowest CPU tests |
| `fixture`  | the cache-warming entries, which ctest adds to any selection that needs them |

In a sanitizer build (`-fsanitize` in the compile flags) every budget is 4 times longer. A test
that exceeds its budget is too heavy for its label: split it into shards (`SHARDS`), shrink its
sweep under sanitizers with `scaled()` (`tests/support/scale.hpp`), or give it a `TIMEOUT` with
the reason in a comment.

### How to add a CLI command

Add one file, `src/cli/cmd_<name>.cpp`:

```cpp
#include "collatz/cli.hpp"
#include <cstdio>

namespace {
int run_hello(int argc, char** argv) {
    // argv[0] is the command name; argv[1..] are its arguments.
    std::printf("hello\n");
    return 0;
}
} // namespace

COLLATZ_REGISTER_COMMAND("hello", "print a greeting", run_hello)
```

The macro defines a static registrar whose constructor calls `register_command` at startup;
because `src/cli/*.cpp` is compiled directly into the executable, the registrar is kept.
`main.cpp` only calls `dispatch`, which lists every registered command when given none or an
unknown one. No edit to `main.cpp` or CMake is needed.

## Acceptance rule

A change is accepted when all of these hold:

1. `ctest --preset windows-release` passes, `python3 tools/lint/lint_all.py` reports no
   finding, and for a change in `server/`, `npm test` passes there.
2. A performance change is measured on the old and the new binary, run in turns on the same
   machine several times each (with a hardware-counter profiler that reports instructions
   retired and cycles elapsed), and is accepted in one of two ways:
   - A change that checks the same set of numbers is accepted on a clear improvement in
     instructions or cycles, with identical pins in both builds (`candidates_checked`,
     `coverage_checksum`, `pow_checksum`, `blocks_total`, `max_excursion`, `max_excursion_n`).
   - A new sound sieve or prune, which checks a different set by design, is accepted when it
     shrinks `candidates_checked`, keeps `max_excursion` and `max_excursion_n` identical, and
     does not make instructions or cycles worse.

   Wall time alone is not enough, because it depends on the load of the machine. A regression
   in instructions or cycles must be explained.
3. New mathematics needs a proof and a GMP check. A change that adds a new exclusion argument
   (not a re-implementation of an existing one) must explain why it is sound, and
   `collatz certify` must re-derive the new exclusions in GMP, before it becomes a default.

## Clean-room rule

Other verifiers and papers (Barina, Angeltveit and others) may be read for ideas and used for
cross-checks, but their code is not copied into this repository. Angeltveit's verifier is
GPL-3.0.

## Commit message style

Conventional-commit style, imperative mood, lower-case subject:

```
<type>: <summary>

<optional body: what and why, not how>
```

Types: `feat`, `fix`, `perf`, `refactor`, `test`, `docs`, `chore`, `build`. Keep the subject
under about 70 characters, and make one logical change per commit.

## Style checks

Every style check blocks: `python3 tools/lint/lint_all.py` fails on any finding, and so do
`ctest --test-dir build/windows-release -L lint` and `cd server && npm test` (runs `npm run lint`).
`lint_all.py` runs every installed tool and prints a count per tool and rule; raw output goes to
`build/lint/` (`--out-dir DIR` moves it). `--only TOOL` and `--skip TOOL` select tools,
`--offline` never fetches one (uvx and npx use only their caches), and `--strict` also fails when
a selected tool is skipped: not installed, or a ruff that uvx cannot provide. The `lint` label
runs the tests of the checker and of `lint_all.py`, `check_style.py --check`, and
`lint_all.py --skip check_style --offline` for the other tools, with the raw output in the build
tree; `-DCOLLATZ_LINT_STRICT=ON` adds `--strict` to that entry.

| Tool | Scope | Config | Run alone |
|------|-------|--------|-----------|
| `check_style.py` | tracked text files | `tools/lint/config.json` | `python3 tools/lint/check_style.py [--rule R] [PATH...]` |
| clang-format | C++ in `src include tests` | `.clang-format` | `clang-format --dry-run FILE` |
| ESLint | `server/` | `server/eslint.config.mjs` | `cd server && npm run lint` |
| ruff | `tools/` | `ruff.toml` | `ruff check tools` (or `uvx ruff@0.17.0 check tools`) |
| shellcheck | `*.sh` | `.shellcheckrc` | `tools/lint/run_shellcheck.sh` |
| PSScriptAnalyzer | `scripts/windows/` | `tools/lint/PSScriptAnalyzerSettings.psd1` | see below |

`.clang-tidy` and `tests/.clang-tidy` configure clang-tidy for clangd and for LLVM's
`run-clang-tidy -p build/windows-release`; `lint_all.py` does not run it.

`check_style.py --list-rules` prints the rules: the mechanical ones (non-ASCII text, experiment
references and names, banners, emphasis markers, unowned TODOs, placeholder text, whitespace,
file length) and the heuristics for comments (restating comments, doc comments on trivial
functions, commented-out code, stock wording). Every rule has error severity, so every finding
fails `--check`; a rule set to `warning` in `config.json` is reported and never fails. Each rule
has allow patterns (path globs) in `config.json`.

For a heuristic false positive, change the comment, not the code. A doc comment on a function of
one or two statements stays when it states what the signature cannot (a range, a unit, a
precondition, an edge case), and the rule accepts it once it also gives the reason (because,
since, so, only, must, but, see, ...); a doc that only repeats the name goes. A list of formulas
stops looking like code once its items do not end in `;`. A narrow allow pattern is the last
resort. For the other tools, fix the finding or suppress the one line with its reason:
`// NOLINT(check): reason` or `// NOLINTNEXTLINE(check): reason`,
`// eslint-disable-next-line rule -- reason`, `# shellcheck disable=SCnnnn` with a comment, or
`# noqa: RULE`. A check that is noise for the whole tree is dropped in its config file, with the
reason next to it.

`check_style.py --check` compares the counts per (file, rule) with `tools/lint/baseline.json`, so
with `"enforce": true` in `config.json` any finding above it fails, and `lint_all.py` counts only
those. The baseline is empty. `--update-baseline` writes the current counts into it (`--rule R`
and `PATH` limit what it replaces); use it only to bring in a new rule over existing code, and
let those counts go down to zero. `--staged` checks the staged version of the staged files.

`file-length` fails a C, C++, OpenCL, TS, JS, Python, shell, PowerShell, CMake or batch (`.cmd`)
file or a `CMakeLists.txt` with more than 300 lines (`max` in `config.json`); comment and blank
lines count. Its allow list exempts data (JSON, JSONC, CSV, `.map`), test fixtures, Markdown,
`LICENSE` and generated files (`package-lock.json`, `server/migrations/`). It reports each long
file once, on line 1. "Files of at most 300 lines" above describes how a long file is split.

PSScriptAnalyzer runs on Windows PowerShell 5.1 or pwsh 7:

```powershell
Install-Module PSScriptAnalyzer -Scope CurrentUser
Invoke-ScriptAnalyzer -Path scripts/windows -Recurse -Settings tools/lint/PSScriptAnalyzerSettings.psd1
```
