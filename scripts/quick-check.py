"""Run the portable pre-submit subset through the generated manifest."""
import argparse
import pathlib
import subprocess
import tomllib

parser = argparse.ArgumentParser()
parser.add_argument("--build-dir", type=pathlib.Path, required=True)
args = parser.parse_args()
build = args.build_dir.resolve()
runner = build / "tools/chtholly-test/chtholly-test"
if not runner.is_file():
    runner = runner.with_suffix(".exe")
manifest = build / "tests/chtholly-tests.generated.toml"
names = {test["name"] for test in tomllib.loads(manifest.read_text())["test"]}
for name in ("chtholly_check_contract_tests", "chtholly_outcome_tests",
             "chtholly_source_dispatch_tests", "chtholly_feature_matrix_tests",
             "chtholly_semantic_gate_tests", "chtholly_contract_artifact_tests",
             "chtholly_component_abi2_design_tests"):
    if name not in names:
        raise RuntimeError(f"missing required test: {name}")
    subprocess.run([str(runner), "run", "--manifest", str(manifest),
                    "--filter", name, "--jobs", "2"], check=True)
