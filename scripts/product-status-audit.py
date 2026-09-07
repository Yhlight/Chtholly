#!/usr/bin/env python3
"""Validate the product release baseline and capability-status evidence."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
import tomllib


ALLOWED_STATUS = {"preview", "experimental", "design-pending"}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", type=pathlib.Path, default=pathlib.Path("."))
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = args.source_dir.resolve()
    status_path = root / "support/chtholly-product-status.toml"
    failures: list[str] = []
    try:
        status = tomllib.loads(status_path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as error:
        print(f"product-status-audit: unable to read status: {error}")
        return 1

    if status.get("schema") != "chtholly-product-status-v1":
        failures.append("product status schema is not chtholly-product-status-v1")
    if status.get("product") != "0.2.0-preview":
        failures.append("product status does not describe the current preview")
    if status.get("host_target") != "windows-x64":
        failures.append("preview host target must remain windows-x64")
    if status.get("required_hosts") != ["windows-x64", "linux-x64"]:
        failures.append("required preview hosts must be Windows and Linux x64")
    if status.get("optional_hosts") != ["macos-arm64"]:
        failures.append("macOS arm64 must remain the optional preview host")

    language_header = (root / "include/chtholly/Basic/LanguageVersion.h").read_text(
        encoding="utf-8"
    )
    constants = {
        "semantic_artifact_epoch": r"CurrentSemanticArtifactEpoch\s*=\s*(\d+)",
        "standard_library_epoch": r"CurrentStandardLibraryEpoch\s*=\s*(\d+)",
    }
    for key, pattern in constants.items():
        match = re.search(pattern, language_header)
        if not match or int(match.group(1)) != status.get(key):
            failures.append(f"{key} disagrees with LanguageVersion.h")

    stdlib = tomllib.loads(
        (root / "stdlib/manifest.toml").read_text(encoding="utf-8")
    )
    if stdlib.get("library_api") != status.get("standard_library_api"):
        failures.append("standard-library API epoch disagrees with stdlib manifest")

    capabilities = status.get("capability", [])
    if not capabilities:
        failures.append("product status has no capabilities")
    seen: set[str] = set()
    for capability in capabilities:
        identifier = capability.get("id", "")
        if not identifier or identifier in seen:
            failures.append(f"duplicate or missing capability id: {identifier!r}")
        seen.add(identifier)
        if capability.get("status") not in ALLOWED_STATUS:
            failures.append(f"invalid status for capability {identifier!r}")
        evidence = capability.get("evidence", [])
        if not evidence:
            failures.append(f"capability {identifier!r} has no evidence")
        for path in evidence:
            if not (root / path).is_file():
                failures.append(f"capability {identifier!r} evidence is missing: {path}")

    print(
        "product-status-audit "
        f"capabilities={len(capabilities)} "
        f"language_versions={len(status.get('language_versions', []))} "
        f"status={status.get('product', '')}"
    )
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1 if args.check else 0
    print("product release baseline is consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
