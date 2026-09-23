#!/usr/bin/env python3
"""Unit tests for the lexers of tools/lint/check_style.py (CTest lint_checker, label lint)."""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_style_lib.source import Source, file_type  # noqa: E402


class Lexers(unittest.TestCase):
    def comments(self, path, text):
        src = Source(path, text, file_type(path, text))
        return [c.text for ln in src.lines for c in ln.comments]

    def test_cpp(self):
        text = 'auto r = R"(// not a comment)";\nchar q = \'"\';\nint n = 1\'000\'000; // real\n'
        self.assertEqual(self.comments("a.cpp", text), ["real"])

    def test_js(self):
        text = "const re = /\\/\\//g;\nconst t = `${ \"// no\" } // no`;\nconst d = a / b; // yes\n"
        self.assertEqual(self.comments("a.mjs", text), ["yes"])

    def test_shell(self):
        text = 'echo "# no" \'# no\' ${#arr[@]} $#\ncat <<EOF\n# body, not a comment\nEOF\nx=1 # yes\n'
        self.assertEqual(self.comments("a.sh", text), ["yes"])

    def test_python(self):
        text = 'x = "# no"\ny = f"{x}#no"  # yes\n'
        self.assertEqual(self.comments("a.py", text), ["yes"])

    def test_cmake_and_powershell(self):
        self.assertEqual(self.comments("CMakeLists.txt", 'set(X "# no") #[[ yes ]]\n# also\n'), ["yes", "also"])
        self.assertEqual(self.comments("a.ps1", "$a = 'it''s # no' # yes\n<# block #>\n"), ["yes", "block"])


if __name__ == "__main__":
    unittest.main()
