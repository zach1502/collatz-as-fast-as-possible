"""The lexers for sh/bash, PowerShell, CMake and Python."""

import ast
import bisect
import re

from . import _quoted_end


_SH_START = re.compile(
    r"(?P<esc>\\.)|(?P<ansi>\$')|(?P<sq>')|(?P<dq>\")"
    r"|(?P<heredoc><<(?P<dash>-?)[ \t]*(?P<hq>['\"]?)(?P<hd>[A-Za-z_][A-Za-z0-9_]*)(?P=hq))"
    r"|(?P<hash>(?:^|(?<=[\s;&|()<>]))#)",
    re.M,
)


def _heredoc_end(text, pos, delim, strip_tabs):
    lead = r"\t*" if strip_tabs else ""
    m = re.compile(r"^" + lead + re.escape(delim) + r"[ \t]*\r?$", re.M).search(text, pos)
    if m is None:
        return len(text)
    e = text.find("\n", m.end())
    return len(text) if e < 0 else e


def lex_shell(text):
    """sh/bash: # starts a comment only at the start of a word."""
    segs = []
    n = len(text)
    i = 0
    pending = []
    pending_eol = -1
    while True:
        m = _SH_START.search(text, i)
        if pending and (m is None or m.start() > pending_eol):
            pos = pending_eol + 1
            for delim, tabs in pending:
                if pos >= n:
                    break
                e = _heredoc_end(text, pos, delim, tabs)
                segs.append(("string", pos, e, "heredoc"))
                pos = e + 1
            pending = []
            i = min(pos, n)
            continue
        if m is None:
            return segs
        j = m.start()
        kind = m.lastgroup
        if kind == "esc":
            i = m.end()
        elif kind == "ansi":
            e = _quoted_end(text, j + 1, "'", multiline=True)
            segs.append(("string", j, e, "str"))
            i = e
        elif kind == "sq":
            e = text.find("'", j + 1)
            e = n if e < 0 else e + 1
            segs.append(("string", j, e, "str"))
            i = e
        elif kind == "dq":
            e = _quoted_end(text, j, '"', multiline=True)
            segs.append(("string", j, e, "str"))
            i = e
        elif kind == "heredoc":
            line_start = text.rfind("\n", 0, j) + 1
            prefix = text[line_start:j]
            if prefix.count("((") > prefix.count("))"):
                i = j + 2
                continue
            eol = text.find("\n", j)
            pending_eol = n if eol < 0 else eol
            pending.append((m.group("hd"), m.group("dash") == "-"))
            i = m.end()
        else:
            e = text.find("\n", j)
            e = n if e < 0 else e
            line_start = text.rfind("\n", 0, j) + 1
            shebang = text.startswith("#!", j) and not text[line_start:j].strip()
            segs.append(("comment", j, e, "shebang" if shebang else "line"))
            i = e


_PS_START = re.compile(
    r"(?P<block><\#)|(?P<here>@(?P<hq>['\"])[ \t]*\r?$)|(?P<sq>')|(?P<dq>\")|(?P<esc>`.)"
    r"|(?P<hash>(?:^|(?<=[\s;(){}|,=]))\#)",
    re.M,
)


def lex_powershell(text):
    segs = []
    n = len(text)
    i = 0
    while True:
        m = _PS_START.search(text, i)
        if m is None:
            return segs
        j = m.start()
        kind = m.lastgroup
        if kind == "block":
            e = text.find("#>", j + 2)
            e = n if e < 0 else e + 2
            segs.append(("comment", j, e, "block"))
        elif kind == "here":
            close = re.compile(r"^[ \t]*" + m.group("hq") + "@", re.M).search(text, m.end())
            e = n if close is None else close.end()
            segs.append(("string", j, e, "str"))
        elif kind == "sq":
            e = _quoted_end(text, j, "'", escape="\0", multiline=True, doubled=True)
            segs.append(("string", j, e, "str"))
        elif kind == "dq":
            e = _quoted_end(text, j, '"', escape="`", multiline=True, doubled=True)
            segs.append(("string", j, e, "str"))
        elif kind == "esc":
            e = m.end()
        else:
            e = text.find("\n", j)
            e = n if e < 0 else e
            shebang = j == 0 and text.startswith("#!")
            segs.append(("comment", j, e, "shebang" if shebang else "line"))
        i = e


_CMAKE_START = re.compile(r"(?P<bc>#\[(?P<beq>=*)\[)|(?P<hash>#)|(?P<ba>\[(?P<aeq>=*)\[)|(?P<dq>\")|(?P<esc>\\.)")


def lex_cmake(text):
    segs = []
    n = len(text)
    i = 0
    while True:
        m = _CMAKE_START.search(text, i)
        if m is None:
            return segs
        j = m.start()
        kind = m.lastgroup
        if kind in ("bc", "ba"):
            eq = m.group("beq") if kind == "bc" else m.group("aeq")
            close = "]" + eq + "]"
            e = text.find(close, m.end())
            e = n if e < 0 else e + len(close)
            segs.append(("comment", j, e, "block") if kind == "bc" else ("string", j, e, "str"))
        elif kind == "hash":
            e = text.find("\n", j)
            e = n if e < 0 else e
            segs.append(("comment", j, e, "line"))
        elif kind == "dq":
            e = _quoted_end(text, j, '"', multiline=True)
            segs.append(("string", j, e, "str"))
        else:
            e = m.end()
        i = e


_PY_START = re.compile(
    r"(?P<tq>(?<![A-Za-z0-9_])[rRbBuUfF]{0,2}(?:\"\"\"|'''))"
    r"|(?P<q>(?<![A-Za-z0-9_])[rRbBuUfF]{0,2}[\"'])|(?P<hash>#)"
)


def _python_docstrings(text):
    """Start lines of string-only statements (docstrings and bare string blocks)."""
    try:
        tree = ast.parse(text)
    except (SyntaxError, ValueError):
        return set()
    starts = set()
    for node in ast.walk(tree):
        if (isinstance(node, ast.Expr) and isinstance(node.value, ast.Constant)
                and isinstance(node.value.value, str)):
            starts.add((node.lineno, node.col_offset))
    return starts


def lex_python(text):
    segs = []
    docs = _python_docstrings(text)
    line_starts = [0] + [m.end() for m in re.finditer("\n", text)]
    n = len(text)
    i = 0
    while True:
        m = _PY_START.search(text, i)
        if m is None:
            return segs
        j = m.start()
        if m.lastgroup == "hash":
            e = text.find("\n", j)
            e = n if e < 0 else e
            style = "shebang" if j == 0 and text.startswith("#!") else "line"
            segs.append(("comment", j, e, style))
            i = e
            continue
        tok = m.group()
        if m.lastgroup == "tq":
            q = tok[-3:]
            k = m.end()
            while True:
                k = text.find(q, k)
                if k < 0:
                    e = n
                    break
                if _escaped(text, k):
                    k += 1
                    continue
                e = k + 3
                break
        else:
            e = _quoted_end(text, m.end() - 1, tok[-1])
        line = bisect.bisect_right(line_starts, j)
        col = j - line_starts[line - 1]
        if (line, col) in docs:
            segs.append(("comment", j, e, "docstring"))
        else:
            segs.append(("string", j, e, "str"))
        i = e


def _escaped(text, k):
    b = 0
    while k - 1 - b >= 0 and text[k - 1 - b] == "\\":
        b += 1
    return b % 2 == 1
