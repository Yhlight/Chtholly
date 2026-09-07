#!/usr/bin/env python3
"""Verify toolchain install space evidence on a normal signed install."""

from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import stat
import subprocess
import tempfile


def run(executable: pathlib.Path, *arguments: str, expected: int = 0):
    result = subprocess.run(
        [str(executable), *arguments], text=True, encoding="utf-8",
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}: "
            f"{arguments!r}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--toolchain", required=True, type=pathlib.Path)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="chtholly-toolchain-space-") as raw:
        root = pathlib.Path(raw)
        manager = root / "manager"
        secret = root / "release.secret"
        public = root / "release.public"
        trust = root / "root.trust"
        run(args.toolchain, "key", "generate", "--secret", str(secret),
            "--public", str(public))
        run(args.toolchain, "trust", "create", "-o", str(trust),
            "--version", "1", "--threshold", "1", "--key", str(public),
            "--secret-key", str(secret))
        run(args.toolchain, "trust", "init", str(trust), "--root", str(manager))

        install_tree = root / "tree"
        (install_tree / "bin").mkdir(parents=True)
        (install_tree / "bin" / "marker.txt").write_text("space\n",
                                                                encoding="utf-8")
        archive = root / "release.zip"
        package = run(
            args.toolchain, "package", str(install_tree), "-o", str(archive),
            "--version", "0.2.0", "--source-commit", "1" * 40,
            "--secret-key", str(secret))
        release_id = "0.2.0+" + "1" * 40

        install = run(
            args.toolchain, "--output-format", "jsonl-v1", "install",
            str(archive), "--root", str(manager))
        records = [json.loads(line) for line in install.stdout.splitlines()]
        outputs = {
            record.get("name"): record.get("value")
            for record in records if record.get("kind") == "command-output"
        }
        required = {
            "space-payload-bytes", "space-index-bytes", "space-required-bytes",
            "space-available-bytes", "space-path", "space-sufficient",
        }
        missing = sorted(required - outputs.keys())
        if missing:
            raise AssertionError(f"preflight fields missing: {missing}; output={install.stdout!r}")
        if outputs["space-sufficient"] != "true":
            raise AssertionError(f"normal install was not sufficient: {outputs!r}")
        if int(outputs["space-required-bytes"]) <= 0:
            raise AssertionError(f"invalid required bytes: {outputs!r}")
        if pathlib.Path(outputs["space-path"]).name != root.name:
            raise AssertionError(f"unexpected staging path: {outputs['space-path']!r}")

        run(args.toolchain, "activate", release_id, "--root", str(manager))
        active = (manager / "state" / "active-v1").read_text(encoding="utf-8").strip()
        if active != release_id:
            raise AssertionError(f"unexpected active release: {active!r}")
        if not (manager / "generations" / release_id / "bin" / "marker.txt").is_file():
            raise AssertionError("installed generation payload is missing")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
