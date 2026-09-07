#!/usr/bin/env python3

import argparse
import json
import pathlib
import shutil
import subprocess
import tempfile


def invoke(command: list[str], expected: int = 0,
           timeout: int | None = None) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        command, text=True, encoding="utf-8", stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False, timeout=timeout)
    if result.returncode != expected:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected}: "
            f"{command}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def contract(path: pathlib.Path) -> tuple[str, str]:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) == 2:
            values[fields[0]] = fields[1]
    return values["identity"], values["contract-digest"]


def copy_source_tree(source: pathlib.Path, destination: pathlib.Path) -> None:
    """Copy an example without carrying repository-local build state."""
    shutil.copytree(
        source,
        destination,
        ignore=shutil.ignore_patterns(".chtholly", "chtholly.lock"),
    )


def assert_repository_sources_clean(example: pathlib.Path) -> None:
    """The vertical must not rely on artifacts written under the checkout."""
    artifacts = sorted(
        path
        for path in example.rglob("*")
        if path.name in {".chtholly", "chtholly.lock"}
    )
    if artifacts:
        raise AssertionError(
            "component-host source tree contains repository-local artifacts: "
            + ", ".join(str(path) for path in artifacts)
        )


def build(compiler: str, project: pathlib.Path,
          output: pathlib.Path, suffix: str) -> tuple[pathlib.Path, str, str]:
    invoke([compiler, "build", "--project", str(project),
            "--out-dir", str(output)])
    libraries = list(output.glob(f"*{suffix}"))
    if len(libraries) != 1:
        raise AssertionError(f"component build produced {libraries}")
    library = libraries[0].resolve()
    identity, digest = contract(pathlib.Path(str(library) + ".chcomponent"))
    return library, identity, digest


def clean_project_state(project: pathlib.Path) -> None:
    """Force a source rebuild without consulting a prior local artifact."""
    cache = project / ".chtholly"
    if cache.exists():
        shutil.rmtree(cache)
    lockfile = project / "chtholly.lock"
    if lockfile.exists():
        lockfile.unlink()


def run_host(host: str, duration: int, threads: int,
             generation_root: pathlib.Path, target: str, alpha: tuple,
             beta: tuple, alpha_increment: int = 1,
             beta_increment: int = 2) -> dict:
    command = [
        host, str(duration), str(threads), str(generation_root.resolve()),
        target, "v1", str(alpha[0]), alpha[1], alpha[2], str(beta[0]),
        beta[1], beta[2], str(alpha_increment), str(beta_increment),
    ]
    result = invoke(command, timeout=duration + 120)
    report = json.loads(result.stdout)
    if report.get("schema") != "chtholly-component-host-soak-v1":
        raise AssertionError(report)
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    parser.add_argument("--host", required=True)
    parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--duration-seconds", type=int, default=15)
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--source-commit", default="")
    args = parser.parse_args()
    if args.duration_seconds <= 0 or args.threads < 2:
        raise AssertionError("invalid soak duration or thread count")
    suffix = ".dll" if "windows" in args.target else ".so"
    example = args.source_dir / "examples" / "component-host"
    assert_repository_sources_clean(example)
    with tempfile.TemporaryDirectory(prefix="chtholly-component-host-") as raw:
        root = pathlib.Path(raw)
        sources = root / "sources"
        copy_source_tree(example / "alpha", sources / "alpha")
        copy_source_tree(example / "beta", sources / "beta")
        alpha = build(args.chthollyc, sources / "alpha", root / "alpha",
                      suffix)
        beta = build(args.chthollyc, sources / "beta", root / "beta", suffix)
        if alpha[1] == beta[1]:
            raise AssertionError("component deployment identities must differ")
        if alpha[2] == beta[2]:
            raise AssertionError("component contract digests must differ")
        report = run_host(args.host, args.duration_seconds, args.threads,
                          root / "generations", args.target, alpha, beta)
        if report["cycles"] < 4 or report["calls_alpha"] == 0 or \
                report["calls_beta"] == 0:
            raise AssertionError(report)
        if report["closing_rejections"] != report["cycles"] * 2 or \
                report["cross_component_rejections"] != report["cycles"]:
            raise AssertionError(report)
        if report["cleanup_failures"] != 0 or \
                report["resource_after"] > report["resource_before"] + 4:
            raise AssertionError(report)
        beta_source = sources / "beta" / "src" / "plugin.cns"
        original = beta_source.read_text(encoding="utf-8")
        changed = original.replace("return value + 2;", "return value + 3;")
        if changed == original:
            raise AssertionError("beta fixture mutation did not change process")
        beta_source.write_text(changed, encoding="utf-8")
        clean_project_state(sources / "beta")
        rebuilt_beta = build(args.chthollyc, sources / "beta",
                             root / "beta-rebuilt", suffix)
        if rebuilt_beta[1] != beta[1]:
            raise AssertionError(
                "beta body-only rebuild changed Component deployment identity")
        if rebuilt_beta[2] != beta[2]:
            raise AssertionError(
                "beta body-only rebuild changed Component ABI contract digest")
        changed_report = run_host(
            args.host, max(1, args.duration_seconds // 2), args.threads,
            root / "generations-changed", args.target, alpha, rebuilt_beta,
            alpha_increment=1, beta_increment=3)
        if changed_report["calls_alpha"] == 0 or \
                changed_report["calls_beta"] == 0:
            raise AssertionError(changed_report)
        report["contract_identities"] = {"alpha": alpha[1], "beta": beta[1]}
        report["contract_digests"] = {"alpha": alpha[2], "beta": beta[2]}
        report["source_roots"] = [str(sources / "alpha"), str(sources / "beta")]
        report["beta_rebuild"] = {
            "old_contract_digest": beta[2],
            "new_contract_digest": rebuilt_beta[2],
            "contract_digest_unchanged": rebuilt_beta[2] == beta[2],
            "process_before": 2,
            "process_after": 3,
            "calls": changed_report["calls_beta"],
        }
        if args.source_commit:
            report["source_commit"] = args.source_commit
        report["target"] = args.target
        report["valid"] = True
        assert_repository_sources_clean(example)
        print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
