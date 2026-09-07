#!/usr/bin/env python3
"""Measure reproducible cold, warm, incremental, and cross-package builds.

The report is deliberately descriptive rather than a hard performance gate.
It records compiler wall time alongside the compiler-owned artifact metrics so
cache, package scheduling, and LLVM work are not conflated.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any


SCHEMA = "chtholly-build-performance-baseline-v1"
MEMORY_INTERVAL_SECONDS = 0.020
CACHE_MODE = "cold-then-warm-then-root-implementation-change"


def _load_memory_helper(source_dir: Path) -> Any:
    path = source_dir / "scripts" / "process-memory.py"
    if not path.is_file():
        raise RuntimeError(f"process memory helper is missing: {path}")
    spec = importlib.util.spec_from_file_location("chtholly_process_memory", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load process memory helper: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _sample_process(process: subprocess.Popen[str], helper: Any) -> tuple[int | None, str]:
    """Sample a compiler child every 20ms and retain its maximum RSS."""

    peak: int | None = None
    source: str | None = None
    while process.poll() is None:
        try:
            value, observed_source = helper.measure_process(process.pid)
        except (OSError, RuntimeError):
            value, observed_source = None, None
        if observed_source is not None:
            source = observed_source
        if value is not None:
            peak = value if peak is None else max(peak, value)
        time.sleep(MEMORY_INTERVAL_SECONDS)
    # A process can exit between poll() and the first sample.  Probe the host
    # process only to distinguish an unavailable API from a missed sample;
    # supported hosts still fail closed rather than publishing fabricated RSS.
    if source is None:
        try:
            _, source = helper.measure_process(os.getpid())
        except (OSError, RuntimeError):
            source = "unsupported"
    if peak is None and source != "unsupported":
        raise RuntimeError("compiler exited before a memory sample was captured")
    return peak, source


def _diagnostic_counters(records: list[dict[str, Any]]) -> dict[str, int]:
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


def _run_diagnostics(compiler: Path, workspace: Path) -> dict[str, int]:
    result = subprocess.run(
        [str(compiler), "check", "--workspace", str(workspace), "--package", "app",
         "--output-format", "jsonl"],
        text=True,
        capture_output=True,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"diagnostic check failed with exit code {result.returncode}:\n"
            f"{result.stdout}{result.stderr}"
        )
    records = [json.loads(line) for line in result.stdout.splitlines() if line]
    return _diagnostic_counters(records)


def _source_commit(source_dir: Path) -> str:
    """Return the source revision, or an explicit marker outside a checkout."""
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=source_dir, text=True,
        capture_output=True, check=False,
    )
    revision = result.stdout.strip()
    if len(revision) == 40 and all(char in "0123456789abcdef" for char in revision.lower()):
        return revision
    return "unknown"


def _target() -> str:
    if os.name == "nt":
        return "windows-x64"
    if sys.platform.startswith("linux"):
        return "linux-x64"
    return "unsupported"


def run_build(compiler: Path, workspace: Path, jobs: int, label: str,
              metrics: Path | None = None, memory_helper: Any | None = None) -> dict[str, Any]:
    command = [
        str(compiler), "build", "--workspace", str(workspace),
        "--package", "app", "--cache-dir", str(workspace / "cache"),
        "--out-dir", str(workspace / f"out-{label}"), "--jobs", str(jobs),
    ]
    if metrics is not None:
        command += ["--dump-artifact-load-metrics", str(metrics)]
    started = time.perf_counter_ns()
    # Redirect child output to files while sampling. Pipes can fill before a
    # compiler finishes, deadlocking the sampler and child on verbose builds.
    with tempfile.TemporaryFile(mode="w+b") as stdout_file, \
            tempfile.TemporaryFile(mode="w+b") as stderr_file:
        result = subprocess.Popen(command, stdout=stdout_file, stderr=stderr_file)
        peak_rss_bytes: int | None = None
        peak_rss_source = "unsupported"
        try:
            if memory_helper is not None:
                peak_rss_bytes, peak_rss_source = _sample_process(result, memory_helper)
        except Exception as error:
            if result.poll() is None:
                result.terminate()
                try:
                    result.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    result.kill()
                    result.wait()
            stdout_file.seek(0)
            stderr_file.seek(0)
            stdout = stdout_file.read().decode("utf-8", errors="replace")
            stderr = stderr_file.read().decode("utf-8", errors="replace")
            raise RuntimeError(
                f"{label} build memory sampling failed: {error}:\n"
                f"{stdout}{stderr}"
            ) from error
        result.wait()
        stdout_file.seek(0)
        stderr_file.seek(0)
        stdout = stdout_file.read().decode("utf-8", errors="replace")
        stderr = stderr_file.read().decode("utf-8", errors="replace")
    elapsed = time.perf_counter_ns() - started
    if result.returncode != 0:
        raise RuntimeError(
            f"{label} build failed with exit code {result.returncode}:\n"
            f"{stdout}{stderr}"
        )
    record: dict[str, Any] = {
        "label": label,
        "jobs": jobs,
        "wall_nanoseconds": elapsed,
        "peak_rss_bytes": peak_rss_bytes,
        "peak_rss_source": peak_rss_source,
    }
    if metrics is not None:
        if not metrics.is_file():
            raise RuntimeError(f"{label} build did not emit metrics: {metrics}")
        record["artifact_metrics"] = json.loads(metrics.read_text(encoding="utf-8"))
    return record


def mutate_root(workspace: Path) -> None:
    source = workspace / "app" / "src" / "main.cns"
    text = source.read_text(encoding="utf-8")
    if "42" not in text:
        raise RuntimeError(f"performance fixture marker is missing: {source}")
    source.write_text(text.replace("42", "43", 1), encoding="utf-8")


def summarize(records: list[dict[str, Any]]) -> dict[str, Any]:
    walls = [int(record["wall_nanoseconds"]) for record in records]
    rss_values = [record["peak_rss_bytes"] for record in records
                  if isinstance(record.get("peak_rss_bytes"), int)]
    rss_sources = {record.get("peak_rss_source", "unsupported")
                   for record in records}
    return {
        "repetitions": len(records),
        "median_wall_nanoseconds": int(statistics.median(walls)),
        "min_wall_nanoseconds": min(walls),
        "max_wall_nanoseconds": max(walls),
        "peak_rss_bytes": int(statistics.median(rss_values)) if rss_values else None,
        "peak_rss_source": rss_sources.pop() if len(rss_sources) == 1 else "mixed",
        "samples": records,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Measure Chtholly build performance")
    parser.add_argument("--chthollyc", type=Path, required=True)
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--workspace", type=Path,
                        default=Path("tests/fixtures/chtholly-warm/package-diamond"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--target", default="",
                        help="explicit target triple for release provenance")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--jobs", type=int, nargs="+", default=[1, 2, 4])
    args = parser.parse_args()
    if args.repetitions <= 0 or any(job <= 0 for job in args.jobs):
        parser.error("repetitions and jobs must be positive")

    compiler = args.chthollyc.resolve()
    source_dir = args.source_dir.resolve()
    fixture = args.workspace if args.workspace.is_absolute() else source_dir / args.workspace
    fixture = fixture.resolve()
    if not compiler.is_file() or not fixture.is_dir():
        raise RuntimeError(f"compiler or fixture is missing: {compiler}, {fixture}")

    observations: dict[str, dict[str, Any]] = {}
    memory_helper = _load_memory_helper(source_dir)
    with tempfile.TemporaryDirectory(prefix="chtholly-build-baseline-") as raw:
        root = Path(raw)
        for jobs in sorted(set(args.jobs)):
            samples: list[dict[str, Any]] = []
            for repetition in range(args.repetitions):
                workspace = root / f"jobs-{jobs}-repetition-{repetition}"
                shutil.copytree(fixture, workspace)
                cache = workspace / "cache"
                cold = run_build(compiler, workspace, jobs, "cold", memory_helper=memory_helper)
                warm = run_build(compiler, workspace, jobs, "warm", memory_helper=memory_helper)
                mutate_root(workspace)
                metrics = workspace / "incremental-metrics.json"
                incremental = run_build(compiler, workspace, jobs, "incremental", metrics,
                                        memory_helper=memory_helper)
                executable = workspace / "out-incremental" / ("app.exe" if os.name == "nt" else "app")
                if not executable.is_file():
                    raise RuntimeError(f"incremental executable is missing: {executable}")
                execution = subprocess.run([str(executable)], check=False)
                if execution.returncode != 43:
                    raise RuntimeError(
                        f"incremental build did not observe source mutation: {execution.returncode}"
                    )
                samples.append({
                    "cold": cold,
                    "warm": warm,
                    "incremental": incremental,
                    "cache_exists": cache.is_dir(),
                    "diagnostic_counters": _run_diagnostics(compiler, workspace),
                })
            observations[str(jobs)] = {
                "jobs": jobs,
                "repetitions": len(samples),
                "cold": summarize([sample["cold"] for sample in samples]),
                "warm": summarize([sample["warm"] for sample in samples]),
                "incremental": summarize([sample["incremental"] for sample in samples]),
                "cache_verified": all(sample["cache_exists"] for sample in samples),
                "diagnostic_counters": samples[0]["diagnostic_counters"],
            }

    version = subprocess.run([str(compiler), "--version"], text=True,
                             capture_output=True, check=False)
    report = {
        "schema": SCHEMA,
        "compiler": version.stdout.strip() or version.stderr.strip(),
        "source_commit": _source_commit(source_dir),
        "target": args.target or _target(),
        "cache_mode": CACHE_MODE,
        "platform": {"system": sys.platform, "machine": os.uname().machine if hasattr(os, "uname") else "unknown"},
        "workspace": fixture.as_posix(),
        "configuration": {"repetitions": args.repetitions, "jobs": sorted(set(args.jobs))},
        "observations": observations,
        "diagnostic_counters": observations[str(sorted(set(args.jobs))[0])][
            "diagnostic_counters"
        ],
        "valid": True,
    }
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"build-performance-baseline=pass schema={SCHEMA}")
    print(f"output={output.as_posix()}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        print(f"build performance baseline failed: {error}", file=sys.stderr)
        raise SystemExit(1)
