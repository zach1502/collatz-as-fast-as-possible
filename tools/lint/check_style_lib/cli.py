"""The command line: the --report summary tables, the --check output and main()."""

import argparse
import collections
import os
import sys

from .baseline import baseline_scope, compare, count_findings, load_baseline, write_baseline
from .config import DEFAULT_BASELINE, DEFAULT_CONFIG, HERE, Config, ConfigError
from .rules import RULES
from .scan import decode, git, parse_added_lines, read_index, scan_paths, staged_files, tracked_files


def top_dir(path):
    return path.split("/", 1)[0] if "/" in path else "(root)"


def summary_tables(findings, config):
    by_rule = collections.Counter(f.rule for f in findings)
    files_by_rule = collections.defaultdict(set)
    for f in findings:
        files_by_rule[f.rule].add(f.path)
    by_dir = collections.Counter(top_dir(f.path) for f in findings)
    rules = [r for r in RULES if r in config.rules]
    return by_rule, files_by_rule, by_dir, rules


def print_summary(findings, config, out=sys.stdout):
    by_rule, files_by_rule, by_dir, rules = summary_tables(findings, config)
    out.write("\n%-22s %-8s %6s %9s\n" % ("rule", "severity", "files", "findings"))
    for r in rules:
        out.write("%-22s %-8s %6d %9d\n" % (r, config.rules[r][0], len(files_by_rule[r]), by_rule[r]))
    out.write("%-22s %-8s %6d %9d\n" % ("total", "", len({f.path for f in findings}), len(findings)))
    out.write("\n%-22s %9s\n" % ("directory", "findings"))
    for d, n in by_dir.most_common():
        out.write("%-22s %9d\n" % (d, n))


def repo_root():
    try:
        return decode(git(HERE, "rev-parse", "--show-toplevel")).strip()
    except (OSError, RuntimeError):
        return os.path.dirname(os.path.dirname(HERE))


def select_paths(all_paths, wanted):
    if not wanted:
        return all_paths
    wanted = [w.rstrip("/") for w in wanted]
    return [p for p in all_paths if any(p == w or p.startswith(w + "/") for w in wanted)]


def run_check(findings, baseline, config, added, out):
    over, under = compare(findings, baseline)
    by_pair = collections.defaultdict(list)
    for f in findings:
        by_pair[(f.path, f.rule)].append(f)
    failed = False
    for path, rule, n, allowed in over:
        items = by_pair[(path, rule)]
        severity = items[0].severity
        failed = failed or severity == "error"
        if added is None:
            shown = items
        else:
            shown = [f for f in items if f.line in added.get(path, ())] or items
        for f in shown:
            out.write("%s [%s]\n" % (f, severity))
        hidden = len(items) - len(shown)
        note = "" if not hidden else ", %d more on unchanged lines" % hidden
        out.write("%s: %s: %d finding%s, baseline %d%s\n" % (
            path, rule, n, "" if n == 1 else "s", allowed, note))
    if under:
        out.write("%d (file, rule) count%s below the baseline; run --update-baseline to lock them in\n"
                  % (len(under), "" if len(under) == 1 else "s"))
    if failed and not config.enforce:
        out.write("style check: findings above the baseline (report mode: enforce is false in "
                  "tools/lint/config.json, so this does not fail)\n")
        return 0
    return 1 if failed else 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Text-level style checks; see CONTRIBUTING.md.")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--report", action="store_true", help="print all findings and a summary (default)")
    mode.add_argument("--check", action="store_true", help="fail on error findings above the baseline")
    mode.add_argument("--update-baseline", action="store_true", help="write current counts to the baseline")
    mode.add_argument("--list-rules", action="store_true", help="print the rules and exit")
    ap.add_argument("--staged", action="store_true", help="scan the index version of staged files")
    ap.add_argument("--rule", action="append", default=[], help="only this rule (repeatable)")
    ap.add_argument("--config", default=DEFAULT_CONFIG)
    ap.add_argument("--baseline", default=DEFAULT_BASELINE)
    ap.add_argument("--root", help="repository root (default: the one holding this script)")
    ap.add_argument("paths", nargs="*", help="limit the scan to these files or directories")
    args = ap.parse_args(argv)
    out = sys.stdout
    try:
        config = Config.load(args.config)
    except ConfigError as e:
        sys.stderr.write("check_style: %s\n" % e)
        return 2
    if args.list_rules:
        for rule_id, (_, desc) in RULES.items():
            sev, types, _ = config.rules.get(rule_id, ("off", set(), None))
            out.write("%-22s %-8s %s\n    types: %s\n" % (rule_id, sev, desc, ", ".join(sorted(types)) or "-"))
        return 0
    unknown = [r for r in args.rule if r not in RULES]
    if unknown:
        sys.stderr.write("check_style: unknown rule %s\n" % ", ".join(unknown))
        return 2
    if args.rule:
        config.rules = {r: v for r, v in config.rules.items() if r in args.rule}
    if args.update_baseline and args.staged:
        sys.stderr.write("check_style: --update-baseline scans the working tree; drop --staged\n")
        return 2
    root = os.path.abspath(args.root) if args.root else repo_root()
    added = None
    try:
        if args.staged:
            paths = select_paths(staged_files(root), args.paths)
            blobs = read_index(root, paths)
            findings = scan_paths(root, paths, config, blobs)
            if args.check and paths:
                diff = git(root, "diff", "--cached", "-U0", "--no-color", "--no-ext-diff", "--", *paths)
                added = parse_added_lines(decode(diff))
        else:
            tracked = tracked_files(root)
            paths = select_paths(tracked, args.paths)
            findings = scan_paths(root, paths, config)
    except (OSError, RuntimeError) as e:
        sys.stderr.write("check_style: %s\n" % e)
        return 2
    if args.update_baseline:
        write_baseline(args.baseline, count_findings(findings), load_baseline(args.baseline), set(paths),
                       set(config.rules), set(tracked))
        out.write("wrote %s: %d findings in %d files\n" % (
            os.path.relpath(args.baseline, root), len(findings), len({f.path for f in findings})))
        return 0
    if args.check:
        scope = set(paths) if args.staged or args.paths else None
        baseline = baseline_scope(load_baseline(args.baseline), scope, set(config.rules))
        return run_check(findings, baseline, config, added, out)
    for f in findings:
        out.write("%s\n" % f)
    print_summary(findings, config, out=out)
    return 0
