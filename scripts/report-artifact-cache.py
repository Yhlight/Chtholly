#!/usr/bin/env python3
"""Produce deterministic, read-only evidence for a compiler artifact cache.

The report intentionally does not perform garbage collection.  Reachability is
computed only when ``--verify-references`` is supplied; malformed references
then fail closed while preserving the cache namespace for recovery.
"""

from __future__ import annotations

import argparse
import errno
import json
import os
import re
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


SCHEMA = "chtholly-compiler-artifact-store-report-v1"
FAMILIES = (
    "manifests",
    "objects",
    "specializations",
    "specialization-index",
    "type-specifics",
    "type-specific-index",
    "nominal-semantic-witnesses",
    "nominal-semantic-witness-index",
    "type-layouts",
    "type-layout-index",
    "other",
)
HEX_RE = re.compile(r"^[0-9a-f]{64}$")
HEX_TOKEN_RE = re.compile(r"(?<![0-9a-f])[0-9a-f]{64}(?![0-9a-f])")
REFERENCE_MAGIC = "CHNXTREF1"
LEASE_MAGIC = "CHNXTLEASE1"
INDEX_MAGICS = {
    "specialization-index": ("CHNXTSPECREF1", "component"),
    "type-specific-index": ("CHNXTYPEIDX1", "result"),
    "nominal-semantic-witness-index": ("CHNXWITIDX1", "result"),
    "type-layout-index": ("CHNXLAYIDX1", "result"),
}
CANONICAL_SUFFIXES = {
    "manifests": ".manifest",
    "specializations": ".specific",
    "specialization-index": ".ref",
    "type-specifics": ".type",
    "type-specific-index": ".ref",
    "nominal-semantic-witnesses": ".witness",
    "nominal-semantic-witness-index": ".ref",
    "type-layouts": ".layout",
    "type-layout-index": ".ref",
}


class ScanError(RuntimeError):
    pass


@dataclass(frozen=True)
class Entry:
    family: str
    relative: str
    path: Path
    size: int


def _relative(root: Path, path: Path) -> str:
    try:
        relative = path.relative_to(root)
    except ValueError as exc:
        raise ScanError("path-escape: cache entry lies outside cache root") from exc
    # POSIX spelling is part of the deterministic report contract.
    return relative.as_posix()


def _family(relative: str) -> str:
    top = relative.split("/", 1)[0]
    return top if top in FAMILIES else "other"


def _walk(root: Path) -> list[Entry]:
    """Enumerate regular files in sorted relative order, rejecting links."""

    root = root.resolve()
    if not root.is_dir():
        raise ScanError("cache-dir is not a directory")
    entries: list[Entry] = []
    pending = [root]
    while pending:
        directory = pending.pop()
        try:
            children = sorted(os.scandir(directory), key=lambda item: item.name)
        except OSError as exc:
            raise ScanError(f"cannot enumerate cache directory: {exc}") from exc
        for child in children:
            path = Path(child.path)
            if child.is_symlink():
                raise ScanError(f"symlink entry is not permitted: {_relative(root, path)}")
            relative = _relative(root, path)
            try:
                if child.is_dir(follow_symlinks=False):
                    pending.append(path)
                    continue
                if not child.is_file(follow_symlinks=False):
                    raise ScanError(f"non-regular cache entry: {relative}")
                size = child.stat(follow_symlinks=False).st_size
            except OSError as exc:
                raise ScanError(f"cannot stat cache entry {relative}: {exc}") from exc
            entries.append(Entry(_family(relative), relative, path, int(size)))
    return sorted(entries, key=lambda item: item.relative)


def _group(entries: Iterable[Entry]) -> dict[str, list[Entry]]:
    result = {family: [] for family in FAMILIES}
    for entry in entries:
        result.setdefault(entry.family, []).append(entry)
    for values in result.values():
        values.sort(key=lambda item: item.relative)
    return result


def _read(path: Path, relative: str) -> bytes:
    try:
        return path.read_bytes()
    except OSError as exc:
        raise ScanError(f"cannot read {relative}: {exc}") from exc


def _parse_fields(data: bytes, magic: str, fields: tuple[str, ...], relative: str) -> dict[str, str]:
    try:
        lines = data.decode("utf-8").splitlines()
    except UnicodeDecodeError as exc:
        raise ScanError(f"invalid-reference: {relative} is not UTF-8") from exc
    if len(lines) != len(fields) + 1 or lines[0] != magic:
        raise ScanError(f"invalid-reference: malformed {relative}")
    result: dict[str, str] = {}
    for line, field in zip(lines[1:], fields):
        prefix = field + "\t"
        if not line.startswith(prefix) or field in result:
            raise ScanError(f"invalid-reference: malformed {relative}")
        value = line[len(prefix) :]
        if not value or "\t" in value or "\r" in value or "\n" in value:
            raise ScanError(f"invalid-reference: malformed {relative}")
        result[field] = value
    return result


def _fingerprint(value: str, relative: str) -> str:
    if not HEX_RE.fullmatch(value):
        raise ScanError(f"invalid-reference: invalid fingerprint in {relative}")
    return value


def _digest_from_name(relative: str) -> str | None:
    name = relative.rsplit("/", 1)[-1]
    digest = name.split(".", 1)[0]
    return digest if HEX_RE.fullmatch(digest) else None


def _is_canonical(entry: Entry, digest: str | None = None) -> bool:
    """Match CompilerArtifactPathService's shard and suffix rules."""

    digest = digest or _digest_from_name(entry.relative)
    if not digest:
        return False
    parts = entry.relative.split("/")
    if len(parts) != 3 or parts[0] != entry.family or parts[1] != digest[:2]:
        return False
    name = parts[2]
    if entry.family == "objects":
        return name.startswith(digest + ".") and len(name) > len(digest) + 1
    suffix = CANONICAL_SUFFIXES.get(entry.family)
    return suffix is not None and name == digest + suffix


def _probe_lease(path: Path) -> bool:
    """Return True when an existing process appears to hold the lease lock."""

    try:
        handle = path.open("r+b")
    except OSError:
        return False
    try:
        if os.name == "nt":
            import msvcrt

            # msvcrt locks one byte at the current position; empty leases are
            # malformed but still treated as stale rather than crashing the
            # observational report.
            handle.seek(0)
            try:
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            except OSError as exc:
                if exc.errno in (errno.EACCES, errno.EDEADLK, errno.EAGAIN):
                    return True
                return False
            try:
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            except OSError:
                pass
            return False
        import fcntl

        try:
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            if exc.errno in (errno.EACCES, errno.EAGAIN):
                return True
            return False
        try:
            fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
        except OSError:
            pass
        return False
    finally:
        handle.close()


def _write_atomic(path: Path, report: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = json.dumps(report, indent=2, sort_keys=True, ensure_ascii=True) + "\n"
    descriptor, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def _empty_family() -> dict[str, int]:
    return {"file_count": 0, "total_bytes": 0, "reachable_bytes": 0, "unreachable_bytes": 0}


def scan(cache_dir: Path, verify_references: bool) -> tuple[dict[str, object], str | None]:
    entries: list[Entry] = []
    reachable: set[str] = set()
    try:
        entries = _walk(cache_dir)
        groups = _group(entries)
        by_digest: dict[str, list[Entry]] = {}
        canonical_by_digest: dict[str, list[Entry]] = {}
        for entry in entries:
            digest = _digest_from_name(entry.relative)
            if digest:
                by_digest.setdefault(digest, []).append(entry)
                if _is_canonical(entry, digest):
                    canonical_by_digest.setdefault(digest, []).append(entry)

        def canonical_targets(digest: str, family: str, relative: str) -> list[Entry]:
            targets = [candidate for candidate in canonical_by_digest.get(digest, [])
                       if candidate.family == family]
            if not targets:
                same_digest = [candidate for candidate in by_digest.get(digest, [])
                               if candidate.family == family]
                if same_digest:
                    raise ScanError(
                        f"invalid-reference: non-canonical {family} path for {relative}"
                    )
            return targets

        lease_status: dict[str, bool] = {}
        active_lease_count = stale_lease_count = 0
        for entry in groups.get("other", []):
            if entry.relative.startswith("leases/") and entry.relative.endswith(".lease"):
                active = _probe_lease(entry.path)
                lease_status[entry.relative] = active
                if active:
                    active_lease_count += 1
                else:
                    stale_lease_count += 1

        if verify_references:
            # Session roots are the authoritative roots of the closure.
            for entry in groups.get("other", []):
                if not entry.relative.startswith("refs/") or not entry.relative.endswith(".ref"):
                    continue
                fields = _parse_fields(_read(entry.path, entry.relative), REFERENCE_MAGIC,
                                       ("target", "root", "manifest"), entry.relative)
                digest = _fingerprint(fields["manifest"], entry.relative)
                roots = canonical_targets(digest, "manifests", entry.relative)
                if not roots:
                    raise ScanError(f"invalid-reference: {entry.relative} points to missing manifest")
                reachable.update(candidate.relative for candidate in roots)

            # A lease also retains its root manifest while it is active.  The
            # lock probe below still classifies active/stale leases separately.
            for entry in groups.get("other", []):
                if not entry.relative.startswith("leases/") or not entry.relative.endswith(".lease"):
                    continue
                # Match CompilerArtifactGCService: probe the lease lock first.
                # Unlocked crash leftovers are stale and are not decoded or
                # treated as roots, even if their bytes are malformed.
                if not lease_status.get(entry.relative, False):
                    continue
                fields = _parse_fields(_read(entry.path, entry.relative), LEASE_MAGIC,
                                       ("session", "target", "root", "manifest"), entry.relative)
                if not HEX_RE.fullmatch(fields["session"]):
                    raise ScanError(f"invalid-reference: malformed lease {entry.relative}")
                digest = _fingerprint(fields["manifest"], entry.relative)
                roots = canonical_targets(digest, "manifests", entry.relative)
                if not roots:
                    raise ScanError(f"invalid-reference: {entry.relative} points to missing manifest")
                reachable.update(candidate.relative for candidate in roots)

            # Verify typed index references and mark their target artifacts.
            for family, (magic, field) in INDEX_MAGICS.items():
                for entry in groups.get(family, []):
                    if not entry.relative.endswith(".ref"):
                        continue
                    digest_from_path = _digest_from_name(entry.relative)
                    if not digest_from_path or not _is_canonical(entry, digest_from_path):
                        raise ScanError(
                            f"invalid-reference: non-canonical {family} path {entry.relative}"
                        )
                    fields = _parse_fields(_read(entry.path, entry.relative), magic, (field,), entry.relative)
                    digest = _fingerprint(fields[field], entry.relative)
                    target_family = {
                        "specialization-index": "specializations",
                        "type-specific-index": "type-specifics",
                        "nominal-semantic-witness-index": "nominal-semantic-witnesses",
                        "type-layout-index": "type-layouts",
                    }[family]
                    targets = canonical_targets(digest, target_family, entry.relative)
                    if not targets:
                        raise ScanError(f"invalid-reference: {entry.relative} points to missing artifact")
                    # The index is live only when its request fingerprint is
                    # present in a reachable manifest/module closure.  The
                    # raw-fingerprint walk below marks it then; orphaned,
                    # otherwise valid indexes remain observable as unreachable.

            # Manifests encode fingerprints as raw 32-byte values.  Repeatedly
            # discover known digest byte strings to cover dependency and module
            # closures without duplicating the C++ decoder in this report tool.
            pending = [entry for entry in entries if entry.relative in reachable]
            visited: set[str] = set()
            while pending:
                source = pending.pop()
                if source.relative in visited:
                    continue
                visited.add(source.relative)
                data = _read(source.path, source.relative)
                for digest, candidates in canonical_by_digest.items():
                    if digest in ("0" * 64,):
                        continue
                    try:
                        marker = bytes.fromhex(digest)
                    except ValueError:
                        continue
                    if marker not in data:
                        continue
                    for candidate in candidates:
                        if candidate.relative not in reachable:
                            reachable.add(candidate.relative)
                            # Binary manifests and specialization components
                            # both carry raw fingerprints; walking every newly
                            # reached artifact preserves transitive component
                            # closures without a second format decoder.
                            pending.append(candidate)

            # Mark all referenced files themselves so index bytes contribute to
            # reachable totals.  Unreferenced objects remain visible as drift.

        families: dict[str, dict[str, int]] = {}
        for family in FAMILIES:
            values = _empty_family()
            for entry in groups.get(family, []):
                values["file_count"] += 1
                values["total_bytes"] += entry.size
                if entry.relative in reachable:
                    values["reachable_bytes"] += entry.size
                else:
                    values["unreachable_bytes"] += entry.size
            families[family] = values

        quarantine_bytes = sum(entry.size for entry in groups.get("other", [])
                               if entry.relative.startswith("trash/"))
        report: dict[str, object] = {
            "schema": SCHEMA,
            "families": families,
            "active_lease_count": active_lease_count,
            "stale_lease_count": stale_lease_count,
            "quarantine_bytes": quarantine_bytes,
            "reclaimed_bytes": 0,
            "valid": True,
        }
        return report, None
    except ScanError as exc:
        groups = _group(entries)
        families: dict[str, dict[str, int]] = {}
        for family in FAMILIES:
            values = _empty_family()
            for entry in groups.get(family, []):
                values["file_count"] += 1
                values["total_bytes"] += entry.size
                if entry.relative in reachable:
                    values["reachable_bytes"] += entry.size
                else:
                    values["unreachable_bytes"] += entry.size
            families[family] = values
        report = {
            "schema": SCHEMA,
            "families": families,
            "active_lease_count": 0,
            "stale_lease_count": 0,
            "quarantine_bytes": 0,
            "reclaimed_bytes": 0,
            "valid": False,
            "recovery_instruction": "retain this cache namespace; retry with a fresh --cache-dir after compiler processes exit",
        }
        return report, str(exc)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--verify-references", action="store_true")
    parser.add_argument("--source-commit", default="")
    parser.add_argument("--target", default="")
    args = parser.parse_args()
    try:
        cache_root = args.cache_dir.resolve()
        output_path = args.output.resolve()
        output_path.relative_to(cache_root)
    except ValueError:
        pass
    else:
        print("artifact-cache-report: output must be outside cache-dir", file=sys.stderr)
        return 1
    report, error = scan(args.cache_dir, args.verify_references)
    if args.source_commit:
        report["source_commit"] = args.source_commit
    if args.target:
        report["target"] = args.target
    try:
        _write_atomic(args.output, report)
    except OSError as exc:
        print(f"artifact-cache-report: cannot write output: {exc}", file=sys.stderr)
        return 1
    if error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
