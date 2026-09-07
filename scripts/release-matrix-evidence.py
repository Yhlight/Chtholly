#!/usr/bin/env python3
"""Aggregate host evidence and enforce required Windows/Linux parity."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from supply_chain_evidence import digest_file


REQUIRED = {"windows-2022", "ubuntu-24.04"}
OPTIONAL = {"macos-14"}


def release_version(value: str) -> str:
    match = re.search(r"\brelease\s+(\S+)", value)
    return match.group(1) if match else value


def runtime_abi(value: str) -> str:
    match = re.search(r"runtime_v(\d+)", value)
    return match.group(1) if match else ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--install-dir", type=Path)
    parser.add_argument("--provenance-dir", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    hosts: dict[str, dict] = {}
    host_paths: dict[str, Path] = {}
    for path in args.input_dir.rglob("*.json"):
        evidence = json.loads(path.read_text(encoding="utf-8"))
        if evidence.get("schema") != "chtholly-release-host-evidence-v3":
            continue
        host = evidence.get("host")
        if host in hosts:
            raise RuntimeError(f"duplicate host evidence: {host}")
        hosts[host] = evidence
        host_paths[host] = path

    missing = REQUIRED - hosts.keys()
    invalid_required = sorted(
        host for host in REQUIRED if host in hosts and hosts[host].get("valid") is not True
    )
    if missing or invalid_required:
        raise RuntimeError(
            f"release matrix incomplete: missing={sorted(missing)}, "
            f"invalid_required={invalid_required}"
        )

    lifecycle: dict[str, dict] = {}
    lifecycle_paths: dict[str, Path] = {}
    if args.install_dir:
        for path in args.install_dir.rglob("*.json"):
            item = json.loads(path.read_text(encoding="utf-8"))
            if item.get("schema") != "chtholly-release-install-evidence-v1":
                continue
            lifecycle[item.get("host", "")] = item
            lifecycle_paths[item.get("host", "")] = path
        missing_lifecycle = REQUIRED - lifecycle.keys()
        invalid_lifecycle = sorted(
            host for host in REQUIRED
            if host in lifecycle and lifecycle[host].get("valid") is not True
        )
        if missing_lifecycle or invalid_lifecycle:
            raise RuntimeError(
                "release lifecycle evidence incomplete: "
                f"missing={sorted(missing_lifecycle)}, invalid={invalid_lifecycle}"
            )

    provenance: list[dict] = []
    if args.provenance_dir:
        for path in sorted(args.provenance_dir.rglob("*.json")):
            item = json.loads(path.read_text(encoding="utf-8"))
            if item.get("schema") != "chtholly-provenance-v1":
                continue
            item = dict(item)
            item["evidence_file_sha256"] = digest_file(path)
            provenance.append(item)
        if len(provenance) != len(REQUIRED):
            raise RuntimeError("release provenance evidence is incomplete")

    required_items = [hosts[name] for name in sorted(REQUIRED)]
    for item in required_items:
        abi = item.get("abi", {})
        if (abi.get("target") != item.get("target") or
                abi.get("pointer_width_bits") != 64 or
                abi.get("endianness") != "little" or
                abi.get("component_epoch") != 1 or
                abi.get("runtime_epoch") != "v1"):
            raise RuntimeError(
                f"host ABI evidence is incomplete or invalid: {item.get('host')}"
            )
    versions = {release_version(item.get("compiler_version", "")) for item in required_items}
    commits = {item.get("source_commit", "") for item in required_items}
    stdlib = {item.get("stdlib", "") for item in required_items}
    runtime = {runtime_abi(item.get("runtime", "")) for item in required_items}
    # Target triples are expected to differ between Windows and Linux.  The
    # representation/runtime ABI facts must agree, while each host keeps its
    # own resolved target in the host record.
    abi_facts = {
        json.dumps({key: item["abi"][key] for key in (
            "pointer_width_bits", "endianness", "component_epoch",
            "runtime_epoch")}, sort_keys=True)
        for item in required_items
    }
    if (len(versions) != 1 or len(commits) != 1 or len(stdlib) != 1 or
            len(runtime) != 1 or len(abi_facts) != 1):
        raise RuntimeError(
            "Windows/Linux preview parity failed: "
            f"versions={sorted(versions)}, commits={sorted(commits)}, "
            f"stdlib={sorted(stdlib)}, runtime_abi={sorted(runtime)}, "
            f"abi={sorted(abi_facts)}"
        )
    if args.provenance_dir:
        expected_commit = next(iter(commits))
        provenance_commits = {item.get("source_commit") for item in provenance}
        if provenance_commits != {expected_commit}:
            raise RuntimeError(
                "release provenance parity failed: "
                f"commits={sorted(provenance_commits)}, "
                f"packages={[item.get('package', {}).get('sha256') for item in provenance]}"
            )
        for host in sorted(REQUIRED):
            matching = [item for item in provenance if item.get("host") == host]
            if len(matching) != 1:
                raise RuntimeError(f"missing provenance for {host}")
            entry = matching[0]
            if hosts[host].get("source_commit") != entry.get("source_commit"):
                raise RuntimeError(f"host/provenance source commit mismatch: {host}")
            host_package = hosts[host].get("package_sha256")
            if host_package and host_package != entry.get("package", {}).get("sha256"):
                raise RuntimeError(f"host/provenance package mismatch: {host}")
            host_sbom = hosts[host].get("sbom_sha256")
            if host_sbom and host_sbom != entry.get("sbom", {}).get("sha256"):
                raise RuntimeError(f"host/provenance SBOM mismatch: {host}")
            host_inputs = hosts[host].get("inputs_sha256")
            if host_inputs and host_inputs != entry.get("inputs", {}).get("sha256"):
                raise RuntimeError(f"host/provenance inputs mismatch: {host}")
            if lifecycle.get(host, {}).get("source_commit") not in (None, expected_commit):
                raise RuntimeError(f"lifecycle source commit mismatch: {host}")
            lifecycle_package = lifecycle.get(host, {}).get("package_sha256")
            if lifecycle_package and lifecycle_package != entry.get("package", {}).get("sha256"):
                raise RuntimeError(f"lifecycle/provenance package mismatch: {host}")
            lifecycle_sbom = lifecycle.get(host, {}).get("sbom_sha256")
            if lifecycle_sbom and lifecycle_sbom != entry.get("sbom", {}).get("sha256"):
                raise RuntimeError(f"lifecycle/provenance SBOM mismatch: {host}")
            referenced = entry.get("evidence", [])
            for reference in referenced:
                reference_name = reference.get("path")
                candidates: list[Path] = []
                # Host and lifecycle evidence are retained in their dedicated
                # download trees; stage-boundary reports are downloaded with
                # the host bundle.  Resolve by basename only after requiring a
                # unique match, preserving the existing digest closure.
                for root in (args.input_dir, args.install_dir):
                    if root:
                        candidates.extend(path for path in root.rglob("*.json")
                                          if path.name == reference_name)
                if len(candidates) != 1 or digest_file(candidates[0]) != reference.get("sha256"):
                    raise RuntimeError(f"provenance evidence digest mismatch: {host}")
    for host, item in hosts.items():
        if host in OPTIONAL and item.get("valid") is not True:
            item = dict(item)
            item["optional_failure"] = True
            hosts[host] = item

    matrix = {
        "schema": "chtholly-release-matrix-v2",
        "required_hosts": sorted(REQUIRED),
        "optional_hosts": sorted(OPTIONAL),
        "hosts": [hosts[name] for name in sorted(hosts)],
        "parity": {
            "release_version": next(iter(versions)),
            "source_commit": next(iter(commits)),
            "stdlib": next(iter(stdlib)),
            "runtime_abi": next(iter(runtime)),
            "abi": json.loads(next(iter(abi_facts))),
            "windows_linux": True,
        },
        "lifecycle": lifecycle,
        "provenance": provenance,
        "valid": True,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(matrix, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        print(f"release matrix evidence failed: {error}", file=sys.stderr)
        raise SystemExit(1)
