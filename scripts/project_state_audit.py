#!/usr/bin/env python3
import argparse
import sys
from dataclasses import dataclass
from pathlib import Path


SCAN_ROOTS = (
    "include",
    "lib",
    "tools",
    "runtime",
    "stdlib",
    "examples",
    "docs",
    "tests",
    "scripts",
)
ROOT_FILES = ("CMakeLists.txt", "README.md")
TEXT_SUFFIXES = {
    ".c",
    ".cc",
    ".cns",
    ".cpp",
    ".h",
    ".hpp",
    ".md",
    ".ps1",
    ".py",
    ".toml",
    ".txt",
}
EXCLUDED_PARTS = {"__pycache__"}
LEGACY_MARKERS = (
    # "follow-up" is valid historical release vocabulary and is not itself a
    # stale capability boundary.  Active-state drift is checked by the
    # generated surface and compatibility audits instead.
    "future " + "boundary",
    "future " + "rfc",
)
EXTERNAL_SYNTAX_MARKERS = (
    "Parse" + "RemovedSyntax",
    "removed" + "-syntax.md",
    "C-style " + "ternary",
    "C++-style " + "lifecycle",
    "C++-style " + "destructor",
    "increment/decrement syntax " + "is not supported",
    "bare extern function " + "spelling is not supported",
    "inline struct member function declarations " + "are not supported",
    "operator '=' declarations " + "are not supported",
    "top-level operator overloads " + "are not supported",
    "T[] dynamic array suffix " + "is not supported",
    "fallthrough statement " + "is not supported",
)
GENERATED_PROJECT_STATE_NAMES = {".chtholly", "chtholly.lock"}
GENERATED_STATE_ROOTS = ("examples", "tests/fixtures")


@dataclass(frozen=True)
class Violation:
    path: Path
    line: int
    marker: str


def source_files(source_dir: Path) -> list[Path]:
    files: list[Path] = []
    for root_name in SCAN_ROOTS:
        root = source_dir / root_name
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if (path.is_file() and path.suffix.lower() in TEXT_SUFFIXES and
                    not any(part in EXCLUDED_PARTS for part in path.parts)):
                files.append(path)
    for name in ROOT_FILES:
        path = source_dir / name
        if path.is_file():
            files.append(path)
    return sorted(set(files), key=lambda path: path.as_posix())


def audit(source_dir: Path) -> tuple[list[Path], list[Violation]]:
    files = source_files(source_dir)
    violations: list[Violation] = []
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        for line_number, line in enumerate(text.splitlines(), start=1):
            lowered = line.lower()
            normalized = lowered.replace("_", " ").replace("-", " ")
            for marker in LEGACY_MARKERS:
                normalized_marker = marker.replace("-", " ")
                if marker in lowered or normalized_marker in normalized:
                    violations.append(Violation(path, line_number, marker))
            for marker in EXTERNAL_SYNTAX_MARKERS:
                if marker.lower() in lowered:
                    violations.append(Violation(path, line_number, marker))
    return files, violations


def generated_project_state(source_dir: Path) -> list[Path]:
    """Find compiler state that must not remain in repository examples.

    Project builds intentionally write state under their project root. The
    repository examples and checked-in fixtures must remain source-only so
    tests and documentation never depend on a previous local invocation.
    Restrict this check to those controlled trees; user projects outside the
    checkout are not affected.
    """
    paths: list[Path] = []
    for root_name in GENERATED_STATE_ROOTS:
        root = source_dir / root_name
        if not root.is_dir():
            continue
        paths.extend(
            path for path in root.rglob("*")
            if path.name in GENERATED_PROJECT_STATE_NAMES
        )
    return sorted(paths)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Reject obsolete capability-boundary descriptions")
    parser.add_argument("--source-dir", type=Path, default=Path.cwd())
    args = parser.parse_args()
    source_dir = args.source_dir.resolve()
    try:
        files, violations = audit(source_dir)
        generated_state = generated_project_state(source_dir)
    except OSError as error:
        print("stable-reason=project-state-audit-io", file=sys.stderr)
        print(str(error), file=sys.stderr)
        return 1
    if violations:
        print("stable-reason=project-state-obsolete-description", file=sys.stderr)
        for violation in violations:
            relative = violation.path.relative_to(source_dir).as_posix()
            print(
                f"{relative}:{violation.line}: obsolete marker "
                f"{violation.marker!r}",
                file=sys.stderr,
            )
        return 1
    if generated_state:
        print("stable-reason=project-state-generated-fixture-state", file=sys.stderr)
        for path in generated_state:
            print(
                f"{path.relative_to(source_dir).as_posix()}: generated project "
                "state must not remain in controlled source fixtures",
                file=sys.stderr,
            )
        return 1
    print("project-state-audit=pass")
    print(f"project-state-files={len(files)}")
    print("project-state-obsolete-descriptions=0")
    print("project-state-external-syntax-markers=0")
    print("project-state-generated-controlled-state=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
