#!/usr/bin/env python3

import argparse
import pathlib
import shutil
import tempfile

from chtholly_test_support import run, single_native_executable


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="chtholly-std-fs-") as raw:
        root = pathlib.Path(raw)
        project = root / "project"
        shutil.copytree(
            args.source_dir / "tests" / "fixtures" / "chtholly-std-safe-e2e",
            project,
            ignore=shutil.ignore_patterns(".chtholly", "chtholly.lock"),
        )
        file_path = root / "utf8-文件.txt"
        run([args.chthollyc, "build", "--project", project])
        executable = single_native_executable(project / ".chtholly")
        result = run([executable, "preview-argument", file_path], cwd=root)
        if file_path.exists():
            raise AssertionError("std::fs::remove left the test file behind")
        expected_output = "safe-io\nsafe-result\npreview-argument\n"
        if result.stdout != expected_output:
            raise AssertionError(f"unexpected safe stdlib output: {result.stdout!r}")

        # Run the same already-built program while its project still exists.
        # A compiler or project-discovery failure is not a filesystem result.
        missing_parent = root / "missing" / "error.txt"
        failed = run([executable, "preview-argument", missing_parent], 21, cwd=root)
        if failed.stdout != expected_output or failed.stderr:
            raise AssertionError(f"filesystem failure did not reach the program: {failed}")
        if missing_parent.exists():
            raise AssertionError("failed file creation left an output behind")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
