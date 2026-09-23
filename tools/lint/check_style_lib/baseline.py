"""baseline.json: the accepted finding count per (file, rule), its update, and
the comparison that --check reports."""

import collections
import json
import os


def count_findings(findings):
    counts = collections.defaultdict(collections.Counter)
    for f in findings:
        counts[f.path][f.rule] += 1
    return counts


def load_baseline(path):
    if not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8") as f:
        return json.load(f).get("counts", {})


def write_baseline(path, counts, previous, scanned, rules, tracked):
    """Store the counts of `rules` for the scanned files. Other entries are kept
    while their file is tracked, so --rule and PATH arguments limit the update
    and the entries of a deleted file go."""
    merged = {}
    for p, prev in previous.items():
        kept = {r: n for r, n in prev.items() if p not in scanned or r not in rules}
        if kept and p in tracked:
            merged[p] = kept
    for p, found in counts.items():
        merged.setdefault(p, {}).update(found)
    data = {"version": 1, "counts": {p: dict(sorted(r.items())) for p, r in sorted(merged.items())}}
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1, sort_keys=False)
        f.write("\n")


def baseline_scope(baseline, paths, rules):
    """The entries of `rules` for `paths` (None: every path), so that --check
    reports a count below the baseline only for what it scanned."""
    return {p: {r: n for r, n in counts.items() if r in rules}
            for p, counts in baseline.items() if paths is None or p in paths}


def compare(findings, baseline):
    """(over, under): (path, rule, count, allowed) pairs above and below the baseline."""
    over, under = [], []
    counts = count_findings(findings)
    for path, rules in sorted(counts.items()):
        for rule, n in sorted(rules.items()):
            allowed = baseline.get(path, {}).get(rule, 0)
            if n > allowed:
                over.append((path, rule, n, allowed))
    for path, rules in sorted(baseline.items()):
        for rule, allowed in sorted(rules.items()):
            n = counts.get(path, {}).get(rule, 0)
            if n < allowed:
                under.append((path, rule, n, allowed))
    return over, under
