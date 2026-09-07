#!/usr/bin/env python3
"""Create and verify Chtholly release supply-chain evidence.

The command deliberately has no network access.  CI resolves remote inputs
(actions, vcpkg, and LLVM) and this tool records the immutable identities that
were actually used.  Keeping resolution outside this script makes local
Windows runs cheap while making release evidence reproducible.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import zipfile
from typing import Any


SCHEMA = "chtholly-supply-chain-inputs-v1"
SBOM_SCHEMA = "chtholly-sbom-v1"
PROVENANCE_SCHEMA = "chtholly-provenance-v1"
ACTION_RE = re.compile(r"(?:^|\s)uses:\s*([^\s#]+)")
SHA_RE = re.compile(r"^[0-9a-f]{40}$")


def canonical(value: Any) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True,
                       separators=(",", ":")) + "\n").encode("utf-8")


def digest_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def digest_file(path: pathlib.Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(block)
    return hasher.hexdigest()


def digest_tree(root: pathlib.Path) -> tuple[str, int]:
    """Hash a stable path/size manifest without reading a large toolchain tree."""
    hasher = hashlib.sha256()
    count = 0
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        relative = path.relative_to(root).as_posix()
        hasher.update(relative.encode("utf-8"))
        hasher.update(b"\0")
        hasher.update(str(path.stat().st_size).encode("ascii"))
        hasher.update(b"\0")
        count += 1
    return hasher.hexdigest(), count


def read_json(path: pathlib.Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"expected JSON object: {path}")
    return value


def write_json(path: pathlib.Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(canonical(value))


def source_commit(root: pathlib.Path) -> str:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=root, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    commit = result.stdout.strip().lower()
    if result.returncode != 0 or not SHA_RE.fullmatch(commit):
        raise ValueError("source commit must be a full 40-hex git revision")
    return commit


def action_inputs(root: pathlib.Path, lock: dict[str, Any]) -> list[dict[str, str]]:
    locked = lock.get("github_actions", {})
    found: dict[str, str] = {}
    for workflow in sorted((root / ".github" / "workflows").glob("*.y*ml")):
        for line in workflow.read_text(encoding="utf-8").splitlines():
            match = ACTION_RE.search(line)
            if not match:
                continue
            use = match.group(1)
            if "@" not in use:
                raise ValueError(f"unversioned action in {workflow}: {use}")
            repository, ref = use.rsplit("@", 1)
            if not SHA_RE.fullmatch(ref.lower()):
                raise ValueError(
                    f"action is not pinned to a commit SHA in {workflow}: {use}")
            if repository in found and found[repository] != ref.lower():
                raise ValueError(f"action has multiple pinned SHAs: {repository}")
            found[repository] = ref.lower()
    result = []
    for repository, ref in sorted(found.items()):
        expected = str(locked.get(repository, "")).lower()
        if expected != ref:
            raise ValueError(
                f"action lock mismatch for {repository}: {ref} != {expected}")
        result.append({"repository": repository, "commit": ref})
    return result


def git_head(root: pathlib.Path) -> str:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=root, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    value = result.stdout.strip().lower()
    if result.returncode != 0 or not SHA_RE.fullmatch(value):
        raise ValueError(f"not a git checkout: {root}")
    return value


def collect_inputs(args: argparse.Namespace) -> int:
    root = args.source_dir.resolve()
    lock_path = root / "support" / "supply-chain-lock.json"
    lock = read_json(lock_path)
    commit = source_commit(root)
    actions = action_inputs(root, lock)
    vcpkg_root = args.vcpkg_root.resolve() if args.vcpkg_root else None
    vcpkg_commit = str(lock.get("vcpkg", {}).get("commit", "")).lower()
    if not SHA_RE.fullmatch(vcpkg_commit):
        raise ValueError("vcpkg lock must contain a full commit SHA")
    if vcpkg_root:
        actual = git_head(vcpkg_root)
        if actual != vcpkg_commit:
            raise ValueError(f"vcpkg checkout mismatch: {actual} != {vcpkg_commit}")
    llvm = lock.get("llvm", {})
    llvm_commit = str(llvm.get("source_commit", "")).lower()
    if not SHA_RE.fullmatch(llvm_commit):
        raise ValueError("LLVM lock must contain a source commit SHA")
    llvm_root = args.llvm_root.resolve() if args.llvm_root else None
    llvm_digest = ""
    llvm_count = 0
    if llvm_root:
        llvm_digest, llvm_count = digest_tree(llvm_root)
    archive_digest = ""
    if args.llvm_archive and args.llvm_archive.resolve().is_file():
        archive_digest = digest_file(args.llvm_archive.resolve())
        expected_archive = str(llvm.get("archive_sha256", "")).lower()
        if expected_archive and archive_digest != expected_archive:
            raise ValueError(
                f"LLVM archive digest mismatch: {archive_digest} != {expected_archive}")
    tools: dict[str, str] = {}
    manifest = read_json(root / "vcpkg.json")
    dependencies = []
    for dependency in manifest.get("dependencies", []):
        dependencies.append(dependency if isinstance(dependency, str)
                            else dependency.get("name", ""))
    for name, command in (("python", [sys.executable, "--version"]),
                          ("cmake", ["cmake", "--version"]),
                          ("ninja", ["ninja", "--version"])):
        try:
            result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, check=False)
            tools[name] = result.stdout.splitlines()[0].strip()
        except OSError:
            tools[name] = "unavailable"
    value: dict[str, Any] = {
        "schema": SCHEMA,
        "source_commit": commit,
        "lock_sha256": digest_file(lock_path),
        "github_actions": actions,
        "vcpkg": {"commit": vcpkg_commit,
                  "manifest_sha256": digest_file(root / "vcpkg.json"),
                  "configuration_sha256": digest_file(root / "vcpkg-configuration.json"),
                  "dependencies": sorted(item for item in dependencies if item)},
        "llvm": {"version": llvm.get("version", ""),
                 "release_tag": llvm.get("release_tag", ""),
                 "source_commit": llvm_commit,
                 "root_sha256": llvm_digest,
                 "root_digest_mode": "path-size-manifest",
                 "root_file_count": llvm_count,
                 "archive_sha256": archive_digest},
        "tools": tools,
    }
    value["inputs_sha256"] = digest_bytes(canonical(value))
    write_json(args.output, value)
    return 0


def zip_members(path: pathlib.Path) -> list[dict[str, Any]]:
    with zipfile.ZipFile(path) as archive:
        members = []
        for info in sorted(archive.infolist(), key=lambda item: item.filename):
            if info.is_dir():
                continue
            data = archive.read(info)
            members.append({"path": info.filename, "size": len(data),
                            "sha256": digest_bytes(data)})
        return members


def evidence_ref(path: pathlib.Path, root: pathlib.Path) -> dict[str, Any]:
    return {"path": path.relative_to(root).as_posix(),
            "sha256": digest_file(path), "size": path.stat().st_size}


def make_sbom(args: argparse.Namespace) -> int:
    inputs = read_json(args.inputs)
    if inputs.get("source_commit") != args.source_commit:
        raise ValueError("SBOM source commit does not match input evidence")
    package = args.package.resolve()
    package_digest = digest_file(package)
    members = zip_members(package)
    spdx_files = [
        {"SPDXID": f"SPDXRef-File-{index}",
         "fileName": member["path"],
         "checksums": [{"algorithm": "SHA256", "checksumValue": member["sha256"]}],
         "licenseConcluded": "NOASSERTION"}
        for index, member in enumerate(members, start=1)
    ]
    components: list[dict[str, Any]] = [
        {"name": "chtholly-source", "version": args.source_commit,
         "type": "source", "source_commit": args.source_commit},
        {"name": package.name, "version": args.release_version,
         "type": "package", "sha256": package_digest, "files": members},
        {"name": "vcpkg", "version": inputs["vcpkg"]["commit"],
         "type": "build-input", "commit": inputs["vcpkg"]["commit"]},
        {"name": "LLVM", "version": inputs["llvm"]["version"],
         "type": "build-input", "source_commit": inputs["llvm"]["source_commit"],
         "commit": inputs["llvm"]["source_commit"],
         "sha256": inputs["llvm"].get("archive_sha256") or
                   inputs["llvm"].get("root_sha256", "")},
    ]
    components.extend(
        {"name": dependency, "version": inputs["vcpkg"]["commit"],
         "type": "vcpkg-port", "registry_commit": inputs["vcpkg"]["commit"]}
        for dependency in inputs["vcpkg"].get("dependencies", [])
    )
    sbom = {
        "schema": SBOM_SCHEMA,
        "spdxVersion": "SPDX-2.3",
        "SPDXID": "SPDXRef-DOCUMENT",
        "dataLicense": "CC0-1.0",
        "documentNamespace":
            f"https://chtholly.dev/sbom/{package_digest}",
        "creationInfo": {
            "createdBy": ["Tool: chtholly-supply-chain-evidence"],
            "licenseListVersion": "3.23",
        },
        "name": package.name,
        "source_commit": args.source_commit,
        "package_sha256": package_digest,
        "packages": [{
            "SPDXID": "SPDXRef-Package-Chtholly",
            "name": package.name,
            "versionInfo": args.release_version,
            "downloadLocation": "NOASSERTION",
            "filesAnalyzed": True,
            "checksums": [{"algorithm": "SHA256", "checksumValue": package_digest}],
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": "NOASSERTION",
            "files": spdx_files,
        }],
        "components": components,
    }
    sbom["sbom_sha256"] = digest_bytes(canonical(sbom))
    write_json(args.output, sbom)
    return 0


def make_provenance(args: argparse.Namespace) -> int:
    inputs = read_json(args.inputs)
    sbom = read_json(args.sbom)
    if inputs.get("source_commit") != args.source_commit:
        raise ValueError("provenance source commit does not match input evidence")
    if sbom.get("source_commit") != args.source_commit:
        raise ValueError("provenance source commit does not match SBOM")
    package = args.package.resolve()
    evidence_root = args.evidence_root.resolve()
    refs = []
    for raw in args.evidence:
        path = pathlib.Path(raw).resolve()
        if not path.is_file():
            raise ValueError(f"evidence file does not exist: {path}")
        refs.append(evidence_ref(path, evidence_root))
        try:
            evidence_value = read_json(path)
        except (OSError, json.JSONDecodeError, ValueError) as error:
            raise ValueError(f"invalid evidence JSON: {path}: {error}") from error
        evidence_commit = evidence_value.get("source_commit")
        if evidence_commit and evidence_commit != args.source_commit:
            raise ValueError(f"evidence source commit mismatch: {path}")
    package_digest = digest_file(package)
    if package_digest != sbom.get("package_sha256"):
        raise ValueError("SBOM package digest does not match package")
    value: dict[str, Any] = {
        "schema": PROVENANCE_SCHEMA,
        "source_commit": args.source_commit,
        "release_version": args.release_version,
        "host": args.host,
        "target": args.target,
        "package": {"path": package.name, "sha256": package_digest},
        "sbom": {"path": pathlib.Path(args.sbom).name,
                 "sha256": digest_file(args.sbom)},
        "inputs": {"path": pathlib.Path(args.inputs).name,
                   "sha256": digest_file(args.inputs),
                   "identity": inputs.get("inputs_sha256", "")},
        "evidence": refs,
    }
    value["provenance_sha256"] = digest_bytes(canonical(value))
    write_json(args.output, value)
    return 0


def parser() -> argparse.ArgumentParser:
    command = argparse.ArgumentParser()
    sub = command.add_subparsers(dest="command", required=True)
    collect = sub.add_parser("inputs")
    collect.add_argument("--source-dir", type=pathlib.Path, required=True)
    collect.add_argument("--vcpkg-root", type=pathlib.Path)
    collect.add_argument("--llvm-root", type=pathlib.Path)
    collect.add_argument("--llvm-archive", type=pathlib.Path)
    collect.add_argument("--output", type=pathlib.Path, required=True)
    collect.set_defaults(function=collect_inputs)
    sbom = sub.add_parser("sbom")
    sbom.add_argument("--inputs", type=pathlib.Path, required=True)
    sbom.add_argument("--package", type=pathlib.Path, required=True)
    sbom.add_argument("--source-commit", required=True)
    sbom.add_argument("--release-version", required=True)
    sbom.add_argument("--output", type=pathlib.Path, required=True)
    sbom.set_defaults(function=make_sbom)
    provenance = sub.add_parser("provenance")
    provenance.add_argument("--inputs", type=pathlib.Path, required=True)
    provenance.add_argument("--sbom", type=pathlib.Path, required=True)
    provenance.add_argument("--package", type=pathlib.Path, required=True)
    provenance.add_argument("--source-commit", required=True)
    provenance.add_argument("--release-version", required=True)
    provenance.add_argument("--host", required=True)
    provenance.add_argument("--target", required=True)
    provenance.add_argument("--evidence-root", type=pathlib.Path, required=True)
    provenance.add_argument("--evidence", nargs="+", required=True)
    provenance.add_argument("--output", type=pathlib.Path, required=True)
    provenance.set_defaults(function=make_provenance)
    return command


def main() -> int:
    args = parser().parse_args()
    return int(args.function(args))


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError, zipfile.BadZipFile) as error:
        print(f"supply-chain evidence failed: {error}", file=sys.stderr)
        raise SystemExit(1)
