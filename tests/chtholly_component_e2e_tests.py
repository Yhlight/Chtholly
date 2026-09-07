#!/usr/bin/env python3

import argparse
import pathlib
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    parser.add_argument("--loader-test", required=True)
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--target", required=True)
    args = parser.parse_args()
    fixture = pathlib.Path(args.source_dir) / "tests" / "fixtures" / "chtholly-component-v1"
    with tempfile.TemporaryDirectory(prefix="chtholly-component-v1-") as raw:
        output = pathlib.Path(raw)
        clean_fixture = output / "source"
        shutil.copytree(fixture, clean_fixture,
                        ignore=shutil.ignore_patterns(".chtholly", "chtholly.lock"))
        invoke([args.chthollyc, "check", "--project", str(clean_fixture)])
        built = invoke([args.chthollyc, "build", "--project", str(clean_fixture),
                        "--out-dir", str(output)])
        suffix = ".dll" if "windows" in args.target else ".so"
        libraries = list(output.glob(f"*{suffix}"))
        assert len(libraries) == 1, (built.stdout, list(output.iterdir()))
        library = libraries[0].resolve()
        sidecar = pathlib.Path(str(library) + ".chcomponent")
        assert sidecar.is_file()
        invoke([args.loader_test, str(library), str(sidecar), args.target])
        # Running against the repository fixture would create .chtholly and
        # chtholly.lock in the source tree. Keep every compiler invocation in
        # the isolated copy so this test is source-tree pure.
        rejected = invoke([args.chthollyc, "run", "--project", str(clean_fixture)],
                          expected=1)
        assert "support check and build, not run" in rejected.stderr

        broken = output / "broken"
        shutil.copytree(clean_fixture, broken,
                        ignore=shutil.ignore_patterns(".chtholly", "chtholly.lock"))
        manifest = broken / "chtholly.toml"
        original = manifest.read_text(encoding="utf-8")
        manifest.write_text(original.replace(
            'identity = "org.chtholly.tests.component-v1"\n', ""),
            encoding="utf-8")
        missing_identity = invoke(
            [args.chthollyc, "check", "--project", str(broken)], expected=1)
        assert "requires abi = 1, identity" in missing_identity.stderr
        manifest.write_text(original.replace(
            '"component::math::add"', '"component::math::missing"'),
            encoding="utf-8")
        missing_export = invoke(
            [args.chthollyc, "check", "--project", str(broken)], expected=1)
        assert "was not found" in missing_export.stderr
        math = broken / "src" / "math.cns"
        math.write_text(math.read_text(encoding="utf-8") +
                        "\npub fn bad_pointer(value: void*): i32 { return 0; }\n",
                        encoding="utf-8")
        manifest.write_text(original.replace(
            '"component::math::add"', '"component::math::bad_pointer"'),
            encoding="utf-8")
        bad_pointer = invoke(
            [args.chthollyc, "check", "--project", str(broken)], expected=1)
        assert "parameter outside ABI epoch 1" in bad_pointer.stderr
        math.write_text(math.read_text(encoding="utf-8") +
                        "\npub fn generic<T>(value: T): T { return move value; }\n",
                        encoding="utf-8")
        manifest.write_text(original.replace(
            '"component::math::add"', '"component::math::generic"'),
            encoding="utf-8")
        generic = invoke(
            [args.chthollyc, "check", "--project", str(broken)], expected=1)
        assert "concrete synchronous safe free definition" in generic.stderr
        manifest.write_text(original.replace(
            '[build]\n', '[build]\nentry = "src/math.cns"\n'),
            encoding="utf-8")
        entry_conflict = invoke(
            [args.chthollyc, "check", "--project", str(broken)], expected=1)
        assert "cannot declare build.entry" in entry_conflict.stderr
        assert not any(path.name in {".chtholly", "chtholly.lock"}
                       for path in fixture.rglob("*"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
