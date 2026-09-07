import argparse
import json
import os
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any


SCHEMA = "chtholly-build-artifact-report-v1"
OUTPUT_CATEGORIES = {"executable", "shared-library", "static-library"}


def category_for(path: Path) -> str:
    name = path.name.lower()
    suffix = path.suffix.lower()
    if suffix == ".exe":
        return "executable"
    if os.name != "nt" and not suffix and os.access(path, os.X_OK):
        return "executable"
    if suffix in {".dll", ".so", ".dylib"}:
        return "shared-library"
    if suffix in {".lib", ".a"}:
        return "static-library"
    if suffix in {".obj", ".o"}:
        return "object"
    if suffix in {".pdb", ".ilk", ".idb", ".ipdb", ".iobj", ".tlog"}:
        return "debug-symbols"
    if (
        name
        in {
            "cmakecache.txt",
            "build.ninja",
            "rules.ninja",
            ".ninja_deps",
            ".ninja_log",
        }
        or suffix in {".ninja", ".cmake", ".rsp", ".manifest", ".res", ".d"}
    ):
        return "build-metadata"
    if suffix in {".h", ".hpp", ".c", ".cc", ".cpp", ".inc"}:
        return "generated-source"
    if suffix in {".json", ".txt", ".log", ".xml"}:
        return "report-data"
    return "other"


def relative_path(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def build_report(build_dir: Path, largest_limit: int) -> dict[str, Any]:
    root = build_dir.resolve(strict=True)
    if not root.is_dir():
        raise NotADirectoryError(f"build directory is not a directory: {root}")

    files: list[dict[str, Any]] = []
    categories: dict[str, dict[str, int]] = defaultdict(
        lambda: {"file_count": 0, "total_bytes": 0}
    )
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        size = path.stat().st_size
        category = category_for(path)
        entry = {
            "path": relative_path(path, root),
            "bytes": size,
            "category": category,
        }
        files.append(entry)
        categories[category]["file_count"] += 1
        categories[category]["total_bytes"] += size

    files.sort(key=lambda entry: entry["path"])
    largest = sorted(files, key=lambda entry: (-entry["bytes"], entry["path"]))
    outputs = [entry for entry in files if entry["category"] in OUTPUT_CATEGORIES]
    ordered_categories = {name: categories[name] for name in sorted(categories)}
    return {
        "schema": SCHEMA,
        "valid": True,
        "build_dir": root.as_posix(),
        "file_count": len(files),
        "total_bytes": sum(entry["bytes"] for entry in files),
        "categories": ordered_categories,
        "outputs": outputs,
        "largest_files": largest[:largest_limit],
    }


def write_json(report: dict[str, Any], path: Path) -> None:
    resolved = path.resolve()
    resolved.parent.mkdir(parents=True, exist_ok=True)
    resolved.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def print_report(report: dict[str, Any], json_out: Path | None) -> None:
    print("build-artifact-report=pass")
    print(f"schema={report['schema']}")
    print(f"build-dir={report['build_dir']}")
    print(f"files={report['file_count']}")
    print(f"bytes={report['total_bytes']}")
    for name, values in report["categories"].items():
        print(
            f"category={name} files={values['file_count']} bytes={values['total_bytes']}"
        )
    for entry in report["largest_files"]:
        print(f"largest={entry['path']} bytes={entry['bytes']}")
    if json_out is not None:
        print(f"json={json_out.resolve().as_posix()}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Report Chtholly build artifacts")
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--largest", type=int, default=20)
    args = parser.parse_args()

    if args.largest < 0:
        print("stable-reason=build-artifact-report-arguments", file=sys.stderr)
        print("--largest must be non-negative", file=sys.stderr)
        return 1

    try:
        report = build_report(args.build_dir, args.largest)
    except (OSError, RuntimeError) as error:
        print("stable-reason=build-artifact-report-build-dir", file=sys.stderr)
        print(str(error), file=sys.stderr)
        return 1

    if args.json_out is not None:
        try:
            write_json(report, args.json_out)
        except OSError as error:
            print("stable-reason=build-artifact-report-json", file=sys.stderr)
            print(str(error), file=sys.stderr)
            return 1

    print_report(report, args.json_out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
