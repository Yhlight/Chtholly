#!/usr/bin/env python3

import argparse
import json
import pathlib
import shutil
import subprocess
import tempfile


BASELINE_SCAN_EQUIVALENT = {
    "smoke": 4_871_544,
    "telemetry": 2_427_138,
}


def invoke(command: list[str], expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command, text=True, encoding="utf-8", stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False)
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}: {command!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def deterministic_totals(metrics: dict) -> dict:
    totals = json.loads(json.dumps(metrics["totals"]))
    totals["callable-ownership"].pop("elapsed-us")
    totals["place-state"].pop("elapsed-us")
    return totals


def verify_metrics(name: str, metrics: dict) -> None:
    if metrics.get("schema") != "chtholly-compiler-analysis-metrics-v1":
        raise AssertionError(f"{name} has an invalid analysis metrics schema")
    ownership = metrics["totals"]["callable-ownership"]
    place = metrics["totals"]["place-state"]
    if ownership["region-widenings"] != 0 or \
            ownership["postcondition-widenings"] != 0:
        raise AssertionError(f"{name} unexpectedly widened: {ownership}")
    ownership_work = (ownership["provenance-work-items"] +
                      ownership["postcondition-work-items"])
    graph_size = ownership["cfg-instructions"] + ownership["cfg-edges"] + 1
    if ownership_work > 32 * graph_size:
        raise AssertionError(
            f"{name} ownership worklist exceeded structural gate: "
            f"{ownership_work} > {32 * graph_size}")
    if ownership_work * 100 > BASELINE_SCAN_EQUIVALENT[name] * 40:
        raise AssertionError(
            f"{name} did not beat 40% of scan baseline: {ownership_work}")
    place_work = place["loan-flow-work-items"] + place["liveness-work-items"]
    if place_work > 32 * (place["place-work-items"] + 1):
        raise AssertionError(
            f"{name} PlaceState worklist exceeded structural gate: {place_work}")
    if place["loan-region-widenings"] > place["place-work-items"]:
        raise AssertionError(f"{name} widened too many unique loan regions")
    if ownership["scc-function-evaluations"] > ownership["functions"]:
        raise AssertionError(f"{name} re-evaluated unchanged SCC functions")


def check(compiler: str, target: pathlib.Path, workspace: bool,
          metrics_path: pathlib.Path) -> dict:
    command = [compiler, "check",
               "--workspace" if workspace else "--project", str(target),
               "--dump-analysis-metrics", str(metrics_path)]
    invoke(command)
    return json.loads(metrics_path.read_text(encoding="utf-8"))


def build(compiler: str, target: pathlib.Path,
          metrics_path: pathlib.Path, out_dir: pathlib.Path) -> dict:
    invoke([compiler, "build", "--workspace", str(target),
            "--out-dir", str(out_dir),
            "--dump-analysis-metrics", str(metrics_path)])
    return json.loads(metrics_path.read_text(encoding="utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="chtholly-ownership-perf-") as raw:
        root = pathlib.Path(raw)
        cases = [
            ("smoke", args.source_dir / "tests" / "fixtures" /
             "chtholly-1-9-smoke", False),
            ("telemetry", args.source_dir / "examples" /
             "telemetry-pipeline", True),
        ]
        for name, source, workspace in cases:
            target = root / name
            shutil.copytree(source, target,
                            ignore=shutil.ignore_patterns(
                                ".chtholly", "chtholly.lock"))
            first = check(args.chthollyc, target, workspace,
                          root / f"{name}-first.json")
            verify_metrics(name, first)
            cache = target / ".chtholly"
            if cache.exists():
                shutil.rmtree(cache)
            second = check(args.chthollyc, target, workspace,
                           root / f"{name}-second.json")
            verify_metrics(name, second)
            if deterministic_totals(first) != deterministic_totals(second):
                raise AssertionError(f"{name} analysis counters are not deterministic")

        telemetry = root / "telemetry"
        build(args.chthollyc, telemetry, root / "telemetry-build-first.json",
              root / "native")
        warm = build(args.chthollyc, telemetry,
                     root / "telemetry-build-second.json", root / "native")
        reused = [unit for unit in warm["units"] if unit["reused"]]
        if not reused:
            raise AssertionError("warm telemetry build reported no reused units")
        for unit in reused:
            ownership = unit["callable-ownership"]
            place = unit["place-state"]
            if any(value != 0 for value in ownership.values()) or \
                    any(value != 0 for value in place.values()):
                raise AssertionError(
                    f"reused unit performed analysis work: {unit['unit']}")

        invoke([args.chthollyc, "run", "--workspace",
                str(telemetry)])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
