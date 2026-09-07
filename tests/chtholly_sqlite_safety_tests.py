#!/usr/bin/env python3

import argparse
import pathlib
import shutil
import tempfile

from chtholly_test_support import run


def quote(value: object) -> str:
    return '"' + str(value).replace("\\", "/").replace('"', '\\"') + '"'


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cffi", required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--chthollyc", required=True)
    parser.add_argument("--source-dir", type=pathlib.Path, required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--sqlite-include", required=True)
    parser.add_argument("--sqlite-provider", required=True)
    parser.add_argument("--sqlite-library", required=True)
    args = parser.parse_args()

    source = args.source_dir.resolve() / "examples" / "sqlite-safety"
    sqlite_include = pathlib.Path(args.sqlite_include).resolve()
    sqlite_provider = pathlib.Path(args.sqlite_provider).resolve()
    sqlite_library = pathlib.Path(args.sqlite_library).resolve()
    with tempfile.TemporaryDirectory(prefix="chtholly-sqlite-safety-") as raw:
        root = pathlib.Path(raw)
        project = root / "project"
        cffi_config = root / "sqlite-provider-cffi.toml"
        generated = root / "generated.cfdl"
        cffi_config.write_text(
            "version = 3\n"
            "module = \"sqlite_generated\"\n"
            f"target = {quote(args.target)}\n"
            "headers = [\"provider.h\"]\n\n"
            f"[toolchain]\ncompiler = {quote(args.cc)}\n\n"
            "[clang]\nlanguage = \"c\"\nstandard = \"c17\"\n"
            f"include_paths = [{quote(sqlite_include)}, {quote(source / 'native')}]\n"
            "system_include_paths = []\ndefines = []\nundefines = []\n"
            "arguments = []\n\n[probe]\ncompile_arguments = []\n"
            "link_arguments = []\n"
            f"library_paths = [{quote(sqlite_library.parent)}]\n"
            f"libraries = [{quote(sqlite_library)}]\n"
            "timeout_ms = 30000\n"
            "\n[[roots]]\nkind = \"type\"\nname = \"sqlite3\"\n"
            "\n[[roots]]\nkind = \"function\"\nname = \"chtholly_sqlite_version\"\n"
            "\n[[roots]]\nkind = \"function\"\nname = \"chtholly_sqlite_open_memory\"\n"
            "\n[[roots]]\nkind = \"function\"\nname = \"chtholly_sqlite_close\"\n"
            "\n[[roots]]\nkind = \"function\"\nname = \"chtholly_sqlite_open_invalid\"\n"
            "\n[[type_mappings]]\nc_type = \"sqlite3*\"\n"
            "cfdl_name = \"Database\"\ncarrier = \"void*\"\n",
            encoding="utf-8",
        )
        generated_result = run([
            args.cffi, "generate", "--config", str(cffi_config), "-o",
            str(generated),
        ])
        generated_text = generated.read_text(encoding="utf-8")
        if generated_result.returncode != 0 or not all(
            marker in generated_text
            for marker in (
                "chtholly_sqlite_version",
                "chtholly_sqlite_open_memory",
                "chtholly_sqlite_close",
                "chtholly_sqlite_open_invalid",
            )
        ):
            raise AssertionError(generated_text)
        shutil.copytree(
            source,
            project,
            ignore=shutil.ignore_patterns(".chtholly", "chtholly.lock"),
        )
        manifest = project / "chtholly.toml"
        manifest.write_text(
            manifest.read_text(encoding="utf-8").replace(
                "__SQLITE_PROVIDER__",
                str(sqlite_provider).replace("\\", "/"),
            ).replace(
                "__SQLITE_LIBRARY__",
                str(sqlite_library).replace("\\", "/"),
            ),
            encoding="utf-8",
        )
        checked = run([args.chthollyc, "check", "--project", str(project)])
        if "checked" not in checked.stdout:
            raise AssertionError(checked.stdout)
        result = run([args.chthollyc, "run", "--project", str(project)])
        if result.returncode != 0:
            raise AssertionError(result)
        if any(path.name in {".chtholly", "chtholly.lock"}
               for path in source.rglob("*")):
            raise AssertionError("sqlite example source tree was modified")
    print("sqlite safety vertical: CFFI-derived resource/error overlay passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
