#!/usr/bin/env python3
"""Unit tests for the rules of tools/lint/check_style.py (CTest lint_checker, label lint)."""

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_style_lib.config import DEFAULT_CONFIG, Config, ConfigError  # noqa: E402
from check_style_lib.scan import scan_text  # noqa: E402
from test_check_style_support import SHIPPED, lines, make_config  # noqa: E402


class NonAscii(unittest.TestCase):
    def test_flags_first_non_ascii_per_line(self):
        self.assertEqual(lines("a.cpp", "int x;\n// x \u2192 y \u2192 z\n", "non-ascii"), [2])

    def test_ascii_is_clean(self):
        self.assertEqual(lines("a.cpp", "// x -> y\n", "non-ascii"), [])

    def test_allowlisted_path(self):
        data = make_config()
        data["rules"]["non-ascii"]["allow"] = ["notes/**"]
        config = Config(data)
        self.assertEqual(lines("notes/a.md", "\u2264\n", "non-ascii", config), [])
        self.assertEqual(lines("src/a.cpp", "// \u2264\n", "non-ascii", config), [1])


class ExperimentRefs(unittest.TestCase):
    def test_comment_references(self):
        text = "// see E237\n// wave M result\n// round 14 pin\n/* memo 104 */\n// docs/experiments.md\nint x;\n"
        self.assertEqual(lines("a.cpp", text, "experiment-ref"), [1, 2, 3, 4, 5])

    def test_other_languages(self):
        self.assertEqual(lines("a.py", "x = 1  # E95b\n", "experiment-ref"), [1])
        self.assertEqual(lines("README.md", "Latest, wave Z adds\n", "experiment-ref"), [1])

    def test_string_literal_is_not_a_comment(self):
        text = 'const char* s = "E237";\n'
        self.assertEqual(lines("a.cpp", text, "experiment-ref"), [])
        self.assertEqual(lines("a.cpp", text, "experiment-ref-code"), [1])

    def test_lint_directive_codes_are_not_experiments(self):
        text = "import os  # noqa: E402\nx = 1  # see E237\n"
        self.assertEqual(lines("a.py", text, "experiment-ref"), [2])

    def test_not_an_experiment_id(self):
        text = "// E2E test of 1E10 and 0xE123\nint a = 0xE123;\n"
        self.assertEqual(lines("a.cpp", text, "experiment-ref"), [])
        self.assertEqual(lines("a.cpp", text, "experiment-ref-code"), [])

    def test_verdicts(self):
        text = ("// E83 was KILLed\n// the child was killed by a signal\n"
                "// killed by theory\n// KEEP\n// keep in sync\n")
        self.assertEqual(lines("a.cpp", text, "experiment-verdict"), [1, 3, 4])

    def test_identifiers(self):
        text = ('int e213_setup();\nauto kE209Pinned = 1;\nstruct E235Pool {};\n'
                'const char* v = getenv("COLLATZ_GPU_E95");\n'
                'bool e2e_ok = test_unit_worker_e2e;\ndouble d = 1e100;\n')
        self.assertEqual(lines("a.cpp", text, "experiment-identifier"), [1, 2, 3, 4])

    def test_filename(self):
        self.assertEqual(lines("src/gpu/e213_defaults.hpp", "int x;\n", "experiment-filename"), [1])
        self.assertEqual(lines("src/gpu/defaults.hpp", "int x;\n", "experiment-filename"), [])


class CommentShape(unittest.TestCase):
    def test_banners(self):
        self.assertEqual(lines("a.cpp", "// ======\nint x;\n/********/\n//////////\n", "banner"), [1, 3, 4])
        self.assertEqual(lines("a.py", "# ------\nx = 1\n", "banner"), [1])

    def test_not_banners(self):
        self.assertEqual(lines("a.cpp", "/**\n * Doc.\n */\nint x; // ---\n", "banner"), [])
        text = '# -*- coding: utf-8 -*-\n"""Title\n-----\n"""\n'
        self.assertEqual(lines("a.py", text, "banner"), [])

    def test_emphasis(self):
        text = "// NOTE: a\n// IMPORTANT: b\n// Note that c\n// a note that is fine\n"
        self.assertEqual(lines("a.cpp", text, "emphasis"), [1, 2, 3])
        self.assertEqual(lines("a.cpp", 'puts("WARNING: low memory");\n', "emphasis"), [])

    def test_ai_vocabulary(self):
        text = "// A robust fallback.\nint robust_mean;\n// robust_mean is the trimmed mean\n// Let's go\n"
        self.assertEqual(lines("a.cpp", text, "ai-vocabulary"), [1, 4])
        self.assertEqual(lines("a.cpp", "// Step 2: the sieve\n// Step 5: narration\n", "ai-vocabulary"), [2])

    def test_todo_owner(self):
        text = '// TODO: fix\n// TODO(zach): fix\nconst char* s = "TODO";\n// FIXME later\n'
        self.assertEqual(lines("a.cpp", text, "todo-owner"), [1, 4])

    def test_placeholder(self):
        text = "// In a real implementation, retry.\n// disabled for now\n// a simple bound\n"
        self.assertEqual(lines("a.cpp", text, "placeholder"), [1, 2])
        self.assertEqual(lines("README.md", "Disabled on Windows for now.\n", "placeholder"), [1])


class Heuristics(unittest.TestCase):
    def test_restating(self):
        self.assertEqual(lines("a.cpp", "int x = 0;\n\n// Increment x\nx++;\n", "restating-comment"), [3])
        self.assertEqual(lines("a.py", "x = x + 1  # Increment x\n", "restating-comment"), [1])

    def test_comment_with_reason_is_kept(self):
        text = "// Increment x because the border is excluded\nx++;\n"
        self.assertEqual(lines("a.cpp", text, "restating-comment"), [])

    def test_math_and_block_comments_are_kept(self):
        self.assertEqual(lines("a.cpp", "l = 1 << L; // 2^L\n", "restating-comment"), [])
        text = "// Increment x\n// by one\nx++;\n"
        self.assertEqual(lines("a.cpp", text, "restating-comment"), [])

    def test_trivial_doc_cpp(self):
        self.assertEqual(lines("a.hpp", "/// Returns n.\nint n() const { return n_; }\n", "trivial-doc"), [1])
        self.assertEqual(lines("a.hpp", "/// Returns n.\nint n() const;\n", "trivial-doc"), [])
        body = "/// Sums.\nint s(int a) {\n    int t = a;\n    t += 1;\n    t *= 2;\n    return t;\n}\n"
        self.assertEqual(lines("a.cpp", body, "trivial-doc"), [])
        self.assertEqual(lines("a.hpp", "/// Words, not bytes.\nint n() const { return n_; }\n",
                               "trivial-doc"), [])

    def test_trivial_doc_ts_and_python(self):
        ts = "/** Doubles x. */\nexport function dbl(x: number): number {\n    return 2 * x;\n}\n"
        self.assertEqual(lines("a.ts", ts, "trivial-doc"), [1])
        self.assertEqual(lines("a.mjs", "/** Adds one. */\nconst inc = (x) => x + 1;\n", "trivial-doc"), [1])
        py = 'def f(x):\n    """Return x."""\n    return x\n'
        self.assertEqual(lines("a.py", py, "trivial-doc"), [2])

    def test_trivial_doc_ts_body_shapes(self):
        # An object type in the return type is not the body.
        ts = ("/** Splits s. */\nexport function parts(s: string): { a: string; b: string } {\n"
              "    const i = s.indexOf(\":\");\n    const a = s.slice(0, i);\n"
              "    return { a, b: s.slice(i + 1) };\n}\n")
        self.assertEqual(lines("a.ts", ts, "trivial-doc"), [])
        one = "/** Wraps x. */\nexport function wrap(x: number): { x: number } {\n    return { x };\n}\n"
        self.assertEqual(lines("a.ts", one, "trivial-doc"), [1])
        # The body of a curried arrow is the innermost one.
        curried = "/** Sets x. */\nconst setX = (x) => (d) => {\n    d.x = x;\n    d.y = x;\n    d.z = x;\n};\n"
        self.assertEqual(lines("a.mjs", curried, "trivial-doc"), [])
        # An expression body counts its own lines.
        sql = ("/** The query. */\nconst q = (t) =>\n    `SELECT a\n     FROM ${t}\n     WHERE b\n"
               "     AND c\n     AND d\n     ORDER BY a`;\n")
        self.assertEqual(lines("a.ts", sql, "trivial-doc"), [])
        self.assertEqual(lines("a.ts", "/** One. */\nconst one = () =>\n    1;\n", "trivial-doc"), [1])

    def test_commented_out_code(self):
        text = "// foo(bar);\n// if (x) {\n// }\nint y;\n"
        self.assertEqual(lines("a.cpp", text, "commented-out-code"), [1, 2, 3])
        self.assertEqual(lines("a.py", "# if x > 0:\n# import os\ny = 1\n", "commented-out-code"), [1, 2])

    def test_prose_and_examples_are_not_code(self):
        text = "// the queue is drained here; the tail runs later (see flush);\nint y;\n"
        self.assertEqual(lines("a.cpp", text, "commented-out-code"), [])
        self.assertEqual(lines("a.cpp", "// Usage:\n//   foo(bar);\nint y;\n", "commented-out-code"), [])


class Whitespace(unittest.TestCase):
    def test_trailing_whitespace(self):
        self.assertEqual(lines("a.cpp", "int x; \nint y;\n", "trailing-whitespace"), [1])
        self.assertEqual(lines("a.cpp", "int x;\r\n", "trailing-whitespace"), [])
        self.assertEqual(lines("README.md", "a line break  \nnext\n", "trailing-whitespace"), [])

    def test_tabs_only_in_c_and_ts(self):
        self.assertEqual(lines("a.cpp", "\tint x;\n", "tab"), [1])
        self.assertEqual(lines("a.sh", "\techo x\n", "tab"), [])

    def test_final_newline(self):
        self.assertEqual(lines("a.cpp", "int x;", "final-newline"), [1])
        self.assertEqual(lines("a.cpp", "int x;\n", "final-newline"), [])


class FileLength(unittest.TestCase):
    def findings(self, path, n, config=SHIPPED, end="\n"):
        # n lines, all comments and blank lines but the last when n is odd.
        text = "\n".join(["// c", ""] * (n // 2) + ["x;"] * (n % 2)) + end
        return [str(f) for f in scan_text(path, text, config) if f.rule == "file-length"]

    def test_limit_counts_every_line(self):
        self.assertEqual(self.findings("src/a.cpp", 300), [])
        self.assertEqual(self.findings("src/a.cpp", 301),
                         ["src/a.cpp:1:file-length: file has 301 lines; the limit is 300"])
        self.assertEqual(len(self.findings("src/a.cpp", 301, end="")), 1)
        for path in ("a.py", "a.sh", "a.ps1", "a.mjs", "a.ts", "a.cl", "CMakeLists.txt", "a.cmd"):
            self.assertEqual(len(self.findings(path, 301)), 1, path)

    def test_exempt_paths(self):
        with open(DEFAULT_CONFIG, encoding="utf-8") as f:
            data = json.load(f)
        # With every type checked, the shipped allow list alone exempts these.
        data["rules"]["file-length"]["types"] = ["*"]
        for path in ("server/package-lock.json", "server/wrangler.jsonc", "server/test/fixtures/x.mjs",
                     "README.md", "CONTRIBUTING.md", "LICENSE", "data/a.csv", "x.js.map"):
            self.assertEqual(self.findings(path, 301), [], path)
            self.assertEqual(self.findings(path, 301, Config(data)), [], path)

    def test_max_from_config(self):
        data = make_config()
        data["rules"]["file-length"]["max"] = 2
        self.assertEqual(len(self.findings("a.cpp", 3, Config(data))), 1)
        for bad in (0, "300", True):
            data["rules"]["file-length"]["max"] = bad
            self.assertRaisesRegex(ConfigError, "max must be a positive integer", Config, data)
        data["rules"]["file-length"]["max"] = 300
        data["rules"]["file-length"]["maximum"] = 300
        self.assertRaisesRegex(ConfigError, r"unknown keys \['maximum'\]", Config, data)


if __name__ == "__main__":
    unittest.main()
