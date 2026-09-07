#!/usr/bin/env python3
"""Fast, host-local checks for the release supply-chain evidence contract."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile
import zipfile


def invoke(command: list[str], expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, encoding="utf-8",
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            check=False)
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}: {command}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", type=pathlib.Path, required=True)
    parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="chtholly-supply-chain-") as raw:
        root = pathlib.Path(raw)
        inputs = root / "inputs.json"
        invoke([sys.executable, str(args.script), "inputs", "--source-dir",
                str(args.source_dir), "--output", str(inputs)])
        input_value = json.loads(inputs.read_text(encoding="utf-8"))
        assert input_value["schema"] == "chtholly-supply-chain-inputs-v1"
        assert len(input_value["source_commit"]) == 40
        assert input_value["github_actions"]
        assert input_value["vcpkg"]["commit"]

        package = root / "chtholly-0.2.0-preview.zip"
        with zipfile.ZipFile(package, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("bin/chthollyc.exe", b"preview")
            archive.writestr("share/chtholly/stdlib/manifest.toml", b"epoch=10\n")
        sbom = root / "sbom.json"
        commit = input_value["source_commit"]
        invoke([sys.executable, str(args.script), "sbom", "--inputs", str(inputs),
                "--package", str(package), "--source-commit", commit,
                "--release-version", "0.2.0-preview", "--output", str(sbom)])
        sbom_value = json.loads(sbom.read_text(encoding="utf-8"))
        assert sbom_value["schema"] == "chtholly-sbom-v1"
        assert sbom_value["spdxVersion"] == "SPDX-2.3"
        assert sbom_value["packages"][0]["filesAnalyzed"] is True
        assert sbom_value["package_sha256"]

        evidence = root / "evidence"
        evidence.mkdir()
        host = evidence / "windows-2022.json"
        lifecycle = evidence / "windows-2022-install.json"
        host.write_text(json.dumps({"schema": "host", "source_commit": commit}), encoding="utf-8")
        lifecycle.write_text(json.dumps({"schema": "lifecycle", "source_commit": commit}), encoding="utf-8")
        provenance = root / "provenance.json"
        invoke([sys.executable, str(args.script), "provenance", "--inputs", str(inputs),
                "--sbom", str(sbom), "--package", str(package),
                "--source-commit", commit, "--release-version", "0.2.0-preview",
                "--host", "windows-2022", "--target", "x86_64-pc-windows-msvc",
                "--evidence-root", str(evidence), "--evidence", str(host),
                str(lifecycle), "--output", str(provenance)])
        provenance_value = json.loads(provenance.read_text(encoding="utf-8"))
        assert provenance_value["schema"] == "chtholly-provenance-v1"
        assert provenance_value["package"]["sha256"] == sbom_value["package_sha256"]

        broken = root / "broken.json"
        broken.write_text(sbom.read_text(encoding="utf-8").replace(
            sbom_value["package_sha256"], "0" * 64), encoding="utf-8")
        invoke([sys.executable, str(args.script), "provenance", "--inputs", str(inputs),
                "--sbom", str(broken), "--package", str(package),
                "--source-commit", commit, "--release-version", "0.2.0-preview",
                "--host", "windows-2022", "--target", "x86_64-pc-windows-msvc",
                "--evidence-root", str(evidence), "--evidence", str(host),
                str(lifecycle), "--output", str(root / "rejected.json")], expected=1)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
