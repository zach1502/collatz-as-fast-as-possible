"""commented-out-code: comment lines that match a code pattern of their language
and do not read as prose; example and fenced blocks are skipped."""

import re

from .mechanical import _comments
from .restating import STOPWORDS


CODE_IN_COMMENT = {
    "c": [
        re.compile(r"(?:\w\s*\([^()]*\)|\b(?:break|continue|return\b.*)|[\w\]]\s*(?:[+\-*/%&|^]?=)\s*[^=].*"
                   r"|\+\+|--)\s*;\s*$"),
        re.compile(r"^\s*[A-Za-z_][\w:<>,]*[\s*&]+[A-Za-z_]\w*(?:\[\w*\])?\s*(?:=.*)?;\s*$"),
        re.compile(r"\{\s*$"),
        re.compile(r"^\s*\}\s*(?:else\b.*)?;?\s*$"),
        re.compile(r"^\s*(?:if|for|while|switch)\s*\(.*\)\s*\{?\s*$"),
        re.compile(r"^\s*#\s*(?:include|define|if|ifdef|ifndef|endif)\b"),
    ],
    "py": [
        re.compile(r"^\s*(?:if|elif|while)\s+[^:]*(?:==|!=|<=|>=|[<>]|\w\(|\w\[)[^:]*:\s*$"),
        re.compile(r"^\s*for\s+\S.*\s+in\s+\S.*:\s*$"),
        re.compile(r"^\s*(?:else|try|finally)\s*:\s*$"),
        re.compile(r"^\s*(?:def|class)\s+\w+.*:\s*$"),
        re.compile(r"^\s*except\b.*:\s*$"),
        re.compile(r"^\s*(?:import\s+[\w.]+(?:\s+as\s+\w+)?|from\s+[\w.]+\s+import\s+[\w*, ]+)\s*$"),
        re.compile(r"^\s*print\(.*\)\s*$"),
    ],
    "sh": [
        re.compile(r"^\s*(?:if|elif|while|until)\s+.*;\s*(?:then|do)\s*$"),
        re.compile(r"^\s*for\s+\w+\s+in\b.*;\s*do\s*$"),
        re.compile(r"^\s*(?:fi|done|esac|then|else)\s*$"),
        re.compile(r"^\s*(?:export\s+|local\s+)?[A-Za-z_][A-Za-z0-9_]*=(?:\"[^\"]*\"|'[^']*'|\S*)\s*$"),
    ],
    "ps1": [
        re.compile(r"^\s*\$[\w:]+\s*=\s*\S"),
        re.compile(r"^\s*(?:if|elseif|foreach|for|while|switch)\s*\(.*\)\s*\{?\s*$"),
        re.compile(r"\{\s*$"),
        re.compile(r"^\s*\}\s*$"),
    ],
    "cmake": [
        re.compile(r"^\s*(?:set|unset|if|elseif|else|endif|foreach|endforeach|while|endwhile|"
                   r"function|endfunction|macro|endmacro|add_\w+|target_\w+|option|message|"
                   r"find_\w+|include|list|string|file|set_\w+|get_\w+)\s*\(.*\)?\s*$"),
    ],
    "sql": [
        re.compile(r";\s*$"),
        re.compile(r"^\s*(?:SELECT|INSERT INTO|UPDATE|DELETE FROM|CREATE (?:TABLE|INDEX)|ALTER TABLE|DROP)\b"),
    ],
}
CODE_IN_COMMENT["ts"] = CODE_IN_COMMENT["c"][:5] + [
    re.compile(r"^\s*(?:const|let|var)\s+[\w$]+\s*=\s*\S"),
    re.compile(r"^\s*import\s+.*\bfrom\s+[\"']"),
    re.compile(r"\bconsole\.log\("),
]
EXAMPLE_START = re.compile(r"(?i)\b(?:usage|example|examples|e\.g\.)\s*:?\s*$|^\s*(?:usage|examples?)\b")
EXAMPLE_FENCE = re.compile(r"^\s*(?:```|~~~|[@\\]code\b|[@\\]endcode\b)")


def rule_commented_out_code(src):
    patterns = CODE_IN_COMMENT.get(src.ftype)
    if not patterns:
        return
    in_example = False
    in_fence = False
    prev_seg = None
    prev_no = -2
    for no, _ln, c in _comments(src, docstrings=False):
        if c.trailing:
            continue
        if c.seg != prev_seg and no != prev_no + 1:
            in_example = in_fence = False
        prev_seg, prev_no = c.seg, no
        text = c.text
        if EXAMPLE_FENCE.match(text):
            in_fence = not in_fence
            continue
        if in_fence or in_example:
            continue
        if EXAMPLE_START.search(text):
            in_example = True
            continue
        if any(p.search(text) for p in patterns) and not looks_like_prose(text):
            yield no, "comment looks like commented-out code"


def looks_like_prose(text):
    """Two or more English function words, or four plain words in a row."""
    words = re.findall(r"[A-Za-z]+", text)
    if sum(1 for w in words if w.lower() in STOPWORDS) >= 2:
        return True
    return re.search(r"(?:\b[A-Za-z]{2,} ){3}[A-Za-z]{2,}\b", text) is not None
