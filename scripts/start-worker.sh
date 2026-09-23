#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Runs collatz worker from build/windows-release on this machine, in an MSYS2
shell.

Usage: scripts/start-worker.sh --server URL [--token-file PATH] [--block-bits N]
                               [--cpu] [--threads N]

Options:
  --server URL       Work server address (required; or set COLLATZ_SERVER)
  --token-file PATH  File holding the worker token (or set COLLATZ_TOKEN_FILE;
                     default %USERPROFILE%\collatz-token.txt, where the setup
                     writes it)
  --block-bits N     Block size 2^N, from 32 to 64 (default 50)
  --cpu              Run on the CPU instead of the GPU
  --threads N        CPU threads with --cpu (default: all)
  -h, --help         Show this help

Ctrl-C pauses and hands the block back; the next run resumes it.
EOF
}

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# The windows-release preset's binary, and the token where
# scripts/windows/setup-server.cmd keeps it. MSYS2's ~ is its own /home/USER,
# not the Windows profile.
binary="$repo_dir/build/windows-release/collatz.exe"
preset=windows-release
default_token="$(cygpath -u "${USERPROFILE:-$HOME}" 2>/dev/null || echo "$HOME")/collatz-token.txt"
server="${COLLATZ_SERVER:-}"
token_file="${COLLATZ_TOKEN_FILE:-$default_token}"
block_bits=50
cpu=false
threads=0

while (($#)); do
    case "$1" in
        --server)
            (($# >= 2)) || { echo "Missing value for --server" >&2; exit 2; }
            server="$2"
            shift 2
            ;;
        --token-file)
            (($# >= 2)) || { echo "Missing value for --token-file" >&2; exit 2; }
            token_file="$2"
            shift 2
            ;;
        --block-bits)
            (($# >= 2)) || { echo "Missing value for --block-bits" >&2; exit 2; }
            block_bits="$2"
            shift 2
            ;;
        --cpu)
            cpu=true
            shift
            ;;
        --threads)
            (($# >= 2)) || { echo "Missing value for --threads" >&2; exit 2; }
            threads="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "$server" ]]; then
    echo "No server address: pass --server URL or set COLLATZ_SERVER" >&2
    usage >&2
    exit 2
fi
if [[ ! "$block_bits" =~ ^[0-9]+$ ]] || ((block_bits < 32 || block_bits > 64)); then
    echo "--block-bits must be an integer from 32 to 64" >&2
    exit 2
fi
if [[ ! "$threads" =~ ^[0-9]+$ ]]; then
    echo "--threads must be a non-negative integer" >&2
    exit 2
fi

if [[ ! -x "$binary" ]]; then
    echo "Missing executable $binary; build it with: cmake --preset $preset && cmake --build --preset $preset" >&2
    exit 1
fi
if [[ ! -r "$token_file" ]]; then
    echo "Missing or unreadable worker token file: $token_file (pass --token-file PATH)" >&2
    exit 1
fi
cd "$repo_dir"

args=(worker --server "$server" --block-bits "$block_bits" --token-file "$token_file")
if [[ "$cpu" == true ]]; then
    if ((threads > 0)); then
        args+=(--threads "$threads")
    fi
else
    args+=(--gpu)
fi

echo "Running collatz worker on $server (blocks of 2^$block_bits). Ctrl-C to pause."
exec "$binary" "${args[@]}"
