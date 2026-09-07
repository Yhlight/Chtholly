#!/usr/bin/env python3
"""Smoke coverage for the release compatibility contract audit."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", type=pathlib.Path, required=True)
    parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()
    result = subprocess.run(
        [sys.executable, str(args.script), "--source-dir", str(args.source_dir), "--check"],
        text=True,
        capture_output=True,
    )
    if result.returncode != 0:
        raise AssertionError(result.stdout + result.stderr)
    if "compatibility-audit=pass" not in result.stdout:
        raise AssertionError(result.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
