#!/usr/bin/env python3
"""Verify CFFI toolchain cache reuse across independent commands."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import tempfile


def run(command: list[str]) -> list[dict]:
    result = subprocess.run(command, text=True, encoding="utf-8",
                            errors="replace", stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,
                            check=False)
    if result.returncode != 0:
        raise AssertionError(
            f"command failed: {command}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return [json.loads(line) for line in result.stdout.splitlines() if line]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cffi", type=pathlib.Path, required=True)
    parser.add_argument("--target", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="chtholly-cffi-cache-") as raw:
        cache = pathlib.Path(raw) / "toolchain"
        command = [str(args.cffi), "doctor", "--target", args.target,
                   "--cache-dir", str(cache), "--output-format", "jsonl-v1"]
        first = run(command)
        if any(event.get("value") == "cache-hit-disk"
               for event in first if event.get("event") == "discovery"):
            raise AssertionError("first discovery unexpectedly hit disk cache")
        first_metrics = next((event for event in first
                              if event.get("event") == "cache-metrics"), None)
        if not first_metrics or first_metrics.get("misses") != 1:
            raise AssertionError(f"first discovery metrics missing: {first_metrics}")
        files = list(cache.glob("*.toolchain"))
        if len(files) != 1 or not re.fullmatch(r"[0-9a-f]{64}\.toolchain",
                                                files[0].name):
            raise AssertionError(f"unexpected cache files: {files}")
        second = run(command)
        if not any(event.get("value") == "cache-hit-disk"
                   for event in second if event.get("event") == "discovery"):
            raise AssertionError(f"second discovery did not hit disk cache: {second}")
        second_metrics = next((event for event in second
                               if event.get("event") == "cache-metrics"), None)
        if not second_metrics or second_metrics.get("disk_hits") != 1:
            raise AssertionError(f"disk hit metrics missing: {second_metrics}")
        files[0].write_bytes(files[0].read_bytes() + b"broken\n")
        third = run(command)
        if not any(event.get("value", "").startswith("inherited-msvc")
                   for event in third if event.get("event") == "discovery"):
            raise AssertionError("corrupt cache did not fall back to discovery")
        third_metrics = next((event for event in third
                              if event.get("event") == "cache-metrics"), None)
        if not third_metrics or third_metrics.get("invalid") != 1:
            raise AssertionError(f"invalid cache metrics missing: {third_metrics}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
