"""Lexers. Each returns a list of (kind, start, end, style) segments covering
the comments and string literals of a file; everything between them is code.
They are approximate (no preprocessor, no full grammar) but must never
report a comment inside a string literal or a string inside a comment.

c_like: C, C++, OpenCL, JS, TS and CSS. scripting: sh, PowerShell, CMake and
Python. data: TOML, SQL and HTML. The three share _quoted_end.
"""


def _quoted_end(text, i, quote, escape="\\", multiline=False, doubled=False):
    """Index just past the string that opens at text[i]."""
    n = len(text)
    j = i + 1
    while j < n:
        c = text[j]
        if c == escape:
            j += 2
            continue
        if c == quote:
            if doubled and text.startswith(quote, j + 1):
                j += 2
                continue
            return j + 1
        if c == "\n" and not multiline:
            return j
        j += 1
    return n
