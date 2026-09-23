# collatz as fast as possible

A C++20 Collatz verifier for CPUs and OpenCL GPUs.

It implements Vigleik Angeltveit's three-step sieve, with Christian Hercher's merge rules as used by David Barina's sieve. Runs may start at arbitrary blocks, report the exact maximum excursion, and detect path records along the way.

The public campaign continues from David Barina's verified frontier at `2075*2^60`, below which every number has already been checked. On an RTX 3070, frontier blocks run at about `2e13` (two trillion) numbers per second

The program builds with clang or GCC on Windows/MSYS2, using GMP and OpenCL. CUDA is not required.

As far as I can tell, this is the fastest Collatz verifier available.
I've been running it on my 3070 fairly casually for a few weeks, and verified ~2^63.5 numbers so far.

Uses a lot of tricks and optimizations. Minimal algorithmic improvements from the stated papers, but a lot of engineering to make it run fast. Came up with a better way to do a checksum of the sieve coverage (in my opinion). And the ability to do Angeltveit's approach but starting at any arbitrary block, which is useful for distributed verification.

Current Progress:
[https://collatz-frontier.zach1502.workers.dev/](https://collatz-frontier.zach1502.workers.dev/)

Frontier as of 2026-10-25: 2^71.0274

## Performance

Measured on one (undervolted) NVIDIA RTX 3070 in a Windows 10 machine with a Ryzen 7 2700X:

| Run | Time | Numbers/s |
|---|---:|---:|
| `2^50 at 0` | 45.2 s | `2.49e13` |
| `2^50 at frontier` | 55.3 s | `2.04e13` |
| `2^54 at frontier` | 702 s | `2.56e13` |

Performance depends on sieve parameters, hardware and block location (due to the integer math being done taking extra bits).

Vs Reported Benchmarks from David Barina's paper:
NVIDIA GeForce RTX 2080 Ti

| Run | Time | Numbers/s |
|---|---:|---:|
| `2^40` | 215 s | `~2.62e11` |

From Angelveit's paper:
NVIDIA GeForce RTX 3060 

| Run | Time | Numbers/s |
|---|---:|---:|
| `2^50 at 0` | 166 s | `~0.666e13` |
| `2^55 at 0` | 3636 s | `~1.01e13` |

From Honda et al. (2017):
NVIDIA GeForce GTX TITAN

| Run | Time | Numbers/s |
|---|---:|---:|
| `GPU` | n/a | `~5.01e11` |



## Features

- Verify every `n < 2^N`, or an independent block
  `[H*2^K, (H+1)*2^K)`.
- Exact maximum excursion and starting value.
- Path-record detection while verifying frontier blocks.
- GMP replay when a walk would exceed 128-bit arithmetic.
- CPU and OpenCL 1.2 GPU backends with deterministic results.
- Independent GMP certification of sieve exclusions.
- Checkpoints, slicing and merging for multi-machine runs.
- Optional distributed worker/server with a public hash-chained ledger.

## What it proves

With

```text
T(n) = n/2       if n is even
T(n) = (3n+1)/2  if n is odd
```

the command

```sh
collatz verify --N K
```

prints a successful result only if every

```text
1 <= n < 2^K
```

reaches `1`.

Likewise,

```sh
collatz verify --block H --K K
```

verifies every number in

```text
[H*2^K, (H+1)*2^K)
```

provided every smaller number is already known to converge.

Numbers that are skipped by the sieve receive a certificate showing that their trajectory either drops below the starting value or merges with the trajectory of a smaller number. Strong induction then covers the entire range.

This proves only the stated finite range, not the Collatz conjecture.

David Barina's project has verified every number below

```text
2075*2^60
```

which is about `2^71.02`:

https://pcbarina.fit.vutbr.cz/

Block results record the lower-bound assumption they rely on in

```text
params.assumes_below
params.assumption_source
```

## How it works

The verifier uses Angeltveit's three-step sieve.

1. **Residue-class search.**  
   Step 1 searches classes modulo powers of two. Classes are removed when their trajectories descend or provably merge with smaller trajectories, using Hercher-style path merges and preimage chains.

2. **Look-ahead sieve.**  
   Step 2 considers many extensions of each surviving residue class at once using precomputed look-ahead tables and bitvectors.

3. **Direct walks.**  
   Step 3 walks the remaining candidates using a 16-step table until they fall below their starting value. Walks use 128-bit arithmetic and are replayed with GMP if necessary.

A small exhaustive preflight provides the induction base case.

The verifier also tracks the maximum excursion. Candidates that might exceed the current maximum are re-walked exactly, and the result reports whether the maximum is proven exact with

```text
max_exact = yes
```

## Build

Requirements:

- CMake 3.25+
- Ninja
- GMP
- clang or GCC
- OpenCL headers and loader for GPU support

### Windows

Use the MSYS2 UCRT64 shell:

```sh
pacman -S --needed \
  git \
  mingw-w64-ucrt-x86_64-{gcc,gmp,cmake,ninja,pkgconf,opencl-headers,opencl-icd}

cmake --preset windows-release
cmake --build --preset windows-release
```

The binary is:

```text
build/windows-release/collatz.exe
```

To build with clang instead of GCC (install `mingw-w64-ucrt-x86_64-clang`; the preset sets `CMAKE_CXX_COMPILER` to `g++`):

```sh
cmake --preset windows-release -DCMAKE_CXX_COMPILER=clang++
```

To build without OpenCL:

```sh
cmake --preset windows-release -DCOLLATZ_OPENCL=OFF
```

To build offline without downloading the test dependency:

```sh
cmake --preset windows-release -DCOLLATZ_BUILD_TESTS=OFF
```

## Usage

Verify every number below `2^40`:

```sh
collatz verify --N 40 --threads 8
```

Run Steps 2 and 3 on the GPU:

```sh
collatz verify --N 40 --gpu
```

Write the result as JSON:

```sh
collatz verify --N 40 --threads 8 --json result.json
```

Verify the first `2^40` numbers after Barina's frontier:

```sh
collatz verify --block 2175795200 --K 40 --gpu
```

Run with a resumable checkpoint:

```sh
collatz verify --N 56 --gpu --checkpoint run56
```

Re-derive sieve exclusions independently with GMP:

```sh
collatz certify --N 36 --threads 8
```

Merge checkpoint directories from several machines:

```sh
collatz merge pc1 pc2 --json merged.json
```

Run a distributed worker:

```sh
collatz worker \
  --server https://SERVER \
  --token-file ~/collatz-token.txt \
  --gpu \
  --block-bits 50
```

Run

```sh
collatz
```

to list commands, or

```sh
collatz <command> --help
```

for command-specific options.

### GPU selection

`--gpu` uses the OpenCL device selected by

```sh
collatz gpu-info
```

Set

```sh
COLLATZ_GPU_DEVICE=INDEX
```

to choose another device.

If no usable GPU is present, the program can run on the CPU. An OpenCL failure during a GPU run terminates the run without producing a successful result.

### Checkpoints and slicing

With

```sh
--checkpoint DIR
```

an interrupted run can be continued by running the same command again.

Large runs can also be divided into independent slices:

```sh
--slices K
--slice-range a:b
```

Run different slice ranges on different machines and combine them afterward with `collatz merge`.

## Correctness and reproducibility

The implementation is tested several ways:

- Native Collatz steps are compared against GMP on randomized inputs.
- Small ranges and blocks are compared against exhaustive linear verification.
- An independent implementation in `ref/` is compared against the production verifier.
- `certify` independently re-derives sieve exclusions using GMP.
- CPU and GPU paths are checked for identical results.
- clang and GCC builds are checked for identical results.
- Known published maximum-excursion records are reproduced.

For a standard reproducibility check,

```sh
collatz verify --N 40 --threads 8
```

must report:

```text
candidates_checked = 635943714
coverage_checksum  = 2224189903094082880
pow_checksum       = 810565753336788107
blocks_total       = 2464262900
max_excursion      = 200279370410625061016864
max_excursion_n    = 871673828443
max_exact          = yes
```

The digests are independent of thread count, slicing and backend when the same sieve parameters are used.

Above the ranges covered by exhaustive independent checks, correctness rests on the certificate lemmas together with cross-checks between implementations, GMP certification, CPU/GPU agreement and known path records.

## Distributed campaign

The `server/` directory contains an optional Cloudflare Workers/D1 coordinator for running a public verification campaign.

Workers lease independent blocks, verify them, submit deterministic result digests and optionally perform audits or rechecks. Accepted results advance the frontier.

Each accepted or withdrawn block is also appended to a public hash-chained ledger, allowing the published campaign history to be independently checked.

A worker is started with:

```sh
collatz worker \
  --server https://SERVER \
  --token-file ~/collatz-token.txt \
  --gpu \
  --block-bits 50
```

The worker supports checkpoints, graceful pausing, block hand-back and optional GMP audits.

See `server/` and its scripts for deployment and campaign administration.

## Testing

Run the C++ test suite with:

```sh
ctest --preset windows-release
```

Run the style checks:

```sh
python3 tools/lint/lint_all.py
```

Run the server tests:

```sh
cd server
npm install
npm test
```

See `CONTRIBUTING.md` and `tests/README.md` for the repository layout and contribution requirements.

## References

- R. Terras, *A stopping time problem on the positive integers*, Acta Arithmetica 30 (1976), 241-252.
- T. Oliveira e Silva, *Maximum excursion and stopping time record-holders for the 3x+1 problem: computational results*, Mathematics of Computation 68 (1999), 371-384.
- T. Oliveira e Silva, *Empirical verification of the 3x+1 and related conjectures*, in *The Ultimate Challenge: The 3x+1 Problem*, AMS, 2010.
- C. Hercher, *Über die Länge nicht-trivialer Collatz-Zyklen*, Die Wurzel 52 (2018).
- C. Hercher, *There are no Collatz-m-Cycles with m <= 91*, Journal of Integer Sequences 26 (2023), Article 23.3.5.
- D. Barina, *Convergence verification of the Collatz problem*, The Journal of Supercomputing 77 (2021), 2681-2688.
- D. Barina, *Improved verification limit for the convergence of the Collatz conjecture*, The Journal of Supercomputing 81 (2025).
- V. Angeltveit, *An improved algorithm for checking the Collatz Conjecture for all n < 2^N*, arXiv:2602.10466 (2026).

Related implementations:

- https://github.com/vigleik0/collatz
- https://github.com/xbarin02/collatz
- https://github.com/xbarin02/collatz-sieve

## License and data credits

MIT. See `LICENSE`.

The public campaign builds on David Barina's verification of every number below

```text
2075*2^60
```

https://pcbarina.fit.vutbr.cz/

The path-record data in `server/data/` combines published records from Barina, OEIS A006884/A006885, Eric Roosendaal and Tomas Oliveira e Silva. See `server/data/README.md` for details.