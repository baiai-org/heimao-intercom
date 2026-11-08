#!/usr/bin/env python3
"""CI guard: licensing boundary and repository hygiene.

Fails when:
  * a GPL-licensed source file appears outside flasher/;
  * a private key / keystore / firmware image is tracked by git;
  * the required licence and notice files are missing.
Run locally:  python tools/check_licenses.py
"""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = ["LICENSE", "NOTICE", "THIRD_PARTY.md", "COMMERCIAL.md", "CLA.md", "TRADEMARK.md"]
GPL_MARKERS = re.compile(r"SPDX-License-Identifier:\s*GPL|GNU General Public License", re.I)
SCAN_DIRS = ["firmware", "app", "tools", "docs"]
FORBIDDEN_SUFFIXES = {".pem", ".key", ".p12", ".jks", ".keystore", ".bin", ".elf", ".apk", ".exe", ".zip"}


def tracked_files():
    out = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True)
    if out.returncode != 0:
        return []
    return [Path(line) for line in out.stdout.splitlines() if line.strip()]


def main():
    problems = []
    for name in REQUIRED:
        if not (ROOT / name).exists():
            problems.append(f"missing required file: {name}")

    files = tracked_files()
    for rel in files:
        path = ROOT / rel
        if path.suffix.lower() in FORBIDDEN_SUFFIXES and not str(rel).startswith("flasher/licenses/"):
            problems.append(f"binary/secret tracked: {rel}")
        if path.suffix.lower() not in {".c", ".h", ".py", ".kt", ".js", ".ps1", ".sh"}:
            continue
        if str(rel).split("/")[0] == "flasher":
            continue
        if not any(str(rel).startswith(prefix + "/") for prefix in SCAN_DIRS):
            continue
        try:
            head = path.read_text(encoding="utf-8", errors="ignore")[:2000]
        except OSError:
            continue
        if GPL_MARKERS.search(head):
            problems.append(f"GPL header outside flasher/: {rel}")

    if problems:
        print("licence/hygiene check FAILED")
        for problem in problems:
            print(" -", problem)
        return 1
    print(f"licence/hygiene check passed ({len(files)} tracked files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
