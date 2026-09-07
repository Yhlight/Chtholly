#!/usr/bin/env python3

import argparse
import pathlib
import subprocess
import tempfile

from chtholly_test_support import single_native_executable


def invoke(command: list[str], expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command, text=True, encoding="utf-8", stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False)
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}: {command!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def project(root: pathlib.Path, name: str, version: str, source: str,
            dependencies: dict[str, str] | None = None,
            entry: bool = True) -> pathlib.Path:
    target = root / name
    (target / "src").mkdir(parents=True)
    manifest = (f'[package]\nname = "{name}"\nlanguage = "{version}"\n\n'
                '[build]\n')
    if entry:
        manifest += 'entry = "src/main.cns"\n'
    manifest += 'module_paths = ["src"]\n'
    if dependencies:
        manifest += "\n[dependencies]\n"
        for dependency, path in dependencies.items():
            manifest += f'{dependency} = {{ path = "{path}" }}\n'
    (target / "chtholly.toml").write_text(manifest, encoding="utf-8")
    (target / "src" / "main.cns").write_text(source, encoding="utf-8")
    return target


def expect_failure(compiler: str, target: pathlib.Path, code: str) -> None:
    result = invoke([compiler, "check", "--project", str(target)], expected=1)
    if code not in result.stderr:
        raise AssertionError(
            f"negative value-enum case omitted {code!r}\nstderr:\n{result.stderr}")
    if "chtholly.next.sem.unsupported" in result.stderr:
        raise AssertionError("value-enum rejection used UnsupportedSemantics")


RUNTIME_SOURCE = r"""module main;

enum Color {
  Red = 10,
  Green = 20,
  Blue = -4,
}

enum Limit {
  Min = -2147483648,
  Max = 2147483647,
}

const ColorSize: usize = sizeof(Color);
const ColorAlignment: usize = alignof(Color);

fn raw(color: Color): i32 { color as i32 }
fn raw_limit(limit: Limit): i32 { limit as i32 }

fn classify(color: Color): i32 {
  switch (color) {
    Color::Red => 1;
    Color::Green => 2;
    Color::Blue => 3;
  }
}

fn main(): i32 {
  if (ColorSize != 4usize || ColorAlignment != 4usize) { return 1; }
  if (raw(Color::Red) != 10 || raw(Color::Green) != 20 ||
      raw(Color::Blue) != -4) { return 2; }
  if (raw_limit(Limit::Min) != -2147483648 ||
      raw_limit(Limit::Max) != 2147483647) { return 4; }
  if (classify(Color::Red) != 1 || classify(Color::Green) != 2 ||
      classify(Color::Blue) != 3) { return 3; }
  return 0;
}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="chtholly-value-enum-") as raw:
        root = pathlib.Path(raw)

        gated = project(root, "value-enum-v17", "1.7", RUNTIME_SOURCE)
        expect_failure(args.chthollyc, gated,
                       "chtholly.next.version.value-enum-requires-1-8")

        runtime = project(root, "value-enum-runtime", "1.8", RUNTIME_SOURCE)
        semir = root / "value-enum.semir"
        command = "check" if args.check_only else "build"
        invoke([args.chthollyc, command, "--project", str(runtime),
                "--dump-semir", str(semir)])
        if not args.check_only:
            executable = single_native_executable(
                runtime / ".chtholly", "value-enum runtime project")
            invoke([str(executable)])
        semir_text = semir.read_text(encoding="utf-8")
        if "EnumTag" not in semir_text or "EnumInit" not in semir_text:
            raise AssertionError("value-enum SemIR omitted enum operations")

        provider = project(
            root, "colors", "1.8",
            "module colors;\npub enum Color { Red = 10, Blue = -4, }\n"
            "pub fn blue(): Color { Color::Blue }\n", entry=False)
        consumer = project(
            root, "consumer", "1.8",
            "module main;\nimport colors;\nfn main(): i32 { "
            "let color = colors::blue(); return color as i32 + 4; }\n",
            {"colors": "../colors"})
        invoke([args.chthollyc, command, "--project", str(consumer)])
        if not args.check_only:
            executable = single_native_executable(
                consumer / ".chtholly", "cross-package value enum")
            invoke([str(executable)])

        cases = [
            ("missing", "enum E { A = 1, B, }",
             "chtholly.next.sem.value-enum.explicit-discriminant-required"),
            ("duplicate", "enum E { A = 1, B = 1, }",
             "chtholly.next.sem.value-enum.duplicate-discriminant"),
            ("overflow", "enum E { A = 2147483648, }",
             "chtholly.next.sem.value-enum.invalid-discriminant"),
            ("underflow", "enum E { A = -2147483649, }",
             "chtholly.next.sem.value-enum.invalid-discriminant"),
            ("expression", "enum E { A = 1 + 2, }",
             "chtholly.next.sem.value-enum.invalid-discriminant"),
            ("negative-zero", "enum E { A = -0, B = 0, }",
             "chtholly.next.sem.value-enum.duplicate-discriminant"),
            ("payload", "enum E { A { i32 } = 1, }",
             "chtholly.next.sem.value-enum.invalid-declaration"),
            ("generic", "enum E<T> { A = 1, }",
             "chtholly.next.sem.value-enum.invalid-declaration"),
            ("lifecycle",
             "lifecycle(copy = delete, move = default, drop = default) "
             "enum E { A = 1, }",
             "chtholly.next.sem.value-enum.invalid-declaration"),
            ("representation", "repr(C) enum E { A = 1, }",
             "chtholly.next.sem.value-enum.invalid-declaration"),
            ("braces", "enum E { A = 1, }",
             "chtholly.next.sem.value-enum.braced-construction"),
            ("reverse", "enum E { A = 1, }",
             "chtholly.next.sem.invalid-cast"),
            ("implicit", "enum E { A = 1, }",
             "chtholly.next.sem.type-mismatch"),
            ("equality", "enum E { A = 1, B = 2, }",
             "chtholly.next.sem.operator.missing-implementation"),
        ]
        for name, declaration, diagnostic in cases:
            expression = "E::A {}" if name == "braces" else (
                "1 as E" if name == "reverse" else (
                    "E::A == E::B" if name == "equality" else "0"))
            body = ("let value: i32 = E::A; return 0;" if name == "implicit"
                    else f"let value = {expression}; return 0;")
            imports = "import std::ops;\n" if name == "equality" else ""
            source = (
                f"module main;\n{imports}{declaration}\n"
                f"fn main(): i32 {{ {body} }}\n")
            expect_failure(
                args.chthollyc,
                project(root, f"value-enum-{name}", "1.8", source),
                diagnostic)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
