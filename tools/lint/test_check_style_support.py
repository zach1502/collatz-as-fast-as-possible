"""The setup that the test_check_style_*.py tests share: the shipped config and
the line numbers of one rule's findings."""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_style_lib.config import DEFAULT_CONFIG, Config  # noqa: E402
from check_style_lib.scan import scan_text  # noqa: E402


def make_config(enforce=False):
    """The shipped config with every allow list emptied, so fixture paths are checked."""
    with open(DEFAULT_CONFIG, encoding="utf-8") as f:
        data = json.load(f)
    for spec in data["rules"].values():
        spec["allow"] = []
    data["enforce"] = enforce
    return data


CONFIG = Config(make_config())
SHIPPED = Config.load(DEFAULT_CONFIG)


def lines(path, text, rule, config=CONFIG):
    return [f.line for f in scan_text(path, text, config) if f.rule == rule]
