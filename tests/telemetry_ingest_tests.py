#!/usr/bin/env python3
"""Exercise telemetry file and localhost TCP ingestion through Component ABI."""

from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import socket
import struct
import subprocess
import tempfile
import re
import time


FRAME = struct.Struct("<4sHHIQiiQ")


def frame(timestamp: int, sensor: int, value: int, sequence: int) -> bytes:
    return FRAME.pack(b"CHTM", 1, 0, 24, timestamp, sensor, value, sequence)


def run(command: list[str], expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, encoding="utf-8",
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            check=False)
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}: {command}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def available_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def semantic_report(summary: dict[str, object]) -> tuple[object, ...]:
    return tuple(summary[key] for key in (
        "frames", "batches", "checksum", "value_sum", "sequence_gaps",
        "input_bytes",
    ))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", type=pathlib.Path, required=True)
    parser.add_argument("--host", type=pathlib.Path, required=True)
    parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--deployment-tool", type=pathlib.Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="chtholly-telemetry-ingest-") as raw:
        root = pathlib.Path(raw)
        source_workspace = args.source_dir / "examples" / "telemetry-pipeline"
        workspace = root / "telemetry-pipeline"
        shutil.copytree(source_workspace, workspace)
        project = workspace / "telemetry-component"
        run([str(args.chthollyc), "check", "--workspace", str(workspace)])
        run([str(args.chthollyc), "run", "--workspace", str(workspace)])
        component_dir = root / "component"
        run([str(args.chthollyc), "build", "--project", str(project),
             "--out-dir", str(component_dir)])
        suffix = ".dll" if "windows" in args.target else ".so"
        libraries = list(component_dir.glob(f"*{suffix}"))
        if len(libraries) != 1:
            raise AssertionError(f"expected one component library: {libraries}")
        library = libraries[0].resolve()
        contract = pathlib.Path(str(library) + ".chcomponent")
        if not contract.is_file():
            raise AssertionError("component contract was not emitted")
        contract_text = contract.read_text(encoding="utf-8")
        identity = re.search(r"^identity\t(.+)$", contract_text, re.MULTILINE)
        digest = re.search(r"^contract-digest\t([0-9a-fA-F]{64})$",
                           contract_text, re.MULTILINE)
        if identity is None or digest is None:
            raise AssertionError("component contract lacks deployment facts")
        deployment = root / "telemetry-component.toml"
        library_name = f"telemetry-component{suffix}"
        contract_name = f"{library_name}.chcomponent"
        deployment.write_text(
            "[component]\n"
            f"identity = \"{identity.group(1)}\"\n"
            "version = \"0.1.0\"\n"
            f"target = \"{args.target}\"\n"
            "runtime = \"v1\"\n"
            f"library = \"component/{library_name}\"\n"
            f"contract = \"component/{contract_name}\"\n"
            f"contract_digest = \"{digest.group(1).lower()}\"\n",
            encoding="utf-8")
        deployment_text = deployment.read_text(encoding="utf-8")
        if digest.group(1).lower() not in deployment_text:
            raise AssertionError("deployment manifest did not retain digest")

        # Deliberately leave one sequence gap while retaining monotonic order.
        frames = b"".join(
            frame(1_000 + index, index % 8, index - 50,
                  index + 1 if index < 10 else index + 2)
            for index in range(130))
        input_file = root / "input.bin"
        report_file = root / "report.json"
        input_file.write_bytes(frames)
        result = run([
            str(args.host), "--deployment", str(deployment),
            "--file", str(input_file.resolve()), "--output",
            str(report_file.resolve()),
        ])
        summary = json.loads(result.stdout)
        if summary["deployment_version"] != "0.1.0":
            raise AssertionError(f"deployment version was not consumed: {summary}")
        if summary["frames"] != 130 or summary["batches"] != 2:
            raise AssertionError(f"unexpected batch summary: {summary}")
        if summary["sequence_gaps"] != 1 or summary["value_sum"] != sum(
                index - 50 for index in range(130)):
            raise AssertionError(f"unexpected sequence/value summary: {summary}")
        if json.loads(report_file.read_text(encoding="utf-8"))["last_timestamp"] != 1129:
            raise AssertionError("file report did not retain the last timestamp")
        if summary["input_bytes"] != len(frames) or summary["elapsed_ms"] < 0:
            raise AssertionError(f"missing ingest metrics: {summary}")

        # The same deployment and input can be driven by a strict key=value
        # config, exercising the configuration path without changing results.
        config_report = root / "config-report.json"
        config = root / "telemetry.conf"
        config.write_text(
            f"file = {input_file.resolve()}\n"
            f"output = {config_report.resolve()}\n",
            encoding="utf-8")
        configured = run([
            str(args.host), "--deployment", str(deployment), "--config",
            str(config.resolve()),
        ])
        configured_summary = json.loads(configured.stdout)
        if configured_summary["frames"] != summary["frames"] or \
                json.loads(config_report.read_text(encoding="utf-8"))["checksum"] != \
                json.loads(report_file.read_text(encoding="utf-8"))["checksum"]:
            raise AssertionError("config-driven ingest changed the result")
        invalid_config = root / "invalid.conf"
        invalid_config.write_text("unknown = value\n", encoding="utf-8")
        rejected_config = run([
            str(args.host), "--deployment", str(deployment), "--config",
            str(invalid_config.resolve()),
        ], 1)
        if "config" not in rejected_config.stderr.lower():
            raise AssertionError("invalid config was not diagnosed")

        expected_semantic = semantic_report(summary)
        for _ in range(8):
            repeated = run([
                str(args.host), "--deployment", str(deployment), "--file",
                str(input_file.resolve()), "--output",
                str(report_file.resolve()),
            ])
            repeated_summary = json.loads(repeated.stdout)
            if semantic_report(repeated_summary) != expected_semantic:
                raise AssertionError("repeated ingest lost frames")

        if args.deployment_tool:
            deployment_root = root / "deployment-tree"
            run([str(args.deployment_tool), "install", deployment_root, deployment])
            run([str(args.deployment_tool), "activate", deployment_root,
                 "0.1.0-" + digest.group(1).lower()])
            managed_report = root / "managed-report.json"
            managed = run([str(args.host), "--deployment-root", deployment_root,
                           "--file", input_file, "--output", managed_report])
            if json.loads(managed.stdout)["deployment_version"] != "0.1.0":
                raise AssertionError("deployment-root did not load active generation")

            upgraded_deployment = root / "telemetry-component-v011.toml"
            upgraded_deployment.write_text(
                deployment_text.replace('version = "0.1.0"',
                                        'version = "0.1.1"'),
                encoding="utf-8")
            run([str(args.deployment_tool), "install", deployment_root,
                 upgraded_deployment])
            upgraded_id = "0.1.1-" + digest.group(1).lower()
            run([str(args.deployment_tool), "activate", deployment_root,
                 upgraded_id])
            upgraded_report = root / "upgraded-report.json"
            upgraded = run([
                str(args.host), "--deployment-root", str(deployment_root),
                "--file", str(input_file), "--output", str(upgraded_report),
            ])
            if json.loads(upgraded.stdout)["deployment_version"] != "0.1.1":
                raise AssertionError("upgraded generation was not activated")
            protected = run([
                str(args.deployment_tool), "remove", str(deployment_root),
                upgraded_id,
            ], 1)
            if "active" not in protected.stderr.lower():
                raise AssertionError("active generation was removable")
            run([str(args.deployment_tool), "rollback", str(deployment_root)])
            rolled_back = run([
                str(args.host), "--deployment-root", str(deployment_root),
                "--file", str(input_file),
                "--output", str(root / "rollback-report.json"),
            ])
            if json.loads(rolled_back.stdout)["deployment_version"] != "0.1.0":
                raise AssertionError("deployment rollback selected wrong generation")
            run([str(args.deployment_tool), "remove", str(deployment_root),
                 upgraded_id])

        truncated = root / "truncated.bin"
        truncated.write_bytes(frames[:-1])
        failed = run([
            str(args.host), "--deployment", str(deployment),
            "--file", str(truncated.resolve()), "--output",
            str((root / "truncated.json").resolve()),
        ], 1)
        if "truncated frame" not in failed.stderr:
            raise AssertionError(f"unexpected truncation diagnostic: {failed.stderr}")

        broken = root / "broken.chcomponent"
        broken.write_text(
            contract.read_text(encoding="utf-8").replace(
                "contract-digest\t", "contract-digest\t" + "0" * 64 + "\n#", 1),
            encoding="utf-8")
        broken_deployment = root / "broken-deployment.toml"
        broken_deployment.write_text(
            deployment.read_text(encoding="utf-8").replace(
                f"contract = \"component/{contract_name}\"",
                "contract = \"broken.chcomponent\""), encoding="utf-8")
        failed = run([
            str(args.host), "--deployment", str(broken_deployment),
            "--file", str(input_file.resolve()), "--output",
            str((root / "broken.json").resolve()),
        ], 1)
        if "contract" not in failed.stderr.lower():
            raise AssertionError(f"unexpected contract diagnostic: {failed.stderr}")

        port = available_port()
        tcp_report = root / "tcp-report.json"
        process = subprocess.Popen([
            str(args.host), "--deployment", str(deployment),
            "--listen", str(port), "--output", str(tcp_report.resolve()),
        ], text=True, encoding="utf-8", stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        connection = None
        try:
            for _ in range(100):
                if process.poll() is not None:
                    stdout, stderr = process.communicate()
                    raise AssertionError(
                        f"TCP ingest host exited before listening:\n{stdout}\n{stderr}"
                    )
                try:
                    connection = socket.create_connection(("127.0.0.1", port), 0.2)
                    break
                except OSError:
                    time.sleep(0.1)
            if connection is None:
                raise AssertionError("telemetry TCP listener did not start")
            connection.sendall(frames[:5 * FRAME.size])
            connection.shutdown(socket.SHUT_WR)
            connection.close()
            connection = None
            stdout, stderr = process.communicate(timeout=10)
            if process.returncode != 0:
                raise AssertionError(f"TCP ingest failed:\n{stdout}\n{stderr}")
            tcp_summary = json.loads(stdout)
            if tcp_summary["frames"] != 5 or tcp_summary["batches"] != 1:
                raise AssertionError(f"unexpected TCP summary: {tcp_summary}")
        finally:
            if connection is not None:
                connection.close()
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
