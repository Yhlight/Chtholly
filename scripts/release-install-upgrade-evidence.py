#!/usr/bin/env python3
"""Exercise signed packaging, install, upgrade, rollback, and removal."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from supply_chain_evidence import digest_file


def run(command: list[str], *, expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        encoding="utf-8", errors="replace", check=False,
    )
    if result.returncode != expected:
        raise RuntimeError(
            f"command returned {result.returncode}, expected {expected}: {command!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def jsonl_command_outputs(result: subprocess.CompletedProcess[str]) -> dict[str, str]:
    """Extract the stable command-output fields from a JSONL-v1 invocation."""

    values: dict[str, str] = {}
    for line in result.stdout.splitlines():
        if not line.strip():
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError as error:
            raise RuntimeError(f"toolchain install emitted invalid JSONL: {error}") from error
        if record.get("kind") == "command-output" and isinstance(record.get("name"), str):
            values[record["name"]] = str(record.get("value", ""))
    return values


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--toolchain", required=True, type=Path)
    parser.add_argument("--install-prefix", required=True, type=Path)
    parser.add_argument("--host", required=True)
    parser.add_argument("--host-name")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--package", type=Path)
    parser.add_argument("--sbom", type=Path)
    parser.add_argument("--inputs", type=Path)
    parser.add_argument("--source-commit")
    args = parser.parse_args()

    suffix = ".exe" if args.host == "x86_64-pc-windows-msvc" else ""
    with tempfile.TemporaryDirectory(prefix="chtholly-release-lifecycle-") as raw:
        root = Path(raw)
        manager_root = root / "manager"
        secret = root / "release.secret"
        public = root / "release.public"
        trust = root / "root.trust"
        run([str(args.toolchain), "key", "generate", "--secret", str(secret),
             "--public", str(public)])
        run([str(args.toolchain), "trust", "create", "-o", str(trust),
             "--version", "1", "--threshold", "1", "--key", str(public),
             "--secret-key", str(secret)])
        run([str(args.toolchain), "trust", "init", str(trust), "--root",
             str(manager_root)])

        releases: list[tuple[str, Path]] = []
        for version, commit_digit in (("0.2.0", "1"), ("0.2.1", "2")):
            tree = root / f"tree-{version}"
            shutil.copytree(args.install_prefix, tree)
            marker = tree / "release-marker.txt"
            marker.write_text(f"{version}\n", encoding="utf-8")
            commit = commit_digit * 40
            archive = root / f"chtholly-{version}.zip"
            run([str(args.toolchain), "package", str(tree), "-o", str(archive),
                 "--version", version, "--source-commit", commit,
                 "--host", args.host, "--secret-key", str(secret)])
            releases.append((f"{version}+{commit}", archive))

        first_id, first_archive = releases[0]
        second_id, second_archive = releases[1]
        run([str(args.toolchain), "verify", str(first_archive), "--root",
             str(manager_root), "--host", args.host])
        install_result = run(
            [str(args.toolchain), "--output-format", "jsonl-v1", "install",
             str(first_archive), "--root", str(manager_root), "--host", args.host])
        space_values = jsonl_command_outputs(install_result)
        required_space = {
            "space-payload-bytes", "space-index-bytes", "space-required-bytes",
            "space-available-bytes", "space-path", "space-sufficient",
        }
        missing_space = sorted(required_space - space_values.keys())
        if missing_space or space_values.get("space-sufficient") != "true":
            raise RuntimeError(
                "toolchain install preflight evidence is incomplete: "
                f"missing={missing_space}, values={space_values!r}"
            )
        run([str(args.toolchain), "activate", first_id, "--root", str(manager_root)])
        run([str(args.toolchain), "upgrade", str(second_archive), "--root",
             str(manager_root), "--host", args.host])
        listed = run([str(args.toolchain), "list", "--root", str(manager_root)]).stdout
        if f"{first_id}\tinactive" not in listed or f"{second_id}\tactive" not in listed:
            raise RuntimeError(f"upgrade did not activate the new generation: {listed!r}")

        active_compiler = manager_root / "generations" / second_id / "bin" / f"chthollyc{suffix}"
        run([str(active_compiler), "--version"])
        run([str(args.toolchain), "rollback", "--root", str(manager_root)])
        rollback_list = run([str(args.toolchain), "list", "--root", str(manager_root)]).stdout
        if f"{first_id}\tactive" not in rollback_list:
            raise RuntimeError(f"rollback did not restore the first generation: {rollback_list!r}")

        tampered = root / "tampered.zip"
        tampered_bytes = bytearray(first_archive.read_bytes())
        tampered_bytes[len(tampered_bytes) // 2] ^= 0x01
        tampered.write_bytes(tampered_bytes)
        run([str(args.toolchain), "verify", str(tampered), "--root",
             str(manager_root), "--host", args.host], expected=1)
        run([str(args.toolchain), "remove", second_id, "--root", str(manager_root)])
        final_list = run([str(args.toolchain), "list", "--root", str(manager_root)]).stdout
        if final_list.strip() != f"{first_id}\tactive":
            raise RuntimeError(f"unexpected final generation state: {final_list!r}")

    evidence = {
        "schema": "chtholly-release-install-evidence-v1",
        "host": args.host_name or args.host,
        "target": args.host,
        "package": True,
        "verify": True,
        "install": True,
        "activate": True,
        "upgrade": True,
        "active_compiler_preflight": True,
        "rollback": True,
        "tamper_rejection": True,
        "remove_inactive": True,
        "space_preflight": {
            key: (int(space_values[key]) if key.endswith("-bytes") else space_values[key])
            for key in sorted(required_space)
        },
        "valid": True,
    }
    if args.source_commit:
        evidence["source_commit"] = args.source_commit
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
    args.output.write_text(json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        print(f"release install evidence failed: {error}", file=sys.stderr)
        raise SystemExit(1)
