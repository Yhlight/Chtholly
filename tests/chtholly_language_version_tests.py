#!/usr/bin/env python3

import argparse
import pathlib
import subprocess
import tempfile


def run(compiler: str, project: pathlib.Path, expected: int = 0):
    result = subprocess.run(
        [compiler, "check", "--project", str(project)],
        text=True, encoding="utf-8", stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False)
    if result.returncode != expected:
        raise AssertionError(
            f"check returned {result.returncode}, expected {expected}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def run_project(compiler: str, project_path: pathlib.Path):
    return subprocess.run(
        [compiler, "run", "--project", str(project_path)],
        text=True, encoding="utf-8", stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False)


def project(root: pathlib.Path, name: str, version: str,
            sources: dict[str, str]) -> pathlib.Path:
    target = root / f"{name}-{version.replace('.', '_')}"
    (target / "src").mkdir(parents=True)
    (target / "chtholly.toml").write_text(
        f'[package]\nname = "{name}"\nlanguage = "{version}"\n\n'
        '[build]\nentry = "src/main.cns"\nmodule_paths = ["src"]\n',
        encoding="utf-8")
    for filename, source in sources.items():
        (target / "src" / filename).write_text(source, encoding="utf-8")
    return target


def expect_gate(compiler: str, root: pathlib.Path, name: str,
                previous: str, admitted: str, sources: dict[str, str],
                diagnostic: str):
    rejected = run(compiler, project(root, name, previous, sources), expected=1)
    if diagnostic not in rejected.stderr:
        raise AssertionError(
            f"{name} omitted version diagnostic {diagnostic!r}: "
            f"{rejected.stderr!r}")
    run(compiler, project(root, name, admitted, sources))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="chtholly-language-version-") as raw:
        root = pathlib.Path(raw)
        expect_gate(
            args.chthollyc, root, "async_gate", "1.0", "1.1",
            {"main.cns": "module main;\nasync fn main(): i32 { return 0; }\n"},
            "chtholly.next.version.async-requires-1-1")
        expect_gate(
            args.chthollyc, root, "closure_gate", "1.1", "1.2",
            {"main.cns": (
                "module main;\n"
                "fn readonly(): i32 { let offset = 2; "
                "let add = fn [copy offset](value: i32): i32 { "
                "return value + offset; }; return add(1) + add(2); }\n"
                "fn main(): i32 { return readonly(); }\n")},
            "chtholly.next.version.callable-requires-1-2")
        expect_gate(
            args.chthollyc, root, "alias_gate", "1.2", "1.3",
            {"provider.cns": (
                "module provider;\npub fn value(): i32 { return 0; }\n"),
             "main.cns": (
                "module main;\nimport provider as p;\n"
                "fn main(): i32 { return p::value(); }\n")},
            "chtholly.next.version.module-alias-requires-1-3")
        expect_gate(
            args.chthollyc, root, "label_gate", "1.2", "1.3",
            {"main.cns": (
                "module main;\nfn main(): i32 { "
                "outer: while (true) { break outer; } return 0; }\n")},
            "chtholly.next.version.loop-label-requires-1-3")

        operator_rejected_source = {
            "main.cns": (
                "module main;\nstruct Number { value: i32; }\n"
                "fn main(): i32 { let left = Number { .value = 1 }; "
                "let right = Number { .value = 2 }; "
                "let result = left + right; return result.value; }\n")}
        operator_rejected = run(
            args.chthollyc,
            project(root, "operator_gate_rejected", "1.2",
                    operator_rejected_source),
            expected=1)
        if "chtholly.next.version.operator-requires-1-3" not in operator_rejected.stderr:
            raise AssertionError(
                "operator omitted 1.3 gate: " + operator_rejected.stderr)

        operator_admitted_source = {
            "main.cns": (
                "module main;\nimport std::ops;\n"
                "struct Number { value: i32; }\n"
                "impl std::ops::Addition<Number> for Number { "
                "alias Output = Number; "
                "fn add(self: const Self&, rhs: const Number&): Number { "
                "return Number { .value = self.value + rhs.value }; } }\n"
                "fn main(): i32 { let left = Number { .value = 1 }; "
                "let right = Number { .value = 2 }; "
                "let result = left + right; return result.value - 3; }\n")}
        run(args.chthollyc,
            project(root, "operator_gate_admitted", "1.3",
                    operator_admitted_source))

        concurrency_source = {
            "main.cns": (
                "module main;\nimport std::atomic;\n"
                "fn main(): i32 { let value = "
                "std::atomic::Atomic<i32>::init(1); "
                "return value.load(std::atomic::Ordering::Relaxed) - 1; }\n")}
        concurrency_rejected = run(
            args.chthollyc,
            project(root, "concurrency_gate", "1.0", concurrency_source),
            expected=1)
        if "introduced in 1.1" not in concurrency_rejected.stderr:
            raise AssertionError(
                "concurrency omitted 1.1 module gate: " +
                concurrency_rejected.stderr)
        run(args.chthollyc,
            project(root, "concurrency_gate", "1.1", concurrency_source))

        foreach_source = {
            "main.cns": (
                "module main;\nimport std::vec;\nfn main(): i32 { "
                "var values = std::vec::Vec<i32>::init(); "
                "values.push(17); "
                "foreach (let item: const i32& in values.iter()) { "
                "if (item != 17) { return 2; } } return 0; }\n")}
        rejected = run(
            args.chthollyc,
            project(root, "foreach_gate", "1.3", foreach_source), expected=1)
        if "chtholly.next.version.foreach-requires-1-4" not in rejected.stderr:
            raise AssertionError(f"foreach omitted 1.4 gate: {rejected.stderr!r}")
        run(args.chthollyc,
            project(root, "foreach_gate", "1.4", foreach_source))

        invalid_foreach = {
            "main.cns": (
                "module main;\nfn main(): i32 { "
                "foreach (let item in 0) { return item; } return 0; }\n")}
        invalid = run(
            args.chthollyc,
            project(root, "foreach_invalid", "1.4", invalid_foreach),
            expected=1)
        if "chtholly.next.sem.foreach.invalid-iterator" not in invalid.stderr:
            raise AssertionError(
                f"invalid foreach omitted iterator diagnostic: {invalid.stderr!r}")

        iterator_module = {
            "main.cns": (
                "module main;\nimport std::iter;\n"
                "fn main(): i32 { return 0; }\n")}
        iterator_rejected = run(
            args.chthollyc,
            project(root, "iterator_protocol_gate", "1.4", iterator_module),
            expected=1)
        if "introduced in 1.5" not in iterator_rejected.stderr:
            raise AssertionError(
                "iterator protocol omitted 1.5 module gate: " +
                iterator_rejected.stderr)
        run(args.chthollyc,
            project(root, "iterator_protocol_gate", "1.5", iterator_module))

        adapter_module = {
            "main.cns": (
                "module main;\nimport std::iter::adapters;\n"
                "fn main(): i32 { return 0; }\n")}
        adapter_rejected = run(
            args.chthollyc,
            project(root, "iterator_adapter_gate", "1.5", adapter_module),
            expected=1)
        if "introduced in 1.6" not in adapter_rejected.stderr:
            raise AssertionError(
                "iterator adapters omitted 1.6 module gate: " +
                adapter_rejected.stderr)
        run(args.chthollyc,
            project(root, "iterator_adapter_gate", "1.6", adapter_module))

        slice_v19 = {
            "main.cns": (
                "module main;\n"
                "fn read(values: slice<i32>, index: i32): i32 { "
                "let length: i32 = values.len; "
                "return if (index < length) { values[index] } else { 0 }; }\n"
                "fn main(): i32 { return 0; }\n")}
        slice_v110 = {
            "main.cns": (
                "module main;\n"
                "fn read(values: slice<i32>, index: u64): i32 { "
                "let length: u64 = values.len; "
                "return if (index < length) { values[index] } else { 0 }; }\n"
                "fn main(): i32 { return 0; }\n")}
        run(args.chthollyc, project(root, "slice_width_v19", "1.9", slice_v19))
        run(args.chthollyc, project(root, "slice_width_v110", "1.10", slice_v110))
        run(args.chthollyc,
            project(root, "slice_width_v19_rejects_u64", "1.9", slice_v110),
            expected=1)
        run(args.chthollyc,
            project(root, "slice_width_v110_rejects_i32", "1.10", slice_v19),
            expected=1)

        array_bounds = {
            "main.cns": (
                "module main;\n"
                "fn read(values: const i32[2]&, index: u64): i32 { "
                "return values[index]; }\n"
                "fn main(): i32 { let values: i32[2] = [10, 20]; "
                "return read(&values, 2u64); }\n")}
        slice_bounds = {
            "main.cns": (
                "module main;\n"
                "fn read(values: slice<i32>, index: u64): i32 { "
                "return values[index]; }\n"
                "fn main(): i32 { let values: i32[2] = [10, 20]; "
                "return read(values[..], 2u64); }\n")}
        for name, sources in (("array_bounds", array_bounds),
                              ("slice_bounds", slice_bounds)):
            trapped = run_project(
                args.chthollyc, project(root, name, "1.10", sources))
            if trapped.returncode == 0:
                raise AssertionError(f"{name} did not trap on an invalid index")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
