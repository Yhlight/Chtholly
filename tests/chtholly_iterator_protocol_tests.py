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


def write_project(root: pathlib.Path, name: str, version: str,
                  source: str, entry: str | None = "src/main.cns",
                  dependencies: dict[str, str] | None = None) -> pathlib.Path:
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
    filename = "main.cns" if entry is not None else "provider.cns"
    (project / "src" / filename).write_text(source, encoding="utf-8")
    return project


OWNED_ITERATOR = r"""module main;

import std::iter;

struct Counter {
  current: i32;
  end: i32;
}

impl Counter {
  fn make(end: i32): Counter {
    return Counter { .current = 0, .end = end };
  }
}

impl std::iter::Iterator for Counter {
  alias Item = i32;

  fn next(self: Self): std::iter::IterationStep<i32, Self> {
    if (self.current < self.end) {
      let value = self.current;
      let continuation = Counter {
        .current = self.current + 1,
        .end = self.end,
      };
      return std::iter::IterationStep<i32, Self>::Item {
        value, move continuation
      };
    }
    return std::iter::IterationStep<i32, Self>::Done {};
  }
}

fn main(): i32 {
  var total = 0;
  foreach (let item: i32 in Counter::make(4)) {
    total = total + item;
  }
  return total - 6;
}
"""


SHARED_ITERATOR = r"""module main;

import std::iter;

struct RefOnce {
  value: const i32&;
  remaining: bool;
}

impl RefOnce {
  fn make(value: const i32&): RefOnce {
    return RefOnce { .value = value, .remaining = true };
  }
}

impl std::iter::Iterator for RefOnce {
  alias Item = const i32&;

  fn next(self: Self): std::iter::IterationStep<const i32&, Self> {
    if (self.remaining) {
      let item = self.value;
      let continuation = RefOnce {
        .value = self.value, .remaining = false
      };
      return std::iter::IterationStep<const i32&, Self>::Item {
        item, move continuation
      };
    }
    return std::iter::IterationStep<const i32&, Self>::Done {};
  }
}

fn main(): i32 {
  let value = 41;
  var total = 0;
  foreach (let item: const i32& in RefOnce::make(&value)) {
    total = total + item;
  }
  return total - 41;
}
"""


GENERIC_SHARED_ITERATOR = SHARED_ITERATOR.replace(
    "struct RefOnce {", "struct RefOnce<T> {").replace(
    "value: const i32&;", "value: const T&;").replace(
    "impl RefOnce {", "impl<T> RefOnce<T> {").replace(
    "fn make(value: const i32&): RefOnce {",
    "fn make(value: const T&): RefOnce<T> {").replace(
    "return RefOnce {", "return RefOnce<T> {").replace(
    "impl std::iter::Iterator for RefOnce {",
    "impl<T> std::iter::Iterator for RefOnce<T> {").replace(
    "alias Item = const i32&;", "alias Item = const T&;").replace(
    "std::iter::IterationStep<const i32&, Self>",
    "std::iter::IterationStep<const T&, Self>").replace(
    "let continuation = RefOnce {", "let continuation = RefOnce<T> {").replace(
    "RefOnce::make(&value)", "RefOnce<i32>::make(&value)")


GENERIC_WRAPPER = r"""module main;

import std::iter;

struct Counter {
  current: i32;
  end: i32;
}

impl Counter {
  fn make(end: i32): Counter {
    return Counter { .current = 0, .end = end };
  }
}

impl std::iter::Iterator for Counter {
  alias Item = i32;
  fn next(self: Self): std::iter::IterationStep<i32, Self> {
    if (self.current < self.end) {
      let continuation = Counter {
        .current = self.current + 1, .end = self.end
      };
      return std::iter::IterationStep<i32, Self>::Item {
        self.current, move continuation
      };
    }
    return std::iter::IterationStep<i32, Self>::Done {};
  }
}

struct Wrapper<I> { inner: I; }

impl<I> Wrapper<I> {
  fn make(inner: I): Wrapper<I> {
    return Wrapper<I> { .inner = move inner };
  }
}

impl<I> std::iter::Iterator for Wrapper<I> where I: std::iter::Iterator {
  alias Item = I::Item;
  fn next(self: Self): std::iter::IterationStep<I::Item, Self> {
    let step = (move self.inner).next();
    return switch (step) {
      std::iter::IterationStep<I::Item, I>::Done =>
          std::iter::IterationStep<I::Item, Self>::Done {};
      std::iter::IterationStep<I::Item, I>::Item {
        item = move .0,
        continuation = move .1
      } => std::iter::IterationStep<I::Item, Self>::Item {
        move item, Wrapper<I> { .inner = move continuation }
      };
    };
  }
}

fn main(): i32 {
  var total = 0;
  foreach (let item: i32 in Wrapper<Counter>::make(Counter::make(4))) {
    total = total + item;
  }
  return total - 6;
}
"""


PROVIDER = OWNED_ITERATOR.replace("module main;", "module counter_provider;").replace(
    "struct Counter", "pub struct Counter").replace(
    "fn make(end: i32)", "pub fn make(end: i32)").split("fn main(): i32")[0]

CROSS_PACKAGE_CONSUMER = r"""module main;

import counter_provider;

fn main(): i32 {
  var total = 0;
  foreach (let item: i32 in counter_provider::Counter::make(5)) {
    total = total + item;
  }
  return total - 10;
}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="chtholly-iterator-protocol-") as raw:
        root = pathlib.Path(raw)
        rejected = write_project(root, "iterator-v14", "1.4", OWNED_ITERATOR)
        result = invoke(
            [args.chthollyc, "check", "--project", str(rejected)], expected=1)
        if "introduced in 1.5" not in result.stderr:
            raise AssertionError(f"1.4 iterator protocol omitted gate: {result.stderr}")

        admitted = write_project(root, "iterator-v15", "1.5", OWNED_ITERATOR)
        if args.check_only:
            invoke([args.chthollyc, "check", "--project", str(admitted)])
        else:
            semir = root / "iterator-v15.semir"
            lowir = root / "iterator-v15.lowir"
            invoke([
                args.chthollyc, "build", "--project", str(admitted),
                "--dump-semir", str(semir),
                "--dump-lowir", str(lowir),
            ])
            executable = single_native_executable(
                admitted / ".chtholly", "owned iterator")
            invoke([str(executable)])
            if semir.stat().st_size == 0 or lowir.stat().st_size == 0:
                raise AssertionError("owned iterator omitted SemIR or LowIR evidence")

        shared = write_project(root, "iterator-shared", "1.5", SHARED_ITERATOR)
        if args.check_only:
            invoke([args.chthollyc, "check", "--project", str(shared)])
        else:
            invoke([args.chthollyc, "build", "--project", str(shared)])
            executable = single_native_executable(
                shared / ".chtholly", "shared iterator")
            invoke([str(executable)])

        generic_shared = write_project(
            root, "iterator-generic-shared", "1.5", GENERIC_SHARED_ITERATOR)
        invoke([args.chthollyc, "check", "--project", str(generic_shared)])

        generic_wrapper = write_project(
            root, "iterator-generic-wrapper", "1.5", GENERIC_WRAPPER)
        if args.check_only:
            invoke([args.chthollyc, "check", "--project", str(generic_wrapper)])
        else:
            invoke([args.chthollyc, "build", "--project", str(generic_wrapper)])
            executable = single_native_executable(
                generic_wrapper / ".chtholly", "generic wrapper")
            invoke([str(executable)])

        provider = write_project(
            root, "counter_provider", "1.5", PROVIDER, entry=None)
        consumer = write_project(
            root, "iterator-cross-package", "1.5", CROSS_PACKAGE_CONSUMER,
            dependencies={"counter_provider": "../counter_provider"})
        if args.check_only:
            invoke([args.chthollyc, "check", "--project", str(consumer)])
        else:
            invoke([args.chthollyc, "build", "--project", str(consumer)])
            executable = single_native_executable(
                consumer / ".chtholly", "cross-package iterator")
            invoke([str(executable)])
        if not provider.is_dir():
            raise AssertionError("iterator provider was not created")

        missing = write_project(
            root, "iterator-missing", "1.5",
            "module main;\nfn main(): i32 { foreach (let item in 0) { "
            "return item; } return 0; }\n")
        missing_result = invoke(
            [args.chthollyc, "check", "--project", str(missing)], expected=1)
        if "chtholly.next.sem.foreach.invalid-iterator" not in missing_result.stderr:
            raise AssertionError("missing iterator conformance omitted diagnostic")

        fake = write_project(
            root, "iterator-fake-protocol", "1.5", r"""module main;
trait Iterator {}
struct Fake {}
impl Iterator for Fake {}
fn main(): i32 {
  foreach (let item in Fake {}) { return 1; }
  return 0;
}
""")
        fake_result = invoke(
            [args.chthollyc, "check", "--project", str(fake)], expected=1)
        if "chtholly.next.sem.foreach.invalid-iterator" not in fake_result.stderr:
            raise AssertionError("same-named local trait bypassed protocol identity")

        named_source = OWNED_ITERATOR.replace(
            "foreach (let item: i32 in Counter::make(4)) {",
            "let iterator = Counter::make(4);\n"
            "  foreach (let item: i32 in iterator) {")
        named = write_project(root, "iterator-named", "1.5", named_source)
        named_result = invoke(
            [args.chthollyc, "check", "--project", str(named)], expected=1)
        if "chtholly.next.sem.foreach.move-required" not in named_result.stderr:
            raise AssertionError("named iterator omitted explicit transfer diagnostic")

        mismatch_source = OWNED_ITERATOR.replace(
            "foreach (let item: i32 in Counter::make(4)) {",
            "foreach (let item: i64 in Counter::make(4)) {")
        mismatch = write_project(
            root, "iterator-item-mismatch", "1.5", mismatch_source)
        mismatch_result = invoke(
            [args.chthollyc, "check", "--project", str(mismatch)], expected=1)
        if "chtholly.next.sem.foreach.item-type-mismatch" not in mismatch_result.stderr:
            raise AssertionError("iterator Item mismatch omitted diagnostic")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
