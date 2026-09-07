#!/usr/bin/env python3
"""Focused contract checks for the stage-boundary evidence index."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile


REPORT_SCHEMAS = {
    "size-full": "chtholly-release-size-v1",
    "size-minimal": "chtholly-release-size-v1",
    "space-lifecycle": "chtholly-release-install-evidence-v1",
    "artifact-cache": "chtholly-compiler-artifact-store-report-v1",
    # Profile inventories are the two Task 1 size reports; no parallel
    # install-profile schema is introduced by the stage-boundary task.
    "install-profiles": "chtholly-release-size-v1",
    "component-app": "chtholly-component-host-soak-v1",
    "performance": "chtholly-build-performance-baseline-v1",
    # Diagnostic counters are part of the Task 6 performance report.
    "diagnostics": "chtholly-build-performance-baseline-v1",
}


def invoke(script: pathlib.Path, root: pathlib.Path, output: pathlib.Path,
           reports: dict[str, pathlib.Path], source_commit: str,
           target: str, source_dir: pathlib.Path) -> subprocess.CompletedProcess[str]:
    command = [
        sys.executable, str(script), "--evidence-dir", str(root),
        "--output", str(output), "--source-commit", source_commit,
        "--target", target, "--source-dir", str(source_dir),
    ]
    for name, path in reports.items():
        command += ["--report", f"{name}={path.name}"]
    return subprocess.run(command, text=True, encoding="utf-8",
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          check=False)


def write_reports(root: pathlib.Path, source_commit: str, target: str) -> dict[str, pathlib.Path]:
    reports: dict[str, pathlib.Path] = {}
    for name, schema in REPORT_SCHEMAS.items():
        value = {"schema": schema, "valid": True,
                 "source_commit": source_commit, "target": target}
        if name == "space-lifecycle":
            value["space_preflight"] = {
                "space-available-bytes": 4096,
                "space-index-bytes": 512,
                "space-payload-bytes": 1024,
                "space-path": "/tmp/stage",
                "space-required-bytes": 1536,
                "space-sufficient": "true",
            }
        if name in {"size-full", "install-profiles"}:
            value["profile"] = "full"
        elif name == "size-minimal":
            value["profile"] = "minimal"
        if name == "diagnostics":
            value["diagnostic_counters"] = {
                "related_note_count": 0,
                "unavailable_location_count": 0,
                "quick_fix_count": 0,
            }
        path = root / f"{name}.json"
        path.write_text(json.dumps(value), encoding="utf-8")
        reports[name] = path
    return reports


def write_docs(root: pathlib.Path) -> None:
    (root / "docs").mkdir(parents=True, exist_ok=True)
    (root / "support").mkdir(parents=True, exist_ok=True)
    (root / "docs" / "compiler-status.md").write_text(
        "Current capability status is authoritative in support/chtholly-product-status.toml.\n"
        "Older notes are historical context only; no providers are paused.\n"
        "No new source syntax or ABI wave was opened. ABI-2 remains deferred.\n",
        encoding="utf-8")
    (root / "docs" / "DEVELOPMENT_STATUS.md").write_text(
        "Current capability status is authoritative in support/chtholly-product-status.toml.\n"
        "Historical implementation notes are not current claims. ABI-2 remains deferred.\n",
        encoding="utf-8")
    (root / "docs" / "design").mkdir(parents=True, exist_ok=True)
    (root / "docs" / "design" / "three-stage-execution-2026-09.md").write_text(
        "This is an evidence index, not a second language specification.\n"
        "No new source syntax was opened; Component ABI-2 remains deferred.\n",
        encoding="utf-8")
    (root / "support" / "chtholly-product-status.toml").write_text(
        'schema = "chtholly-product-status-v1"\n'
        'product = "0.2.0-preview"\n'
        'host_target = "windows-x64"\n'
        'required_hosts = ["windows-x64", "linux-x64"]\n'
        'optional_hosts = ["macos-arm64"]\n', encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", type=pathlib.Path, required=True)
    args = parser.parse_args()
    commit = "a" * 40
    target = "x86_64-pc-windows-msvc"
    with tempfile.TemporaryDirectory(prefix="chtholly-release-contract-") as raw:
        root = pathlib.Path(raw)
        source_dir = root / "source"
        write_docs(source_dir)
        evidence = root / "evidence"
        evidence.mkdir()
        reports = write_reports(evidence, commit, target)
        output = root / "stage-boundary.json"

        passed = invoke(args.script, evidence, output, reports, commit, target,
                        source_dir)
        if passed.returncode != 0:
            raise AssertionError(passed.stderr)
        index = json.loads(output.read_text(encoding="utf-8"))
        assert index["schema"] == "chtholly-stage-boundary-evidence-v1"
        assert index["valid"] is True
        assert len(index["reports"]) == len(REPORT_SCHEMAS)

        symlink_target = evidence / "performance.json"
        symlink_alias = evidence / "performance-link.json"
        try:
            symlink_alias.symlink_to(symlink_target)
        except OSError:
            symlink_alias = None
        if symlink_alias is not None:
            symlink_reports = dict(reports)
            symlink_reports["performance"] = symlink_alias
            failed = invoke(args.script, evidence, root / "symlink.json",
                            symlink_reports, commit, target, source_dir)
            assert failed.returncode == 1
            assert "symlink is not permitted" in failed.stderr
            symlink_alias.unlink()

        malformed_space = json.loads(
            reports["space-lifecycle"].read_text(encoding="utf-8"))
        malformed_space["space_preflight"]["space-required-bytes"] = -1
        reports["space-lifecycle"].write_text(
            json.dumps(malformed_space), encoding="utf-8")
        failed = invoke(args.script, evidence, root / "space.json", reports,
                        commit, target, source_dir)
        assert failed.returncode == 1
        assert "space-evidence" in failed.stderr
        reports["space-lifecycle"].write_text(
            json.dumps({"schema": REPORT_SCHEMAS["space-lifecycle"], "valid": True,
                        "source_commit": commit, "target": target,
                        "space_preflight": {
                            "space-available-bytes": 4096,
                            "space-index-bytes": 512,
                            "space-payload-bytes": 1024,
                            "space-path": "/tmp/stage",
                            "space-required-bytes": 1536,
                            "space-sufficient": "true",
                        }}), encoding="utf-8")

        missing = dict(reports)
        missing["performance"].unlink()
        failed = invoke(args.script, evidence, root / "missing.json", missing,
                        commit, target, source_dir)
        assert failed.returncode == 1
        assert "missing-report" in failed.stderr
        reports["performance"] = evidence / "performance.json"
        reports["performance"].write_text(
            json.dumps({"schema": REPORT_SCHEMAS["performance"], "valid": True,
                        "source_commit": commit, "target": target}),
            encoding="utf-8")

        wrong = json.loads(reports["artifact-cache"].read_text(encoding="utf-8"))
        wrong["schema"] = "wrong-schema"
        reports["artifact-cache"].write_text(json.dumps(wrong), encoding="utf-8")
        failed = invoke(args.script, evidence, root / "schema.json", reports,
                        commit, target, source_dir)
        assert failed.returncode == 1
        assert "wrong-schema" in failed.stderr
        reports["artifact-cache"].write_text(
            json.dumps({"schema": REPORT_SCHEMAS["artifact-cache"], "valid": True,
                        "source_commit": commit, "target": target}),
            encoding="utf-8")

        invalid = json.loads(reports["performance"].read_text(encoding="utf-8"))
        invalid["valid"] = False
        reports["performance"].write_text(json.dumps(invalid), encoding="utf-8")
        failed = invoke(args.script, evidence, root / "invalid.json", reports,
                        commit, target, source_dir)
        assert failed.returncode == 1
        assert "failed-valid" in failed.stderr

        invalid["valid"] = True
        invalid.pop("source_commit")
        reports["performance"].write_text(json.dumps(invalid), encoding="utf-8")
        failed = invoke(args.script, evidence, root / "provenance.json", reports,
                        commit, target, source_dir)
        assert failed.returncode == 1
        assert "missing-source-commit" in failed.stderr

        invalid["source_commit"] = commit
        invalid.pop("target")
        reports["performance"].write_text(json.dumps(invalid), encoding="utf-8")
        failed = invoke(args.script, evidence, root / "target.json", reports,
                        commit, target, source_dir)
        assert failed.returncode == 1
        assert "missing-target" in failed.stderr

        write_docs(source_dir)
        status = source_dir / "docs" / "compiler-status.md"
        status.write_text("A provider is paused.\n", encoding="utf-8")
        invalid["target"] = target
        reports["performance"].write_text(json.dumps(invalid), encoding="utf-8")
        failed = invoke(args.script, evidence, root / "docs.json", reports,
                        commit, target, source_dir)
        assert failed.returncode == 1
        assert "historical-status" in failed.stderr
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
