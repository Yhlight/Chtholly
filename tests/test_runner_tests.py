#!/usr/bin/env python3

import argparse
import json
import pathlib
import subprocess
import sys
import tempfile


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(list(args), text=True, encoding="utf-8",
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          check=False)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True, type=pathlib.Path)
    args = parser.parse_args()
    runner = str(args.runner)

    with tempfile.TemporaryDirectory(prefix="chtholly-runner-fixture-") as fixture_raw:
        fixture = pathlib.Path(fixture_raw) / "fixture.toml"
        fixture.write_text(
            "format = 1\n"
            "suite = \"runner-fixture\"\n\n"
            "[[test]]\n"
            "name = \"runner_fixture_pass\"\n"
            "kind = \"process\"\n"
            "labels = [\"runner\"]\n"
            "capabilities = [\"python\", \"smoke\"]\n"
            f"command = [{json.dumps(sys.executable)}, \"-c\", "
            f"{json.dumps('import sys; sys.exit(0)')}]\n"
            "timeout_seconds = 5\n"
            "serial = true\n",
            encoding="utf-8",
        )

        listed = run(runner, "list", "--manifest", str(fixture))
        if listed.returncode != 0 or "runner_fixture_pass" not in listed.stdout:
            raise AssertionError(f"list failed: {listed.stdout}\n{listed.stderr}")

        validated = run(runner, "validate", "--manifest", str(fixture))
        if validated.returncode != 0 or "manifest-valid tests=1" not in validated.stdout:
            raise AssertionError(
                f"validate failed: {validated.stdout}\n{validated.stderr}")

        described = run(runner, "describe", "runner_fixture_pass", "--manifest",
                        str(fixture))
        if described.returncode != 0 or "timeout_seconds=5" not in described.stdout:
            raise AssertionError(f"describe failed: {described.stdout}\n{described.stderr}")
        if "capabilities=python,smoke" not in described.stdout:
            raise AssertionError(f"describe omitted capabilities: {described.stdout}")

        passed = run(runner, "run", "--manifest", str(fixture),
                     "--filter", "runner_fixture_pass", "--format", "json")
        if passed.returncode != 0:
            raise AssertionError(f"filtered run failed: {passed.stdout}\n{passed.stderr}")
        report = json.loads(passed.stdout)
        if len(report.get("tests", [])) != 1 or report["tests"][0]["code"] != 0:
            raise AssertionError(f"unexpected JSON report: {report!r}")
        if report["tests"][0]["capabilities"] != ["python", "smoke"]:
            raise AssertionError(f"JSON report omitted capabilities: {report!r}")

        progress = run(runner, "run", "--manifest", str(fixture),
                       "--filter", "runner_fixture_pass", "--progress")
        if (progress.returncode != 0 or
                "RUN runner_fixture_pass" not in progress.stderr or
                "DONE runner_fixture_pass code=0" not in progress.stderr):
            raise AssertionError(
                f"progress reporting failed: {progress.stdout}\n{progress.stderr}")

        capability = run(runner, "run", "--manifest", str(fixture),
                         "--capability", "smoke")
        if capability.returncode != 0:
            raise AssertionError(f"capability selection failed: {capability.stdout}")
        absent = run(runner, "run", "--manifest", str(fixture),
                     "--capability", "missing")
        if absent.returncode != 0 or absent.stdout.strip():
            raise AssertionError(f"missing capability was not an empty selection: {absent.stdout}")

        artifacts = pathlib.Path(fixture_raw) / "artifacts"
        retained = run(runner, "run", "--manifest", str(fixture),
                       "--filter", "runner_fixture_pass", "--artifact-dir",
                       str(artifacts))
        if retained.returncode != 0:
            raise AssertionError(f"artifact retention failed: {retained.stdout}\n{retained.stderr}")
        if not (artifacts / "runner_fixture_pass.stdout").is_file() or \
                not (artifacts / "runner_fixture_pass.stderr").is_file():
            raise AssertionError("runner did not retain subprocess output artifacts")

    with tempfile.TemporaryDirectory(prefix="chtholly-runner-test-") as raw:
        manifest = pathlib.Path(raw) / "timeout.toml"
        manifest.write_text(
            "format = 1\n"
            "[[test]]\n"
            "name = \"runner_fixture_timeout\"\n"
            "kind = \"process\"\n"
            "labels = [\"runner\"]\n"
            f"command = [{json.dumps(sys.executable)}, \"-c\", "
            f"{json.dumps('import time; time.sleep(2)')}]\n"
            "timeout_seconds = 1\n",
            encoding="utf-8",
        )
        timed_out = run(runner, "run", "--manifest", str(manifest))
        if timed_out.returncode != 1 or "timed out" not in timed_out.stdout:
            raise AssertionError(
                f"timeout classification failed: {timed_out.stdout}\n{timed_out.stderr}")

        environment_manifest = pathlib.Path(raw) / "environment.toml"
        environment_manifest.write_text(
            "format = 1\n"
            "[[test]]\n"
            "name = \"runner_fixture_environment\"\n"
            "kind = \"process\"\n"
            "labels = [\"runner\"]\n"
            f"command = [{json.dumps(sys.executable)}, \"-c\", "
            f"{json.dumps('import os; raise SystemExit(0 if os.getenv(\"CHTHOLLY_TEST_ENV\") == \"ok\" else 1)')}]\n"
            "environment = [\"CHTHOLLY_TEST_ENV=ok\"]\n"
            "timeout_seconds = 5\n",
            encoding="utf-8",
        )
        environment = run(runner, "run", "--manifest", str(environment_manifest))
        if environment.returncode != 0:
            raise AssertionError(
                f"environment propagation failed: {environment.stdout}\n{environment.stderr}")

        invalid = pathlib.Path(raw) / "invalid.toml"
        invalid.write_text(
            "format = 1\n[[test]]\nname = \"duplicate\"\n"
            "kind = \"process\"\ncommand = [\"missing\"]\n\n"
            "[[test]]\nname = \"duplicate\"\nkind = \"process\"\n"
            "command = [\"missing\"]\n",
            encoding="utf-8",
        )
        rejected = run(runner, "validate", "--manifest", str(invalid))
        if rejected.returncode == 0 or "duplicate test name" not in rejected.stderr:
            raise AssertionError(
                f"invalid manifest was accepted: {rejected.stdout}\n{rejected.stderr}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
