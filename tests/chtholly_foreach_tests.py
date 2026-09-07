#!/usr/bin/env python3

import argparse
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


def build_and_run(chthollyc: str, fixture: pathlib.Path, emit_llvm: bool) -> None:
    with tempfile.TemporaryDirectory(prefix=f"chtholly-{fixture.name}-") as raw:
        project = pathlib.Path(raw) / fixture.name
        shutil.copytree(
            fixture,
            project,
            ignore=shutil.ignore_patterns(".chtholly", "chtholly.lock"),
        )
        semir = pathlib.Path(raw) / f"{fixture.name}.semir"
        lowir = pathlib.Path(raw) / f"{fixture.name}.lowir"
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
        if not semir.is_file() or semir.stat().st_size == 0:
            raise AssertionError(f"foreach fixture omitted SemIR evidence: {fixture.name}")
        if not lowir.is_file() or lowir.stat().st_size == 0:
            raise AssertionError(f"foreach fixture omitted LowIR evidence: {fixture.name}")

        executable = single_native_executable(
            project / ".chtholly" / "build", f"foreach fixture {fixture.name}")
        invoke([str(executable)])

        if emit_llvm:
            llvm = pathlib.Path(raw) / f"{fixture.name}.ll"
            invoke([
                chthollyc,
                str(project / "main.cns"),
                "-emit-llvm",
                "-o",
                str(llvm),
            ])
            llvm_text = llvm.read_text(encoding="utf-8")
            for marker in (
                "iterator.item",
                "iterator.done",
                "define i32 @chtholly.entry()",
            ):
                if marker not in llvm_text:
                    raise AssertionError(
                        f"foreach LLVM evidence omitted {marker!r}: {fixture.name}"
                    )


def write_project(root: pathlib.Path, name: str, source: str) -> pathlib.Path:
    project = root / name
    (project / "src").mkdir(parents=True)
    (project / "chtholly.toml").write_text(
        f'[package]\nname = "{name}"\nlanguage = "1.4"\n\n'
        '[build]\nentry = "src/main.cns"\nmodule_paths = ["src"]\n',
        encoding="utf-8",
    )
    (project / "src" / "main.cns").write_text(source, encoding="utf-8")
    return project


def expect_diagnostic(
    chthollyc: str,
    root: pathlib.Path,
    name: str,
    source: str,
    diagnostic: str,
) -> None:
    result = invoke(
        [chthollyc, "check", "--project", str(write_project(root, name, source))],
        expected=1,
    )
    if diagnostic not in result.stderr:
        raise AssertionError(
            f"foreach case {name!r} omitted {diagnostic!r}\nstderr:\n{result.stderr}"
        )
    if "chtholly.next.sem.unsupported" in result.stderr:
        raise AssertionError(
            f"foreach case {name!r} fell back to UnsupportedSemantics\n"
            f"stderr:\n{result.stderr}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    parser.add_argument("--source-dir", required=True)
    args = parser.parse_args()

    fixtures = pathlib.Path(args.source_dir) / "tests" / "fixtures"
    build_and_run(args.chthollyc, fixtures / "chtholly-foreach-runtime", True)
    build_and_run(args.chthollyc, fixtures / "chtholly-foreach-lifecycle", False)

    with tempfile.TemporaryDirectory(prefix="chtholly-foreach-negative-") as raw:
        root = pathlib.Path(raw)
        expect_diagnostic(
            args.chthollyc,
            root,
            "invalid_binding",
            "module main;\nfn main(): i32 { foreach (item in 0) { return 1; } return 0; }\n",
            "chtholly.next.parse.foreach.invalid-binding",
        )
        expect_diagnostic(
            args.chthollyc,
            root,
            "invalid_iterator",
            "module main;\nfn main(): i32 { foreach (let item in 0) { return item; } return 0; }\n",
            "chtholly.next.sem.foreach.invalid-iterator",
        )
        expect_diagnostic(
            args.chthollyc,
            root,
            "move_required",
            "module main;\nimport std::vec;\nfn main(): i32 { "
            "var values = std::vec::Vec<i32>::init(); let iterator = values.iter(); "
            "foreach (let item in iterator) { return item; } return 0; }\n",
            "chtholly.next.sem.foreach.move-required",
        )
        expect_diagnostic(
            args.chthollyc,
            root,
            "item_type_mismatch",
            "module main;\nimport std::vec;\nfn main(): i32 { "
            "var values = std::vec::Vec<i32>::init(); "
            "foreach (let item: i32 in values.iter()) { return item; } return 0; }\n",
            "chtholly.next.sem.foreach.item-type-mismatch",
        )
        expect_diagnostic(
            args.chthollyc,
            root,
            "structural_mutation",
            "module main;\nimport std::vec;\nfn main(): i32 { "
            "var values = std::vec::Vec<i32>::init(); values.push(1); "
            "foreach (let item in values.iter()) { values.push(2); "
            "if (item == 0) { return 1; } } return 0; }\n",
            "chtholly.next.sem.borrow.region-conflict",
        )
        expect_diagnostic(
            args.chthollyc,
            root,
            "item_escape",
            "module main;\nimport std::vec;\nfn first(): const i32& { "
            "var values = std::vec::Vec<i32>::init(); values.push(1); "
            "foreach (let item in values.iter()) { return item; } "
            "return values.at(0usize); }\nfn main(): i32 { return 0; }\n",
            "chtholly.next.sem.ownership.borrow-return-escape",
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
