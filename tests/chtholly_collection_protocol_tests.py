#!/usr/bin/env python3

import argparse
import pathlib
import shutil
import subprocess
import tempfile


def invoke(command: list[str], expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command, text=True, encoding="utf-8", stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False)
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}: {command!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()

    fixtures = args.source_dir / "tests" / "fixtures"
    with tempfile.TemporaryDirectory(prefix="chtholly-1-9-gate-") as raw:
        root = pathlib.Path(raw)
        positive = root / "positive"
        negative = root / "negative"
        shutil.copytree(
            fixtures / "chtholly-1-9-smoke", positive,
            ignore=shutil.ignore_patterns(".chtholly", "chtholly.lock"),
        )
        shutil.copytree(
            fixtures / "chtholly-1-9-staged-negative", negative,
            ignore=shutil.ignore_patterns(".chtholly", "chtholly.lock"),
        )

        invoke([args.chthollyc, "check", "--project", str(positive)])
        invoke([args.chthollyc, "check", "--project", str(positive)])
        invoke([args.chthollyc, "run", "--project", str(positive)])

        rejected = invoke(
            [args.chthollyc, "check", "--project", str(negative)], expected=1)
        if "chtholly.next.sem.borrow.region-conflict" not in rejected.stderr:
            raise AssertionError(
                "staged mutable continuation omitted borrow conflict:\n" +
                rejected.stderr)

        project = root / "gate"
        (project / "src").mkdir(parents=True)
        (project / "chtholly.toml").write_text(
            '[package]\nname = "gate"\nlanguage = "1.8"\n\n'
            '[build]\nentry = "src/main.cns"\nmodule_paths = ["src"]\n',
            encoding="utf-8")
        (project / "src" / "main.cns").write_text(
            "module main;\nimport std::collections;\n"
            "fn main(): i32 { return 0; }\n", encoding="utf-8")
        gated = invoke(
            [args.chthollyc, "check", "--project", str(project)], expected=1)
        if "introduced in 1.9" not in gated.stderr:
            raise AssertionError(
                "collection protocol omitted 1.9 module gate:\n" + gated.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
