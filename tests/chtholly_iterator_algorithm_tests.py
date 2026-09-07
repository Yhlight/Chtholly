#!/usr/bin/env python3

import argparse
import pathlib
import subprocess
import tempfile

from chtholly_test_support import single_native_executable


def invoke(command: list[str], expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, encoding="utf-8",
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            check=False)
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}: {command!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def project(root: pathlib.Path, name: str, version: str, source: str) -> pathlib.Path:
    target = root / name
    (target / "src").mkdir(parents=True)
    (target / "chtholly.toml").write_text(
        f'[package]\nname = "{name}"\nlanguage = "{version}"\n\n'
        '[build]\nentry = "src/main.cns"\nmodule_paths = ["src"]\n',
        encoding="utf-8")
    (target / "src" / "main.cns").write_text(source, encoding="utf-8")
    return target


SOURCE = r"""module main;
import std::iter::adapters;
import std::iter::algorithms;
import std::vec;

fn enumerate_sum(): i32 {
  let source = std::iter::adapters::range(3, 6);
  let values = std::iter::algorithms::enumerate(move source);
  var total = 0;
  foreach (let entry: (usize, i32) in move values) {
    total = total + (entry.0 as i32) + entry.1;
  }
  return total;
}

fn fold_sum(): i32 {
  let folder = fn [](accumulator: i32, item: i32): i32 {
    accumulator + item
  };
  let source = std::iter::adapters::range(1, 5);
  return std::iter::algorithms::fold(move source, 0, move folder);
}

fn query(): bool {
  let is_three = fn [](value: i32): bool { value == 3 };
  let below_five = fn [](value: i32): bool { value < 5 };
  let any_source = std::iter::adapters::range(0, 5);
  let all_source = std::iter::adapters::range(0, 5);
  return std::iter::algorithms::any(move any_source, move is_three) &&
         std::iter::algorithms::all(move all_source, move below_five);
}

fn collected(): i32 {
  let source = std::iter::adapters::range(2, 5);
  var values: std::vec::Vec<i32> =
      std::iter::algorithms::collect_vec(move source);
  if (values.len() != 3usize) { return 1; }
  if (values.at(0usize) != 2 || values.at(1usize) != 3 ||
      values.at(2usize) != 4) { return 2; }
  return 0;
}

fn main(): i32 {
  if (enumerate_sum() != 15) { return 1; }
  if (fold_sum() != 10) { return 2; }
  if (!query()) { return 3; }
  if (collected() != 0) { return 4; }
  return 0;
}
"""

REFERENCE_COLLECT = r"""module main;
import std::iter::adapters;
import std::iter::algorithms;
fn main(): i32 {
  let values = [1, 2, 3];
  let iterator = std::iter::adapters::from_slice(values[..]);
  let collected = std::iter::algorithms::collect_vec(move iterator);
  return 0;
}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="chtholly-iterator-algorithms-") as raw:
        root = pathlib.Path(raw)
        rejected = project(
            root, "iterator-algorithms-v16", "1.6",
            "module main;\nimport std::iter::algorithms;\n"
            "fn main(): i32 { return 0; }\n")
        result = invoke([args.chthollyc, "check", "--project", str(rejected)],
                        expected=1)
        if "introduced in 1.7" not in result.stderr:
            raise AssertionError(
                f"iterator algorithms omitted 1.7 module gate: {result.stderr}")
        reference_collect = project(
            root, "iterator-algorithms-reference-collect", "1.7",
            REFERENCE_COLLECT)
        result = invoke(
            [args.chthollyc, "check", "--project", str(reference_collect)],
            expected=1)
        if "chtholly.next.std.iter.collect-vec-owned-item-required" not in result.stderr:
            raise AssertionError(
                f"reference collect omitted owned Item diagnostic: {result.stderr}")
        admitted = project(root, "iterator-algorithms-v17", "1.7", SOURCE)
        invoke([args.chthollyc, "build", "--project", str(admitted)])
        executable = single_native_executable(
            admitted / ".chtholly", "iterator algorithms")
        invoke([str(executable)])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
