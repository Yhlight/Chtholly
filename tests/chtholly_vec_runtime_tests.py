#!/usr/bin/env python3

import argparse
import pathlib
import shutil
import subprocess
import tempfile

from chtholly_test_support import single_native_executable


def run(chthollyc: str, project: pathlib.Path, expect_success: bool) -> None:
    with tempfile.TemporaryDirectory(prefix=f"chtholly-{project.name}-") as raw:
        clean_project = pathlib.Path(raw) / project.name
        shutil.copytree(
            project,
            clean_project,
            ignore=shutil.ignore_patterns(".chtholly", "chtholly.lock"),
        )
        built = subprocess.run(
            [chthollyc, "build", "--project", str(clean_project)],
            text=True,
            encoding="utf-8",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if built.returncode != 0:
            raise AssertionError(
                f"Vec fixture did not build: {project.name}\n"
                f"stdout:\n{built.stdout}\nstderr:\n{built.stderr}"
            )
        executable = single_native_executable(
            clean_project / ".chtholly" / "build", f"Vec fixture {project.name}")
        result = subprocess.run(
            [str(executable)],
            text=True,
            encoding="utf-8",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if (result.returncode == 0) != expect_success:
            raise AssertionError(
                f"unexpected Vec fixture result for {project.name}: "
                f"{result.returncode}\nstdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    parser.add_argument("--source-dir", required=True)
    args = parser.parse_args()

    fixtures = pathlib.Path(args.source_dir) / "tests" / "fixtures"
    run(args.chthollyc, fixtures / "chtholly-vec-runtime", True)
    run(args.chthollyc, fixtures / "chtholly-vec-lifecycle", True)
    run(args.chthollyc, fixtures / "chtholly-vec-iterator-runtime", True)
    run(args.chthollyc, fixtures / "chtholly-vec-mut-iterator-runtime", True)
    run(args.chthollyc, fixtures / "chtholly-vec-oob", False)
    run(args.chthollyc, fixtures / "chtholly-vec-oom", False)
    run(args.chthollyc, fixtures / "chtholly-vec-overflow", False)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
