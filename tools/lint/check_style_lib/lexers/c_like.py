"""The lexer for C, C++, OpenCL, JS, TS and CSS: raw strings, digit separators,
template literals and regex literals."""

import re

from . import _quoted_end


IDENT_CHAR = re.compile(r"[A-Za-z0-9_$]")


def _raw_string(text, i, floor):
    """(start, end) of a C++ raw string whose quote is at text[i], else None."""
    if i < 1 or text[i - 1] != "R":
        return None
    p = i - 1
    if text[max(p - 2, 0):p] == "u8":
        p -= 2
    elif p >= 1 and text[p - 1] in "uUL":
        p -= 1
    if p > 0 and IDENT_CHAR.match(text[p - 1]):
        return None
    m = re.match(r'([^()\\\s]{0,16})\(', text[i + 1:i + 18])
    if not m:
        return None
    close = ")" + m.group(1) + '"'
    e = text.find(close, i + 1 + len(m.group(0)))
    return max(p, floor), (len(text) if e < 0 else e + len(close))


def _digit_separator(text, i):
    """True for the quote in 1'000'000 (C++14), which is not a char literal."""
    k = i - 1
    while k >= 0 and (text[k].isalnum() or text[k] in "_.'"):
        k -= 1
    word = text[k + 1:i]
    return bool(word) and word[0].isdigit() and i + 1 < len(text) and text[i + 1].isalnum()


def _template_end(text, i):
    """Index just past a JS template literal starting at text[i]."""
    n = len(text)
    j = i + 1
    while j < n:
        c = text[j]
        if c == "\\":
            j += 2
        elif c == "`":
            return j + 1
        elif c == "$" and text.startswith("{", j + 1):
            j += 2
            depth = 1
            while j < n and depth:
                c = text[j]
                if c in "'\"":
                    j = _quoted_end(text, j, c)
                    continue
                if c == "`":
                    j = _template_end(text, j)
                    continue
                if c == "{":
                    depth += 1
                elif c == "}":
                    depth -= 1
                j += 1
        else:
            j += 1
    return n


REGEX_PREFIX_WORDS = {"return", "typeof", "case", "do", "else", "in", "of", "new",
                      "delete", "void", "throw", "instanceof", "yield", "await"}


def _regex_allowed(text, i):
    """Whether a slash at text[i] can start a JS regex literal (not a division)."""
    k = i - 1
    while k >= 0 and text[k] in " \t\r\n":
        k -= 1
    if k < 0:
        return True
    c = text[k]
    if c in "(,=:[!&|?{};+-*%<>~^":
        return True
    if IDENT_CHAR.match(c):
        s = k
        while s >= 0 and IDENT_CHAR.match(text[s]):
            s -= 1
        return text[s + 1:k + 1] in REGEX_PREFIX_WORDS
    return False


def _regex_end(text, i):
    n = len(text)
    j = i + 1
    in_class = False
    while j < n:
        c = text[j]
        if c == "\\":
            j += 2
            continue
        if c == "\n":
            return None
        if in_class:
            in_class = c != "]"
        elif c == "[":
            in_class = True
        elif c == "/":
            j += 1
            while j < n and text[j].isalpha():
                j += 1
            return j
        j += 1
    return None


_C_START = {
    "c": re.compile(r"//|/\*|\"|'"),
    "ts": re.compile(r"//|/\*|\"|'|`|/"),
    "css": re.compile(r"/\*|\"|'"),
}


def lex_c_like(text, lang="c"):
    segs = []
    pat = _C_START[lang]
    n = len(text)
    i = 0
    while True:
        m = pat.search(text, i)
        if m is None:
            return segs
        j = m.start()
        tok = m.group()
        if tok == "//":
            e = text.find("\n", j)
            e = n if e < 0 else e
            doc = (text.startswith("///", j) and not text.startswith("////", j)) or text.startswith("//!", j)
            segs.append(("comment", j, e, "doc" if doc else "line"))
            i = e
        elif tok == "/*":
            e = text.find("*/", j + 2)
            e = n if e < 0 else e + 2
            doc = (text.startswith("/**", j) and not text.startswith("/***", j)
                   and not text.startswith("/**/", j)) or text.startswith("/*!", j)
            segs.append(("comment", j, e, "doc" if doc else "block"))
            i = e
        elif tok == '"':
            raw = _raw_string(text, j, i) if lang == "c" else None
            if raw:
                segs.append(("string", raw[0], raw[1], "raw"))
                i = raw[1]
            else:
                e = _quoted_end(text, j, '"')
                segs.append(("string", j, e, "str"))
                i = e
        elif tok == "'":
            if lang == "c" and _digit_separator(text, j):
                i = j + 1
                continue
            e = _quoted_end(text, j, "'")
            segs.append(("string", j, e, "str"))
            i = e
        elif tok == "`":
            e = _template_end(text, j)
            segs.append(("string", j, e, "str"))
            i = e
        else:
            e = _regex_end(text, j) if _regex_allowed(text, j) else None
            if e is None:
                i = j + 1
            else:
                segs.append(("string", j, e, "regex"))
                i = e
