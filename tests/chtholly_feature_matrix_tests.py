#!/usr/bin/env python3

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

from chtholly_test_support import single_native_executable


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
    version: str,
    sources: dict[str, str],
    dependencies: dict[str, str] | None = None,
    entry: str | None = "src/main.cns",
) -> pathlib.Path:
    project = root / name
    (project / "src").mkdir(parents=True)
    manifest = f'[package]\nname = "{name}"\nlanguage = "{version}"\n\n[build]\n'
    if entry is not None:
        manifest += f'entry = "{entry}"\n'
    manifest += 'module_paths = ["src"]\n'
    if dependencies:
        manifest += "\n[dependencies]\n"
        for dependency, relative in dependencies.items():
            manifest += f'{dependency} = {{ path = "{relative}" }}\n'
    (project / "chtholly.toml").write_text(manifest, encoding="utf-8")
    for filename, source in sources.items():
        (project / "src" / filename).write_text(source, encoding="utf-8")
    return project


def build_and_run(
    chthollyc: str,
    project: pathlib.Path,
    evidence_root: pathlib.Path,
    expected: int = 0,
) -> None:
    semir = evidence_root / f"{project.name}.semir"
    lowir = evidence_root / f"{project.name}.lowir"
    build_result = invoke([
        chthollyc,
        "build",
        "--project",
        str(project),
        "--dump-semir",
        str(semir),
        "--dump-lowir",
        str(lowir),
    ])
    if "compiler GC aborted" in build_result.stderr:
        raise AssertionError(
            "fresh project build unexpectedly consumed an invalid compiler "
            f"artifact reference:\n{build_result.stderr}"
        )
    for artifact in (semir, lowir):
        if not artifact.is_file() or artifact.stat().st_size == 0:
            raise AssertionError(f"missing feature evidence artifact: {artifact}")
    executable = single_native_executable(
        project / ".chtholly" / "build", f"feature project {project.name}")
    run_result = invoke([str(executable)], expected=expected)
    if "compiler GC aborted" in run_result.stderr:
        raise AssertionError(
            "native run unexpectedly consumed an invalid compiler artifact "
            f"reference:\n{run_result.stderr}"
        )


def expect_failure(chthollyc: str, project: pathlib.Path, code: str = "") -> None:
    result = invoke(
        [chthollyc, "check", "--project", str(project)], expected=1)
    if code and code not in result.stderr:
        raise AssertionError(
            f"negative feature case omitted {code!r}: {project.name}\n"
            f"stderr:\n{result.stderr}"
        )
    if "chtholly.next.sem.unsupported" in result.stderr:
        raise AssertionError(
            f"negative feature case used UnsupportedSemantics: {project.name}\n"
            f"stderr:\n{result.stderr}"
        )


CORE_SOURCE = """module main;

pub const Base: i32 = 3;

struct Pair {
  pub left: i32;
  pub right: i32;
}

fn identity<T>(value: T): T { return move value; }

fn main(): i32 {
  let pair = Pair { .left = 1, .right = 2 };
  var total: i32 = 0;
  for (var index: i32 = 0; index < 3; index = index + 1) {
    total = total + 1;
  }
  do { total = total + 1; } while (false);
  let selected = if (true) { pair.left } else { pair.right };
  if (identity(total + selected + Base) != 8) { return 1; }
  return 0;
}
"""


CONSTANT_SOURCE = """module main;

pub const BufferBytes: usize = sizeof(i32[4]);
pub const SameInteger: bool = type_same(i32, i32);
pub const IntegerKind: bool = type_is(i32, integer);
pub const ArrayLength: usize = array_extent(i32[4]);
pub static DefaultAlignment: usize = alignof(i64);

pub const fn triangular(limit: i32): i32 {
  var total: i32 = 0;
  for (var index: i32 = 0; index < limit; index = index + 1) {
    total = total + index;
  }
  return total;
}

fn main(): i32 {
  let narrowed = 7i64 as i32;
  if (BufferBytes != 16usize || !SameInteger || !IntegerKind ||
      ArrayLength != 4usize || DefaultAlignment != 8usize ||
      triangular(4) != 6 || narrowed != 7) {
    return 1;
  }
  return 0;
}
"""


OPERATOR_PROVIDER = """module operator_provider;

import std::ops;

pub struct Number { pub value: i32; }

impl Number {
  pub fn make(value: i32): Number { return Number { .value = value }; }
}

impl std::ops::Addition<Number> for Number {
  alias Output = Number;
  fn add(self: const Self&, rhs: const Number&): Number {
    return Number { .value = self.value + rhs.value };
  }
}

impl std::ops::BitwiseAnd<Number> for Number {
  alias Output = Number;
  fn bit_and(self: const Self&, rhs: const Number&): Number {
    return Number { .value = self.value & rhs.value };
  }
}

impl std::ops::AdditionAssignment<Number> for Number {
  fn add_assign(self: Self&, rhs: const Number&): void {
    self.value = self.value + rhs.value;
    return;
  }
}
"""


OPERATOR_CONSUMER = """module main;

import operator_provider;
import std::ops;

fn main(): i32 {
  var left = operator_provider::Number::make(6);
  let right = operator_provider::Number::make(3);
  let sum = left + right;
  let masked = left & right;
  left += right;
  if (sum.value != 9 || masked.value != 2 || left.value != 9) { return 1; }
  return 0;
}
"""


LABELED_PROVIDER = """module labeled_provider;

pub fn identity<T>(value: T): T { return move value; }

pub fn value(): i32 {
  var result = 0;
  outer: for (var index = 0; index < 3; index = index + 1) {
    while (true) {
      if (index == 1) { continue outer; }
      break;
    }
    result = result + index;
  }
  return identity(result);
}
"""


LABELED_CONSUMER = """module main;
import labeled_provider as provider;
fn main(): i32 { return if (provider::value() == 2) { 0 } else { 1 }; }
"""


ASYNC_PROVIDER = """module async_provider;
pub async fn value(): i32 { return 7; }
"""


ASYNC_CONSUMER = """module main;
import async_provider;
async fn main(): i32 {
  let task = async_provider::value();
  let value = wait task;
  return if (value == 7) { 0 } else { 1 };
}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    parser.add_argument("--source-dir", required=True)
    args = parser.parse_args()
    source_dir = pathlib.Path(args.source_dir).resolve()

    invoke([
        sys.executable,
        str(source_dir / "scripts" / "chtholly-v1-surface.py"),
        "--source-dir",
        str(source_dir),
        "--manifest",
        "support/chtholly-v1.10.toml",
        "--generated",
        "docs/spec/v1.10-surface.generated.md",
        "--check",
    ])

    with tempfile.TemporaryDirectory(prefix="chtholly-feature-matrix-") as raw:
        root = pathlib.Path(raw)
        core = write_project(root, "feature_core", "1.0", {"main.cns": CORE_SOURCE})
        legacy_refs = core / ".chtholly" / "cache" / "next-v47" / "refs"
        legacy_refs.mkdir(parents=True)
        (legacy_refs / "legacy.ref").write_text("obsolete artifact header\n",
                                                encoding="utf-8")
        build_and_run(args.chthollyc, core, root)
        llvm = root / "feature-core.ll"
        invoke([args.chthollyc, str(core / "src" / "main.cns"),
                "-emit-llvm", "-o", str(llvm)])
        llvm_text = llvm.read_text(encoding="utf-8")
        if "define i32 @chtholly.entry()" not in llvm_text:
            raise AssertionError("core feature evidence omitted hosted LLVM entry")

        constants = write_project(
            root, "feature_constants", "1.0", {"main.cns": CONSTANT_SOURCE})
        build_and_run(args.chthollyc, constants, root)

        expect_failure(
            args.chthollyc,
            write_project(
                root,
                "feature_core_negative",
                "1.0",
                {"main.cns": "module main;\nfn main(): i32 { return missing; }\n"},
            ),
            "chtholly.next.sem.unknown-name",
        )
        expect_failure(
            args.chthollyc,
            write_project(
                root,
                "feature_layout_negative",
                "1.0",
                {"main.cns": (
                    "module main;\nconst Invalid: usize = sizeof(void);\n"
                    "fn main(): i32 { return 0; }\n")},
            ),
            "chtholly.next.sem.layout.invalid-query",
        )
        expect_failure(
            args.chthollyc,
            write_project(
                root,
                "feature_recursive_nominal_negative",
                "1.0",
                {"main.cns": (
                    "module main;\nstruct Node { next: Node; }\n"
                    "fn main(): i32 { return 0; }\n")},
            ),
            "chtholly.next.sem.nominal.recursive-value",
        )
        expect_failure(
            args.chthollyc,
            write_project(
                root,
                "feature_empty_array_negative",
                "1.0",
                {"main.cns": (
                    "module main;\nfn main(): i32 { let values = []; "
                    "return 0; }\n")},
            ),
            "chtholly.next.sem.array.invalid-literal",
        )
        expect_failure(
            args.chthollyc,
            write_project(
                root,
                "feature_version_negative",
                "0.9",
                {"main.cns": "module main;\nfn main(): i32 { return 0; }\n"},
            ),
        )

        operator_provider = write_project(
            root, "operator_provider", "1.3",
            {"provider.cns": OPERATOR_PROVIDER}, entry=None)
        operator_consumer = write_project(
            root, "operator_consumer", "1.3",
            {"main.cns": OPERATOR_CONSUMER},
            dependencies={"operator_provider": "../operator_provider"})
        build_and_run(args.chthollyc, operator_consumer, root)
        if not operator_provider.is_dir():
            raise AssertionError("operator provider was not created")
        expect_failure(
            args.chthollyc,
            write_project(
                root,
                "operator_negative",
                "1.3",
                {"main.cns": (
                    "module main;\nstruct Number { value: i32; }\n"
                    "fn main(): i32 { let a = Number { .value = 1 }; "
                    "let b = Number { .value = 2 }; let c = a + b; "
                    "return c.value; }\n")},
            ),
            "chtholly.next.sem.operator.missing-import",
        )

        write_project(
            root, "labeled_provider", "1.3",
            {"provider.cns": LABELED_PROVIDER}, entry=None)
        labeled_consumer = write_project(
            root, "labeled_consumer", "1.3",
            {"main.cns": LABELED_CONSUMER},
            dependencies={"labeled_provider": "../labeled_provider"})
        build_and_run(args.chthollyc, labeled_consumer, root)

        concurrency_source = source_dir / "tests" / "fixtures" / "chtholly-concurrency"
        concurrency = root / "feature_concurrency"
        shutil.copytree(concurrency_source, concurrency)
        build_and_run(args.chthollyc, concurrency, root, expected=42)
        expect_failure(
            args.chthollyc,
            write_project(
                root,
                "concurrency_negative",
                "1.1",
                {"main.cns": (
                    "module main;\nimport std::atomic;\nfn main(): i32 { "
                    "let value = std::atomic::Atomic<f32>::init(1.0f32); "
                    "return 0; }\n")},
            ),
            "chtholly.next.sem.atomic.unsupported-type",
        )

        write_project(
            root, "async_provider", "1.1",
            {"provider.cns": ASYNC_PROVIDER}, entry=None)
        async_consumer = write_project(
            root, "async_consumer", "1.1",
            {"main.cns": ASYNC_CONSUMER},
            dependencies={"async_provider": "../async_provider"})
        build_and_run(args.chthollyc, async_consumer, root)
        expect_failure(
            args.chthollyc,
            write_project(
                root,
                "async_negative",
                "1.1",
                {"main.cns": (
                    "module main;\nasync fn child(): i32 { return 1; }\n"
                    "async fn main(): i32 { child(); return 0; }\n")},
            ),
            "chtholly.next.sem.async.task-discard",
        )
        expect_failure(
            args.chthollyc,
            write_project(
                root,
                "async_reference_negative",
                "1.1",
                {"main.cns": (
                    "module main;\n"
                    "async fn child(): i32 { return 1; }\n"
                    "async fn main(): i32 {\n"
                    "  let value = 7;\n"
                    "  let reference = &value;\n"
                    "  let task = child();\n"
                    "  let result = wait task;\n"
                    "  return reference + result;\n"
                    "}\n")},
            ),
            "chtholly.next.sem.async.reference-across",
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
