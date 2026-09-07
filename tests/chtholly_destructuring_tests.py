#!/usr/bin/env python3

import argparse
import pathlib
import shutil
import subprocess
import tempfile

from chtholly_test_support import native_exit_code, single_native_executable


def invoke(command: list[str], expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command,
        text=True,
        encoding="utf-8",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}: {command!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def write_project(
    root: pathlib.Path,
    name: str,
    source: str,
    dependencies: dict[str, str] | None = None,
    entry: bool = True,
) -> pathlib.Path:
    project = root / name
    (project / "src").mkdir(parents=True)
    manifest = f'[package]\nname = "{name}"\nlanguage = "1.0"\n\n[build]\n'
    if entry:
        manifest += 'entry = "src/main.cns"\n'
    manifest += 'module_paths = ["src"]\n'
    if dependencies:
        manifest += "\n[dependencies]\n"
        for dependency, relative in dependencies.items():
            manifest += f'{dependency} = {{ path = "{relative}" }}\n'
    (project / "chtholly.toml").write_text(manifest, encoding="utf-8")
    (project / "src" / "main.cns").write_text(source, encoding="utf-8")
    return project


def executable(project: pathlib.Path) -> pathlib.Path:
    return single_native_executable(
        project / ".chtholly" / "build", "destructuring project")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    parser.add_argument("--source-dir", required=True)
    args = parser.parse_args()
    source_dir = pathlib.Path(args.source_dir)

    with tempfile.TemporaryDirectory(prefix="chtholly-destructuring-") as raw:
        root = pathlib.Path(raw)
        positive_source = (source_dir / "tests" /
                           "chtholly_destructuring_assignment_tests.cns").read_text(
                               encoding="utf-8")
        positive = write_project(root, "destructuring_positive", positive_source)
        semir = root / "destructuring.semir"
        lowir = root / "destructuring.lowir"
        invoke([
            args.chthollyc,
            "build",
            "--project",
            str(positive),
            "--dump-semir",
            str(semir),
            "--dump-lowir",
            str(lowir),
        ])
        if not semir.is_file() or not lowir.is_file():
            raise AssertionError("destructuring omitted semantic lowering evidence")
        invoke([str(executable(positive))], expected=native_exit_code(355))
        llvm = root / "destructuring.ll"
        invoke([
            args.chthollyc,
            str(positive / "src" / "main.cns"),
            "-emit-llvm",
            "-o",
            str(llvm),
        ])
        if "define i32 @chtholly.entry()" not in llvm.read_text(encoding="utf-8"):
            raise AssertionError("destructuring LLVM omitted the hosted entry")

        negative_source = (source_dir / "tests" /
                           "chtholly_destructuring_assignment_errors.cns").read_text(
                               encoding="utf-8")
        negative = write_project(root, "destructuring_negative", negative_source)
        rejected = invoke(
            [args.chthollyc, "check", "--project", str(negative)], expected=1)
        required = {
            "chtholly.next.sem.duplicate-name",
            "chtholly.next.sem.assign.immutable-place",
            "chtholly.next.sem.unknown-name",
            "chtholly.next.sem.type-mismatch",
            "chtholly.next.sem.pattern-projection-overlap",
            "chtholly.next.sem.pattern-incomplete",
        }
        missing = {code for code in required if code not in rejected.stderr}
        if missing:
            raise AssertionError(
                f"destructuring negative evidence omitted {sorted(missing)}\n"
                f"stderr:\n{rejected.stderr}"
            )

        parser_prefix = (
            "module main;\nstruct Pair { pub x: i32; pub y: i32; }\n"
            "fn main(): i32 { var x = 0; var y = 0; "
            "let pair = Pair { .x = 1, .y = 2 }; ")
        invalid_rest = write_project(
            root,
            "destructuring_invalid_rest",
            parser_prefix +
            "{ x = copy .x, .., y = copy .y } = pair; return 0; }\n",
        )
        invalid_rest_result = invoke(
            [args.chthollyc, "check", "--project", str(invalid_rest)], expected=1)
        if "chtholly.next.parse.pattern-rest-placement" not in invalid_rest_result.stderr:
            raise AssertionError(
                "destructuring rest placement omitted its parser diagnostic")

        missing_transfer = write_project(
            root,
            "destructuring_missing_transfer",
            parser_prefix + "{ x = .x, y = copy .y } = pair; return 0; }\n",
        )
        missing_transfer_result = invoke(
            [args.chthollyc, "check", "--project", str(missing_transfer)],
            expected=1,
        )
        if "chtholly.next.parse.expected-token" not in missing_transfer_result.stderr:
            raise AssertionError(
                "destructuring missing transfer omitted its parser diagnostic")

        provider_source = """module destructuring_provider;
pub struct Pair { pub left: i32; pub right: i32; }
pub fn make(): Pair { return Pair { .left = 20, .right = 22 }; }
"""
        write_project(
            root, "destructuring_provider", provider_source, entry=False)
        consumer_source = """module main;
import destructuring_provider;
fn main(): i32 {
  let pair = destructuring_provider::make();
  let { left = copy .left, right = copy .right } = pair;
  return if (left + right == 42) { 0 } else { 1 };
}
"""
        consumer = write_project(
            root,
            "destructuring_consumer",
            consumer_source,
            dependencies={"destructuring_provider": "../destructuring_provider"},
        )
        invoke([args.chthollyc, "build", "--project", str(consumer)])
        invoke([str(executable(consumer))])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
