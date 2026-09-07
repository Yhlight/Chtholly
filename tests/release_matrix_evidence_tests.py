#!/usr/bin/env python3
"""Unit coverage for required-host release parity aggregation."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", type=pathlib.Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="chtholly-release-matrix-") as raw:
        root = pathlib.Path(raw)
        hosts = root / "hosts"
        lifecycle = root / "lifecycle"
        hosts.mkdir()
        lifecycle.mkdir()
        common = {
            "schema": "chtholly-release-host-evidence-v3",
            "valid": True,
            "compiler_version": "chthollyc release 0.2.0-preview+" + "a" * 40,
            "source_commit": "a" * 40,
            "stdlib": "format=5 contract=14 api=19 modules=28",
            "runtime": "runtime_v1",
            "target": "x86_64-pc-windows-msvc",
            "abi": {
                "target": "x86_64-pc-windows-msvc",
                "pointer_width_bits": 64,
                "endianness": "little",
                "component_epoch": 1,
                "runtime_epoch": "v1",
            },
        }
        for host in ("windows-2022", "ubuntu-24.04"):
            host_common = dict(common)
            host_common["target"] = ("x86_64-pc-windows-msvc" if
                                      host == "windows-2022" else
                                      "x86_64-unknown-linux-gnu")
            host_common["abi"] = dict(common["abi"], target=host_common["target"])
            (hosts / f"{host}.json").write_text(
                json.dumps(dict(host_common, host=host)), encoding="utf-8"
            )
            (lifecycle / f"{host}-install.json").write_text(
                json.dumps({
                    "schema": "chtholly-release-install-evidence-v1",
                    "host": host,
                    "valid": True,
                }),
                encoding="utf-8",
            )
        output = root / "matrix.json"
        subprocess.run(
            [sys.executable, str(args.script), "--input-dir", str(hosts),
             "--install-dir", str(lifecycle), "--output", str(output)],
            check=True,
        )
        matrix = json.loads(output.read_text(encoding="utf-8"))
        assert matrix["valid"] is True
        assert matrix["parity"]["windows_linux"] is True
        assert set(matrix["required_hosts"]) == {"windows-2022", "ubuntu-24.04"}

        provenance = root / "provenance"
        provenance.mkdir()
        for host in ("windows-2022", "ubuntu-24.04"):
            package_digest = ("b" if host == "windows-2022" else "c") * 64
            host_value = json.loads((hosts / f"{host}.json").read_text(encoding="utf-8"))
            host_value["package_sha256"] = package_digest
            (hosts / f"{host}.json").write_text(json.dumps(host_value), encoding="utf-8")
            lifecycle_value = json.loads((lifecycle / f"{host}-install.json").read_text(encoding="utf-8"))
            lifecycle_value["source_commit"] = "a" * 40
            (lifecycle / f"{host}-install.json").write_text(json.dumps(lifecycle_value), encoding="utf-8")
            (provenance / f"{host}.json").write_text(json.dumps({
                "schema": "chtholly-provenance-v1",
                "host": host,
                "source_commit": "a" * 40,
                "package": {"sha256": package_digest},
                "evidence": [
                    {"path": f"{host}.json", "sha256": hashlib.sha256(
                        (hosts / f"{host}.json").read_bytes()).hexdigest()},
                    {"path": f"{host}-install.json", "sha256": hashlib.sha256(
                        (lifecycle / f"{host}-install.json").read_bytes()).hexdigest()},
                ],
            }), encoding="utf-8")
        strict_output = root / "strict-matrix.json"
        subprocess.run(
            [sys.executable, str(args.script), "--input-dir", str(hosts),
             "--install-dir", str(lifecycle), "--provenance-dir", str(provenance),
             "--output", str(strict_output)], check=True,
        )
        assert len(json.loads(strict_output.read_text(encoding="utf-8"))["provenance"]) == 2

        broken = dict(common, host="ubuntu-24.04", stdlib="format=5 contract=14 api=17 modules=28")
        broken["target"] = "x86_64-unknown-linux-gnu"
        broken["abi"] = dict(common["abi"], target=broken["target"])
        (hosts / "ubuntu-24.04.json").write_text(json.dumps(broken), encoding="utf-8")
        failed = subprocess.run(
            [sys.executable, str(args.script), "--input-dir", str(hosts),
             "--install-dir", str(lifecycle), "--output", str(root / "broken.json")],
            check=False, stderr=subprocess.DEVNULL,
        )
        assert failed.returncode != 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
