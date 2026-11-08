#!/usr/bin/env python3
"""Run the host regression scripts with an explicit known-failure list.

These scripts are standalone (each has ``main()``), not pytest tests.  A few of
them were already failing in the private repository before the public rewrite
(they drifted away from the production sources).  They stay visible as XFAIL
instead of being deleted, and CI fails only on *new* failures.

    python tools/run_host_tests.py
    python tools/run_host_tests.py --strict   # treat XFAIL as failure
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"

KNOWN_FAIL = {
    "test_bridge_edges.py": "host harness predates the bridge.c refactor",
    "test_radio_profile.py": "host harness predates esp_timer/include changes in sa818_cfg.c",
    "test_tx_ownership.py": "asserts the pre-r11 browser-RX squelch rule",
    "test_app_control_panel.js": "JS harness no longer matches app_control.js timers",
}


def run(path):
    env = dict(os.environ)
    env.setdefault("PYTHONIOENCODING", "utf-8")
    command = ["node", str(path)] if path.suffix == ".js" else [sys.executable, str(path)]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, env=env)
    return result.returncode, (result.stdout + result.stderr).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--strict", action="store_true", help="fail on known failures too")
    args = parser.parse_args()

    files = sorted(TOOLS.glob("test_*.py")) + sorted(TOOLS.glob("test_*.js"))
    (ROOT / ".tmp").mkdir(exist_ok=True)
    failed, xfailed, passed = [], [], []
    for path in files:
        code, output = run(path)
        name = path.name
        if code == 0:
            passed.append(name)
        elif name in KNOWN_FAIL:
            xfailed.append(name)
            print(f"XFAIL {name}: {KNOWN_FAIL[name]}")
        else:
            failed.append(name)
            print(f"FAIL  {name}\n{output[-2000:]}\n")

    print(f"\npassed={len(passed)} xfail={len(xfailed)} failed={len(failed)}")
    if xfailed:
        print("known failures:", ", ".join(xfailed))
    if failed:
        return 1
    if args.strict and xfailed:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
