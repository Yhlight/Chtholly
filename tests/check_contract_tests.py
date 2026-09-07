"""Prove that native checks execute, including in NDEBUG builds."""
import argparse
import pathlib
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--fixture", required=True)
parser.add_argument("--source-dir", type=pathlib.Path, required=True)
args = parser.parse_args()
for suffix, expected in (([], 0), (["--fail"], 97)):
    result = subprocess.run([args.fixture, *suffix], capture_output=True,
                            text=True, timeout=10)
    if result.returncode != expected:
        raise AssertionError((result.returncode, result.stdout, result.stderr))
    if expected and "CHECK failed: false" not in result.stderr:
        raise AssertionError("failed check did not report its expression")
for source in (args.source_dir / "tests").glob("*.cpp"):
    import re
    if re.search(r"(?<![\w])assert\s*\(", source.read_text(encoding="utf-8")):
        raise AssertionError(f"configuration-dependent test assertion: {source}")
