#!/usr/bin/env python3

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import tempfile


def run(executable: str, *arguments: str, cwd: pathlib.Path | None = None,
        expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        [executable, *arguments],
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


def diagnostic_counters(records: list[dict]) -> dict[str, int]:
    """Count JSONL explanation records without changing diagnostic codes."""
    related_note_count = 0
    unavailable_location_count = 0
    quick_fix_count = 0
    for record in records:
        if record.get("kind") != "diagnostic":
            continue
        related = record.get("related", [])
        if isinstance(related, list):
            related_note_count += len(related)
            unavailable_location_count += sum(
                item.get("location-available") is False
                for item in related if isinstance(item, dict)
            )
        fixits = record.get("fixits", [])
        if isinstance(fixits, str):
            try:
                fixits = json.loads(fixits)
            except json.JSONDecodeError:
                fixits = []
        if isinstance(fixits, list):
            quick_fix_count += len(fixits)
    return {
        "related_note_count": related_note_count,
        "unavailable_location_count": unavailable_location_count,
        "quick_fix_count": quick_fix_count,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="chtholly-project-workflow-") as raw:
        root = pathlib.Path(raw)
        app = root / "app"
        library = root / "preview_lib"

        doctor = run(args.chthollyc, "doctor", "--output-format", "jsonl")
        doctor_records = [json.loads(line) for line in doctor.stdout.splitlines()]
        doctor_names = {
            record.get("name") for record in doctor_records
            if record.get("kind") == "command-output"
        }
        assert {"compiler", "resources", "runtime", "stdlib", "target",
                "pointer-width", "endianness", "component-abi", "runtime-abi",
                "c-compiler", "c-sdk", "c-includes", "c-libraries",
                "cffi-tool", "libclang", "cffi-doctor", "cffi-probe", "linker",
                "doctor"} <= doctor_names
        assert doctor_records[-1]["action"] == "doctor"
        assert doctor_records[-1]["status"] == "success"
        values = {
            record.get("name"): record.get("value")
            for record in doctor_records if record.get("kind") == "command-output"
        }
        assert values["pointer-width"] == "64"
        assert values["endianness"] == "little"
        assert values["component-abi"] == "1"
        assert values["runtime-abi"] == "v1"

        broken_doctor = run(
            args.chthollyc,
            "doctor",
            "--resource-dir",
            str(root / "missing-resources"),
            expected=1,
        )
        assert "chtholly.doctor.failed" in broken_doctor.stderr
        assert "--resource-dir" in broken_doctor.stderr

        typo = run(args.chthollyc, "chek", expected=2)
        assert "unknown command 'chek'" in typo.stderr
        assert "did you mean 'check'" in typo.stderr

        created = run(args.chthollyc, "new", str(app), "--name", "preview_app")
        assert "created\tpreview_app" in created.stdout
        run(args.chthollyc, "new", str(library), "--lib")
        assert 'language = "1.10"' in (app / "chtholly.toml").read_text(
            encoding="utf-8"
        )
        assert (library / "src" / "lib.cns").is_file()
        assert not (library / "src" / "entry.cns").exists()
        library_manifest = (library / "chtholly.toml").read_text(
            encoding="utf-8")
        assert "entry =" not in library_manifest

        debug_source = root / "debug-source" / "main.cns"
        debug_source.parent.mkdir()
        debug_source.write_text(
            "module debug_source;\n\n"
            "struct Pair { left: i32; right: i64; }\n\n"
            "fn main(): i32 {\n"
            "  let pair = Pair { .left = 1, .right = 2i64 };\n"
            "  let tuple = (pair.left, 2i32);\n"
            "  let values: i32[2] = [3, 4];\n"
            "  let value = pair.left + tuple.0 + values[1];\n"
            "  return if (value == 6) { 0 } else { 1 };\n"
            "}\n",
            encoding="utf-8",
        )
        debug_ir = root / "debug-main.ll"
        run(
            args.chthollyc,
            str(debug_source),
            "-emit-llvm",
            "-gline-tables-only",
            "-o",
            str(debug_ir),
        )
        debug_text = debug_ir.read_text(encoding="utf-8")
        assert "!DICompileUnit" in debug_text
        assert "!DISubprogram(name: \"main\"" in debug_text
        assert "emissionKind: LineTablesOnly" in debug_text
        assert "!DILocation(line:" in debug_text
        assert "DILocalVariable" not in debug_text
        full_debug_ir = root / "debug-main-full.ll"
        run(
            args.chthollyc,
            str(debug_source),
            "-emit-llvm",
            "-g",
            "-o",
            str(full_debug_ir),
        )
        full_debug_text = full_debug_ir.read_text(encoding="utf-8")
        assert "emissionKind: FullDebug" in full_debug_text
        assert "DILocalVariable(name: \"value\"" in full_debug_text
        assert "llvm.dbg.declare" in full_debug_text
        assert "DICompositeType" in full_debug_text
        assert "DIDerivedType(tag: DW_TAG_member, name: \"left\"" in full_debug_text
        assert "DIDerivedType(tag: DW_TAG_member, name: \"right\"" in full_debug_text
        assert "name: \"[0]\"" in full_debug_text
        assert "DICompositeType(tag: DW_TAG_array_type" in full_debug_text

        debug_executable = root / (
            "debug-program.exe" if os.name == "nt" else "debug-program")
        run(args.chthollyc, str(debug_source), "-emit-exe", "-g", "-o",
            str(debug_executable))
        run(str(debug_executable))
        if os.name == "nt":
            assert debug_executable.with_suffix(".pdb").is_file()

        # Optimization levels are part of the native build contract.  Keep a
        # small executable smoke check here so accepting -O1/-O2 cannot regress
        # back to the old "optimization options are not implemented" path.
        for optimization in ("-O1", "-O2", "-O3", "-Os", "-Oz"):
            optimized_executable = root / (
                f"optimized-{optimization[1:]}.exe"
                if os.name == "nt"
                else f"optimized-{optimization[1:]}"
            )
            run(args.chthollyc, str(debug_source), "-emit-exe", optimization,
                "-o", str(optimized_executable))
            run(str(optimized_executable))

        with (app / "chtholly.toml").open("a", encoding="utf-8") as manifest:
            manifest.write(
                '\n[dependencies]\npreview_lib = { path = "../preview_lib" }\n'
            )
        (app / "src" / "main.cns").write_text(
            "module main;\n\nimport preview_lib;\n\n"
            "fn main(): i32 { return preview_lib::identity(0); }\n",
            encoding="utf-8",
        )

        checked = run(args.chthollyc, "check", "--project", str(library))
        assert "checked\tpreview_lib" in checked.stdout
        library_build = run(
            args.chthollyc, "build", "--project", str(library), expected=1)
        assert "requires build.entry" in library_build.stderr
        run(args.chthollyc, "run", "--project", str(app))

        jsonl = run(
            args.chthollyc,
            "check",
            "--project",
            str(app),
            "--output-format",
            "jsonl",
        )
        records = [json.loads(line) for line in jsonl.stdout.splitlines()]
        assert records[-1]["kind"] == "command-result"
        assert records[-1]["action"] == "check"
        assert records[-1]["status"] == "success"
        assert diagnostic_counters(records) == {
            "related_note_count": 0,
            "unavailable_location_count": 0,
            "quick_fix_count": 0,
        }

        # Semantic ownership failures are emitted as structured diagnostics;
        # JSONL v2 must retain the borrow origin and callable-effect notes.
        staged_negative = root / "staged-negative"
        shutil.copytree(
            pathlib.Path(__file__).parent / "fixtures" /
            "chtholly-1-9-staged-negative",
            staged_negative,
            ignore=shutil.ignore_patterns(".chtholly", "chtholly.lock"),
        )
        ownership_failure = run(
            args.chthollyc,
            "check",
            "--project",
            str(staged_negative),
            "--output-format",
            "jsonl",
            expected=1,
        )
        ownership_records = [
            json.loads(line) for line in ownership_failure.stdout.splitlines()
        ]
        ownership_diagnostics = [
            record for record in ownership_records
            if record.get("kind") == "diagnostic"
            and record.get("code", "").startswith("chtholly.next.sem.borrow")
        ]
        assert ownership_diagnostics
        related_codes = {
            item["code"]
            for item in ownership_diagnostics[0].get("related", [])
        }
        assert "chtholly.next.note.ownership.borrow-origin" in related_codes
        assert "chtholly.next.note.ownership.call-effect" in related_codes
        assert all(item.get("location-available") is True
                   for item in ownership_diagnostics[0].get("related", []))
        assert diagnostic_counters(ownership_records) == {
            "related_note_count": 2,
            "unavailable_location_count": 0,
            "quick_fix_count": 0,
        }

        initialized = root / "initialized"
        initialized.mkdir()
        run(args.chthollyc, "init", "--name", "initialized", cwd=initialized)
        assert (initialized / "src" / "main.cns").is_file()

        occupied = root / "occupied"
        occupied.mkdir()
        marker = occupied / "keep.txt"
        marker.write_text("keep", encoding="utf-8")
        rejected = run(
            args.chthollyc,
            "new",
            str(occupied),
            "--name",
            "occupied",
            expected=1,
        )
        assert "not empty" in rejected.stderr
        assert marker.read_text(encoding="utf-8") == "keep"

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
