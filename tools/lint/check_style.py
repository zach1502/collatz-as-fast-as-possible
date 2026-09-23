#!/usr/bin/env python3
"""Text-level style checks for the collatz tree (no dependencies).

Scans tracked text files and prints one line per finding:

    path:line:rule: message

Rules, severities, file types and per-path allow patterns are read from
tools/lint/config.json. tools/lint/baseline.json holds the accepted count per
(file, rule); only counts above it fail.

Modes:
  --report            print every finding and a summary, exit 0 (default)
  --check             compare counts with the baseline; error-severity
                      counts above it fail when "enforce" is true in the config
  --update-baseline   write the current counts to the baseline (only those of
                      the --rule rules and PATH files, if given)
  --staged            scan the staged (index) version of staged files only

Exit status: 0 clean or not enforcing, 1 error findings above the baseline,
2 usage or setup error. See CONTRIBUTING.md, section "Style checks".

The code is in the check_style_lib package next to this file.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from check_style_lib.cli import main

if __name__ == "__main__":
    sys.exit(main())
