#include "cmd_verify_internal.hpp"

#include <cstdio>

// The help text of the verify command.

namespace collatz::cli::verify_detail {

void print_usage(std::FILE* out) {
    std::fprintf(
        out,
        "usage: collatz verify --N K --threads T [--A a --B b --bitvectors i]\n"
        "                      [--kernel table16] [--mod3t T] [--no-partial-oee]\n"
        "  --N K           required; verify every n < 2^K (2 <= K <= 96, N-A <= 76)\n"
        "  --block H --K K   frontier block mode: verify [H*2^K, (H+1)*2^K)\n"
        "                  given that every smaller n converges;\n"
        "                  H decimal >= 0, 32 <= K <= 76+A; H=0 is the full run at N=K.\n"
        "                  --K is an alias of --N\n"
        "  --record-threshold X  list every walked n whose excursion exceeds X (decimal)\n"
        "                  as `records` (printed and in --json); `none` = off. Default:\n"
        "                  off, except for a block starting at or above 2^71, where it\n"
        "                  is the T-map peak of Barina's path record #97\n"
        "  --assumption-source S  where \"every n < L converges\" comes from, recorded\n"
        "                  in --json (default: Barina's 2^71 bound for L <= 2^71, \"prior\n"
        "                  blocks of this project\" above it)\n"
        "  --threads T     worker threads (default 1; 0 = hardware; at most 4 per\n"
        "                  hardware thread or 256, whichever is larger)\n"
        "  --checkpoint DIR  pause/resume: run as --slices K slices, saving each finished\n"
        "                  slice to DIR; rerunning the same command skips finished slices\n"
        "                  and prints the merged Result. Ctrl-C (or creating DIR/PAUSE)\n"
        "                  stops after the current slice; a second Ctrl-C aborts now.\n"
        "                  DIR must be new, empty or made by --checkpoint\n"
        "  --slices K      slice count for --checkpoint (power of two <= 1024; default\n"
        "                  2^(N-52) on the GPU, 2^(N-46) on the CPU, at least 1)\n"
        "  --slice-range a:b  with --checkpoint: run only slices a..b-1 of K (one machine's\n"
        "                  share); combine the DIRs with `collatz merge DIR1 DIR2 ...`\n"
        "  --checkpoint-force  discard DIR's previous run (other parameters) and start\n"
        "                  over, or use a non-empty DIR without run.json; only the files\n"
        "                  --checkpoint writes are removed\n"
        "  --A a           Step 2 high bits (default by N: 10, 9 from N=44 on the CPU,\n"
        "                  12 from N=48 with --gpu; 0 disables Step 2; max 14)\n"
        "  --B b           Step 2 look-ahead width (default by N: 24 below N=40, 30 for\n"
        "                  N=40..43 (28 with --gpu), 31 from N=44 (32 from N=48 with\n"
        "                  --gpu and A=12); 2..32)\n"
        "  --bitvectors i  Step 2 bitvector count (default 8; at most 32)\n"
        "  --no-bv-cache   disable the on-disk bitvector/dip cache (on by default)\n"
        "  --json PATH     write the full Result (u128 as strings) + params/commit/machine\n"
        "                  to PATH as JSON (through PATH.tmp and a rename)\n"
        "  --progress-file PATH  every 2 s, replace PATH with one line \"tasks_done\n"
        "                  tasks_total slices_done slices_total slices_resumed\n"
        "                  survivors_done survivors_total\" (Step-1 split tasks of the\n"
        "                  slice in progress; checkpoint slices; its Step-1 survivors\n"
        "                  finished and in total, estimated until Step 1 ends), for\n"
        "                  `collatz worker`. Results unchanged\n"
        "  --kernel table16  Step 3 kernel (the default and only value)\n"
        "  --gpu           run Step 2+3 on the GPU (OpenCL); falls back to CPU\n"
        "                  (defaults --threads to hardware_threads() when unset, and\n"
        "                   --A 12 --B 32 from N=48)\n"
        "  --mod3t T       mod-3^t Step 1 sieve (default 6; 0=off)\n"
        "  --window-chains T    fold chain-merge terms into the Step-2 dip (default 6; "
        "0=off)\n"
        "  --window-templates-D D  fold general merge-templates into the dip (default 12; "
        "0=off)\n"
        "  --no-partial-oee     disable high-bit partial OEE (on by default)\n"
        "  -h, --help      print this text and exit\n");
}

} // namespace collatz::cli::verify_detail
