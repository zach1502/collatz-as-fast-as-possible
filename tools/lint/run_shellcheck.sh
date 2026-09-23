#!/usr/bin/env bash
# Runs shellcheck (settings: .shellcheckrc) on the tracked shell scripts, in gcc
# format.
#
# Usage: tools/lint/run_shellcheck.sh [FILE...]
# Exit status: 0 no findings, 1 findings, 3 skipped (shellcheck not installed).
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
if ! command -v shellcheck >/dev/null 2>&1; then
    echo "run_shellcheck: skipped, shellcheck not installed (winget install koalaman.shellcheck)" >&2
    exit 3
fi
if [ $# -eq 0 ]; then
    files=()
    while IFS= read -r -d '' f; do
        # A deletion that is not staged yet leaves the file in the index only.
        if [ -f "$f" ]; then
            files+=("$f")
        fi
    done < <(git ls-files -z -- '*.sh')
    set -- "${files[@]}"
fi
echo "run_shellcheck: $(shellcheck --version | sed -n 's/^version: //p'), $# files" >&2
shellcheck -f gcc "$@"
