#!/usr/bin/env python3
"""End-to-end regression gate for the language semantic foundations."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import tempfile
from chtholly_test_support import single_native_executable, native_exit_code


def run(command: list[str], expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, encoding="utf-8",
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            check=False)
    if result.returncode != native_exit_code(expected):
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
    fixture = root / "tests" / "fixtures" / "chtholly-semantic-gate"
    with tempfile.TemporaryDirectory(prefix="chtholly-semantic-gate-") as project_temp:
        project = pathlib.Path(project_temp) / "project"
        shutil.copytree(
            fixture,
            project,
            ignore=shutil.ignore_patterns(".chtholly", "chtholly.lock"),
        )
        return run_semantic_project(args.chthollyc, project, root)


def run_semantic_project(compiler: str, project: pathlib.Path,
                         source_root: pathlib.Path) -> int:
    run([compiler, "check", "--project", str(project)])
    jsonl = run([compiler, "check", "--project", str(project),
                 "--output-format", "jsonl"]).stdout
    if '"schema":"chtholly-cli-jsonl-v2"' not in jsonl:
        raise AssertionError(f"semantic check did not publish JSONL v2: {jsonl}")
    with tempfile.TemporaryDirectory(prefix="chtholly-char-negative-") as temp:
        negative = pathlib.Path(temp) / "negative"
        (negative / "src").mkdir(parents=True)
        (negative / "chtholly.toml").write_text(
            '[package]\nname = "char_negative"\nlanguage = "1.10"\n\n'
            '[build]\nentry = "src/main.cns"\nmodule_paths = ["src"]\n',
            encoding="utf-8")
        (negative / "src" / "main.cns").write_text(
            "module main; fn main(): i32 { let invalid: char = 'AB'; return 0; }\n",
            encoding="utf-8")
        failure = run([compiler, "check", "--project", str(negative)],
                      expected=1)
        if "invalid-char-literal" not in failure.stdout + failure.stderr:
            raise AssertionError("invalid char fixture lost its stable diagnostic")
        ref_project = pathlib.Path(temp) / "reference_negative"
        (ref_project / "src").mkdir(parents=True)
        (ref_project / "chtholly.toml").write_text(
            '[package]\nname = "reference_negative"\nlanguage = "1.10"\n\n'
            '[build]\nentry = "src/main.cns"\nmodule_paths = ["src"]\n',
            encoding="utf-8")
        (ref_project / "src" / "main.cns").write_text(
            "module main; fn take(value: const i32&): i32 { return value; } "
            "fn read<T>(value: const T&): T { return value; } "
            "fn main(): i32 { let local = 3i32; let ref = &local; "
            "return take(1i32) + read(ref) + read(1i32); }\n", encoding="utf-8")
        reference_success = run(
            [compiler, "check", "--project", str(ref_project)], expected=0)
        if "checked\treference_negative" not in reference_success.stdout:
            raise AssertionError("transparent const-reference binding was not reported")
        run([compiler, "build", "--project", str(ref_project)])
        reference_executables = [single_native_executable(ref_project / ".chtholly" / "build", "semantic gate")]
        if len(reference_executables) != 1:
            raise AssertionError("transparent reference fixture did not produce an executable")
        run([str(reference_executables[0])], expected=5)
        primitive_impl = pathlib.Path(temp) / "primitive_impl"
        (primitive_impl / "src").mkdir(parents=True)
        (primitive_impl / "chtholly.toml").write_text(
            '[package]\nname = "primitive_impl"\nlanguage = "1.10"\n\n'
            '[build]\nentry = "src/main.cns"\nmodule_paths = ["src"]\n',
            encoding="utf-8")
        (primitive_impl / "src" / "main.cns").write_text(
            "module main; import std::hash; "
            "impl std::hash::Hash for i32 { "
            "fn hash(self: const Self&, seed: u64): u64 { return seed; } } "
            "fn main(): i32 { return 0; }\n", encoding="utf-8")
        primitive_failure = run(
            [compiler, "check", "--project", str(primitive_impl)], expected=1)
        if not any(reason in primitive_failure.stdout + primitive_failure.stderr
                   for reason in ("orphan-conformance", "duplicate-conformance")):
            raise AssertionError("user code was allowed to redefine a primitive witness")
        bytes_project = pathlib.Path(temp) / "string_bytes"
        (bytes_project / "src").mkdir(parents=True)
        (bytes_project / "chtholly.toml").write_text(
            '[package]\nname = "string_bytes"\nlanguage = "1.10"\n\n'
            '[build]\nentry = "src/main.cns"\nmodule_paths = ["src"]\n',
            encoding="utf-8")
        (bytes_project / "src" / "main.cns").write_text(
            "module main; import std::text; "
            "fn byte_count(value: string): usize { "
            "let bytes = std::text::as_bytes(value); return bytes.len; } "
            "fn main(): i32 { return byte_count(\"hello\") as i32; }\n",
            encoding="utf-8")
        run([compiler, "check", "--project", str(bytes_project)])
        run([compiler, "build", "--project", str(bytes_project)])
        byte_executables = [single_native_executable(bytes_project / ".chtholly" / "build", "semantic gate")]
        if len(byte_executables) != 1:
            raise AssertionError("string byte-view fixture did not produce an executable")
        run([str(byte_executables[0])], expected=5)
        fs_project = pathlib.Path(temp) / "typed_fs"
        (fs_project / "src").mkdir(parents=True)
        (fs_project / "chtholly.toml").write_text(
            '[package]\nname = "typed_fs"\nlanguage = "1.10"\n\n'
            '[build]\nentry = "src/main.cns"\nmodule_paths = ["src"]\n',
            encoding="utf-8")
        (fs_project / "src" / "main.cns").write_text(
            "module main; import std::error; import std::fs; "
            "import std::result; "
            "fn main(): i32 { "
            "let written = std::fs::write_error(\"typed-fs.tmp\", \"ABC\"); "
            "let opened = std::fs::open(\"typed-fs.tmp\"); "
            "return switch (opened) { "
            "std::result::Result<std::fs::File, std::error::ErrorCode>::Ok "
            "{ file = move .0 } => { var bytes: u8[3] = [0u8, 0u8, 0u8]; "
            "var mutable_file = move file; let view: slice_mut<u8> = bytes[..]; "
            "let read = std::fs::read(&mutable_file, view); "
            "let closed = std::fs::close(move mutable_file); "
            "std::fs::remove(\"typed-fs.tmp\"); 0 }; "
            "std::result::Result<std::fs::File, std::error::ErrorCode>::Err "
            "{ .. } => 1; }; }\n",
            encoding="utf-8")
        run([compiler, "check", "--project", str(fs_project)])
        run([compiler, "build", "--project", str(fs_project)])
        fs_executables = [single_native_executable(fs_project / ".chtholly" / "build", "semantic gate")]
        if len(fs_executables) != 1:
            raise AssertionError("typed file stream fixture did not produce an executable")
        run([str(fs_executables[0])])
        bytes_negative = pathlib.Path(temp) / "string_bytes_negative"
        (bytes_negative / "src").mkdir(parents=True)
        (bytes_negative / "chtholly.toml").write_text(
            '[package]\nname = "string_bytes_negative"\nlanguage = "1.10"\n\n'
            '[build]\nentry = "src/main.cns"\nmodule_paths = ["src"]\n',
            encoding="utf-8")
        (bytes_negative / "src" / "main.cns").write_text(
            "module main; import std::text; "
            "fn escaped(): const slice<u8> { "
            "return std::text::as_bytes(\"temporary\"); } "
            "fn main(): i32 { let bytes = escaped(); return bytes.len as i32; }\n",
            encoding="utf-8")
        escaped_failure = run(
            [compiler, "check", "--project", str(bytes_negative)], expected=1)
        if "borrow" not in escaped_failure.stdout + escaped_failure.stderr:
            raise AssertionError("string byte-view temporary escape was accepted")
    run([compiler, "build", "--project", str(project)])
    executables = [single_native_executable(project / ".chtholly" / "build", "semantic gate")]
    if len(executables) != 1:
        raise AssertionError(f"expected one semantic gate executable, got {executables}")
    run([str(executables[0])])
    hash_project = source_root / "tests" / "fixtures" / "chtholly-error-smoke"
    # Use a fresh project root so the witness artifact is rebuilt when the
    # stdlib implementation or semantic epoch changes.
    with tempfile.TemporaryDirectory(prefix="chtholly-hash-witness-") as hash_temp:
        hash_project = pathlib.Path(hash_temp) / "error_smoke"
        shutil.copytree(source_root / "tests" / "fixtures" / "chtholly-error-smoke",
                        hash_project, ignore=shutil.ignore_patterns(".chtholly"))
        run([compiler, "check", "--project", str(hash_project)])
        run([compiler, "build", "--project", str(hash_project)])
        hash_executables = [single_native_executable(hash_project / ".chtholly" / "build", "semantic gate")]
        if len(hash_executables) != 1:
            raise AssertionError(f"expected one hash witness executable, got {hash_executables}")
        run([str(hash_executables[0])], expected=5)
    print("semantic gate: char, casts, Result<void>, primitive hash witnesses")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
