#!/usr/bin/env python3
"""Run the focused telemetry evidence path inside a WSL/ext4 checkout."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    source = args.source_dir.resolve()
    manifest = source / "build-wsl/tests/chtholly-tests.generated.toml"
    runner = source / "build-wsl/tools/chtholly-test/chtholly-test"
    result = subprocess.run(
        [str(runner), "run", "--manifest", str(manifest),
         "--filter", "chtholly_telemetry_ingest_tests", "--format", "json"],
        cwd=source, text=True, encoding="utf-8", capture_output=True,
        check=False)
    if result.returncode != 0:
        raise SystemExit(result.stderr or result.stdout)
    payload = json.loads(result.stdout)
    record = {
        "schema": "chtholly-telemetry-wsl-evidence-v1",
        "source_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=source, text=True).strip(),
        "tests": payload,
    }
    args.output.write_text(json.dumps(record, sort_keys=True) + "\n",
                           encoding="utf-8")
    print(json.dumps(record, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
