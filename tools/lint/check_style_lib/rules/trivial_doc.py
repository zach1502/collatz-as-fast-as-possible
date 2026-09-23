"""trivial-doc: a doc comment or docstring on a function of at most two
statements, from a brace scan of C-family and JS/TS code and the ast of Python."""

import ast
import re

from .restating import REASON_WORDS, WORD


CONTROL_START = re.compile(r"^\s*(?:if|for|while|switch|catch|do|else|return|case|namespace|"
                           r"class|struct|union|enum|typedef|using|extern)\b")
TS_FUNCTION = re.compile(
    r"^\s*(?:export\s+)?(?:default\s+)?(?:async\s+)?function\b"
    r"|^\s*(?:(?:public|private|protected|static|readonly|async|override|get|set)\s+)*"
    r"[A-Za-z_$][\w$]*\s*(?:<[^>]*>)?\s*\(")
TS_ARROW = re.compile(
    r"^\s*(?:export\s+)?(?:const|let|var)\s+[A-Za-z_$][\w$]*\s*(?::[^=]+)?=\s*(?:async\s+)?"
    r"(?:\([^)]*\)|[A-Za-z_$][\w$]*)\s*(?::\s*[^=]+?)?\s*=>")
TS_ARROW_HEAD = re.compile(r"\s*(?:async\s+)?(?:\([^)]*\)|[A-Za-z_$][\w$]*)\s*(?::\s*[^=]+?)?\s*=>")


def _expression_lines(text):
    """Lines of an arrow function's expression body: up to its `;`, or up to the
    bracket that closes an enclosing call."""
    depth = 0
    for k, ch in enumerate(text):
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
            if depth < 0:
                return text.count("\n", 0, k) + 1
        elif ch == ";" and depth == 0:
            return text.count("\n", 0, k) + 1
    return text.count("\n") + 1


def _type_brace(text, k):
    """True when the `{` at text[k] opens an object type in a TS return type,
    as in `f(): { a: number } {`, not the function body."""
    j = k - 1
    while j >= 0 and text[j] in " \t\r\n":
        j -= 1
    return j >= 0 and text[j] in ":|&<,"


def _closing_brace(text, k):
    depth = 0
    for j in range(k, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return j
    return len(text) - 1


def _body_statements(src, start, max_lines=40):
    """Statement count and line span of the function whose signature starts at
    line index `start`, or None when the code there is not a function body."""
    lines = src.lines[start:start + max_lines]
    text = "\n".join(ln.code_text for ln in lines)
    if src.ftype == "ts":
        m = TS_ARROW.match(text)
        if m:
            rest = text[m.end():]
            inner = TS_ARROW_HEAD.match(rest)
            while inner:  # a curried arrow: (a) => (b) => ...
                rest = rest[inner.end():]
                inner = TS_ARROW_HEAD.match(rest)
            if not rest.lstrip().startswith("{"):
                return 1, _expression_lines(rest)
        elif not TS_FUNCTION.match(text) or CONTROL_START.match(text):
            return None
    elif CONTROL_START.match(text) and not text.lstrip().startswith("template"):
        return None
    depth = 0
    open_at = None
    k = 0
    while k < len(text):
        ch = text[k]
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        elif ch == ";" and depth == 0:
            return None
        elif ch == "{" and depth == 0:
            if src.ftype == "ts" and _type_brace(text, k):
                k = _closing_brace(text, k) + 1
                continue
            open_at = k
            break
        k += 1
    if open_at is None:
        return None
    sig = text[:open_at]
    if "(" not in sig:
        return None
    if re.search(r"(?<![=!<>])=(?!=)", sig.split("(")[0]) and "operator" not in sig and "](" not in sig.replace(" ", ""):
        return None
    braces = 0
    parens = 0
    statements = 0
    for k in range(open_at, len(text)):
        ch = text[k]
        if ch == "(":
            parens += 1
        elif ch == ")":
            parens -= 1
        elif ch == ";" and parens == 0:
            statements += 1
        elif ch == "{":
            braces += 1
        elif ch == "}":
            braces -= 1
            if braces == 0:
                span = text.count("\n", open_at, k) + 1
                return statements, span
    return None


def rule_trivial_doc(src):
    if src.ftype == "py":
        yield from _trivial_docstrings(src)
        return
    lines = src.lines
    i = 0
    while i < len(lines):
        ln = lines[i]
        if not (ln.comment_only() and all(c.style == "doc" for c in ln.comments)):
            i += 1
            continue
        start = i
        while i < len(lines) and lines[i].comment_only() and all(c.style == "doc" for c in lines[i].comments):
            i += 1
        j = i
        while j < len(lines) and not lines[j].raw.strip():
            j += 1
        if j >= len(lines) or lines[j].comment_only():
            continue
        words = {w.lower() for k in range(start, i) for c in lines[k].comments for w in WORD.findall(c.text)}
        if words & REASON_WORDS:
            continue
        body = _body_statements(src, j)
        if body and body[0] <= 2 and body[1] <= 6:
            yield start + 1, "doc comment on a function whose body has %d statement%s" % (
                body[0], "" if body[0] == 1 else "s")


def _trivial_docstrings(src):
    try:
        tree = ast.parse(src.text)
    except (SyntaxError, ValueError):
        return
    for node in ast.walk(tree):
        if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        body = node.body
        if not (body and isinstance(body[0], ast.Expr) and isinstance(body[0].value, ast.Constant)
                and isinstance(body[0].value.value, str)):
            continue
        doc = body[0].value.value
        rest = body[1:]
        if len(rest) > 2 or {w.lower() for w in WORD.findall(doc)} & REASON_WORDS:
            continue
        span = (node.end_lineno or node.lineno) - (body[0].end_lineno or body[0].lineno)
        if span <= 3:
            n = len(rest)
            yield body[0].lineno, "docstring on a function whose body has %d statement%s" % (
                n, "" if n == 1 else "s")
