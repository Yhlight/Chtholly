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

enum Mode { First, Second }

fn plain(): i32 { 42 }

pub fn identity<T>(value: T): T { { move value } }

fn choose(flag: bool): i32 {
  if (flag) { 7 } else { 9 }
}

fn switch_value(mode: Mode): i32 {
  switch (mode) {
    Mode::First => 11;
    Mode::Second => { let extra = 2; 20 + extra };
  }
}

fn implicit_void(): void {
  let local = 1;
}

fn scoped(): i32 {
  var result = 0;
  {
    let local = 5;
    result = result + local;
  }
  let value = { let extra = 3; result + extra };
  let empty = unsafe {};
  empty;
  return value;
}

fn switch_statement(mode: Mode): i32 {
  var result = 0;
  switch (mode) {
    Mode::First => { result = 13; }
    Mode::Second => result = 17;
  }
  return result;
}

fn main(): i32 {
  implicit_void();
  if (plain() != 42 || choose(true) != 7 || choose(false) != 9) { return 1; }
  if (identity(6) != 6) { return 5; }
  if (switch_value(Mode::First) != 11 ||
      switch_value(Mode::Second) != 22) { return 2; }
  if (scoped() != 8) { return 3; }
  if (switch_statement(Mode::First) != 13 ||
      switch_statement(Mode::Second) != 17) { return 4; }
  return 0;
}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="chtholly-value-block-") as raw:
        root = pathlib.Path(raw)
        rejected = project(root, "value-block-v16", "1.6", SOURCE)
        invoke([args.chthollyc, "check", "--project", str(rejected)], expected=1)
        admitted = project(root, "value-block-v17", "1.7", SOURCE)
        semir = root / "value-block.semir"
        lowir = root / "value-block.lowir"
        invoke([args.chthollyc, "build", "--project", str(admitted),
                "--dump-semir", str(semir),
                "--dump-lowir", str(lowir)])
        executable = single_native_executable(
            admitted / ".chtholly", "value-block project")
        invoke([str(executable)])
        if "ScopedBlock" not in semir.read_text(encoding="utf-8"):
            raise AssertionError("value-block SemIR omitted ScopedBlock")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
