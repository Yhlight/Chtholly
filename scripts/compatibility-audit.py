#!/usr/bin/env python3
"""Audit the published compatibility matrix against implementation facts.

The compiler owns the behavior and constants.  This audit owns the release
contract: it catches documentation/support-manifest drift before a release or
artifact is published.  It intentionally does not attempt to deserialize
artifacts and never changes the source tree.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - Python 3.10 fallback
    import tomli as tomllib  # type: ignore


SCHEMA = "chtholly-compatibility-v1"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def require_text(path: Path, needle: str, errors: list[str]) -> None:
    if needle not in read(path):
        errors.append(f"{path}: missing {needle!r}")


def require_pattern(path: Path, pattern: str, expected: str, errors: list[str]) -> None:
    text = read(path)
    if not re.search(pattern, text, re.MULTILINE):
        errors.append(f"{path}: missing {expected}")


def value(table: dict[str, Any], *keys: str) -> Any:
    current: Any = table
    for key in keys:
        current = current[key]
    return current


def audit(source_dir: Path) -> list[str]:
    errors: list[str] = []
    matrix_path = source_dir / "support" / "chtholly-compatibility.toml"
    try:
        matrix = tomllib.loads(read(matrix_path))
    except (OSError, ValueError, KeyError) as error:
        return [f"compatibility matrix cannot be read: {error}"]

    if matrix.get("schema") != SCHEMA:
        errors.append(f"compatibility matrix schema is not {SCHEMA}")

    language = source_dir / "include" / "chtholly" / "Basic" / "LanguageVersion.h"
    public = source_dir / "include" / "chtholly" / "Compiler" / "PublicInterface.h"
    concrete_header = source_dir / "include" / "chtholly" / "Compiler" / "ConcreteSpecialization.h"
    concrete = source_dir / "lib" / "Compiler" / "IR" / "ConcreteSpecializationSupportInternal.h"
    concrete_format = source_dir / "lib" / "Compiler" / "IR" / "ConcreteSpecializationCodecEncoding.inc"
    package = source_dir / "lib" / "Compiler" / "IR" / "IncrementalDependenciesCodecInternal.h"
    package_format = source_dir / "include" / "chtholly" / "Driver" / "PackageArtifact.h"
    package_state = source_dir / "lib" / "Compiler" / "IR" / "IncrementalDependenciesCodecPrimitives.inc"
    stdlib = source_dir / "include" / "chtholly" / "Driver" / "CompilerStandardLibrary.h"
    cfdl = source_dir / "include" / "chtholly" / "Compiler" / "PublicInterface.h"
    cffi = source_dir / "lib" / "Compiler" / "Interop" / "CFFIIdentity.cpp"
    product_path = source_dir / "support" / "chtholly-product-status.toml"

    for path in (language, public, concrete_header, concrete, concrete_format, package, package_format,
                 package_state, stdlib, cfdl, cffi, product_path):
        if not path.is_file():
            errors.append(f"missing compatibility source: {path}")

    if errors:
        return errors

    semantic_epoch = value(matrix, "semantic", "artifact_epoch")
    require_text(language, f"CurrentSemanticArtifactEpoch = {semantic_epoch}", errors)
    require_text(language, f"CurrentStandardLibraryEpoch = {value(matrix, 'stdlib', 'epoch')}", errors)
    require_text(public, f"CurrentSemanticEpoch = {value(matrix, 'semantic', 'public_interface_epoch')}", errors)
    require_text(concrete_header,
                 f"ConcreteSpecializationComponentFormat = {value(matrix, 'concrete', 'format')}", errors)
    require_text(concrete, f'"{value(matrix, "concrete", "magic")}"', errors)
    require_text(concrete_format, "ComponentVersion =", errors)
    require_text(package, f'"{value(matrix, "package", "state_magic")}"', errors)
    require_text(package_state,
                 f"StateFormatVersion = {value(matrix, 'package', 'state_format')}", errors)
    require_text(package_format,
                 f'PackageArtifactFormatVersion = "{value(matrix, "package", "artifact_format")}"', errors)
    require_text(stdlib, f"CompilerStandardLibraryFormatVersion = {value(matrix, 'stdlib', 'format')}", errors)
    require_text(stdlib, f"CompilerStandardLibraryApiEpoch = {value(matrix, 'stdlib', 'api_epoch')}", errors)
    require_text(public, f"CurrentCFDLSemanticEpoch = {value(matrix, 'interop', 'cfdl_semantic_epoch')}", errors)
    require_text(public, f"CurrentSchemaEpoch = {value(matrix, 'interop', 'cfdl_schema_epoch')}", errors)
    require_text(cffi, f'out << "{value(matrix, "interop", "cffi_receipt")}', errors)

    product = read(product_path)
    for token in (
        f'product = "{matrix["product"]}"',
        f'semantic_artifact_epoch = {semantic_epoch}',
        f'standard_library_epoch = {value(matrix, "stdlib", "epoch")}',
        f'standard_library_api = {value(matrix, "stdlib", "api_epoch")}',
        f'package_state_format = {value(matrix, "package", "state_format")}',
        f'concrete_component_format = {value(matrix, "concrete", "format")}',
        f'component_abi_epoch = {value(matrix, "abi", "component_epoch")}',
    ):
        if token not in product:
            errors.append(f"{product_path}: missing {token!r}")

    policy = matrix.get("policy", {})
    for key in ("unknown_records", "stale_records", "future_records", "rebuild"):
        if policy.get(key) not in {"reject", "explicit"}:
            errors.append(f"compatibility policy {key} must be reject or explicit")

    versions = matrix.get("source_versions", [])
    if versions != [f"1.{minor}" for minor in range(0, 10)] + ["1.10"]:
        errors.append("source_versions must enumerate frozen 1.0 through 1.10")
    if "windows-x64" not in matrix.get("required_hosts", []) or "linux-x64" not in matrix.get("required_hosts", []):
        errors.append("windows-x64 and linux-x64 must be required hosts")
    if "macos-arm64" not in matrix.get("optional_hosts", []):
        errors.append("macos-arm64 must remain an optional preview host")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Audit Chtholly compatibility facts")
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--check", action="store_true", help="fail on drift")
    args = parser.parse_args()
    errors = audit(args.source_dir.resolve())
    if errors:
        print("compatibility-audit=fail", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(f"compatibility-audit=pass schema={SCHEMA}")
    print("policy=unknown/stale/future reject; rebuild explicit")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
