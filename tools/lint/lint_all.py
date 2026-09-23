#!/usr/bin/env python3
"""Runs every installed style tool and prints a count per tool and rule. Raw
output goes to build/lint/<tool>.txt, or to the --out-dir directory.

Tools: check_style.py, clang-format, ESLint (server/), ruff, shellcheck and
PSScriptAnalyzer. A tool that is not installed is skipped with a note; so is
ruff when uvx cannot provide it (no network and not in uv's cache).
check_style.py counts only the findings above tools/lint/baseline.json.

Usage: tools/lint/lint_all.py [--strict] [--offline] [--out-dir DIR]
                              [--only TOOL]... [--skip TOOL]...
  --strict   a selected tool that is skipped fails the run
  --offline  never fetch a tool: uvx and npx use only their caches
  --out-dir  where the raw output goes (default build/lint)
Exit status: 0 when no tool that ran has a finding, 1 otherwise (and, with
--strict, when a selected tool was skipped).
"""

import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LINT = os.path.join(ROOT, "tools", "lint")
RUFF_VERSION = "0.17.0"
RUFF_PATHS = ["tools"]
SKIPPED = 3


def run(cmd, cwd=ROOT, env=None):
    try:
        p = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, errors="replace")
    except OSError as e:
        return 127, "", str(e)
    return p.returncode, p.stdout, p.stderr


def count(pattern, text, key):
    counts = collections.Counter()
    for line in text.splitlines():
        m = pattern.match(line)
        if not m:
            continue
        counts[key(m)] += 1
    return counts


def run_failed(c, code, label):
    """Adds a row for a run that exited non-zero without a finding, so a tool that
    fails to run fails the lint instead of passing it."""
    if code != 0 and not +c:
        c["%s exited %d" % (label, code)] = 1
    return c


def tool_check_style(args):
    # --check prints only the findings above tools/lint/baseline.json and exits 1
    # when one of them is an error; 2 is a usage or setup error.
    code, out, err = run([sys.executable, os.path.join(LINT, "check_style.py"), "--check"])
    if code not in (0, 1):
        return collections.Counter({"check_style.py failed": 1}), err.strip()[-200:], out + err
    c = count(re.compile(r"^(.+?):(\d+):([a-z-]+): "), out, lambda m: m.group(3))
    return run_failed(c, code, "check_style.py"), "", out + err


def tool_clang_format(args):
    exe = shutil.which("clang-format")
    if not exe:
        return None, "clang-format not found", ""
    _, listing, _ = run(["git", "ls-files", "--", "src/*.cpp", "src/*.hpp", "include/*.hpp",
                         "tests/*.cpp", "tests/*.hpp"])
    # A deletion that is not staged yet leaves the file in the index only.
    files = [f for f in listing.split() if os.path.isfile(os.path.join(ROOT, f))]
    code, out, err = run([exe, "--dry-run"] + files)
    hits = re.compile(r"^(.+?):\d+:\d+: warning: .*\[-Wclang-format-violations\]")
    bad = {m.group(1) for m in map(hits.match, err.splitlines()) if m}
    c = count(hits, err, lambda m: "violations")
    c["files needing format (of %d)" % len(files)] = len(bad)
    return run_failed(c, code, "clang-format"), "", out + err


def tool_eslint(args):
    server = os.path.join(ROOT, "server")
    if not os.path.exists(os.path.join(server, "node_modules", ".bin", "eslint")):
        return None, "server/node_modules missing (cd server && npm install)", ""
    env = None
    if args.offline:
        # eslint is local (--no-install); this also stops npm's update check.
        env = dict(os.environ, npm_config_offline="true", npm_config_update_notifier="false")
    _, out, err = run(["npx", "--no-install", "eslint", ".", "-f", "json"], cwd=server, env=env)
    try:
        results = json.loads(out)
    except ValueError:
        return collections.Counter({"eslint failed to run": 1}), err.strip()[-200:], out + err
    c = collections.Counter()
    for f in results:
        for m in f["messages"]:
            c["%s (%s)" % (m.get("ruleId") or "parse", "error" if m["severity"] == 2 else "warning")] += 1
    return c, "", out + err


def tool_ruff(args):
    if shutil.which("ruff"):
        cmd = ["ruff"]
    elif shutil.which("uvx"):
        # uvx fetches ruff on first use. A probe first, so that a ruff it cannot
        # provide (offline, or not in uv's cache under --offline) is skipped
        # instead of counted as a finding.
        cmd = ["uvx"] + (["--offline"] if args.offline else []) + ["ruff@" + RUFF_VERSION]
        code, out, err = run(cmd + ["--version"])
        if code != 0:
            where = "not in uv's cache (--offline)" if args.offline else "no network?"
            return None, "uvx could not provide ruff %s: %s" % (RUFF_VERSION, where), out + err
    else:
        return None, "ruff not found (pip install ruff, or install uv for uvx)", ""
    code, out, err = run(cmd + ["check"] + RUFF_PATHS + ["--output-format", "concise", "--no-cache"])
    c = count(re.compile(r"^.+?:\d+:\d+: ([A-Z]+\d+|invalid-syntax)\b"), out, lambda m: m.group(1))
    return run_failed(c, code, "ruff"), "", out + err


def tool_shellcheck(args):
    code, out, err = run(["bash", os.path.join(LINT, "run_shellcheck.sh")])
    if code == SKIPPED:
        return None, err.strip().splitlines()[-1], err
    c = count(re.compile(r"^.+?:\d+:\d+: (\w+): .* \[(SC\d+)\]$"), out,
              lambda m: "%s (%s)" % (m.group(2), m.group(1)))
    return run_failed(c, code, "run_shellcheck.sh"), "", out + err


def tool_psscriptanalyzer(args):
    pwsh = shutil.which("pwsh") or shutil.which("powershell")
    if not pwsh:
        return None, "pwsh not found; run it on Windows (see CONTRIBUTING.md)", ""
    script = ("Invoke-ScriptAnalyzer -Path scripts/windows -Recurse "
              "-Settings tools/lint/PSScriptAnalyzerSettings.psd1 | "
              "ForEach-Object { '{0}:{1}:{2}' -f $_.ScriptName, $_.Line, $_.RuleName }")
    code, out, err = run([pwsh, "-NoProfile", "-NonInteractive", "-Command", script])
    if code != 0 and not out:
        return None, "PSScriptAnalyzer failed (Install-Module PSScriptAnalyzer): " + err.strip()[-120:], err
    return count(re.compile(r"^(.+?):(\d+):(\w+)$"), out, lambda m: m.group(3)), "", out + err


TOOLS = [
    ("check_style", tool_check_style),
    ("clang-format", tool_clang_format),
    ("eslint", tool_eslint),
    ("ruff", tool_ruff),
    ("shellcheck", tool_shellcheck),
    ("psscriptanalyzer", tool_psscriptanalyzer),
]


def main():
    ap = argparse.ArgumentParser(description="Run all style tools; fail on any finding.")
    ap.add_argument("--strict", action="store_true", help="fail when a selected tool is skipped")
    ap.add_argument("--offline", action="store_true", help="never fetch a tool")
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "build", "lint"),
                    help="directory for the raw output (default build/lint)")
    ap.add_argument("--only", action="append", default=[], choices=[t[0] for t in TOOLS])
    ap.add_argument("--skip", action="append", default=[], choices=[t[0] for t in TOOLS])
    args = ap.parse_args()
    out_dir = os.path.abspath(args.out_dir)
    os.makedirs(out_dir, exist_ok=True)
    status = []
    rows = []
    found = skipped = 0
    for name, fn in TOOLS:
        if (args.only and name not in args.only) or name in args.skip:
            status.append((name, "not run", ""))
            continue
        sys.stderr.write("lint: %s ...\n" % name)
        counts, note, raw = fn(args)
        with open(os.path.join(out_dir, name + ".txt"), "w", encoding="utf-8") as f:
            f.write(raw)
        if counts is None:
            status.append((name, "skipped", note))
            skipped += 1
            continue
        total = sum(v for k, v in counts.items() if not k.startswith("files needing"))
        found += total
        status.append((name, "%d findings" % total, note))
        rows += [(name, rule, n) for rule, n in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))]
    print("%-18s %-58s %7s" % ("tool", "rule", "count"))
    for name, rule, n in rows:
        print("%-18s %-58s %7d" % (name, rule[:58], n))
    print()
    for name, state, note in status:
        print("%-18s %-14s %s" % (name, state, note))
    inside = out_dir.startswith(os.path.join(ROOT, ""))
    print("\nraw output: %s" % (os.path.relpath(out_dir, ROOT) if inside else out_dir))
    if found:
        print("lint: %d findings; fix them (see CONTRIBUTING.md, \"Style checks\")" % found)
        return 1
    if skipped and args.strict:
        print("lint: --strict and %d selected tool(s) skipped" % skipped)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
