import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile

from chtholly_test_support import run


def quote(value):
    return '"' + str(value).replace('\\', '/').replace('"', '\\"') + '"'


def cffi_config(target, header_dir, compiler, provider):
    link_arguments = '["bcrypt.lib"]' if "windows" in target else "[]"
    return (
        "version = 3\nmodule = \"generated_api\"\n"
        f"target = {quote(target)}\nheaders = [\"fixture.h\"]\n\n"
        f"[toolchain]\ncompiler = {quote(compiler)}\n\n"
        "[clang]\nlanguage = \"c\"\nstandard = \"c17\"\n"
        f"include_paths = [{quote(header_dir)}]\n"
        "system_include_paths = []\ndefines = []\nundefines = []\n"
        "arguments = []\n\n[probe]\n"
        "compile_arguments = []\n"
        f"link_arguments = {link_arguments}\nlibrary_paths = []\n"
        f"libraries = [{quote(provider)}]\ntimeout_ms = 30000\n\n"
        "[[roots]]\nkind = \"function\"\nname = \"c_add_one\"\n"
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--chthollyc", required=True)
    parser.add_argument("--cffi", required=True)
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--provider", required=True)
    args = parser.parse_args()

    source = pathlib.Path(args.source_dir).resolve()
    source_header = source / "tests" / "fixtures" / "cffi" / "fixture.h"
    provider_library = pathlib.Path(args.provider).resolve()
    with tempfile.TemporaryDirectory(prefix="chtholly-cffi-cross-package-") as name:
        root = pathlib.Path(name)
        provider = root / "provider"
        generated = provider / "generated"
        include = provider / "include"
        consumer = root / "consumer"
        generated.mkdir(parents=True)
        include.mkdir()
        consumer.mkdir()
        header = include / "fixture.h"
        header.write_bytes(source_header.read_bytes())

        config = provider / "chtholly-cffi.toml"
        cfdl = generated / "generated_api.cfdl"
        receipt = provider / "bindings.cffi-verify"
        config.write_text(
            cffi_config(args.target, include, args.cc, provider_library),
            encoding="utf-8",
        )
        run([args.cffi, "generate", "--config", config, "-o", cfdl])
        generated_text = cfdl.read_text(encoding="utf-8")
        if ("module generated_api;" not in generated_text or
                "c_add_one" not in generated_text):
            raise AssertionError(f"generated CFDL is incomplete:\n{generated_text}")
        run([args.cffi, "verify", "--config", config, cfdl,
             "--receipt", receipt])

        (provider / "chtholly.toml").write_text(
            '[package]\nname = "cffi_provider"\nlanguage = "1.9"\n\n'
            '[build]\nmodule_paths = ["generated"]\n\n'
            f'[native]\nlink_libraries = [{quote(provider_library)}' +
            (', "bcrypt.lib"' if "windows" in args.target else '') +
            ']\n\n'
            '[cffi]\nreceipt = "bindings.cffi-verify"\nrequired = true\n',
            encoding="utf-8",
        )
        (consumer / "main.cns").write_text(
            "module main; import generated_api; "
            "fn main(): i32 { unsafe { return "
            "generated_api::c_add_one(41) - 42; } }\n",
            encoding="utf-8",
        )
        (consumer / "chtholly.toml").write_text(
            '[package]\nname = "cffi_consumer"\nlanguage = "1.9"\n\n'
            f'[target]\ntriple = {quote(args.target)}\n\n'
            '[build]\nentry = "main.cns"\nmodule_paths = ["."]\n\n'
            '[dependencies]\ncffi_provider = { path = "../provider" }\n',
            encoding="utf-8",
        )

        run([args.chthollyc, "run", "--project", consumer])
        lock_text = (consumer / "chtholly.lock").read_text(encoding="utf-8")
        receipt_digest = hashlib.sha256(receipt.read_bytes()).hexdigest()
        if f'cffi-receipt-sha256 = "{receipt_digest}"' not in lock_text:
            raise AssertionError("consumer lockfile omits provider CFFI identity")
        run([args.chthollyc, "run", "--project", consumer, "--locked"])

        original_cfdl = cfdl.read_bytes()
        header_text = header.read_text(encoding="utf-8")
        header.write_text(
            header_text.replace("c_add_one(int32_t value)",
                                "c_add_one(int32_t sample)"),
            encoding="utf-8",
        )
        preview = run([args.cffi, "regenerate", "--config", config, cfdl], 3)
        if "parameter-rename" not in preview.stdout:
            raise AssertionError(f"missing parameter rename diff: {preview.stdout}")
        preview_json = run([args.cffi, "regenerate", "--config", config, cfdl,
                            "--output-format", "jsonl-v1"], 3)
        events = [json.loads(line) for line in preview_json.stdout.splitlines()]
        if not any(event.get("kind") == "parameter-rename" for event in events):
            raise AssertionError(f"missing JSON rename event: {events}")
        if cfdl.read_bytes() != original_cfdl:
            raise AssertionError("CFFI regeneration preview modified CFDL")
        run([args.cffi, "regenerate", "--config", config, cfdl, "--write"])
        if "sample:" not in cfdl.read_text(encoding="utf-8"):
            raise AssertionError("CFFI regeneration did not update parameter")
        run([args.cffi, "verify", "--config", config, cfdl,
             "--receipt", receipt])
        run([args.chthollyc, "run", "--project", consumer])
        run([args.chthollyc, "run", "--project", consumer, "--locked"])

        original = receipt.read_text(encoding="utf-8")
        receipt.write_bytes(
            original.replace("facts\t", "unknown\t", 1).encode("utf-8"))
        failed = run([args.chthollyc, "check", "--project", consumer], 1)
        if "CFFI receipt" not in failed.stderr:
            raise AssertionError(
                f"missing strict receipt diagnostic: {failed.stderr}")

        wrong_target = ("x86_64-unknown-linux-gnu"
                        if args.target == "x86_64-pc-windows-msvc"
                        else "x86_64-pc-windows-msvc")
        receipt.write_bytes(
            original.replace(f"target\t{args.target}",
                             f"target\t{wrong_target}", 1).encode("utf-8"))
        failed = run([args.chthollyc, "check", "--project", consumer], 1)
        if "resolved build target" not in failed.stderr:
            raise AssertionError(
                f"missing receipt target diagnostic: {failed.stderr}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
