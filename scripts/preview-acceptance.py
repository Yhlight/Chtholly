#!/usr/bin/env python3
"""Run the supported 0.2.0 preview workflow in a clean temporary tree.

This is intentionally a user-facing workflow check rather than a compiler
unit test. It exercises the installed-style CLI contract: doctor, project
scaffolding, a local library dependency, check/build/run, warm reuse, and a
stable negative diagnostic. The script emits JSONL so CI and the dedicated
Chtholly test runner can retain the evidence without parsing human output.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import tempfile
from typing import Any


def run(
    compiler: str,
    *arguments: str,
    cwd: pathlib.Path | None = None,
    expected: int = 0,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        [compiler, *arguments],
        cwd=cwd,
        text=True,
        encoding="utf-8",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}: "
            f"{arguments!r}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def emit(events: list[dict[str, Any]], name: str, **fields: Any) -> None:
    events.append({"schema": "chtholly-preview-acceptance-v1", "name": name, **fields})


def doctor_check(compiler: str, events: list[dict[str, Any]]) -> None:
    result = run(compiler, "doctor", "--output-format", "jsonl")
    records = [json.loads(line) for line in result.stdout.splitlines() if line]
    names = {
        record.get("name")
        for record in records
        if record.get("kind") == "command-output"
    }
    required = {
        "compiler",
        "resources",
        "runtime",
        "stdlib",
        "target",
        "c-compiler",
        "c-sdk",
        "cffi-tool",
        "libclang",
        "cffi-doctor",
        "cffi-probe",
        "linker",
        "doctor",
    }
    missing = sorted(required - names)
    if missing:
        raise AssertionError(f"doctor omitted preview checks: {missing}")
    if not records or records[-1].get("status") != "success":
        raise AssertionError("doctor did not report a successful command result")
    emit(events, "doctor", checks=len(names), status="success")


def workflow_check(compiler: str, events: list[dict[str, Any]]) -> None:
    with tempfile.TemporaryDirectory(prefix="chtholly-preview-acceptance-") as raw:
        root = pathlib.Path(raw)
        app = root / "app"
        library = root / "preview_lib"

        created = run(compiler, "new", str(app), "--name", "preview_app")
        if "created\tpreview_app" not in created.stdout:
            raise AssertionError(f"unexpected application scaffold output: {created.stdout!r}")
        run(compiler, "new", str(library), "--lib", "--name", "preview_lib")

        manifest = app / "chtholly.toml"
        manifest.write_text(
            manifest.read_text(encoding="utf-8")
            + '\n[dependencies]\npreview_lib = { path = "../preview_lib" }\n',
            encoding="utf-8",
        )
        (app / "src" / "main.cns").write_text(
            "module main;\n\n"
            "import preview_lib;\n\n"
            "fn main(): i32 { return preview_lib::identity(0); }\n",
            encoding="utf-8",
        )

        checked = run(compiler, "check", "--project", str(app))
        if "checked\tpreview_app" not in checked.stdout:
            raise AssertionError(f"unexpected check output: {checked.stdout!r}")

        first_build = run(compiler, "build", "--project", str(app))
        if "built\tpreview_app" not in first_build.stdout:
            raise AssertionError(f"unexpected cold build output: {first_build.stdout!r}")
        second_build = run(compiler, "build", "--project", str(app))
        if "built\tpreview_app" not in second_build.stdout:
            raise AssertionError(f"unexpected warm build output: {second_build.stdout!r}")

        run_result = run(compiler, "run", "--project", str(app), "--", "preview")
        if run_result.returncode != 0:
            raise AssertionError("preview application did not run successfully")

        broken = run(
            compiler,
            "check",
            "--project",
            str(root / "missing-project"),
            expected=1,
        )
        if "chtholly" not in broken.stderr.lower():
            raise AssertionError("missing project did not produce a Chtholly diagnostic")

        emit(
            events,
            "workflow",
            status="success",
            cold_build=True,
            warm_build=True,
            local_path_dependency=True,
            native_run=True,
            negative_diagnostic=True,
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()

    events: list[dict[str, Any]] = []
    try:
        doctor_check(args.chthollyc, events)
        workflow_check(args.chthollyc, events)
    except (AssertionError, OSError, json.JSONDecodeError) as error:
        emit(events, "acceptance", status="failed", error=str(error))
        output = "\n".join(json.dumps(event, sort_keys=True) for event in events)
        if args.output:
            args.output.write_text(output + "\n", encoding="utf-8")
        print(output)
        return 1

    emit(events, "acceptance", status="success")
    output = "\n".join(json.dumps(event, sort_keys=True) for event in events)
    if args.output:
        args.output.write_text(output + "\n", encoding="utf-8")
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
