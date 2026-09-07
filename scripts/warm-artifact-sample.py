#!/usr/bin/env python3

import argparse
import json
import os
import shutil
import statistics
import subprocess
import tempfile
import time
from pathlib import Path


SCHEMA = "chtholly-compiler-warm-artifact-sample-v1"
WORKSPACES = (
    ("multi-module", Path("tests/fixtures/chtholly-warm/multi-module"), 1, 1),
    ("package-diamond", Path("tests/fixtures/chtholly-warm/package-diamond"), 4, 2),
    ("shared-component-fanout",
     Path("tests/fixtures/chtholly-warm/shared-component-fanout"), 7, 4),
)


def run_build(chthollyc: Path, workspace: Path, jobs: int, label: str,
              metrics: Path | None = None) -> tuple[int, str, float]:
    command = [
        str(chthollyc), "build", "--workspace", str(workspace),
        "--package", "app",
        "--cache-dir", str(workspace / "cache"),
        "--out-dir", str(workspace / f"out-{label}"),
        "--jobs", str(jobs),
    ]
    if metrics is not None:
        command += ["--dump-artifact-load-metrics", str(metrics)]
    started = time.perf_counter_ns()
    result = subprocess.run(command, text=True, capture_output=True)
    elapsed = time.perf_counter_ns() - started
    return result.returncode, result.stdout + result.stderr, elapsed


def mutate_root(workspace: Path) -> None:
    source = workspace / "app" / "src" / "main.cns"
    text = source.read_text(encoding="utf-8")
    if "42" not in text:
        raise RuntimeError(f"sampling mutation marker is missing in {source}")
    source.write_text(text.replace("42", "43", 1), encoding="utf-8")


def require_success(code: int, output: str, phase: str) -> None:
    if code != 0:
        raise RuntimeError(f"{phase} failed with exit code {code}:\n{output}")


def require_incremental_output(workspace: Path) -> None:
    executable = workspace / "out-incremental" / (
        "app.exe" if os.name == "nt" else "app")
    if not executable.is_file():
        raise RuntimeError("warm incremental build did not emit app")
    result = subprocess.run([str(executable)], capture_output=True)
    if result.returncode != 43:
        raise RuntimeError(
            "warm incremental output did not observe the root implementation "
            f"change: exit={result.returncode}")


def observation(chthollyc: Path, fixture: Path, jobs: int,
                temporary: Path, repetition: int) -> dict:
    workspace = temporary / f"jobs-{jobs}-repetition-{repetition}"
    shutil.copytree(fixture, workspace)
    code, output, _ = run_build(chthollyc, workspace, jobs, "cold")
    require_success(code, output, "cold build")
    code, output, _ = run_build(chthollyc, workspace, jobs, "warm")
    require_success(code, output, "warm build")
    mutate_root(workspace)
    metrics_path = workspace / "artifact-load.json"
    code, output, elapsed = run_build(
        chthollyc, workspace, jobs, "incremental", metrics_path)
    require_success(code, output, "warm incremental build")
    require_incremental_output(workspace)
    if not metrics_path.is_file():
        raise RuntimeError("warm incremental build did not emit metrics")
    metrics = json.loads(metrics_path.read_text(encoding="utf-8"))
    if metrics.get("schema") != "chtholly-compiler-artifact-load-metrics-v1":
        raise RuntimeError("warm incremental build emitted an unknown schema")
    scheduling = metrics["package-scheduling"]
    if scheduling["completed"] != scheduling["package-count"]:
        raise RuntimeError("warm incremental package scheduling was incomplete")
    artifact_io = metrics["artifact-io"]
    if artifact_io["metadata-probes"] != 0:
        raise RuntimeError("warm artifact loading performed metadata probes")
    if artifact_io["read-attempts"] != sum(
            artifact_io[name] for name in ("found", "missing", "error")):
        raise RuntimeError("warm artifact I/O outcome counts are inconsistent")
    return {"cli-wall-nanoseconds": elapsed, "metrics": metrics}


def median(values: list[int]) -> int:
    return int(statistics.median(values))


def summarize(observations: list[dict]) -> dict:
    family_total = lambda value, field: sum(
        family[field] for family in value["metrics"]["families"].values())
    paths = {
        "cli-wall-nanoseconds": lambda value: value["cli-wall-nanoseconds"],
        "artifact-load-span-nanoseconds": lambda value: value["metrics"]["executor"]["artifact-load-span-nanoseconds"],
        "worker-busy-nanoseconds": lambda value: value["metrics"]["executor"]["worker-busy-nanoseconds"],
        "dfs-nanoseconds": lambda value: value["metrics"]["specialization-closure"]["dfs-nanoseconds"],
        "available-parallelism-milli": lambda value: value["metrics"]["specialization-closure"]["available-parallelism-milli"],
        "package-wall-nanoseconds": lambda value: value["metrics"]["package-scheduling"]["wall-nanoseconds"],
        "package-execution-nanoseconds": lambda value: value["metrics"]["package-scheduling"]["execution-nanoseconds"],
        "package-critical-path-nanoseconds": lambda value: value["metrics"]["package-scheduling"]["critical-path-nanoseconds"],
        "io-nanoseconds": lambda value: value["metrics"]["artifact-io"]["read-nanoseconds"],
        "artifact-read-attempts": lambda value: value["metrics"]["artifact-io"]["read-attempts"],
        "artifact-read-bytes": lambda value: value["metrics"]["artifact-io"]["bytes"],
        "component-requests": lambda value: value["metrics"]["specialization-component-reuse"]["requests"],
        "component-unique": lambda value: value["metrics"]["specialization-component-reuse"]["unique-components"],
        "component-duplicate-requests": lambda value: value["metrics"]["specialization-component-reuse"]["duplicate-requests"],
        "component-duplicate-bytes": lambda value: value["metrics"]["specialization-component-reuse"]["duplicate-bytes"],
        "component-cache-hits": lambda value: value["metrics"]["specialization-component-reuse"]["cache-hits"],
        "component-cache-misses": lambda value: value["metrics"]["specialization-component-reuse"]["cache-misses"],
        "component-coalesced-waits": lambda value: value["metrics"]["specialization-component-reuse"]["coalesced-waits"],
        "component-duplicate-disk-reads": lambda value: value["metrics"]["specialization-component-reuse"]["duplicate-disk-reads"],
        "decode-nanoseconds": lambda value: value["metrics"]["specialization-closure"]["decode-nanoseconds"] + value["metrics"]["specialization-closure"]["verify-nanoseconds"],
        "queue-nanoseconds": lambda value: value["metrics"]["executor"]["backpressure-wait-nanoseconds"] + family_total(value, "queue-wait-nanoseconds"),
        "consumer-wait-nanoseconds": lambda value: family_total(value, "consumer-wait-nanoseconds"),
    }
    result = {name: median([read(value) for value in observations])
              for name, read in paths.items()}
    result["raw-cli-wall-nanoseconds"] = [
        value["cli-wall-nanoseconds"] for value in observations]
    representative_observation = min(
        observations,
        key=lambda value: abs(value["cli-wall-nanoseconds"] -
                              result["cli-wall-nanoseconds"]))
    representative = representative_observation["metrics"]
    result["configuration"] = representative["configuration"]
    result["executor"] = representative["executor"]
    result["artifact-io"] = representative["artifact-io"]
    result["specialization-component-reuse"] = representative["specialization-component-reuse"]
    result["families"] = representative["families"]
    result["specialization-closure"] = representative["specialization-closure"]
    result["package-scheduling"] = representative["package-scheduling"]
    return result


def classify(workspace: dict) -> dict:
    points = workspace["points"]
    serial = points["1"]
    best_span = min(
        (point["artifact-load-span-nanoseconds"] for point in points.values()
         if point["artifact-load-span-nanoseconds"] > 0), default=0)
    reference = points[max(points, key=int)]
    span = reference["artifact-load-span-nanoseconds"]
    dfs = reference["dfs-nanoseconds"]
    workers = reference["configuration"]["worker-count"]
    busy_ratio_milli = (0 if span == 0 or workers == 0 else
                        reference["worker-busy-nanoseconds"] * 1000 //
                        (span * workers))
    dfs_share_milli = 0 if span == 0 else dfs * 1000 // span
    serial_span = serial["artifact-load-span-nanoseconds"]
    stalled = serial_span > 0 and best_span * 100 >= serial_span * 90
    underused = (workers > 0 and
                 reference["executor"]["active-high-water"] < workers and
                 busy_ratio_milli < 700)
    closure_gate = {
        "dfs-share-at-least-30-percent": dfs_share_milli >= 300,
        "available-parallelism-at-least-1.5x": reference["available-parallelism-milli"] >= 1500,
        "jobs-scaling-stalled": stalled,
        "workers-underused": underused,
        "dfs-share-milli": dfs_share_milli,
        "worker-busy-ratio-milli": busy_ratio_milli,
    }
    closure_gate["passed"] = all(
        closure_gate[key] for key in (
            "dfs-share-at-least-30-percent",
            "available-parallelism-at-least-1.5x",
            "jobs-scaling-stalled", "workers-underused"))

    io_time = reference["io-nanoseconds"]
    decode_time = reference["decode-nanoseconds"]
    queue_time = reference["queue-nanoseconds"]
    consumer_wait = reference["consumer-wait-nanoseconds"]
    package_serial = max(
        0, reference["package-wall-nanoseconds"] -
        reference["package-critical-path-nanoseconds"])
    candidates = {
        "io": io_time,
        "decode": decode_time,
        "queue": queue_time,
        "package-scheduling": package_serial,
    }
    dominant = max(candidates, key=candidates.get)
    duplicate_requests = reference["component-duplicate-requests"]
    component_requests = reference["component-requests"]
    cache_gate = {
        "duplicate-requests-at-least-8": duplicate_requests >= 8,
        "duplicate-share-at-least-20-percent": (
            component_requests > 0 and duplicate_requests * 5 >= component_requests),
        "duplicate-requests": duplicate_requests,
        "component-requests": component_requests,
    }
    cache_gate["passed"] = (
        cache_gate["duplicate-requests-at-least-8"] and
        cache_gate["duplicate-share-at-least-20-percent"])
    return {"closure-gate": closure_gate,
            "component-cache-gate": cache_gate,
            "bottleneck-nanoseconds": candidates,
            "overlapping-consumer-wait-nanoseconds": consumer_wait,
            "dominant-bottleneck": dominant}


def main() -> int:
    parser = argparse.ArgumentParser(
        description="sample real v1 CLI warm incremental artifact loading")
    parser.add_argument("--chthollyc", type=Path, required=True)
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--repetitions", type=int, default=7)
    parser.add_argument("--jobs", type=int, nargs="+", default=[1, 2, 4, 8])
    parser.add_argument("--temp", type=Path)
    args = parser.parse_args()
    if args.repetitions <= 0 or any(job <= 0 for job in args.jobs):
        parser.error("repetitions and jobs must be positive")
    chthollyc = args.chthollyc.resolve()
    source_dir = args.source_dir.resolve()
    parent = args.temp.resolve() if args.temp else None
    with tempfile.TemporaryDirectory(prefix="chtholly-compiler-warm-", dir=parent) as raw:
        temporary = Path(raw)
        workspaces = []
        for name, relative, package_count, maximum_frontier in WORKSPACES:
            fixture = source_dir / relative
            workspace_temporary = temporary / name
            workspace_temporary.mkdir()
            points = {}
            for jobs in sorted(set(args.jobs)):
                values = [observation(chthollyc, fixture, jobs,
                                      workspace_temporary, repetition)
                          for repetition in range(args.repetitions)]
                points[str(jobs)] = summarize(values)
                scheduling = points[str(jobs)]["package-scheduling"]
                expected_workers = min(jobs, maximum_frontier)
                if (scheduling["package-count"] != package_count or
                        scheduling["worker-count"] != expected_workers):
                    raise RuntimeError(
                        f"{name}: unexpected package scheduling shape at "
                        f"jobs={jobs}: {scheduling}")
            workspace = {"name": name, "fixture": relative.as_posix(),
                         "points": points}
            workspace.update(classify(workspace))
            workspaces.append(workspace)
    gate_passed = len(workspaces) >= 2 and all(
        item["closure-gate"]["passed"] for item in workspaces)
    dominant_totals: dict[str, int] = {}
    for workspace in workspaces:
        for name, value in workspace["bottleneck-nanoseconds"].items():
            dominant_totals[name] = dominant_totals.get(name, 0) + value
    report = {
        "schema": SCHEMA,
        "cache-mode": "cold-then-warm-then-root-implementation-change",
        "repetitions": args.repetitions,
        "workspaces": workspaces,
        "closure-graph-gate": "pass" if gate_passed else "defer",
        "recommended-direction": (
            "nonblocking-closure-graph" if gate_passed else
            max(dominant_totals, key=dominant_totals.get)),
    }
    encoded = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
