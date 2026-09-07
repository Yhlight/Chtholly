#!/usr/bin/env python3
"""Verify ordered native lifecycle events and record actual build provenance."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import time


def sha256_file(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_metadata(build):
    cache_path = build / "CMakeCache.txt"
    cache = {}
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        match = re.match(r"([^:#/][^:=]*):[^=]*=(.*)", line)
        if match:
            cache[match[1]] = match[2]
    source = pathlib.Path(cache["CMAKE_HOME_DIRECTORY"])
    headers = sorted((build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
    compiler_info = headers[-1].read_text(encoding="utf-8")
    def compiler_value(key):
        match = re.search(r'set\(' + key + r' "([^"\n]+)"\)', compiler_info)
        if not match:
            raise ValueError("missing compiler fact: " + key)
        return match[1]
    version_header = (build / "include/chtholly/Config/Version.h").read_text()
    commit = re.search(r'CHTHOLLY_SOURCE_COMMIT "([0-9a-f]{40})"', version_header)
    if not commit:
        raise ValueError("missing source commit")
    source_hash = hashlib.sha256()
    directories = ("include", "lib", "runtime", "stdlib", "tools", "tests", "scripts", "cmake")
    files = [source / "CMakeLists.txt"]
    for directory in directories:
        files.extend(p for p in (source / directory).rglob("*") if p.is_file()
                     and ".chtholly" not in p.parts and "__pycache__" not in p.parts
                     and p.suffix in (".h", ".cpp", ".c", ".inc", ".def", ".cns", ".cfdl", ".toml", ".py", ".ps1", ".cmake", ".txt"))
    for path in sorted(files):
        source_hash.update(path.relative_to(source).as_posix().encode() + b"\0")
        source_hash.update(path.read_bytes().replace(b"\r\n", b"\n"))
    ninja = subprocess.check_output([cache["CMAKE_MAKE_PROGRAM"], "--version"], text=True).strip()
    cmake = subprocess.check_output([cache.get("CMAKE_COMMAND", "cmake"), "--version"], text=True).splitlines()[0]
    dirty = False
    if (source / ".git").exists():
        dirty = bool(subprocess.check_output(["git", "-C", str(source), "status", "--porcelain", "--untracked-files=no"], text=True).strip())
    if cache.get("CMAKE_TOOLCHAIN_FILE"):
        lock = json.loads((source / "support/supply-chain-lock.json").read_text())
        dependencies = {"vcpkg_commit": lock["vcpkg"]["commit"]}
        status = build / "vcpkg_installed/vcpkg/status"
        if status.exists(): dependencies["installed_status_sha256"] = sha256_file(status)
    else:
        versions = subprocess.check_output(["dpkg-query", "-W", "-f=${Package}=${Version}\n",
            "libsodium-dev", "libcurl4-openssl-dev", "libssl-dev", "zlib1g-dev",
            "libcpp-httplib-dev", "libsqlite3-dev"], text=True)
        dependencies = {"system_packages": versions.splitlines()}
    return {"dependencies": dependencies, "source_commit": commit[1], "source_sha256": source_hash.hexdigest(),
            "source_dirty": dirty, "compiler": compiler_value("CMAKE_CXX_COMPILER_ID"),
            "compiler_version": compiler_value("CMAKE_CXX_COMPILER_VERSION"),
            "compiler_path": compiler_value("CMAKE_CXX_COMPILER"),
            "ninja_version": ninja, "cmake_version": cmake,
            "build_directory": str(build), "remote_source": str(source),
            "build_cache_sha256": sha256_file(cache_path),
            "dependency_lock_sha256": sha256_file(source / "support/supply-chain-lock.json"),
            "dependency_mode": "vcpkg" if cache.get("CMAKE_TOOLCHAIN_FILE") else "native-system",
            "sanitizer": cache.get("CHTHOLLY_SANITIZER") or "none",
            "build_type": cache["CMAKE_BUILD_TYPE"]}


def validate_events(transcript, minimum_cycles, seed):
    events = []
    for line in transcript.splitlines():
        if line.startswith("{"):
            events.append(json.loads(line))
    if len(events) % 4 or len(events) < minimum_cycles * 4:
        raise ValueError("incomplete lifecycle transcript")
    calls = 0
    for cycle in range(len(events) // 4):
        group = events[cycle * 4: cycle * 4 + 4]
        if [x.get("event") for x in group] != ["load", "close", "joined", "unloaded"] or any(x.get("cycle") != cycle for x in group):
            raise ValueError("invalid lifecycle event order")
        if group[0].get("seed") != seed or group[1].get("status") != 0 or group[2].get("workers") != 4 or group[2].get("calls", 0) <= 0:
            raise ValueError("vacuous or failed native cycle")
        calls += group[2]["calls"]
    return len(events) // 4, calls


def main():
    p = argparse.ArgumentParser()
    for name in ("soak", "provider", "output"):
        p.add_argument("--" + name, type=pathlib.Path, required=True)
    p.add_argument("--build-directory", type=pathlib.Path)
    p.add_argument("--target", required=True)
    p.add_argument("--sanitizer")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--min-cycles", type=int, default=4)
    p.add_argument("--min-seconds", type=int, default=0)
    p.add_argument("--gate", action="store_true")
    p.add_argument("--source-commit")
    p.add_argument("--remote-source")
    args = p.parse_args()
    build = (args.build_directory or args.soak.resolve().parent.parent).resolve()
    evidence = {"schema": "chtholly-abi2-evidence-v2", "target": args.target,
                "seed": args.seed, "valid": False, "cycles": 0, "calls": 0,
                "minimum_cycles": args.min_cycles, "minimum_seconds": args.min_seconds}
    started = time.monotonic()
    transcript = ""
    try:
        if args.min_cycles <= 0 or args.min_seconds < 0:
            raise ValueError("invalid duration/cycle requirement")
        evidence.update(build_metadata(build))
        if args.source_commit and args.source_commit != evidence["source_commit"]:
            raise ValueError("source commit disagrees with built compiler")
        if args.sanitizer and args.sanitizer != evidence["sanitizer"]:
            raise ValueError("sanitizer disagrees with build configuration")
        if args.gate and (evidence["source_dirty"] or args.min_cycles < 100 or args.min_seconds < 60):
            raise ValueError("gate requires clean sources, 100 cycles and 60 seconds")
        result = subprocess.run([str(args.soak.resolve()), str(args.provider.resolve()),
                                 "--cycles", str(args.min_cycles), "--seconds", str(args.min_seconds),
                                 "--seed", str(args.seed)], capture_output=True, text=True,
                                timeout=max(120, args.min_seconds + 120))
        transcript = result.stdout + "\n" + result.stderr
        evidence["return_code"] = result.returncode
        if result.returncode != 0 or any(marker in transcript for marker in
            ("ERROR: AddressSanitizer", "WARNING: ThreadSanitizer", "runtime error:", "incompatible ASan")):
            raise ValueError("native test or sanitizer failure")
        evidence["cycles"], evidence["calls"] = validate_events(transcript, args.min_cycles, args.seed)
        evidence["valid"] = True
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        evidence["failure_class"] = str(error)
    evidence["duration_seconds"] = round(time.monotonic() - started, 3)
    evidence["transcript_sha256"] = hashlib.sha256(transcript.encode()).hexdigest()
    evidence["provider_sha256"] = sha256_file(args.provider) if args.provider.is_file() else None
    evidence["soak_sha256"] = sha256_file(args.soak) if args.soak.is_file() else None
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.with_suffix(".transcript.log").write_text(transcript, encoding="utf-8")
    args.output.write_text(json.dumps(evidence, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(evidence, sort_keys=True))
    return 0 if evidence["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
