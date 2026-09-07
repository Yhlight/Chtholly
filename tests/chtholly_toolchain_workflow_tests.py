#!/usr/bin/env python3

import argparse
import pathlib
import shutil
import stat
import subprocess
import tempfile


def run(executable: str, *arguments: str, expected: int = 0):
    result = subprocess.run(
        [executable, *arguments], text=True, encoding="utf-8",
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}: "
            f"{arguments!r}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--toolchain", required=True)
    parser.add_argument("--compiler", required=True, type=pathlib.Path)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="chtholly-toolchain-workflow-") as raw:
        root = pathlib.Path(raw)
        manager = root / "manager"
        secret = root / "release.secret"
        public = root / "release.public"
        trust = root / "root.trust"

        run(args.toolchain, "key", "generate", "--secret", str(secret),
            "--public", str(public))
        run(args.toolchain, "key", "generate", "--secret", str(secret),
            "--public", str(public), expected=1)
        run(args.toolchain, "trust", "create", "-o", str(trust),
            "--version", "1", "--threshold", "1", "--key", str(public),
            "--secret-key", str(secret))
        run(args.toolchain, "trust", "init", str(trust), "--root", str(manager))

        releases = []
        for version, digit in (("0.2.0", "1"), ("0.2.1", "2")):
            commit = digit * 40
            release_id = f"{version}+{commit}"
            install_tree = root / f"tree-{version}"
            compiler_name = "chthollyc.exe" if args.compiler.suffix == ".exe" else "chthollyc"
            installed_compiler = install_tree / "bin" / compiler_name
            installed_compiler.parent.mkdir(parents=True)
            shutil.copy2(args.compiler, installed_compiler)
            installed_compiler.chmod(
                installed_compiler.stat().st_mode | stat.S_IXUSR)
            (install_tree / "release-marker.txt").write_text(
                release_id + "\n", encoding="utf-8")
            archive = root / f"{version}.zip"
            run(args.toolchain, "package", str(install_tree), "-o", str(archive),
                "--version", version, "--source-commit", commit,
                "--secret-key", str(secret))
            releases.append((release_id, archive))

        first_id, first_archive = releases[0]
        second_id, second_archive = releases[1]
        run(args.toolchain, "install", str(first_archive), "--root", str(manager))
        run(args.toolchain, "activate", first_id, "--root", str(manager))
        run(args.toolchain, "upgrade", str(second_archive), "--root", str(manager))

        listed = run(args.toolchain, "list", "--root", str(manager)).stdout
        if f"{first_id}\tinactive" not in listed or \
                f"{second_id}\tactive" not in listed:
            raise AssertionError(f"unexpected installed releases: {listed!r}")

        rolled_back = run(args.toolchain, "rollback", "--root", str(manager))
        if f"active\t{first_id}" not in rolled_back.stdout:
            raise AssertionError(f"rollback selected wrong generation: {rolled_back.stdout!r}")
        run(args.toolchain, "remove", second_id, "--root", str(manager))
        final = run(args.toolchain, "list", "--root", str(manager)).stdout
        if final.strip() != f"{first_id}\tactive":
            raise AssertionError(f"unexpected final generation set: {final!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
