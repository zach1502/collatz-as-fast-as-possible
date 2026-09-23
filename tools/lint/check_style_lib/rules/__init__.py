"""Rules. Each yields (line number, message) for one Source.

mechanical: fixed patterns, whitespace and file length. restating, trivial_doc
and commented_code: the comment heuristics. RULES maps each rule id to its
function and description, in the order of --list-rules and the summary.
"""

from .commented_code import rule_commented_out_code
from .mechanical import (
    rule_ai_vocabulary, rule_banner, rule_emphasis, rule_experiment_filename, rule_experiment_identifier,
    rule_experiment_ref, rule_experiment_ref_code, rule_experiment_verdict, rule_file_length,
    rule_final_newline, rule_non_ascii, rule_placeholder, rule_tab, rule_todo_owner, rule_trailing_whitespace,
)
from .restating import rule_restating_comment
from .trivial_doc import rule_trivial_doc


RULES = {
    "non-ascii": (rule_non_ascii, "non-ASCII character"),
    "experiment-ref": (rule_experiment_ref,
                       "experiment ID, wave/round/memo label or experiment-log path in a comment"),
    "experiment-ref-code": (rule_experiment_ref_code, "the same references in strings or code"),
    "experiment-verdict": (rule_experiment_verdict, "KILL/KEEP/DEFERRED-style verdict in a comment"),
    "experiment-identifier": (rule_experiment_identifier, "identifier or env name built from an experiment ID"),
    "experiment-filename": (rule_experiment_filename, "file name built from an experiment ID"),
    "banner": (rule_banner, "banner or separator comment"),
    "emphasis": (rule_emphasis, "NOTE:, IMPORTANT, CRITICAL, WARNING: or 'Note that' in a comment"),
    "ai-vocabulary": (rule_ai_vocabulary, "stock wording or narration in a comment"),
    "restating-comment": (rule_restating_comment, "short comment that repeats the code it labels"),
    "trivial-doc": (rule_trivial_doc, "doc comment or docstring on a function of at most two statements"),
    "commented-out-code": (rule_commented_out_code, "comment that looks like code"),
    "todo-owner": (rule_todo_owner, "TODO, FIXME or XXX without an owner in parentheses"),
    "placeholder": (rule_placeholder, "placeholder text such as 'for now' or 'in a real implementation'"),
    "trailing-whitespace": (rule_trailing_whitespace, "spaces or tabs at the end of a line"),
    "tab": (rule_tab, "tab character in C, C++, OpenCL, TS or JS"),
    "final-newline": (rule_final_newline, "file does not end with a newline"),
    "file-length": (rule_file_length, "file with more lines than its max; comment and blank lines count"),
}
# Options a rule reads from its entry in config.json, with their defaults; each
# is a positive integer. Such a rule takes them as a second argument.
RULE_OPTIONS = {"file-length": {"max": 300}}
