#!/usr/bin/env python3

import argparse
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import tempfile


def run(*arguments: str, cwd: pathlib.Path | None = None) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        list(arguments), cwd=cwd, text=True, encoding="utf-8",
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False
    )
    if result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}): {arguments!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    parser.add_argument(
        "--expect-vsix", action="store_true",
        help="Require the packaged VS Code extension in the install tree.",
    )
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="chtholly-install-tree-") as raw:
        root = pathlib.Path(raw)

        suffix = ".exe" if os.name == "nt" else ""
        archive_prefix = "" if os.name == "nt" else "lib"
        archive_suffix = ".lib" if os.name == "nt" else ".a"
        def install_profile(profile: str) -> pathlib.Path:
            prefix = root / f"install-{profile}"
            run(args.cmake, "--install", str(args.build_dir), "--prefix",
                str(prefix), "--component", profile)
            return prefix

        def path_set(prefix: pathlib.Path) -> set[str]:
            return {
                path.relative_to(prefix).as_posix()
                for path in prefix.rglob("*")
                if path.is_file()
            }

        minimal = install_profile("minimal")
        full = install_profile("full")
        minimal_paths = path_set(minimal)
        full_paths = path_set(full)
        suffix_name = ".exe" if os.name == "nt" else ""
        minimal_required = {
            f"bin/chthollyc{suffix_name}",
            f"libexec/chtholly/chthollyc-driver{suffix_name}",
            "share/chtholly/stdlib/manifest.toml",
            "share/chtholly/stdlib/host.cfdl",
            "share/chtholly/support/chtholly-v1.toml",
            "share/chtholly/support/chtholly-cffi-tier1.toml",
            "share/chtholly/support/chtholly-branding-trace-policy.toml",
            "share/chtholly/support/supply-chain-lock.json",
            f"share/chtholly/runtime/{archive_prefix}chtholly_next_runtime_v1{archive_suffix}",
            "share/chtholly/runtime/chtholly_next_runtime_v1.links",
        }
        missing = sorted(minimal_required - minimal_paths)
        if missing:
            raise AssertionError(f"minimal install is incomplete: {missing}")
        minimal_forbidden_prefixes = (
            "include/", "share/chtholly/docs/", "share/chtholly/vscode/",
            "share/chtholly/runtime/chtholly_next_runtime_v2",
            "share/chtholly/runtime/chtholly_next_container_v1",
            "lib/", "bin/chtholly-test", "bin/chtholly-lsp",
            "bin/chtholly-cffi", "bin/chtholly-toolchain",
            "bin/chtholly-component-deploy", "bin/chtholly-registry",
        )
        leaked = sorted(
            path for path in minimal_paths
            if path.startswith(minimal_forbidden_prefixes)
        )
        if leaked:
            raise AssertionError(f"developer-only files leaked into minimal install: {leaked}")

        full_required = {
            f"bin/chthollyc{suffix_name}",
            f"bin/chtholly-test{suffix_name}",
            f"bin/chtholly-lsp{suffix_name}",
            f"bin/chtholly-cffi{suffix_name}",
            f"bin/chtholly-toolchain{suffix_name}",
            f"libexec/chtholly/chthollyc-driver{suffix_name}",
            "share/chtholly/stdlib/manifest.toml",
            "share/chtholly/support/chtholly-cffi-tier1.toml",
            "share/chtholly/support/chtholly-branding-trace-policy.toml",
            "share/chtholly/support/supply-chain-lock.json",
            f"share/chtholly/runtime/{archive_prefix}chtholly_next_runtime_v1{archive_suffix}",
            "share/chtholly/runtime/chtholly_next_runtime_v1.links",
            f"share/chtholly/runtime/{archive_prefix}chtholly_next_runtime_v2{archive_suffix}",
            "share/chtholly/runtime/chtholly_next_runtime_v2.links",
            f"share/chtholly/runtime/{archive_prefix}chtholly_next_container_v1{archive_suffix}",
            f"lib/{archive_prefix}chtholly_component_loader_v1{archive_suffix}",
            "include/chtholly/component_abi_v1.h",
            "include/chtholly/component_loader_v1.h",
            "include/chtholly/next_host_v2.h",
            "share/chtholly/docs/quickstart.md",
            "share/chtholly/vscode/source/package.json",
        }
        if os.name == "nt":
            full_required.add("bin/libclang.dll")
        if args.expect_vsix:
            full_required.add(
                "share/chtholly/vscode/chtholly-vscode-0.2.0-preview.vsix"
            )
        missing = sorted(full_required - full_paths)
        if missing:
            raise AssertionError(f"full install is incomplete: {missing}")

        minimal_compiler = minimal / "bin" / f"chthollyc{suffix}"
        minimal_project = root / "minimal-project"
        minimal_doctor = run(str(minimal_compiler), "doctor")
        if "doctor\tready" not in minimal_doctor.stdout:
            raise AssertionError(
                f"minimal doctor did not report readiness: {minimal_doctor.stdout!r}"
            )
        run(str(minimal_compiler), "new", str(minimal_project),
            "--name", "minimal_preview")
        run(str(minimal_compiler), "check", cwd=minimal_project)
        run(str(minimal_compiler), "run", cwd=minimal_project)
        # The compiler owns project state under the project directory. Use a
        # temporary source copy so release-install evidence cannot dirty the
        # repository's examples tree.
        minimal_preview = root / "minimal-preview-source"
        shutil.copytree(
            args.source_dir / "examples" / "hello-preview",
            minimal_preview,
            ignore=shutil.ignore_patterns(".chtholly", "chtholly.lock"),
        )
        minimal_output = run(
            str(minimal_compiler), "run", "--project", str(minimal_preview), "--",
            "minimal-install",
        ).stdout
        if minimal_output != "Hello from Chtholly 0.2.0-preview\nargument: minimal-install\n":
            raise AssertionError(
                f"unexpected minimal installed example output: {minimal_output!r}"
            )

        prefix = full
        project = root / "project"
        compiler = prefix / "bin" / f"chthollyc{suffix}"
        test_runner = prefix / "bin" / f"chtholly-test{suffix}"
        driver = prefix / "libexec" / "chtholly" / f"chthollyc-driver{suffix}"
        lsp = prefix / "bin" / f"chtholly-lsp{suffix}"
        cffi = prefix / "bin" / f"chtholly-cffi{suffix}"
        libclang = prefix / "bin" / ("libclang.dll" if os.name == "nt" else "libclang.so")

        if os.name == "nt":
            host = "windows-2022"
        elif platform.system() == "Linux":
            host = "ubuntu-24.04"
        elif platform.system() == "Darwin":
            host = "macos-14"
        else:
            raise AssertionError(f"unsupported install-tree host: {platform.system()}")
        host_evidence = root / "host-evidence.json"
        run(
            sys.executable,
            str(args.source_dir / "scripts" / "release-host-evidence.py"),
            "--host", host,
            "--install-prefix", str(prefix),
            "--output", str(host_evidence),
        )
        if not host_evidence.is_file():
            raise AssertionError("installed host evidence was not written")
        triple = {
            "windows-2022": "x86_64-pc-windows-msvc",
            "ubuntu-24.04": "x86_64-unknown-linux-gnu",
            "macos-14": "aarch64-apple-darwin",
        }[host]
        lifecycle_evidence = root / "install-lifecycle-evidence.json"
        run(
            sys.executable,
            str(args.source_dir / "scripts" / "release-install-upgrade-evidence.py"),
            "--toolchain", str(prefix / "bin" / f"chtholly-toolchain{suffix}"),
            "--install-prefix", str(prefix),
            "--host", triple,
            "--host-name", host,
            "--output", str(lifecycle_evidence),
        )
        if not lifecycle_evidence.is_file():
            raise AssertionError("installed lifecycle evidence was not written")

        version = run(str(compiler), "--version").stdout
        if not version.startswith("chthollyc 0.2.0-preview"):
            raise AssertionError(f"unexpected installed compiler version: {version!r}")
        doctor = run(str(compiler), "doctor").stdout
        for check in ("compiler\t", "resources\t", "runtime\t", "stdlib\t",
                      "target\t", "c-compiler\t", "c-sdk\t", "cffi-tool\t",
                      "libclang\t", "cffi-doctor\t", "cffi-probe\t", "linker\t",
                      "doctor\tready"):
            if check not in doctor:
                raise AssertionError(f"installed doctor omitted {check!r}: {doctor!r}")
        runner_manifest = root / "runner-manifest.toml"
        runner_manifest.write_text(
            "format = 1\n"
            "[[test]]\n"
            "name = \"installed_framework_smoke\"\n"
            "kind = \"inprocess\"\n"
            "labels = [\"framework\"]\n"
            "registry = \"framework_smoke\"\n",
            encoding="utf-8",
        )
        runner_list = run(str(test_runner), "list", "--manifest",
                          str(runner_manifest))
        if "installed_framework_smoke" not in runner_list.stdout:
            raise AssertionError("installed chtholly-test runner is not executable")
        run(str(compiler), "new", str(project), "--name", "installed_preview")
        run(str(compiler), "check", cwd=project)
        run(str(compiler), "run", cwd=project)

        preview = root / "full-preview-source"
        shutil.copytree(
            args.source_dir / "examples" / "hello-preview",
            preview,
            ignore=shutil.ignore_patterns(".chtholly", "chtholly.lock"),
        )
        output = run(str(compiler), "run", "--project", str(preview), "--",
                     "install-tree").stdout
        if output != "Hello from Chtholly 0.2.0-preview\nargument: install-tree\n":
            raise AssertionError(f"unexpected installed example output: {output!r}")

        if (prefix / "share" / "chtholly" / "docs" / "internal").exists():
            raise AssertionError("internal documentation leaked into install tree")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
