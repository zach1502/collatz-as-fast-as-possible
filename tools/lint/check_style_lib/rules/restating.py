"""restating-comment: a short comment that only repeats the words of the code it
labels. STOPWORDS, REASON_WORDS and WORD serve the other heuristics too."""

import re

from .mechanical import EXPERIMENT_REFS, IDENT, _matches


STOPWORDS = set("""
a an the of to in on at by for from with without into onto over under as and or nor
is are was were be been being am it its this that these those there here we our us
you your i me my they them their then than if when which who what where how do does
did done has have had will would can could should may might shall not no all any each
every some now just also very such up down out off again both more most other same
""".split())
REASON_WORDS = set("""
because since so otherwise must avoid avoids keep keeps until unless why invariant see
lemma bytes words mod instead but only never cannot
""".split())
OP_WORDS = [
    (re.compile(r"\+\+|\+=\s*1\b|\b(\w+)\s*=\s*\1\s*\+\s*1\b"), ("increment",)),
    (re.compile(r"--|-=\s*1\b"), ("decrement",)),
    (re.compile(r"\breturn\b"), ("return",)),
    (re.compile(r"\b(?:new|make_unique|make_shared|malloc|calloc)\b"), ("create", "allocate", "new")),
    (re.compile(r"\b(?:delete|free)\b"), ("delete", "free", "release")),
    (re.compile(r"(?<![=!<>+\-*/%&|^])=(?!=)"), ("set", "assign")),
    (re.compile(r"\bif\b"), ("check", "if")),
    (re.compile(r"\b(?:for|while|foreach)\b"), ("loop", "iterate")),
    (re.compile(r"\b(?:throw|raise)\b"), ("throw", "raise", "error")),
    (re.compile(r"\b(?:printf|fprintf|puts|print|cout|cerr|echo|log)\b"), ("print", "log", "output")),
    (re.compile(r"#\s*include\b|\bimport\b"), ("include", "import")),
]
WORD = re.compile(r"[A-Za-z][A-Za-z0-9_']*")
NOTATION = re.compile(r"[0-9^<>=\[\]|*/+%(){}`]|--")


def split_identifier(tok):
    words = []
    for part in tok.split("_"):
        words += re.findall(r"[A-Z]+(?=[A-Z][a-z])|[A-Z]?[a-z]+|[A-Z]+|\d+", part)
    return [w.lower() for w in words]


def stem(w):
    if len(w) > 4 and w.endswith("ies"):
        return w[:-3] + "y"
    if len(w) > 5 and w.endswith("ing"):
        return w[:-3]
    if len(w) > 4 and w.endswith("ed"):
        return w[:-2]
    if len(w) > 3 and w.endswith("s") and not w.endswith("ss"):
        return w[:-1]
    return w


def _word_in(w, pool):
    if w in pool:
        return True
    if len(w) < 4:
        return False
    return any(len(p) >= 4 and abs(len(p) - len(w)) <= 3 and (p.startswith(w) or w.startswith(p))
               for p in pool)


def restates(comment, code):
    """True when a short comment only repeats the words of the code it labels."""
    if NOTATION.search(comment):
        return False
    raw = WORD.findall(comment)
    if not raw or len(raw) > 7:
        return False
    lower = [w.lower().strip("'") for w in raw]
    if any(w in REASON_WORDS for w in lower) or _matches(EXPERIMENT_REFS, comment):
        return False
    content = set()
    for tok in raw:
        for w in split_identifier(tok):
            if w not in STOPWORDS:
                content.add(stem(w))
    if not content:
        return False
    pool = set()
    for tok in IDENT.findall(code):
        pool.update(stem(w) for w in split_identifier(tok))
    for pattern, words in OP_WORDS:
        if pattern.search(code):
            pool.update(words)
    if not pool:
        return False
    shared = sum(1 for w in content if _word_in(w, pool))
    jaccard = shared / len(content | pool)
    return jaccard >= 0.6 or shared / len(content) >= 0.8


def _next_code_line(src, index):
    for j in range(index + 1, min(index + 4, len(src.lines))):
        ln = src.lines[j]
        if not ln.raw.strip():
            continue
        if ln.comment_only():
            return None
        return ln.code_text
    return None


def rule_restating_comment(src):
    lines = src.lines
    for i, ln in enumerate(lines):
        if not ln.comments or ln.comments[0].style in ("shebang", "docstring"):
            continue
        c = ln.comments[-1]
        if c.trailing:
            if c.style == "block" and re.match(r"/\*\s*\w+\s*\*/$", c.raw.strip()):
                continue
            if restates(c.text, ln.code_text):
                yield i + 1, "comment restates its line"
            continue
        if not ln.comment_only() or not (c.first and c.last):
            continue
        prev = lines[i - 1] if i > 0 else None
        if prev is not None and prev.comment_only():
            continue
        nxt = lines[i + 1] if i + 1 < len(lines) else None
        if nxt is not None and nxt.comment_only():
            continue
        code = _next_code_line(src, i)
        if code and restates(c.text, code):
            yield i + 1, "comment restates the next line"
