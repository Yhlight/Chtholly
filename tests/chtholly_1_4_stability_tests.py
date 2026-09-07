#!/usr/bin/env python3

import argparse
import concurrent.futures
import hashlib
import pathlib
import shutil
import subprocess
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


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cold_build(chthollyc: str, project: pathlib.Path,
               output: pathlib.Path) -> tuple[str, str, str]:
    build = project / ".chtholly"
    if build.exists():
        shutil.rmtree(build)
    semir = output.with_suffix(".semir")
    lowir = output.with_suffix(".lowir")
    llvm = output.with_suffix(".ll")
    invoke([
        chthollyc,
        "build",
        "--project",
        str(project),
        "--dump-semir",
        str(semir),
        "--dump-lowir",
        str(lowir),
    ])
    invoke([
        chthollyc,
        str(project / "main.cns"),
        "-emit-llvm",
        "-o",
        str(llvm),
    ])
    executable = single_native_executable(
        build, "stability fixture")
    invoke([str(executable)])
    return digest(semir), digest(lowir), digest(llvm)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--container-test", required=True)
    args = parser.parse_args()
    source_dir = pathlib.Path(args.source_dir)

    with tempfile.TemporaryDirectory(prefix="chtholly-1-4-stability-") as raw:
        root = pathlib.Path(raw)
        project = root / "foreach"
        shutil.copytree(
            source_dir / "tests" / "fixtures" / "chtholly-foreach-runtime",
            project,
            ignore=shutil.ignore_patterns(".chtholly", "chtholly.lock"),
        )
        fingerprints = [
            cold_build(args.chthollyc, project, root / f"cold-{index}")
            for index in range(3)
        ]
        if len(set(fingerprints)) != 1:
            raise AssertionError(
                f"cold foreach builds are not deterministic: {fingerprints}")

        invalid_sources = {
            "invalid-iterator": (
                "module main;\nfn main(): i32 { foreach (let item in 0) { "
                "return item; } return 0; }\n"),
            "invalid-binding": (
                "module main;\nfn main(): i32 { foreach (item in 0) { "
                "return 1; } return 0; }\n"),
            "truncated-generic": (
                "module main;\nfn identity<T>(value: T): T { "
                "return move value;\n"),
            "broken-control-flow": (
                "module main;\nfn main(): i32 { if (true { "
                "while () { return 1; } }\n"),
        }
        for name, source in invalid_sources.items():
            invalid = root / name
            (invalid / "src").mkdir(parents=True)
            (invalid / "chtholly.toml").write_text(
                f'[package]\nname = "stability-{name}"\nlanguage = "1.4"\n\n'
                '[build]\nentry = "src/main.cns"\nmodule_paths = ["src"]\n',
                encoding="utf-8",
            )
            (invalid / "src" / "main.cns").write_text(source, encoding="utf-8")
            diagnostics = [
                invoke([args.chthollyc, "check", "--project", str(invalid)],
                       expected=1).stderr
                for _ in range(5)
            ]
            if len(set(diagnostics)) != 1:
                raise AssertionError(
                    f"repeated diagnostics are not deterministic for {name}")

        concurrent_projects = []
        for index in range(12):
            target = root / f"parallel-{index}"
            shutil.copytree(project, target,
                            ignore=shutil.ignore_patterns(".chtholly",
                                                          "chtholly.lock"))
            concurrent_projects.append(target)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            results = list(executor.map(
                lambda target: invoke([
                    args.chthollyc, "check", "--project", str(target)]),
                concurrent_projects,
            ))
        if len(results) != len(concurrent_projects):
            raise AssertionError("parallel compiler stress did not complete")

        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            container_results = list(executor.map(
                lambda _: invoke([args.container_test]), range(32)))
        if len(container_results) != 32:
            raise AssertionError("container ownership stress did not complete")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
