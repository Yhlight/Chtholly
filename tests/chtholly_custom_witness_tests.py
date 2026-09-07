#!/usr/bin/env python3
"""Cross-package/native regression for nominal Hash/Equal witnesses."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import subprocess
import tempfile


def run(command: list[str], expected: int = 0) -> subprocess.CompletedProcess[str]:
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


SOURCE = r"""module main;
import std::collections;
import std::hash;
import std::option;

lifecycle(copy = delete, move = default, drop = custom)
pub struct Key {
  pub value: i32;
}

impl Key {
  fn drop(self: Key&): void { return; }
}

impl std::hash::Hash for Key {
  fn hash(self: const Self&, seed: u64): u64 {
    return (self.value as u64) ^ seed;
  }
}

impl std::hash::Equal<Key> for Key {
  fn equal(self: const Self&, rhs: const Key&): bool {
    return self.value == rhs.value;
  }
}

lifecycle(copy = delete, move = default, drop = custom)
pub struct NestedKey {
  pub inner: Key;
}

impl NestedKey {
  fn drop(self: NestedKey&): void { return; }
}

impl std::hash::Hash for NestedKey {
  fn hash(self: const Self&, seed: u64): u64 {
    return (self.inner.value as u64) ^ seed;
  }
}

impl std::hash::Equal<NestedKey> for NestedKey {
  fn equal(self: const Self&, rhs: const NestedKey&): bool {
    return self.inner.value == rhs.inner.value;
  }
}

fn reference_init_probe(): i32 {
  var placeholder: i32 = 0i32;
  var reference: const i32& = &placeholder;
  return copy reference;
}

fn main(): i32 {
  if (reference_init_probe() != 0i32) { return 9; }
  var scalar_map = std::collections::HashMap<i32, i32>::make();
  let scalar_inserted = scalar_map.insert(1i32, 42i32);
  let borrowed = scalar_map.get(1i32);
  if (!borrowed.is_some()) { return 5; }
  let borrowed_value = (move borrowed).unwrap();
  if (*borrowed_value != 42i32) { return 4; }
  var map = std::collections::HashMap<Key, Key>::make();
  let inserted = map.insert(
      Key { .value = 7i32 }, Key { .value = 42i32 });
  if (!map.contains(Key { .value = 7i32 })) { return 1; }
  if (map.len() != 1usize) { return 2; }
  var nested_map = std::collections::HashMap<NestedKey, Key>::make();
  let nested_inserted = nested_map.insert(
      NestedKey { .inner = Key { .value = 5i32 } },
      Key { .value = 6i32 });
  if (!nested_map.contains(
          NestedKey { .inner = Key { .value = 5i32 } })) { return 7; }
  nested_map.clear();
  map.clear();
  if (!map.is_empty()) { return 3; }
  var set = std::collections::HashSet<Key>::make();
  let set_inserted = set.insert(Key { .value = 9i32 });
  if (!set.contains(Key { .value = 9i32 })) { return 6; }
  set.clear();
  return 0;
}

"""

PROVIDER_SOURCE = r"""module custom_witness_provider;
import std::hash;

lifecycle(copy = delete, move = default, drop = custom)
pub struct Key {
  pub value: i32;
}

impl Key {
  pub fn drop(self: Key&): void { return; }
}

impl std::hash::Hash for Key {
  fn hash(self: const Self&, seed: u64): u64 {
    return (self.value as u64) ^ seed;
  }
}

impl std::hash::Equal<Key> for Key {
  fn equal(self: const Self&, rhs: const Key&): bool {
    return self.value == rhs.value;
  }
}

pub fn key(value: i32): Key {
  return Key { .value = value };
}

pub fn ping(): i32 { return 0i32; }
"""

CONSUMER_SOURCE = r"""module main;
import std::collections;
import std::option;
import custom_witness_provider;

fn main(): i32 {
  if (custom_witness_provider::ping() != 0i32) { return 4; }
  var map = std::collections::HashMap<custom_witness_provider::Key,
                                      custom_witness_provider::Key>::make();
  let inserted = map.insert(custom_witness_provider::key(7i32),
                            custom_witness_provider::key(42i32));
  if (!map.contains(custom_witness_provider::key(7i32))) {
    return 1;
  }
  if (map.len() != 1usize) { return 2; }
  map.clear();
  return if (map.is_empty()) { 0 } else { 3 };
}
"""

RECURSIVE_REJECTED_SOURCE = r"""module main;
import std::collections;
import std::hash;

struct RecursiveKey {
  value: i32;
  next: RecursiveKey*;
}

impl std::hash::Hash for RecursiveKey {
  fn hash(self: const Self&, seed: u64): u64 {
    return (self.value as u64) ^ seed;
  }
}

impl std::hash::Equal<RecursiveKey> for RecursiveKey {
  fn equal(self: const Self&, rhs: const RecursiveKey&): bool {
    return self.value == rhs.value;
  }
}

fn main(): i32 {
  var map = std::collections::HashMap<RecursiveKey, i32>::make();
  let ignored = map.insert(
      RecursiveKey { .value = 1i32, .next = null }, 2i32);
  return 0;
}
"""

MISSING_WITNESS_SOURCE = r"""module main;
import std::collections;
import std::hash;

struct MissingEqual {
  value: i32;
}

impl std::hash::Hash for MissingEqual {
  fn hash(self: const Self&, seed: u64): u64 {
    return (self.value as u64) ^ seed;
  }
}

fn main(): i32 {
  var map = std::collections::HashMap<MissingEqual, i32>::make();
  let ignored = map.insert(MissingEqual { .value = 1i32 }, 2i32);
  return 0;
}
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="chtholly-custom-witness-") as raw:
        project = pathlib.Path(raw) / "custom_witness"
        (project / "src").mkdir(parents=True)
        (project / "chtholly.toml").write_text(
            '[package]\nname = "custom_witness"\nlanguage = "1.10"\n\n'
            '[build]\nentry = "src/main.cns"\nmodule_paths = ["src"]\n',
            encoding="utf-8",
        )
        (project / "src" / "main.cns").write_text(SOURCE, encoding="utf-8")
        run([args.chthollyc, "check", "--project", str(project)])
        run([args.chthollyc, "build", "--project", str(project)])
        executables = sorted((project / ".chtholly" / "build").rglob("custom_witness.exe"))
        if len(executables) != 1:
            raise AssertionError(f"expected one executable, got {executables}")
        run([str(executables[0])])
        provider = pathlib.Path(raw) / "provider"
        (provider / "src").mkdir(parents=True)
        (provider / "chtholly.toml").write_text(
            '[package]\nname = "custom_witness_provider"\nlanguage = "1.0"\n\n'
            '[build]\nmodule_paths = ["src"]\n',
            encoding="utf-8",
        )
        (provider / "src" / "main.cns").write_text(
            PROVIDER_SOURCE, encoding="utf-8"
        )
        run([args.chthollyc, "check", "--project", str(provider)])
        consumer = pathlib.Path(raw) / "consumer"
        (consumer / "src").mkdir(parents=True)
        (consumer / "chtholly.toml").write_text(
            '[package]\nname = "custom_witness_consumer"\nlanguage = "1.10"\n\n'
            '[build]\nentry = "src/main.cns"\nmodule_paths = ["src"]\n\n'
            '[dependencies]\ncustom_witness_provider = { path = "../provider" }\n',
            encoding="utf-8",
        )
        (consumer / "src" / "main.cns").write_text(
            CONSUMER_SOURCE, encoding="utf-8"
        )
        run([args.chthollyc, "check", "--project", str(consumer)])
        run([args.chthollyc, "build", "--project", str(consumer)])
        consumer_executables = sorted(
            (consumer / ".chtholly" / "build").rglob("custom_witness_consumer.exe")
        )
        if len(consumer_executables) != 1:
            raise AssertionError(
                f"expected one cross-package executable, got {consumer_executables}"
            )
        run([str(consumer_executables[0])])
        first_digest = hashlib.sha256(consumer_executables[0].read_bytes()).digest()
        # A second build must reuse the verified provider/artifact closure.
        run([args.chthollyc, "build", "--project", str(consumer)])
        provider_source = provider / "src" / "main.cns"
        provider_source.write_text(
            provider_source.read_text(encoding="utf-8").replace(
                "return (self.value as u64) ^ seed;",
                "return (self.value as u64) + seed;",
            ),
            encoding="utf-8",
        )
        run([args.chthollyc, "build", "--project", str(consumer)])
        changed_executables = sorted(
            (consumer / ".chtholly" / "build").rglob("custom_witness_consumer.exe")
        )
        if len(changed_executables) != 1 or hashlib.sha256(
            changed_executables[0].read_bytes()
        ).digest() == first_digest:
            raise AssertionError("provider witness change did not invalidate native output")
        run([str(changed_executables[0])])
        rejected = pathlib.Path(raw) / "recursive_rejected"
        (rejected / "src").mkdir(parents=True)
        (rejected / "chtholly.toml").write_text(
            '[package]\nname = "recursive_rejected"\nlanguage = "1.10"\n\n'
            '[build]\nentry = "src/main.cns"\nmodule_paths = ["src"]\n',
            encoding="utf-8",
        )
        (rejected / "src" / "main.cns").write_text(
            RECURSIVE_REJECTED_SOURCE, encoding="utf-8"
        )
        recursive_result = run(
            [args.chthollyc, "check", "--project", str(rejected)], expected=1
        )
        if "cycle" not in recursive_result.stderr.lower():
            raise AssertionError("recursive nominal witness did not fail closed")
        missing = pathlib.Path(raw) / "missing_witness"
        (missing / "src").mkdir(parents=True)
        (missing / "chtholly.toml").write_text(
            '[package]\nname = "missing_witness"\nlanguage = "1.10"\n\n'
            '[build]\nentry = "src/main.cns"\nmodule_paths = ["src"]\n',
            encoding="utf-8",
        )
        (missing / "src" / "main.cns").write_text(
            MISSING_WITNESS_SOURCE, encoding="utf-8"
        )
        missing_result = run(
            [args.chthollyc, "check", "--project", str(missing)], expected=1
        )
        if "chtholly.next.sem.witness.missing-equal" not in missing_result.stderr:
            raise AssertionError(
                "missing Hash/Equal witness diagnostic omitted stable witness code"
            )
    print("custom witness: nominal Hash/Equal callbacks and non-trivial drop")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
