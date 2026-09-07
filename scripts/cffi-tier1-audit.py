#!/usr/bin/env python3
"""Validate the declared Tier-1 CFFI evidence inventory."""

from __future__ import annotations

import argparse
import pathlib
import sys
import tomllib


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", type=pathlib.Path, default=pathlib.Path("."))
    parser.add_argument("--manifest", type=pathlib.Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = args.source_dir.resolve()
    manifest_path = args.manifest or (root / "support/chtholly-cffi-tier1.toml")
    with manifest_path.open("rb") as stream:
        manifest = tomllib.load(stream)
    failures: list[str] = []
    if manifest.get("schema") != "chtholly-cffi-tier1-v1":
        failures.append("unsupported Tier-1 CFFI manifest schema")
    required_targets = set(manifest.get("required_targets", []))
    if not {"x86_64-pc-windows-msvc", "x86_64-unknown-linux-gnu"} <= required_targets:
        failures.append("Tier-1 CFFI manifest must require Windows and Linux x86_64 targets")
    test_name = manifest.get("test_manifest", "")
    test_source = root / "tests/chtholly_cffi_tool_tests.py"
    if not test_source.is_file():
        failures.append("Tier-1 CFFI test source is missing")
    test_text = test_source.read_text(encoding="utf-8") if test_source.is_file() else ""
    cases = manifest.get("case", [])
    if not cases:
        failures.append("Tier-1 CFFI manifest declares no cases")
    seen: set[str] = set()
    for case in cases:
        case_id = case.get("id", "")
        if not case_id or case_id in seen:
            failures.append(f"duplicate or empty Tier-1 case id: {case_id!r}")
        seen.add(case_id)
        for header in case.get("provider_headers", []):
            if not (root / header).is_file():
                failures.append(f"{case_id}: missing provider header {header}")
        for marker in case.get("markers", []):
            if marker not in test_text:
                failures.append(f"{case_id}: test source lacks marker {marker!r}")
        capabilities = set(case.get("capabilities", []))
        if not capabilities:
            failures.append(f"{case_id}: no capability evidence declared")
        if case_id.endswith("-upgrade"):
            required_capabilities = {
                "generate", "regenerate", "verify", "independent-consumer",
                "native-failure", "warm-reuse",
            }
            missing_capabilities = sorted(required_capabilities - capabilities)
            if missing_capabilities:
                failures.append(
                    f"{case_id}: missing capabilities "
                    + ", ".join(missing_capabilities)
                )
    if not test_name:
        failures.append("Tier-1 CFFI manifest has no test manifest name")
    else:
        test_manifest = (root / "tests/chtholly-tests.toml.in").read_text(
            encoding="utf-8"
        )
        if f'name = "{test_name}"' not in test_manifest:
            failures.append(f"Tier-1 test manifest entry is missing: {test_name}")
        if "--evidence-output" not in test_manifest or \
                "chtholly-cffi-tier1-evidence-v1" not in test_text:
            failures.append("Tier-1 native test does not publish structured evidence")
    docs_text = (root / "docs/cffi.md").read_text(encoding="utf-8")
    for required in ("SQLite", "zlib", "libcurl", "BCrypt", "POSIX"):
        if required not in docs_text:
            failures.append(f"CFFI documentation lacks Tier-1 subject {required!r}")
    print(f"cffi-tier1-audit cases={len(cases)} required_targets={len(manifest.get('required_targets', []))}")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1 if args.check else 0
    print("Tier-1 CFFI evidence inventory is closed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
