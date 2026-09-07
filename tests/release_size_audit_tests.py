import json
import subprocess
import sys
import zipfile
from pathlib import Path


def run_audit(script, archive, tree, budget, output, *extra):
    return subprocess.run(
        [sys.executable, str(script), "--archive", str(archive), "--install-tree", str(tree),
         "--budget", str(budget), "--profile", "full", "--host", "windows-x64",
         "--output", str(output), *extra], capture_output=True, text=True
    )


def make_inputs(tmp_path):
    tree = tmp_path / "tree"
    (tree / "bin").mkdir(parents=True)
    (tree / "bin" / "tool.exe").write_bytes(b"tool-bytes")
    (tree / "docs").mkdir()
    (tree / "docs" / "readme.txt").write_bytes(b"docs")
    archive = tmp_path / "package.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("bin/tool.exe", b"x" * 1000)
    budget = tmp_path / "budget.toml"
    budget.write_text('schema = "chtholly-release-size-budget-v1"\nprofiles = ["full"]\nmax_growth_percent = 15\nmax_file_count_growth = 64\n\n[[budget]]\nhost = "windows-x64"\nprofile = "full"\n[[budget]]\nhost = "linux-x64"\nprofile = "full"\n', encoding="utf-8")
    return archive, tree, budget


def test_report_is_byte_stable(tmp_path, script):
    archive, tree, budget = make_inputs(tmp_path)
    first, second = tmp_path / "one.json", tmp_path / "two.json"
    assert run_audit(script, archive, tree, budget, first).returncode == 0
    assert run_audit(script, archive, tree, budget, second).returncode == 0
    assert json.loads(first.read_text()) == json.loads(second.read_text())


def test_budget_failure_reports_category_and_largest_file(tmp_path, script):
    archive, tree, budget = make_inputs(tmp_path)
    baseline = tmp_path / "baseline.json"
    budget.write_text(budget.read_text().replace('profiles = ["full"]', f'profiles = ["full"]\nbaseline = "{baseline.name}"'), encoding="utf-8")
    baseline.write_text(json.dumps({"schema": "chtholly-release-size-baseline-v1", "records": [{"host": "windows-x64", "profile": "full", "archive_bytes": 1, "archive_uncompressed_bytes": 1, "install_tree_bytes": 1, "file_count": 1}]}))
    result = run_audit(script, archive, tree, budget, tmp_path / "report.json", "--check-budget")
    assert result.returncode == 1
    text = result.stdout + result.stderr
    assert "budget-exceeded" in text and "bin" in text and "bin/tool.exe" in text


def test_archive_uncompressed_size_is_reported(tmp_path, script):
    archive, tree, budget = make_inputs(tmp_path)
    output = tmp_path / "report.json"
    assert run_audit(script, archive, tree, budget, output).returncode == 0
    report = json.loads(output.read_text())
    assert report["archive_bytes"] != report["archive_uncompressed_bytes"]

def test_malformed_budget(tmp_path, script):
    archive, tree, budget = make_inputs(tmp_path); budget.write_text("not = [valid")
    result = run_audit(script, archive, tree, budget, tmp_path / "report.json", "--check-budget")
    assert result.returncode == 1 and "invalid-budget" in result.stderr

def test_missing_baseline(tmp_path, script):
    baseline = tmp_path / "missing.json"; archive, tree, budget = make_inputs(tmp_path)
    budget.write_text(budget.read_text().replace('profiles = ["full"]', f'profiles = ["full"]\nbaseline = "{baseline.name}"'))
    result = run_audit(script, archive, tree, budget, tmp_path / "report.json", "--check-budget")
    assert result.returncode == 1 and "missing baseline" in result.stderr

def test_malformed_baseline(tmp_path, script):
    baseline = tmp_path / "baseline.json"; archive, tree, budget = make_inputs(tmp_path)
    budget.write_text(budget.read_text().replace('profiles = ["full"]', f'profiles = ["full"]\nbaseline = "{baseline.name}"'))
    baseline.write_text("{}")
    result = run_audit(script, archive, tree, budget, tmp_path / "report.json", "--check-budget")
    assert result.returncode == 1 and "invalid-baseline" in result.stderr

def test_unknown_host_profile(tmp_path, script):
    baseline = tmp_path / "baseline.json"; archive, tree, budget = make_inputs(tmp_path)
    budget.write_text(budget.read_text().replace('profiles = ["full"]', f'profiles = ["full"]\nbaseline = "{baseline.name}"'))
    baseline.write_text(json.dumps({"schema": "chtholly-release-size-baseline-v1", "records": []}))
    result = run_audit(script, archive, tree, budget, tmp_path / "report.json", "--check-budget")
    assert result.returncode == 1 and "unknown host/profile" in result.stderr

def test_unmeasured_baseline_skips_arithmetic(tmp_path, script):
    baseline = tmp_path / "baseline.json"; archive, tree, budget = make_inputs(tmp_path)
    budget.write_text(budget.read_text().replace('profiles = ["full"]', f'profiles = ["full"]\nbaseline = "{baseline.name}"'))
    baseline.write_text(json.dumps({"schema": "chtholly-release-size-baseline-v1", "records": [{"host": "windows-x64", "profile": "full", "baseline_status": "unmeasured"}]}))
    output = tmp_path / "report.json"; result = run_audit(script, archive, tree, budget, output, "--check-budget")
    assert result.returncode == 0 and json.loads(output.read_text())["baseline_status"] == "unmeasured"

def test_duplicate_baseline_records_rejected(tmp_path, script):
    baseline = tmp_path / "baseline.json"; archive, tree, budget = make_inputs(tmp_path)
    budget.write_text(budget.read_text().replace('profiles = ["full"]', f'profiles = ["full"]\nbaseline = "{baseline.name}"'))
    record = {"host": "windows-x64", "profile": "full", "baseline_status": "unmeasured"}
    baseline.write_text(json.dumps({"schema": "chtholly-release-size-baseline-v1", "records": [record, record]}))
    result = run_audit(script, archive, tree, budget, tmp_path / "report.json", "--check-budget")
    assert result.returncode == 1 and "duplicate host/profile" in result.stderr

def test_incomplete_budget_profiles_rejected(tmp_path, script):
    archive, tree, budget = make_inputs(tmp_path)
    budget.write_text(budget.read_text().replace('profiles = ["full"]', 'profiles = ["minimal", "full"]'))
    result = run_audit(script, archive, tree, budget, tmp_path / "report.json", "--check-budget")
    assert result.returncode == 1 and "invalid-budget: host/profile" in result.stderr

def test_duplicate_budget_pairs_rejected(tmp_path, script):
    archive, tree, budget = make_inputs(tmp_path)
    budget.write_text(budget.read_text() + '\n[[budget]]\nhost = "windows-x64"\nprofile = "full"\n')
    result = run_audit(script, archive, tree, budget, tmp_path / "report.json", "--check-budget")
    assert result.returncode == 1 and "invalid-budget: host/profile" in result.stderr

def test_requested_pair_must_be_declared(tmp_path, script):
    archive, tree, budget = make_inputs(tmp_path)
    baseline = tmp_path / "baseline.json"
    budget.write_text(budget.read_text().replace(f'[[budget]]\nhost = "linux-x64"\nprofile = "full"\n', ''), encoding="utf-8")
    budget.write_text(budget.read_text().replace('profiles = ["full"]', f'profiles = ["full"]\nbaseline = "{baseline.name}"'), encoding="utf-8")
    baseline.write_text(json.dumps({"schema": "chtholly-release-size-baseline-v1", "records": [{"host": "linux-x64", "profile": "full", "baseline_status": "unmeasured"}]}))
    result = subprocess.run(
        [sys.executable, str(script), "--archive", str(archive), "--install-tree", str(tree),
         "--budget", str(budget), "--profile", "full", "--host", "linux-x64",
         "--output", str(tmp_path / "report.json"), "--check-budget"],
        capture_output=True, text=True,
    )
    assert result.returncode == 1 and "invalid-budget: host/profile" in result.stderr

def test_empty_tree(tmp_path, script):
    archive, _, budget = make_inputs(tmp_path); empty = tmp_path / "empty"; empty.mkdir()
    result = run_audit(script, archive, empty, budget, tmp_path / "report.json")
    assert result.returncode == 1 and "empty tree" in result.stderr

def test_symlink(tmp_path, script):
    archive, tree, budget = make_inputs(tmp_path)
    try: (tree / "link").symlink_to(tree / "bin/tool.exe")
    except OSError: return
    result = run_audit(script, archive, tree, budget, tmp_path / "report.json")
    assert result.returncode == 1 and "symlink" in result.stderr

def test_unexpected_exception_reporting(tmp_path, script):
    _, tree, budget = make_inputs(tmp_path)
    result = run_audit(script, tmp_path / "missing.zip", tree, budget, tmp_path / "report.json")
    assert result.returncode == 1 and "unexpected-error" in result.stdout


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", required=True)
    parser.add_argument("--check-budget", action="store_true")
    args = parser.parse_args()
    import tempfile
    import types
    import inspect
    failures = []
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            with tempfile.TemporaryDirectory() as d:
                try: fn(Path(d), Path(args.script))
                except Exception as e: failures.append(f"{name}: {type(e).__name__}: {e}")
    if failures:
        print("\n".join(failures), file=sys.stderr); raise SystemExit(1)
    print(f"{len([name for name in globals() if name.startswith('test_')])} tests passed")
