#!/usr/bin/env python3
"""Unit tests of tools/lint/lint_all.py (CTest lint_checker, label lint): how it
runs ruff through uvx, with a fake uvx first on PATH and no ruff installed.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

LINT_ALL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lint_all.py")

# Stands in for uvx: appends its arguments to $FAKE_UVX_LOG and exits with
# $FAKE_UVX_RC. It prints nothing, which is what ruff prints on a clean tree.
FAKE_UVX = """#!/bin/sh
echo "$*" >> "$FAKE_UVX_LOG"
exit "$FAKE_UVX_RC"
"""


@unittest.skipIf(os.name == "nt", "the fake uvx is a POSIX shell script")
class RuffThroughUvx(unittest.TestCase):
    def setUp(self):
        system_path = os.pathsep.join(["/usr/bin", "/bin"])
        if shutil.which("ruff", path=system_path):
            self.skipTest("a ruff in /usr/bin or /bin would be used instead of uvx")
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        bin_dir = os.path.join(tmp.name, "bin")
        os.mkdir(bin_dir)
        fake = os.path.join(bin_dir, "uvx")
        with open(fake, "w", encoding="ascii") as f:
            f.write(FAKE_UVX)
        os.chmod(fake, 0o755)
        self.log = os.path.join(tmp.name, "uvx.log")
        self.path = bin_dir + os.pathsep + system_path
        self.out = os.path.join(tmp.name, "out")

    def lint(self, uvx_rc, *flags):
        env = dict(os.environ, PATH=self.path, FAKE_UVX_LOG=self.log, FAKE_UVX_RC=str(uvx_rc))
        cmd = [sys.executable, LINT_ALL, "--only", "ruff", "--out-dir", self.out, *flags]
        return subprocess.run(cmd, env=env, capture_output=True, text=True)

    def test_a_ruff_that_uvx_cannot_provide_is_skipped(self):
        p = self.lint(1)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertRegex(p.stdout, r"(?m)^ruff +skipped +uvx could not provide ruff")
        self.assertTrue(os.path.isfile(os.path.join(self.out, "ruff.txt")))

    def test_strict_fails_on_that_skip(self):
        p = self.lint(1, "--strict")
        self.assertEqual(p.returncode, 1, p.stdout + p.stderr)

    def test_offline_reaches_every_uvx_call(self):
        p = self.lint(0, "--offline")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertRegex(p.stdout, r"(?m)^ruff +0 findings")
        with open(self.log, encoding="ascii") as f:
            calls = f.read().splitlines()
        self.assertEqual(len(calls), 2, calls)  # the probe, then the check
        self.assertTrue(all(c.startswith("--offline ruff@") for c in calls), calls)


if __name__ == "__main__":
    unittest.main()
