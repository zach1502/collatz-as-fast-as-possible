"""config.json: the default paths, path globs and the validated rule settings."""

import json
import os
import re

from .rules import RULE_OPTIONS, RULES
from .source import ALL_TYPES


# The directory above this package (tools/lint), which holds config.json and baseline.json.
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CONFIG = os.path.join(HERE, "config.json")
DEFAULT_BASELINE = os.path.join(HERE, "baseline.json")


def glob_to_regex(pattern):
    """Compile a path glob: ** spans directories, * and ? stay within one.

    A pattern without a slash matches the base name at any depth, as in
    .gitignore; a trailing slash means everything below that directory.
    """
    if pattern.endswith("/"):
        pattern += "**"
    out = [] if "/" in pattern else ["(?:.*/)?"]
    i = 0
    while i < len(pattern):
        if pattern.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif pattern[i] == "*":
            out.append("[^/]*")
            i += 1
        elif pattern[i] == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile("".join(out) + r"\Z")


class PathMatcher:
    def __init__(self, patterns):
        self.regexes = [glob_to_regex(p) for p in patterns]

    def __call__(self, path):
        return any(r.match(path) for r in self.regexes)


RULE_KEYS = {"severity", "types", "allow"}
SEVERITIES = ("error", "warning")


class ConfigError(Exception):
    pass


class Config:
    def __init__(self, data):
        self.enforce = bool(data.get("enforce", False))
        self.exclude = PathMatcher(data.get("exclude", []))
        self.rules = {}
        self.options = {}
        for rule_id, spec in data.get("rules", {}).items():
            if rule_id not in RULES:
                raise ConfigError("unknown rule in config: " + rule_id)
            defaults = RULE_OPTIONS.get(rule_id, {})
            extra = sorted(set(spec) - RULE_KEYS - set(defaults))
            if extra:
                raise ConfigError("rule %s: unknown keys %s" % (rule_id, extra))
            options = {k: spec.get(k, v) for k, v in defaults.items()}
            for k, v in options.items():
                if not isinstance(v, int) or isinstance(v, bool) or v < 1:
                    raise ConfigError("rule %s: %s must be a positive integer" % (rule_id, k))
            severity = spec.get("severity", "error")
            if severity not in SEVERITIES:
                raise ConfigError("rule %s: severity must be error or warning" % rule_id)
            types = spec.get("types", ["*"])
            types = ALL_TYPES if "*" in types else types
            unknown = sorted(set(types) - set(ALL_TYPES))
            if unknown:
                raise ConfigError("rule %s: unknown file types %s" % (rule_id, unknown))
            self.rules[rule_id] = (severity, set(types), PathMatcher(spec.get("allow", [])))
            self.options[rule_id] = options

    @classmethod
    def load(cls, path):
        try:
            with open(path, encoding="utf-8") as f:
                return cls(json.load(f))
        except (OSError, ValueError) as e:
            raise ConfigError("cannot read %s: %s" % (path, e)) from e
