"""The lexers for TOML, SQL and HTML."""

import re

from . import _quoted_end


_HASH_START = re.compile(r"(?P<hash>(?:^|(?<=\s))#)|(?P<sq>')|(?P<dq>\")", re.M)


def lex_hash(text):
    """TOML: # comments, single-line quoted strings."""
    segs = []
    n = len(text)
    i = 0
    while True:
        m = _HASH_START.search(text, i)
        if m is None:
            return segs
        j = m.start()
        if m.lastgroup == "hash":
            e = text.find("\n", j)
            e = n if e < 0 else e
            segs.append(("comment", j, e, "line"))
        else:
            q = m.group()
            e = _quoted_end(text, j, q, escape="\\" if q == '"' else "\0")
            segs.append(("string", j, e, "str"))
        i = e


_SQL_START = re.compile(r"--|/\*|'|\"")


def lex_sql(text):
    segs = []
    n = len(text)
    i = 0
    while True:
        m = _SQL_START.search(text, i)
        if m is None:
            return segs
        j = m.start()
        tok = m.group()
        if tok == "--":
            e = text.find("\n", j)
            e = n if e < 0 else e
            segs.append(("comment", j, e, "line"))
        elif tok == "/*":
            e = text.find("*/", j + 2)
            e = n if e < 0 else e + 2
            segs.append(("comment", j, e, "block"))
        else:
            e = _quoted_end(text, j, tok, escape="\0", multiline=True, doubled=True)
            segs.append(("string", j, e, "str"))
        i = e


def lex_html(text):
    segs = []
    for m in re.finditer(r"<!--.*?(?:-->|\Z)", text, re.S):
        segs.append(("comment", m.start(), m.end(), "block"))
    return segs
