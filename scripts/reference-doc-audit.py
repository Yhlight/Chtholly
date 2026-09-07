#!/usr/bin/env python3
"""Check standard-library and diagnostic reference coverage."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
import tomllib


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", type=pathlib.Path, default=pathlib.Path("."))
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = args.source_dir.resolve()
    failures: list[str] = []
    stdlib_doc = (root / "docs/stdlib-reference.md").read_text(encoding="utf-8")
    diagnostics_doc = (root / "docs/diagnostics.md").read_text(encoding="utf-8")
    with (root / "stdlib/manifest.toml").open("rb") as stream:
        manifest = tomllib.load(stream)
    modules = manifest.get("module", {})
    for key, module in modules.items():
        name = module.get("name", "")
        path = module.get("path", "")
        if name not in stdlib_doc:
            failures.append(f"stdlib module {name!r} ({key}) is undocumented")
        if path and not (root / "stdlib" / path).is_file():
            failures.append(f"stdlib manifest path is missing: {path}")
    diagnostic_text = (root / "include/chtholly/Compiler/DiagnosticKind.def").read_text(
        encoding="utf-8"
    )
    codes = set(re.findall(r'"(chtholly\.[^"]+)"', diagnostic_text))
    categories = {code.split(".")[2] for code in codes if code.count(".") >= 2}
    for category in sorted(categories):
        if f"chtholly.next.{category}." not in diagnostics_doc:
            failures.append(f"diagnostic family {category!r} is undocumented")
    for required in ("DiagnosticKind.def", "output-format jsonl", "LSP"):
        if required not in diagnostics_doc:
            failures.append(f"diagnostic reference is missing {required!r}")
    expected = len(modules)
    print(f"reference-doc-audit stdlib_modules={expected} diagnostic_codes={len(codes)}")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1 if args.check else 0
    print("standard-library and diagnostic references are consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
