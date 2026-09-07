#!/usr/bin/env python3

import argparse
import contextlib
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
                  source: str,
                  dependencies: dict[str, str] | None = None,
                  entry: bool = True) -> pathlib.Path:
    project = root / name
    (project / "src").mkdir(parents=True)
    (project / "chtholly.toml").write_text(
        f'[package]\nname = "{name}"\nlanguage = "{version}"\n\n'
        '[build]\n' + ('entry = "src/main.cns"\n' if entry else '') +
        'module_paths = ["src"]\n' +
        ("\n[dependencies]\n" + "".join(
            f'{dependency} = {{ path = "{path}" }}\n'
            for dependency, path in dependencies.items())
         if dependencies else ""),
        encoding="utf-8")
    (project / "src" / "main.cns").write_text(source, encoding="utf-8")
    return project


ECOSYSTEM_SOURCE = r"""module main;

import std::iter::adapters;

fn range_sum(): i32 {
  var total = 0;
  foreach (let value: i32 in std::iter::adapters::range(-2, 3)) {
    total = total + value;
  }
  foreach (let value: i32 in std::iter::adapters::range(5, 2)) {
    total = total + value + 100;
  }
  foreach (let value: i32 in std::iter::adapters::range(3, 3)) {
    total = total + value + 100;
  }
  return total;
}

fn array_sum(): i32 {
  let values = [1, 2, 3, 4];
  var total = 0;
  foreach (let value: const i32& in
           std::iter::adapters::from_slice(values[..])) {
    total = total + value;
  }
  return total;
}

fn dynamic_array_borrow(): i32 {
  let values = [3, 5, 7, 11];
  let index = 2;
  let item = &values[index];
  return item;
}

fn adapter_sum(): i32 {
  let mapper = fn [](value: i32): i32 { return value * 2; };
  let predicate = fn [](value: i32): bool { return value % 4 == 0; };
  let source = std::iter::adapters::range(0, 10);
  let mapped = std::iter::adapters::map(move source, move mapper);
  let filtered = std::iter::adapters::filter(move mapped, move predicate);
  let limited = std::iter::adapters::take(move filtered, 3usize);
  var total = 0;
  foreach (let value: i32 in move limited) {
    total = total + value;
  }
  return total;
}

fn take_edges(): i32 {
  let empty_source = std::iter::adapters::range(1, 4);
  let empty = std::iter::adapters::take(move empty_source, 0usize);
  var total = 0;
  foreach (let value: i32 in move empty) { total = total + value + 100; }
  let long_source = std::iter::adapters::range(1, 4);
  let long = std::iter::adapters::take(move long_source, 10usize);
  foreach (let value: i32 in move long) { total = total + value; }
  return total;
}

fn main(): i32 {
  if (range_sum() != 0) { return 1; }
  if (array_sum() != 10) { return 2; }
  if (adapter_sum() != 12) { return 3; }
  if (dynamic_array_borrow() != 7) { return 4; }
  if (take_edges() != 6) { return 5; }
  return 0;
}
"""


INVALID_PREDICATE_SOURCE = r"""module main;
import std::iter::adapters;
fn main(): i32 {
  let predicate = fn [](value: i32): i32 { return value; };
  let source = std::iter::adapters::range(0, 2);
  let filtered = std::iter::adapters::filter(move source, move predicate);
  foreach (let value: i32 in move filtered) { return value; }
  return 0;
}
"""


NONCOPY_FILTER_SOURCE = r"""module main;
import std::atomic;
import std::iter::adapters;
fn main(): i32 {
  let mapper = fn [](value: i32): std::atomic::Atomic<i32> {
    return std::atomic::Atomic<i32>::init(value);
  };
  let predicate = fn [](value: std::atomic::Atomic<i32>): bool { return true; };
  let source = std::iter::adapters::range(0, 2);
  let mapped = std::iter::adapters::map(move source, move mapper);
  let filtered = std::iter::adapters::filter(move mapped, move predicate);
  foreach (let value: std::atomic::Atomic<i32> in move filtered) { return 0; }
  return 0;
}
"""


PROVIDER_SOURCE = r"""module iterator_provider;
import std::iter;
import std::iter::adapters;
pub fn source(): std::iter::adapters::Range {
  return std::iter::adapters::range(1, 5);
}
pub fn first<I>(iterator: I, count: usize): std::iter::adapters::Take<I>
    where I: std::iter::Iterator {
  return std::iter::adapters::take(move iterator, count);
}
"""


CONSUMER_SOURCE = r"""module main;
import iterator_provider;
import std::iter;
import std::iter::adapters;
fn main(): i32 {
  let source = iterator_provider::source();
  let limited = iterator_provider::first(move source, 2usize);
  var total = 0;
  foreach (let value: i32 in move limited) { total = total + value; }
  return if (total == 3) { 0 } else { 1 };
}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--work-dir", type=pathlib.Path)
    args = parser.parse_args()
    workspace = (
        contextlib.nullcontext(str(args.work_dir.resolve()))
        if args.work_dir is not None
        else tempfile.TemporaryDirectory(prefix="chtholly-iterator-ecosystem-"))
    with workspace as raw:
        root = pathlib.Path(raw)
        root.mkdir(parents=True, exist_ok=True)
        rejected = write_project(root, "ecosystem-v15", "1.5", ECOSYSTEM_SOURCE)
        result = invoke(
            [args.chthollyc, "check", "--project", str(rejected)], expected=1)
        if "introduced in 1.6" not in result.stderr:
            raise AssertionError(f"1.5 adapter import omitted gate: {result.stderr}")

        admitted = write_project(root, "ecosystem-v16", "1.6", ECOSYSTEM_SOURCE)
        invalid_predicate = write_project(
            root, "ecosystem-invalid-predicate", "1.6", INVALID_PREDICATE_SOURCE)
        result = invoke(
            [args.chthollyc, "check", "--project", str(invalid_predicate)],
            expected=1)
        if "chtholly.next.sem.interface.unsatisfied-constraint" not in result.stderr:
            raise AssertionError(
                f"non-predicate callable omitted constraint diagnostic: {result.stderr}")

        noncopy_filter = write_project(
            root, "ecosystem-noncopy-filter", "1.6", NONCOPY_FILTER_SOURCE)
        result = invoke(
            [args.chthollyc, "check", "--project", str(noncopy_filter)],
            expected=1)
        if "chtholly.next.sem.copy.unavailable" not in result.stderr:
            raise AssertionError(
                f"non-copyable filter Item omitted copy diagnostic: {result.stderr}")

        provider = write_project(
            root, "iterator_provider", "1.6", PROVIDER_SOURCE, entry=False)
        consumer = write_project(
            root, "iterator-consumer", "1.6", CONSUMER_SOURCE,
            {"iterator_provider": "../iterator_provider"})
        invoke([args.chthollyc, "check", "--project", str(provider)])
        if args.check_only:
            invoke([args.chthollyc, "check", "--project", str(consumer)])
        else:
            invoke([args.chthollyc, "build", "--project", str(consumer)])
            executable = single_native_executable(
                consumer / ".chtholly", "cross-package iterator ecosystem")
            invoke([str(executable)])

        if args.check_only:
            invoke([args.chthollyc, "check", "--project", str(admitted)])
        else:
            semir = root / "ecosystem.semir"
            lowir = root / "ecosystem.lowir"
            invoke([
                args.chthollyc, "build", "--project", str(admitted),
                "--dump-semir", str(semir),
                "--dump-lowir", str(lowir),
            ])
            executable = single_native_executable(
                admitted / ".chtholly", "iterator ecosystem")
            invoke([str(executable)])
            if semir.stat().st_size == 0 or lowir.stat().st_size == 0:
                raise AssertionError("iterator ecosystem omitted IR evidence")
            semir_text = semir.read_text(encoding="utf-8")
            lowir_text = lowir.read_text(encoding="utf-8")
            if "Filter::$impl$std::iter::Iterator$$next$specific" not in semir_text:
                raise AssertionError("iterator ecosystem omitted concrete Filter.next")
            if "DynamicIndexBorrow" not in lowir_text:
                raise AssertionError("dynamic array borrowing omitted dedicated LowIR")
            invoke([args.chthollyc, "build", "--project", str(admitted)])
            invoke([str(executable)])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
