#!/usr/bin/env python3
"""Contract test for the platform-neutral process RSS helper."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", type=pathlib.Path, required=True)
    args = parser.parse_args()
    result = subprocess.run(
        [sys.executable, str(args.script), "--pid", str(os.getpid())],
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(result.stdout + result.stderr)
    payload = json.loads(result.stdout)
    assert payload["schema"] == "chtholly-process-memory-v1"
    assert payload["source"] in {"proc-status", "get-process-memory-info", "unsupported"}
    if payload["source"] == "unsupported":
        assert payload["peak_rss_bytes"] is None
    else:
        assert isinstance(payload["peak_rss_bytes"], int)
        assert payload["peak_rss_bytes"] > 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
