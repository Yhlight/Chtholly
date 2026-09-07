#!/usr/bin/env python3
"""Focused contract tests for the artifact-store evidence scanner."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


def run_scanner(script: Path, cache: Path, output: Path, *extra: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(script), "--cache-dir", str(cache), "--output", str(output), *extra],
        text=True,
        capture_output=True,
        check=False,
    )


def write_fixture(root: Path) -> None:
    # Keep names in the same sharded shape used by CompilerArtifactPathService.
    objects = root / "objects" / "aa"
    manifests = root / "manifests" / "aa"
    refs = root / "refs"
    leases = root / "leases"
    for directory in (objects, manifests, refs, leases):
        directory.mkdir(parents=True, exist_ok=True)

    reachable = "aa" * 32
    unreachable = "bb" * 32
    (objects / f"{reachable}.o").write_bytes(b"reachable-object")
    (objects / f"{unreachable}.o").write_bytes(b"unreachable-object")
    # The scanner's lightweight closure verifier accepts the stable reference
    # envelope and uses the root digest to mark the matching manifest.
    (manifests / f"{reachable}.manifest").write_bytes(bytes.fromhex(reachable))
    (refs / "active.ref").write_text(
        "CHNXTREF1\n"
        "target\tx86_64-pc-windows-msvc\n"
        "root\tfixture\n"
        f"manifest\t{reachable}\n",
        encoding="utf-8",
    )
    # An unlocked, well-formed lease is classified as stale by the scanner.
    (leases / ("cc" * 32 + ".lease")).write_text(
        "CHNXTLEASE1\n"
        f"session\t{'cc' * 32}\n"
        "target\tx86_64-pc-windows-msvc\n"
        "root\tfixture\n"
        f"manifest\t{reachable}\n",
        encoding="utf-8",
    )


def test_report_contains_reachability_and_stale_lease(tmp_path: Path, script: Path) -> None:
    cache = tmp_path / "cache"
    output = tmp_path / "report.json"
    write_fixture(cache)
    result = run_scanner(script, cache, output, "--verify-references")
    assert result.returncode == 0, result.stderr
    report = json.loads(output.read_text(encoding="utf-8"))
    assert report["schema"] == "chtholly-compiler-artifact-store-report-v1"
    assert report["families"]["objects"]["total_bytes"] > 0
    assert report["families"]["objects"]["unreachable_bytes"] > 0
    assert report["families"]["objects"]["reachable_bytes"] > 0
    assert report["stale_lease_count"] == 1
    assert report["valid"] is True


def test_invalid_reference_fails_closed(tmp_path: Path, script: Path) -> None:
    cache = tmp_path / "cache"
    output = tmp_path / "report.json"
    write_fixture(cache)
    (cache / "refs" / "malformed.ref").write_text("not-a-reference\n", encoding="utf-8")
    result = run_scanner(script, cache, output, "--verify-references")
    assert result.returncode == 1
    assert "invalid-reference" in result.stderr
    report = json.loads(output.read_text(encoding="utf-8"))
    assert report["valid"] is False
    assert report["families"]["objects"]["unreachable_bytes"] > 0
    # Reporting is observational: malformed input must not remove the cache.
    assert (cache / "objects" / "aa" / ("bb" * 32 + ".o")).exists()


def test_symlink_is_rejected(tmp_path: Path, script: Path) -> None:
    cache = tmp_path / "cache"
    output = tmp_path / "report.json"
    write_fixture(cache)
    outside = tmp_path / "outside"
    outside.write_bytes(b"outside")
    try:
        os.symlink(outside, cache / "objects" / "aa" / "escape.o")
    except OSError:
        # Windows developer-mode/symlink privilege is not guaranteed in CI.
        return
    result = run_scanner(script, cache, output)
    assert result.returncode == 1
    assert "symlink" in result.stderr


def test_reference_rejects_noncanonical_manifest_shard(tmp_path: Path, script: Path) -> None:
    cache = tmp_path / "cache"
    output = tmp_path / "report.json"
    write_fixture(cache)
    source = cache / "manifests" / "aa" / (("aa" * 32) + ".manifest")
    target = cache / "manifests" / "bb" / source.name
    target.parent.mkdir(parents=True, exist_ok=True)
    source.replace(target)
    result = run_scanner(script, cache, output, "--verify-references")
    assert result.returncode == 1
    assert "non-canonical" in result.stderr


def test_stale_lease_does_not_keep_artifacts_reachable(tmp_path: Path, script: Path) -> None:
    cache = tmp_path / "cache"
    output = tmp_path / "report.json"
    write_fixture(cache)
    (cache / "refs" / "active.ref").unlink()
    result = run_scanner(script, cache, output, "--verify-references")
    assert result.returncode == 0, result.stderr
    report = json.loads(output.read_text(encoding="utf-8"))
    assert report["stale_lease_count"] == 1
    assert report["families"]["objects"]["reachable_bytes"] == 0


def test_malformed_stale_lease_is_ignored(tmp_path: Path, script: Path) -> None:
    cache = tmp_path / "cache"
    output = tmp_path / "report.json"
    write_fixture(cache)
    lease = cache / "leases" / (("cc" * 32) + ".lease")
    lease.write_text("malformed stale lease", encoding="utf-8")
    result = run_scanner(script, cache, output, "--verify-references")
    assert result.returncode == 0, result.stderr
    report = json.loads(output.read_text(encoding="utf-8"))
    assert report["valid"] is True
    assert report["stale_lease_count"] == 1


def test_orphan_typed_index_is_unreachable(tmp_path: Path, script: Path) -> None:
    cache = tmp_path / "cache"
    output = tmp_path / "report.json"
    write_fixture(cache)
    component = "dd" * 32
    request = "cc" * 32
    component_path = cache / "specializations" / "dd" / f"{component}.specific"
    component_path.parent.mkdir(parents=True, exist_ok=True)
    component_path.write_bytes(b"orphan-component")
    index_path = cache / "specialization-index" / "cc" / f"{request}.ref"
    index_path.parent.mkdir(parents=True, exist_ok=True)
    index_path.write_text(f"CHNXTSPECREF1\ncomponent\t{component}\n", encoding="utf-8")
    result = run_scanner(script, cache, output, "--verify-references")
    assert result.returncode == 0, result.stderr
    report = json.loads(output.read_text(encoding="utf-8"))
    assert report["families"]["specialization-index"]["unreachable_bytes"] > 0
    assert report["families"]["specialization-index"]["reachable_bytes"] == 0


def test_output_inside_cache_is_rejected(tmp_path: Path, script: Path) -> None:
    cache = tmp_path / "cache"
    write_fixture(cache)
    result = run_scanner(script, cache, cache / "report.json")
    assert result.returncode == 1
    assert "outside cache-dir" in result.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", type=Path, required=True)
    args = parser.parse_args()
    tests = (
        test_report_contains_reachability_and_stale_lease,
        test_invalid_reference_fails_closed,
        test_symlink_is_rejected,
        test_reference_rejects_noncanonical_manifest_shard,
        test_stale_lease_does_not_keep_artifacts_reachable,
        test_malformed_stale_lease_is_ignored,
        test_orphan_typed_index_is_unreachable,
        test_output_inside_cache_is_rejected,
    )
    # The harness invokes this file directly; use unittest-like temporary
    # directories without adding a test framework dependency.
    import tempfile

    for test in tests:
        with tempfile.TemporaryDirectory(prefix="chtholly-artifact-report-") as directory:
            test(Path(directory), args.script)
    print(f"{len(tests)} artifact cache report tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
