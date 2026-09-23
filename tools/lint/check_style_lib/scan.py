"""Findings for one text and for a list of files, read from the work tree or
from the index (git cat-file), and the added lines of a staged diff."""

import collections
import os
import re
import subprocess

from .rules import RULE_OPTIONS, RULES
from .source import Source, file_type


class Finding:
    __slots__ = ("path", "line", "rule", "severity", "message")

    def __init__(self, path, line, rule, severity, message):
        self.path, self.line, self.rule, self.severity, self.message = path, line, rule, severity, message

    def __str__(self):
        return "%s:%d:%s: %s" % (self.path, self.line, self.rule, self.message)


def decode(data):
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return data.decode("utf-8", errors="replace")


def scan_text(path, text, config, ftype=None):
    """Findings for one file's text, after type and allow filtering."""
    ftype = ftype or file_type(path, text[:200])
    if ftype is None:
        return []
    active = [(rule_id, sev) for rule_id, (sev, types, allow) in config.rules.items()
              if ftype in types and not allow(path)]
    if not active:
        return []
    src = Source(path, text, ftype)
    out = []
    for rule_id, sev in active:
        seen = set()
        check = RULES[rule_id][0]
        found = check(src, config.options[rule_id]) if rule_id in RULE_OPTIONS else check(src)
        for line, message in found:
            if line not in seen:
                seen.add(line)
                out.append(Finding(path, line, rule_id, sev, message))
    out.sort(key=lambda f: (f.line, f.rule))
    return out


def git(root, *args, data=None):
    p = subprocess.run(["git", "-C", root] + list(args), input=data, capture_output=True)
    if p.returncode != 0:
        raise RuntimeError("git %s: %s" % (" ".join(args), decode(p.stderr).strip()))
    return p.stdout


def tracked_files(root):
    try:
        return [p for p in decode(git(root, "ls-files", "-z")).split("\0") if p]
    except (OSError, RuntimeError):
        skip = {".git", "build", "node_modules", "__pycache__", ".cache", ".wrangler"}
        out = []
        for d, dirs, files in os.walk(root):
            dirs[:] = [x for x in dirs if x not in skip]
            for f in files:
                out.append(os.path.relpath(os.path.join(d, f), root).replace(os.sep, "/"))
        return sorted(out)


def staged_files(root):
    out = git(root, "diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z")
    return [p for p in decode(out).split("\0") if p]


def read_index(root, paths):
    """Staged contents of `paths`, read with one `git cat-file --batch`."""
    if not paths:
        return {}
    out = git(root, "cat-file", "--batch", data="".join(":%s\n" % p for p in paths).encode())
    blobs = {}
    pos = 0
    for p in paths:
        eol = out.index(b"\n", pos)
        header = out[pos:eol].split()
        pos = eol + 1
        if len(header) < 3 or header[1] != b"blob":
            continue
        size = int(header[2])
        blobs[p] = out[pos:pos + size]
        pos += size + 1
    return blobs


def parse_added_lines(diff_text):
    """{path: set of added line numbers} from `git diff -U0` output."""
    added = collections.defaultdict(set)
    path = None
    for line in diff_text.splitlines():
        if line.startswith("+++ "):
            target = line[4:]
            path = target[2:] if target.startswith("b/") else None
        elif line.startswith("@@") and path:
            m = re.match(r"@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", line)
            if m:
                start, count = int(m.group(1)), int(m.group(2) or "1")
                added[path].update(range(start, start + count))
    return added


def scan_paths(root, paths, config, blobs=None):
    findings = []
    for p in paths:
        if config.exclude(p):
            continue
        base = os.path.basename(p)
        if "." in base and file_type(p) is None:
            continue
        if blobs is not None:
            data = blobs.get(p)
            if data is None:
                continue
        else:
            full = os.path.join(root, p)
            if not os.path.isfile(full) or os.path.islink(full):
                continue
            with open(full, "rb") as f:
                data = f.read()
        if b"\0" in data[:8192]:
            continue
        findings += scan_text(p, decode(data), config)
    return findings
