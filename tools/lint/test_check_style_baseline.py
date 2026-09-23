#!/usr/bin/env python3
"""Unit tests for the baseline and the command line of tools/lint/check_style.py
(CTest lint_checker, label lint).
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_style_lib.baseline import baseline_scope, compare, load_baseline, write_baseline  # noqa: E402
from check_style_lib.cli import main  # noqa: E402
from check_style_lib.scan import Finding, parse_added_lines  # noqa: E402
from test_check_style_support import make_config  # noqa: E402


class Baseline(unittest.TestCase):
    def finding(self, path, rule, line=1):
        return Finding(path, line, rule, "error", "m")

    def test_counts_may_only_go_down(self):
        f = [self.finding("a.cpp", "banner", 1), self.finding("a.cpp", "banner", 2)]
        over, under = compare(f, {"a.cpp": {"banner": 2}})
        self.assertEqual((over, under), ([], []))
        over, _ = compare(f, {"a.cpp": {"banner": 1}})
        self.assertEqual(over, [("a.cpp", "banner", 2, 1)])
        _, under = compare(f[:1], {"a.cpp": {"banner": 2}})
        self.assertEqual(under, [("a.cpp", "banner", 1, 2)])

    def test_update_replaces_only_the_scanned_rules_and_files(self):
        previous = {"a.cpp": {"banner": 2, "file-length": 1}, "c.cpp": {"file-length": 1},
                    "gone.cpp": {"file-length": 1}}
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "baseline.json")
            write_baseline(path, {"b.cpp": {"file-length": 1}}, previous, {"a.cpp", "b.cpp"},
                           {"file-length"}, {"a.cpp", "b.cpp", "c.cpp"})
            self.assertEqual(load_baseline(path), {"a.cpp": {"banner": 2}, "b.cpp": {"file-length": 1},
                                                   "c.cpp": {"file-length": 1}})

    def test_check_compares_only_what_it_scanned(self):
        baseline = {"a.cpp": {"banner": 1, "file-length": 1}, "b.cpp": {"file-length": 1}}
        scoped = baseline_scope(baseline, {"a.cpp"}, {"file-length"})
        self.assertEqual(scoped, {"a.cpp": {"file-length": 1}})
        self.assertEqual(compare([], scoped)[1], [("a.cpp", "file-length", 0, 1)])
        self.assertEqual(baseline_scope(baseline, None, {"banner", "file-length"}), baseline)

    def test_parse_added_lines(self):
        diff = ("diff --git a/x.cpp b/x.cpp\n--- a/x.cpp\n+++ b/x.cpp\n@@ -1,0 +2,3 @@\n+a\n"
                "@@ -9 +12 @@\n+b\n--- a/y.cpp\n+++ /dev/null\n@@ -1 +0,0 @@\n")
        self.assertEqual(dict(parse_added_lines(diff)), {"x.cpp": {2, 3, 4, 12}})


class EndToEnd(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = self.tmp.name
        self.git("init", "-q")
        self.cfg = os.path.join(self.root, "config.json")
        self.baseline = os.path.join(self.root, "baseline.json")

    def tearDown(self):
        self.tmp.cleanup()

    def git(self, *args):
        subprocess.run(["git", "-C", self.root] + list(args), check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def write(self, name, text):
        with open(os.path.join(self.root, name), "w", encoding="utf-8") as f:
            f.write(text)

    def run_main(self, *args, enforce=True):
        with open(self.cfg, "w", encoding="utf-8") as f:
            json.dump(make_config(enforce), f)
        argv = list(args) + ["--root", self.root, "--config", self.cfg, "--baseline", self.baseline]
        with open(os.devnull, "w") as null:
            old, sys.stdout = sys.stdout, null
            try:
                return main(argv)
            finally:
                sys.stdout = old

    def test_staged_check_against_baseline(self):
        self.write("a.cpp", "// ======\nint x;\n")
        self.git("add", "a.cpp")
        self.assertEqual(self.run_main("--check", "--staged"), 1)
        self.assertEqual(self.run_main("--check", "--staged", enforce=False), 0)
        self.assertEqual(self.run_main("--update-baseline"), 0)
        self.assertEqual(self.run_main("--check", "--staged"), 0)
        self.write("a.cpp", "// ======\nint x;\n// ------\n")
        self.git("add", "a.cpp")
        self.assertEqual(self.run_main("--check", "--staged"), 1)
        self.write("a.cpp", "int x;\n")
        self.assertEqual(self.run_main("--check"), 0)

    def test_report_always_exits_zero(self):
        self.write("a.cpp", "// ======\nint x;")
        self.git("add", "a.cpp")
        self.assertEqual(self.run_main("--report"), 0)

    def test_heuristic_rules_block_too(self):
        self.write("a.hpp", "/// Returns n.\nint n() const { return n_; }\n")
        self.git("add", "a.hpp")
        self.assertEqual(self.run_main("--check", "--staged"), 1)
        self.write("a.hpp", "/// Words, not bytes.\nint n() const { return n_; }\n")
        self.git("add", "a.hpp")
        self.assertEqual(self.run_main("--check", "--staged"), 0)


if __name__ == "__main__":
    unittest.main()
