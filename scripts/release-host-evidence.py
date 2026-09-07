#!/usr/bin/env python3
"""Validate an installed Chtholly preview tree on its native host."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from supply_chain_evidence import digest_file


HOSTS = {
    "windows-2022": {
        "system": "Windows", "machines": {"amd64", "x86_64"},
        "suffix": ".exe", "target": "x86_64-pc-windows-msvc", "required": True,
    },
    "ubuntu-24.04": {
        "system": "Linux", "machines": {"amd64", "x86_64"},
        "suffix": "", "target": "x86_64-unknown-linux-gnu", "required": True,
    },
    "macos-14": {
        "system": "Darwin", "machines": {"arm64", "aarch64"},
        "suffix": "", "target": "aarch64-apple-darwin", "required": False,
    },
}


def run(command: list[str], *, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, encoding="utf-8", errors="replace", check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {command!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def tree_digest(root: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    count = 0
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        relative = path.relative_to(root).as_posix()
        data = path.read_bytes()
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(len(data)).encode("ascii"))
        digest.update(b"\0")
        digest.update(hashlib.sha256(data).digest())
        count += 1
    return digest.hexdigest(), count


def command_records(result: subprocess.CompletedProcess[str]) -> list[dict]:
    return [json.loads(line) for line in result.stdout.splitlines() if line.strip()]


def record_value(records: list[dict], name: str) -> str:
    for record in records:
        if record.get("kind") == "command-output" and record.get("name") == name:
            return str(record.get("value", ""))
    return ""


def run_preview_smoke(compiler: Path) -> dict[str, bool]:
    with tempfile.TemporaryDirectory(prefix="chtholly-installed-preview-") as raw:
        root = Path(raw)
        app = root / "app"
        library = root / "library"
        run([str(compiler), "new", str(app), "--name", "installed_app"])
        run([str(compiler), "new", str(library), "--lib", "--name", "installed_lib"])
        manifest = app / "chtholly.toml"
        manifest.write_text(
            manifest.read_text(encoding="utf-8")
            + '\n[dependencies]\ninstalled_lib = { path = "../library" }\n',
            encoding="utf-8",
        )
        (app / "src" / "main.cns").write_text(
            "module main;\n\n"
            "import installed_lib;\n\n"
            "fn main(): i32 { return installed_lib::identity(0); }\n",
            encoding="utf-8",
        )
        run([str(compiler), "check", "--project", str(app)])
        run([str(compiler), "build", "--project", str(app)])
        run([str(compiler), "build", "--project", str(app)])
        run([str(compiler), "run", "--project", str(app), "--", "installed"])
        missing = subprocess.run(
            [str(compiler), "check", "--project", str(root / "missing")],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            encoding="utf-8", errors="replace", check=False,
        )
        if missing.returncode == 0 or "chtholly" not in missing.stderr.lower():
            raise RuntimeError("installed compiler missing-project diagnostic failed")
    return {
        "clean_scaffold": True, "local_path_dependency": True, "check": True,
        "cold_build": True, "warm_build": True, "native_run": True,
        "negative_diagnostic": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True, choices=tuple(HOSTS))
    parser.add_argument("--install-prefix", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--package", type=Path)
    parser.add_argument("--sbom", type=Path)
    parser.add_argument("--inputs", type=Path)
    parser.add_argument("--source-commit")
    args = parser.parse_args()

    host = HOSTS[args.host]
    if platform.system() != host["system"] or platform.machine().lower() not in host["machines"]:
        raise RuntimeError(
            f"host evidence {args.host} cannot run on "
            f"{platform.system()}-{platform.machine()}"
        )
    suffix = host["suffix"]
    compiler = args.install_prefix / "bin" / f"chthollyc{suffix}"
    manager = args.install_prefix / "bin" / f"chtholly-toolchain{suffix}"
    driver = args.install_prefix / "libexec" / "chtholly" / f"chthollyc-driver{suffix}"
    for path in (compiler, manager, driver):
        if not path.is_file():
            raise RuntimeError(f"installed release surface is missing: {path}")

    version = run([str(compiler), "--version"]).stdout.strip()
    version_match = re.search(r"\+([0-9a-f]{40})\b", version)
    source_commit = version_match.group(1) if version_match else ""
    if args.source_commit and source_commit != args.source_commit:
        raise RuntimeError(
            f"installed source commit mismatch: {source_commit} != {args.source_commit}")
    manager_help = run([str(manager), "--help", "--output-format", "jsonl"])
    manager_records = command_records(manager_help)
    if not manager_records or manager_records[-1].get("status") != "success":
        raise RuntimeError("installed manager JSONL contract failed")
    doctor_records = command_records(
        run([str(compiler), "doctor", "--output-format", "jsonl"])
    )
    if not doctor_records or doctor_records[-1].get("status") != "success":
        raise RuntimeError("installed compiler doctor did not report success")
    target = record_value(doctor_records, "target")
    if target != host["target"]:
        raise RuntimeError(
            f"installed target mismatch for {args.host}: expected "
            f"{host['target']}, got {target!r}"
        )
    pointer_width = record_value(doctor_records, "pointer-width")
    endianness = record_value(doctor_records, "endianness")
    component_abi = record_value(doctor_records, "component-abi")
    runtime_abi_epoch = record_value(doctor_records, "runtime-abi")
    if pointer_width != "64" or endianness != "little":
        raise RuntimeError(
            f"installed target ABI facts are unsupported for {args.host}: "
            f"pointer-width={pointer_width!r}, endianness={endianness!r}"
        )
    if component_abi != "1" or runtime_abi_epoch != "v1":
        raise RuntimeError(
            f"installed ABI epoch mismatch for {args.host}: "
            f"component={component_abi!r}, runtime={runtime_abi_epoch!r}"
        )
    smoke = run_preview_smoke(compiler)
    digest, count = tree_digest(args.install_prefix)
    evidence = {
        "schema": "chtholly-release-host-evidence-v3",
        "host": args.host, "required": host["required"],
        "runner_platform": platform.platform(), "compiler_version": version,
        "source_commit": source_commit,
        "target": target,
        "abi": {
            "target": target,
            "pointer_width_bits": int(pointer_width),
            "endianness": endianness,
            "component_epoch": int(component_abi),
            "runtime_epoch": runtime_abi_epoch,
        },
        "stdlib": record_value(doctor_records, "stdlib"),
        "runtime": record_value(doctor_records, "runtime"),
        "install_tree_sha256": digest, "install_tree_file_count": count,
        "launcher_valid": True, "manager_jsonl_valid": True,
        "doctor_ready": True, "preview_smoke": smoke,
        "preview_smoke_ready": all(smoke.values()),
        "test_runner": "chtholly-test", "valid": True,
    }
    if args.package:
        evidence["package_sha256"] = digest_file(args.package.resolve())
        evidence["package"] = args.package.name
    if args.sbom:
        evidence["sbom_sha256"] = digest_file(args.sbom.resolve())
        evidence["sbom"] = args.sbom.name
    if args.inputs:
        evidence["inputs_sha256"] = digest_file(args.inputs.resolve())
        evidence["inputs"] = args.inputs.name
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        print(f"release host evidence failed: {error}", file=sys.stderr)
        raise SystemExit(1)
