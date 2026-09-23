"""File types, the lexer for each, and Source: a file split into lines with the
code, comments and strings of each line."""

import bisect
import os
import re

from .lexers.c_like import lex_c_like
from .lexers.data import lex_hash, lex_html, lex_sql
from .lexers.scripting import lex_cmake, lex_powershell, lex_python, lex_shell


EXT_TYPES = {
    ".c": "c", ".cc": "c", ".cpp": "c", ".cxx": "c", ".h": "c", ".hh": "c",
    ".hpp": "c", ".hxx": "c", ".cl": "c",
    ".ts": "ts", ".mts": "ts", ".js": "ts", ".mjs": "ts", ".cjs": "ts", ".jsonc": "ts",
    ".css": "css", ".html": "html",
    ".py": "py",
    ".sh": "sh", ".bash": "sh",
    ".ps1": "ps1", ".psm1": "ps1", ".psd1": "ps1",
    ".cmd": "cmd",
    ".cmake": "cmake",
    ".md": "md",
    ".toml": "toml",
    ".sql": "sql",
}
NAME_TYPES = {"CMakeLists.txt": "cmake"}
ALL_TYPES = sorted(set(EXT_TYPES.values()))
C_LIKE = ("c", "ts", "css")


def file_type(path, head=""):
    """Type name used to pick the lexer, or None for files that are not scanned."""
    base = os.path.basename(path)
    if base in NAME_TYPES:
        return NAME_TYPES[base]
    if base.endswith(".in"):
        base = base[:-3]
    ext = os.path.splitext(base)[1].lower()
    if ext:
        return EXT_TYPES.get(ext)
    if head.startswith("#!"):
        first = head.split("\n", 1)[0]
        if re.search(r"\b(?:ba|z|da)?sh\b", first):
            return "sh"
        if "python" in first:
            return "py"
    return None


LEXERS = {
    "c": lambda t: lex_c_like(t, "c"),
    "ts": lambda t: lex_c_like(t, "ts"),
    "css": lambda t: lex_c_like(t, "css"),
    "html": lex_html,
    "py": lex_python,
    "sh": lex_shell,
    "ps1": lex_powershell,
    "cmake": lex_cmake,
    "toml": lex_hash,
    "sql": lex_sql,
}


class Comment:
    """One line of one comment: its raw text, the text without markers, and style."""

    __slots__ = ("style", "raw", "text", "first", "last", "seg", "trailing")

    def __init__(self, style, raw, first, last, seg):
        self.style, self.raw, self.first, self.last, self.seg = style, raw, first, last, seg
        self.text = ""
        self.trailing = False


class Line:
    __slots__ = ("raw", "code", "comments", "strings", "kind")

    def __init__(self, raw):
        self.raw = raw
        self.code = []
        self.comments = []
        self.strings = []
        self.kind = "code"

    @property
    def code_text(self):
        return "".join(self.code)

    def comment_only(self):
        return bool(self.comments) and not self.strings and not "".join(self.code).strip()


_FENCE = re.compile(r"^ {0,3}(```|~~~)")


class Source:
    """A file split into lines, each with its code text, comments and strings."""

    def __init__(self, path, text, ftype):
        self.path, self.text, self.ftype = path, text, ftype
        raw_lines = text.split("\n")
        if raw_lines and raw_lines[-1] == "":
            raw_lines.pop()
        self.lines = [Line(r[:-1] if r.endswith("\r") else r) for r in raw_lines]
        if ftype == "md":
            self._split_markdown()
            return
        self.line_starts = [0] + [m.end() for m in re.finditer("\n", text)]
        lexer = LEXERS.get(ftype)
        self._assign(lexer(text) if lexer else [])

    def _split_markdown(self):
        fenced = False
        for ln in self.lines:
            if _FENCE.match(ln.raw):
                ln.kind = "code"
                fenced = not fenced
            else:
                ln.kind = "code" if fenced else "prose"
            ln.code.append(ln.raw)

    def _chunks(self, start, end):
        li = bisect.bisect_right(self.line_starts, start) - 1
        pos = start
        while pos < end and li < len(self.lines):
            eol = self.line_starts[li + 1] - 1 if li + 1 < len(self.line_starts) else len(self.text)
            yield li, pos, min(end, eol)
            li += 1
            pos = eol + 1

    def _assign(self, segs):
        pos = 0
        for seg_id, (kind, s, e, style) in enumerate(sorted(segs, key=lambda x: x[1])):
            if s < pos:
                continue
            for li, a, b in self._chunks(pos, s):
                self.lines[li].code.append(self.text[a:b])
            chunks = list(self._chunks(s, e))
            for k, (li, a, b) in enumerate(chunks):
                piece = self.text[a:b]
                ln = self.lines[li]
                if kind == "string":
                    ln.strings.append(piece)
                    ln.code.append('""')
                else:
                    c = Comment(style, piece.rstrip("\r"), k == 0, k == len(chunks) - 1, seg_id)
                    c.text = strip_markers(self.ftype, c)
                    ln.comments.append(c)
                    ln.code.append(" ")
            pos = e
        for li, a, b in self._chunks(pos, len(self.text)):
            self.lines[li].code.append(self.text[a:b])
        for ln in self.lines:
            if ln.comments and "".join(ln.code).strip():
                for c in ln.comments:
                    c.trailing = True


def strip_markers(ftype, c):
    """Comment text without its comment markers."""
    s = c.raw
    if c.style == "docstring":
        if c.first:
            s = re.sub(r"^\s*[rRbBuUfF]{0,2}(\"\"\"|'''|\"|')", "", s)
        if c.last:
            s = re.sub(r"(\"\"\"|'''|\"|')\s*$", "", s)
        return s.strip()
    if ftype in C_LIKE:
        if s.lstrip().startswith("//"):
            return re.sub(r"^\s*//+!?<?", "", s).strip()
        if c.first:
            s = re.sub(r"^\s*/\*+!?<?", "", s)
        else:
            s = re.sub(r"^\s*\*+(?!/)", "", s)
        if c.last:
            s = re.sub(r"\*+/\s*$", "", s)
        return s.strip()
    if ftype == "sql":
        if s.lstrip().startswith("--"):
            return re.sub(r"^\s*--+", "", s).strip()
        if c.first:
            s = re.sub(r"^\s*/(?:-+!?|\*+)", "", s)
        if c.last:
            s = re.sub(r"(?:-|\*)+/\s*$", "", s)
        return s.strip()
    if ftype == "html":
        return re.sub(r"<!--|-->", "", s).strip()
    if c.style == "block" and ftype == "ps1":
        if c.first:
            s = re.sub(r"^\s*<#", "", s)
        if c.last:
            s = re.sub(r"#>\s*$", "", s)
        return s.strip()
    if c.style == "block" and ftype == "cmake":
        if c.first:
            s = re.sub(r"^\s*#\[=*\[", "", s)
        if c.last:
            s = re.sub(r"\]=*\]\s*$", "", s)
        return s.strip()
    return re.sub(r"^\s*#+", "", s).strip()


def banner_body(ftype, c):
    """Comment text after one marker, for the banner test (keeps repeated markers)."""
    s = c.raw.lstrip()
    if c.style == "docstring":
        return ""
    for marker in ("//", "/*", "<#", "--", "/-", "#", "<!--"):
        if s.startswith(marker) and (c.first or marker in ("//", "--", "#")):
            return s[len(marker):]
    if ftype in C_LIKE and s.startswith("*"):
        return s[1:]
    return s
