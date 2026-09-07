#!/usr/bin/env python3
"""Smoke test for the compiler build-performance baseline report."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", type=pathlib.Path, required=True)
    parser.add_argument("--chthollyc", type=pathlib.Path, required=True)
    parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="chtholly-build-baseline-test-") as raw:
        output = pathlib.Path(raw) / "baseline.json"
        result = subprocess.run(
            [sys.executable, str(args.script), "--chthollyc", str(args.chthollyc),
             "--source-dir", str(args.source_dir), "--output", str(output),
             "--repetitions", "1", "--jobs", "1"],
            text=True, capture_output=True,
        )
        if result.returncode != 0:
            raise AssertionError(result.stdout + result.stderr)
        report = json.loads(output.read_text(encoding="utf-8"))
        assert report["schema"] == "chtholly-build-performance-baseline-v1"
        assert report["valid"] is True
        assert isinstance(report["source_commit"], str)
        assert report["source_commit"]
        assert report["target"] in {"windows-x64", "linux-x64", "unsupported"}
        assert report["cache_mode"] == "cold-then-warm-then-root-implementation-change"
        point = report["observations"]["1"]
        assert point["cache_verified"] is True
        assert point["cold"]["repetitions"] == 1
        assert point["warm"]["repetitions"] == 1
        assert point["incremental"]["repetitions"] == 1
        for phase in ("cold", "warm", "incremental"):
            summary = point[phase]
            assert "peak_rss_bytes" in summary
            assert "peak_rss_source" in summary
            sample = point[phase]["samples"][0]
            assert "peak_rss_bytes" in sample
            assert "peak_rss_source" in sample
            if sample["peak_rss_source"] == "unsupported":
                assert sample["peak_rss_bytes"] is None
            else:
                assert isinstance(sample["peak_rss_bytes"], int)
                assert sample["peak_rss_bytes"] > 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
