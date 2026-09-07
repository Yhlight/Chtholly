#!/usr/bin/env python3
"""Source-level container gate: public generic member publication and calls."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import tempfile


def run(command: list[str], expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, encoding="utf-8",
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            check=False)
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}: {command!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    parser.add_argument("--source-dir", required=True)
    args = parser.parse_args()
    root = pathlib.Path(args.source_dir)
    with tempfile.TemporaryDirectory(prefix="chtholly-hashmap-source-") as raw:
        project = pathlib.Path(raw) / "probe"
        (project / "src").mkdir(parents=True)
        (project / "chtholly.toml").write_text(
            '[package]\nname = "hashmap_source_probe"\nlanguage = "1.9"\n\n'
            '[build]\nentry = "src/main.cns"\nmodule_paths = ["src"]\n',
            encoding="utf-8")
        (project / "src" / "main.cns").write_text(
            "module main; import std::collections; import std::option;\n"
            "fn main(): i32 { var map = "
            "std::collections::HashMap<i32, i32>::make(); "
            "if (map.len() != 0usize) { return 1; } "
            "if (map.contains(1i32)) { return 2; } "
            "let inserted = map.insert(1i32, 9i32); "
            "if (!map.contains(1i32)) { return 3; } "
            "let replaced = map.insert(1i32, 11i32); "
            "if (!map.contains(1i32)) { return 12; } "
            "let i2 = map.insert(2i32, 20i32); let i3 = map.insert(3i32, 30i32); "
            "let i4 = map.insert(4i32, 40i32); let i5 = map.insert(5i32, 50i32); "
            "let i6 = map.insert(6i32, 60i32); let i7 = map.insert(7i32, 70i32); "
            "let eighth = map.insert(8i32, 80i32); let i9 = map.insert(9i32, 90i32); "
            "if (!map.contains(9i32)) { return 13; } "
            "if (map.len() != 9usize) { return 20; } "
            "let found = map.get(1i32); "
            "if (!found.is_some()) { return 14; } "
            "let found_value = (move found).unwrap(); "
            "if (*found_value != 11i32) { return 22; } "
            "let found_mut = map.get_mut(1i32); "
            "if (!found_mut.is_some()) { return 15; } "
            "let found_mut_value = (move found_mut).unwrap(); "
            "if (*found_mut_value != 11i32) { return 23; } "
            "let removed = map.remove(1i32); "
            "if (!removed.is_some()) { return 16; } "
            "if (map.contains(1i32)) { return 4; } "
            "let reserved = map.reserve(16usize); "
            "if (map.capacity() < 16usize) { return 5; } "
            "map.clear(); if (!map.is_empty()) { return 6; } "
            "var set = std::collections::HashSet<i32>::make(); "
            "if (set.contains(1i32)) { return 7; } "
            "let set_inserted = set.insert(1i32); "
            "if (!set.contains(1i32)) { return 8; } "
            "let set_duplicate = set.insert(1i32); "
            "if (set.len() != 1usize) { return 21; } "
            "let set_reserved = set.reserve(16usize); "
            "if (set.capacity() < 16usize) { return 9; } "
            "if (!set.remove(1i32)) { return 10; } "
            "set.clear(); if (!set.is_empty()) { return 11; } return 0; }\n",
            encoding="utf-8")
        run([args.chthollyc, "check", "--project", str(project)])
        run([args.chthollyc, "build", "--project", str(project)])
        executables = sorted((project / ".chtholly" / "build").rglob(
            "hashmap_source_probe.exe"))
        if len(executables) != 1:
            raise AssertionError(f"expected one executable, got {executables}")
        run([str(executables[0])])
        invalid = pathlib.Path(raw) / "borrow_invalid"
        (invalid / "src").mkdir(parents=True)
        (invalid / "chtholly.toml").write_text(
            '[package]\nname = "hashmap_borrow_invalid"\nlanguage = "1.9"\n\n'
            '[build]\nentry = "src/main.cns"\nmodule_paths = ["src"]\n',
            encoding="utf-8")
        (invalid / "src" / "main.cns").write_text(
            "module main; import std::collections; import std::option;\n"
            "fn main(): i32 { var map = "
            "std::collections::HashMap<i32, i32>::make(); "
            "map.insert(1i32, 2i32); var borrowed = map.get_mut(1i32); "
            "map.clear(); if (borrowed.is_some()) { return 1; } return 0; }\n",
            encoding="utf-8")
        invalid_result = run(
            [args.chthollyc, "check", "--project", str(invalid)], expected=1)
        if "borrow" not in invalid_result.stderr.lower():
            raise AssertionError(
                "expected a borrow invalidation diagnostic for get_mut")
    print("hashmap source: open-addressing runtime, Option borrows, and invalidation diagnostics")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
