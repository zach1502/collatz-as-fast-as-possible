"""The mechanical rules: non-ASCII text, fixed patterns (experiment references,
verdicts and names, banners, emphasis, stock wording, TODOs, placeholders),
whitespace, the final newline and file length, and the comment and prose
helpers that the heuristics use too."""

import os
import re

from ..source import banner_body


EXPERIMENT_REFS = [
    re.compile(r"\bE\d{2,3}[a-z]?\b"),
    re.compile(r"\b[Ww]aves? [A-Z]{1,2}\b"),
    re.compile(r"\b[Rr]ounds? \d+\b"),
    re.compile(r"\b[Mm]emos? \d+\b"),
    re.compile(r"\bresearch/\d+"),
    re.compile(r"\bdocs/experiments\.md\b"),
]
VERDICTS = re.compile(r"\b(?:KILL(?:ed|ED|S)?|KEEP|DEFERRED)\b")
KILLED_WORD = re.compile(r"\b[Kk]illed\b")
KILLED_VERDICT = re.compile(r"\b[Kk]illed\s+(?:by|on|for)\s+(?:theory|count|measurement|model|design)\b")
IDENT = re.compile(r"(?<![A-Za-z0-9_$])[A-Za-z_][A-Za-z0-9_]*")
EXPERIMENT_IDENT = re.compile(
    r"(?:^|_)[eE]\d{2,3}[a-z]?(?=_|$)|^k?E\d{2,3}[A-Z]|[a-z]E\d{2,3}(?=[A-Z_]|$)")
EXPERIMENT_ID_ONLY = re.compile(r"^E\d{2,3}[a-z]?$")
EXPERIMENT_FILE = re.compile(r"(?:^|[_\-.])[eE]\d{2,3}[a-z]?(?=[_\-.]|$)")
BANNER = re.compile(r"^\s*[=\-*#~/]{4,}")
EMPHASIS = re.compile(r"\bNOTE:|\bIMPORTANT\b|\bCRITICAL\b|\bWARNING:|\bNote that\b")
AI_WORDS = [
    re.compile(r"\brobust(?:ly|ness)?\b", re.I),
    re.compile(r"\bseamless(?:ly)?\b", re.I),
    re.compile(r"\bleverag(?:e|es|ed|ing)\b", re.I),
    re.compile(r"\bcomprehensive(?:ly)?\b", re.I),
    re.compile(r"\bdelv(?:e|es|ed|ing)\b", re.I),
    re.compile(r"\butili[sz](?:e|es|ed|ing)\b", re.I),
    re.compile(r"\bcrucial(?:ly)?\b", re.I),
    re.compile(r"\bensures? that\b", re.I),
    re.compile(r"\bThis (?:function|method|class)\b"),
    re.compile(r"\b(?:Here|Now) we\b", re.I),
    re.compile(r"\bLet['\u2019]s\b", re.I),
    re.compile(r"\bStep (?:[4-9]|\d{2,}):", re.I),
]
PLACEHOLDERS = [
    re.compile(r"\bin a real implementation\b", re.I),
    re.compile(r"\bin production,? you would\b", re.I),
    re.compile(r"\brest (?:of (?:the )?(?:code|file) )?(?:is )?unchanged\b", re.I),
    re.compile(r"\btodo:? implement\b", re.I),
    re.compile(r"\byou're absolutely right\b", re.I),
    re.compile(r"\bhere's the (?:updated|complete)\b", re.I),
    re.compile(r"\bfor now\b", re.I),
    re.compile(r"\bsimplified\b", re.I),
]
TODO = re.compile(r"\b(?:TODO|FIXME|XXX)\b(?!\()")
NON_ASCII = re.compile(r"[^\x00-\x7f]")


def _comments(src, docstrings=True):
    for no, ln in enumerate(src.lines, 1):
        for c in ln.comments:
            if c.style == "shebang" or (c.style == "docstring" and not docstrings):
                continue
            yield no, ln, c


def _prose(src):
    """Comment text, plus the prose lines of Markdown files."""
    if src.ftype == "md":
        for no, ln in enumerate(src.lines, 1):
            if ln.kind == "prose":
                yield no, ln.raw
        return
    for no, _, c in _comments(src):
        yield no, c.text


def _matches(patterns, text):
    found = []
    for p in patterns:
        found += [m.group() for m in p.finditer(text)]
    return found


def _fmt(items):
    return ", ".join(dict.fromkeys(items))


def rule_non_ascii(src):
    for no, ln in enumerate(src.lines, 1):
        m = NON_ASCII.search(ln.raw)
        if m:
            yield no, "non-ASCII character U+%04X" % ord(m.group())


LINT_DIRECTIVE = re.compile(
    r"(?i)\b(?:noqa|nolint(?:nextline)?|pylint:\s*disable|type:\s*ignore)\b[\s:=(\[]*[\w,\- ]*[)\]]?")


def rule_experiment_ref(src):
    for no, text in _prose(src):
        found = _matches(EXPERIMENT_REFS, LINT_DIRECTIVE.sub("", text))
        if found:
            yield no, "experiment reference in comment: " + _fmt(found)


def rule_experiment_ref_code(src):
    for no, ln in enumerate(src.lines, 1):
        if src.ftype == "md":
            in_strings, in_code = [], (_matches(EXPERIMENT_REFS, ln.raw) if ln.kind == "code" else [])
        else:
            in_strings = [m for part in ln.strings for m in _matches(EXPERIMENT_REFS, part)]
            in_code = _matches(EXPERIMENT_REFS, ln.code_text)
        if in_strings or in_code:
            where = "string" if in_strings else "code"
            yield no, "experiment reference in %s: %s" % (where, _fmt(in_strings + in_code))


def rule_experiment_verdict(src):
    for no, text in _prose(src):
        found = [m.group() for m in VERDICTS.finditer(text)]
        if KILLED_WORD.search(text) and (KILLED_VERDICT.search(text) or _matches(EXPERIMENT_REFS, text)):
            found += [m.group() for m in KILLED_WORD.finditer(text)]
        if found:
            yield no, "experiment verdict in comment: " + _fmt(found)


def rule_experiment_identifier(src):
    for no, ln in enumerate(src.lines, 1):
        names = []
        for part in [ln.code_text] + ln.strings:
            for tok in IDENT.findall(part):
                if EXPERIMENT_IDENT.search(tok) and not EXPERIMENT_ID_ONLY.match(tok):
                    names.append(tok)
        if names:
            yield no, "name refers to an experiment: " + _fmt(names)


def rule_experiment_filename(src):
    base = os.path.basename(src.path)
    if EXPERIMENT_FILE.search(base):
        yield 1, "file name refers to an experiment: " + base


def rule_banner(src):
    for no, _, c in _comments(src, docstrings=False):
        if BANNER.match(banner_body(src.ftype, c)):
            yield no, "banner comment"


def rule_emphasis(src):
    for no, _, c in _comments(src):
        found = [m.group() for m in EMPHASIS.finditer(c.text)]
        if found:
            yield no, "emphasis marker in comment: " + _fmt(found)


def rule_ai_vocabulary(src):
    for no, _, c in _comments(src):
        found = _matches(AI_WORDS, c.text)
        if found:
            yield no, "stock wording in comment: " + _fmt(found)


def rule_todo_owner(src):
    for no, _, c in _comments(src):
        found = [m.group() for m in TODO.finditer(c.text)]
        if found:
            yield no, "%s without an owner; write %s(name): ..." % (found[0], found[0])


def rule_placeholder(src):
    for no, text in _prose(src):
        found = _matches(PLACEHOLDERS, text)
        if found:
            yield no, "placeholder text: " + _fmt(f.lower() for f in found)


def rule_trailing_whitespace(src):
    for no, ln in enumerate(src.lines, 1):
        if ln.raw != ln.raw.rstrip(" \t"):
            if src.ftype == "md" and ln.raw.endswith("  ") and not ln.raw.endswith("   ") and ln.raw.strip():
                continue
            yield no, "trailing whitespace"


def rule_tab(src):
    for no, ln in enumerate(src.lines, 1):
        if "\t" in ln.raw:
            yield no, "tab character"


def rule_final_newline(src):
    if src.text and not src.text.endswith("\n"):
        yield max(len(src.lines), 1), "no newline at end of file"


def rule_file_length(src, options):
    # Code, comment and blank lines all count, and so does a last line without a newline.
    n = len(src.lines)
    if n > options["max"]:
        yield 1, "file has %d lines; the limit is %d" % (n, options["max"])
