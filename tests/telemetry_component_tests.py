#!/usr/bin/env python3
"""Build and exercise the first real telemetry Component ABI slice."""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import subprocess
import tempfile


def invoke(command: list[str], expected: int = 0) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, encoding="utf-8",
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            check=False)
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}: {command}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def contract_digest(path: pathlib.Path) -> tuple[str, str]:
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("\t")
        if separator:
            values[key] = value
    identity = values.get("identity")
    digest = values.get("contract-digest")
    if not identity or not digest or not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise AssertionError("invalid component contract identity/digest")
    return identity, digest


def write_manifest(path: pathlib.Path, library: pathlib.Path,
                   contract: pathlib.Path, version: str, identity: str,
                   digest: str, target: str) -> None:
    path.write_text(
        "[component]\n"
        f"identity = \"{identity}\"\n"
        f"version = \"{version}\"\n"
        f"target = \"{target}\"\n"
        "runtime = \"v1\"\n"
        f"library = \"{library.relative_to(path.parent).as_posix()}\"\n"
        f"contract = \"{contract.relative_to(path.parent).as_posix()}\"\n"
        f"contract_digest = \"{digest}\"\n",
        encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", type=pathlib.Path, required=True)
    parser.add_argument("--host", type=pathlib.Path, required=True)
    parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    parser.add_argument("--target", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="chtholly-telemetry-component-") as raw:
        root = pathlib.Path(raw)
        source_project = args.source_dir / "examples" / "telemetry-pipeline" / "telemetry-component"
        project = root / "telemetry-component-source"
        shutil.copytree(source_project, project)
        output = root / "component"
        invoke([str(args.chthollyc), "check", "--project", str(project)])
        invoke([str(args.chthollyc), "build", "--project", str(project),
                "--out-dir", str(output)])
        suffix = ".dll" if "windows" in args.target else ".so"
        libraries = list(output.glob(f"*{suffix}"))
        if len(libraries) != 1:
            raise AssertionError(f"expected one telemetry component: {libraries}")
        library = libraries[0].resolve()
        contract = pathlib.Path(str(library) + ".chcomponent")
        if not contract.is_file():
            raise AssertionError("telemetry component contract was not emitted")
        identity, digest = contract_digest(contract)
        manifest = root / "v010.toml"
        write_manifest(manifest, library, contract, "0.1.0", identity, digest,
                       args.target)
        result = invoke([str(args.host), "--deployment", str(manifest)])
        if '"schema":"chtholly-telemetry-component-v1"' not in result.stdout:
            raise AssertionError(f"unexpected telemetry host report: {result.stdout}")
        if '"deployment_version":"0.1.0"' not in result.stdout:
            raise AssertionError(f"deployment manifest was not consumed: {result.stdout}")

        broken = root / "broken.chcomponent"
        text = contract.read_text(encoding="utf-8")
        broken.write_text(text.replace("contract-digest\t", "contract-digest\t" + "0" * 64 + "\n#"), encoding="utf-8")
        broken_manifest = root / "broken.toml"
        write_manifest(broken_manifest, library, broken, "0.1.0", identity,
                       "0" * 64, args.target)
        rejected = subprocess.run(
            [str(args.host), "--deployment", str(broken_manifest)],
            text=True, encoding="utf-8", stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False)
        if rejected.returncode == 0:
            raise AssertionError("tampered telemetry contract was accepted")

        upgraded = root / "v011"
        shutil.copytree(project, upgraded)
        upgraded_source = upgraded / "src" / "component.cns"
        upgraded_source.write_text(
            upgraded_source.read_text(encoding="utf-8").replace(
                "return total;", "return total + 1u64;\n}\n\n"
                "pub fn version(): i32 { return 11;"),
            encoding="utf-8")
        upgraded_manifest_file = upgraded / "chtholly.toml"
        upgraded_manifest_file.write_text(
            upgraded_manifest_file.read_text(encoding="utf-8").replace(
                'exports = ["telemetry::component::checksum"]',
                'exports = ["telemetry::component::checksum", '
                '"telemetry::component::version"]'),
            encoding="utf-8")
        upgraded_output = root / "component-v011"
        invoke([str(args.chthollyc), "build", "--project", str(upgraded),
                "--out-dir", str(upgraded_output)])
        upgraded_library = next(upgraded_output.glob(f"*{suffix}"), None)
        if upgraded_library is None:
            raise AssertionError("v0.1.1 component library was not emitted")
        upgraded_library = upgraded_library.resolve()
        upgraded_contract = pathlib.Path(str(upgraded_library) + ".chcomponent")
        upgraded_identity, upgraded_digest = contract_digest(upgraded_contract)
        if upgraded_identity != identity or upgraded_digest == digest:
            raise AssertionError("component upgrade did not preserve identity/change digest")
        upgraded_manifest = root / "v011.toml"
        write_manifest(upgraded_manifest, upgraded_library, upgraded_contract,
                       "0.1.1", upgraded_identity, upgraded_digest,
                       args.target)
        old_against_new = root / "old-against-new.toml"
        write_manifest(old_against_new, upgraded_library, upgraded_contract,
                       "0.1.0", identity, digest, args.target)
        rejected = subprocess.run(
            [str(args.host), "--deployment", str(old_against_new)],
            text=True, encoding="utf-8", stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False)
        if rejected.returncode == 0:
            raise AssertionError("old deployment digest accepted new component")
        upgraded_result = invoke([str(args.host), "--deployment", str(upgraded_manifest)])
        if '"deployment_version":"0.1.1"' not in upgraded_result.stdout or \
                '"checksum":77' not in upgraded_result.stdout:
            raise AssertionError(f"v0.1.1 activation failed: {upgraded_result.stdout}")
        rollback_result = invoke([str(args.host), "--deployment", str(manifest)])
        if '"deployment_version":"0.1.0"' not in rollback_result.stdout:
            raise AssertionError(f"v0.1.0 rollback failed: {rollback_result.stdout}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
