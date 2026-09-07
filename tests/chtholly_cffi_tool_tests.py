import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import tempfile

from chtholly_test_support import run, run_nonzero


def quote(value):
    return '"' + str(value).replace('\\', '/').replace('"', '\\"') + '"'


def assert_missing_native_library(chthollyc, provider_package,
                                  consumer_package, library, label):
    """Require a missing provider link input to fail closed, then recover."""
    manifest = pathlib.Path(provider_package) / "chtholly.toml"
    original = manifest.read_text(encoding="utf-8")
    missing = (pathlib.Path(provider_package).parent /
               f"missing-{label}-native-library")
    broken = original.replace(quote(library), quote(missing), 1)
    if broken == original:
        raise AssertionError(f"provider manifest did not contain {library}")
    manifest.write_text(broken, encoding="utf-8")
    try:
        failed = run([chthollyc, "build", "--project", consumer_package], 1)
        diagnostic = failed.stderr.lower()
        if str(missing).lower() not in diagnostic and not any(
                token in diagnostic for token in ("link", "cannot find", "lnk1104")):
            raise AssertionError(
                f"missing {label} link library produced an unrelated diagnostic:\n"
                f"{failed.stderr}")
    finally:
        manifest.write_text(original, encoding="utf-8")
    run([chthollyc, "run", "--project", consumer_package, "--locked"])


def copy_linux_sysroot(compiler, destination):
    """Copy the compiler's resolved headers, multiarch dirs, and CRT files."""
    destination = pathlib.Path(destination)

    def query(arguments, stdin=""):
        result = subprocess.run([compiler, *arguments], input=stdin, text=True,
                                capture_output=True, check=False)
        if result.returncode != 0:
            raise AssertionError(
                f"compiler query failed: {[compiler, *arguments]}\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
        return result.stdout.strip(), result.stderr

    def copy_path(path):
        path = pathlib.Path(path)
        if not path.exists():
            return
        try:
            relative = path.relative_to(path.anchor)
        except ValueError:
            return
        target = destination / relative
        if path.is_dir():
            shutil.copytree(path, target, dirs_exist_ok=True)
        elif path.is_file():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)

    # Copy only the transitive system-header closure used by the probes. This
    # keeps the fixture self-contained without cloning an entire host /usr.
    dependencies, _ = query(
        ["-M", "-x", "c", "-"],
        "#include <stddef.h>\n#include <stdint.h>\n#include <stdio.h>\n"
        "#include <stdlib.h>\n#include <errno.h>\n#include <string.h>\n")
    for token in dependencies.replace("\\\n", " ").split():
        if token.endswith(":") or token == "\\":
            continue
        copy_path(token)

    search_stdout, _ = query(["-print-search-dirs"])
    for line in search_stdout.splitlines():
        if line.startswith("libraries: ="):
            for path in line.split("=", 1)[1].split(":"):
                for name in ("crt1.o", "crti.o", "crtn.o", "crtbegin.o",
                             "crtend.o", "libc.so", "libc.so.6",
                             "libgcc.a", "libgcc_s.so.1"):
                    copy_path(pathlib.Path(path) / name)

    multiarch, _ = query(["-print-multiarch"])
    if multiarch:
        # Headers are copied from the dependency closure; copy the complete
        # multiarch library directories so the CRT/linker closure is real.
        (destination / "usr" / "include" / multiarch).mkdir(
            parents=True, exist_ok=True)
        for prefix in ("/usr/lib", "/lib"):
            copy_path(pathlib.Path(prefix) / multiarch)

    for name in ("crt1.o", "crti.o", "crtn.o", "libc.so", "libgcc_s.so.1",
                 "ld-linux-x86-64.so.2"):
        resolved, _ = query([f"-print-file-name={name}"])
        if resolved and resolved != name:
            copy_path(resolved)

    return multiarch


def annotate_win32_read(line, buffer, capacity, count, context):
    line = line.replace(f"{buffer}: void*", f"{buffer}: view_mut void*")
    line = re.sub(rf"{re.escape(count)}: ([^,)]*)\*",
                  rf"{count}: out \1", line)
    return (line[:-1] +
            f" outcome win32_read<u8>({buffer}, {capacity}, {count}, "
            f"{context}) error win32 when result == 0;")


def config(module, target, header, include, compiler, library, roots,
           mappings=()):
    link_arguments = []
    library_paths = []
    if "linux" in target:
        # The Linux vcpkg triplet publishes static archives.  Unlike the
        # Windows import-library closure, curl's transitive zlib/OpenSSL
        # dependencies are not encoded in the single archive path, so include
        # the resolved sibling directory and system loader dependencies in
        # every generated probe configuration.
        library_paths.append(pathlib.Path(library).parent)
        link_arguments.extend(("-lz", "-lssl", "-lcrypto", "-ldl", "-lpthread"))
    text = (
        f"version = 3\nmodule = {quote(module)}\ntarget = {quote(target)}\n"
        f"headers = [{quote(header)}]\n\n[toolchain]\n"
        f"compiler = {quote(compiler)}\n\n[clang]\nlanguage = \"c\"\n"
        f"standard = \"c17\"\ninclude_paths = [{quote(include)}]\n"
        "system_include_paths = []\ndefines = []\nundefines = []\narguments = []\n\n"
        f"[probe]\ncompile_arguments = []\n"
        f"link_arguments = [{', '.join(quote(arg) for arg in link_arguments)}]\n"
        f"library_paths = [{', '.join(quote(path) for path in library_paths)}]\n"
        f"libraries = [{quote(library)}]\n"
        "timeout_ms = 30000\n"
    )
    for kind, name in roots:
        text += f"\n[[roots]]\nkind = {quote(kind)}\nname = {quote(name)}\n"
    for c_type, cfdl_name, carrier in mappings:
        text += (f"\n[[type_mappings]]\nc_type = {quote(c_type)}\n"
                 f"cfdl_name = {quote(cfdl_name)}\ncarrier = {quote(carrier)}\n")
    if "windows" in target:
        text = text.replace("link_arguments = []",
                            "link_arguments = [\"bcrypt.lib\"]", 1)
    return text


def native_link_libraries(target, libraries):
    """Expand Unix static-library dependencies while preserving link order."""
    values = [str(value) for value in libraries]
    if "linux" in target:
        search_dirs = {pathlib.Path(value).parent for value in values}
        for directory in search_dirs:
            for name in ("libz.a", "libssl.a", "libcrypto.a"):
                dependency = directory / name
                if dependency.is_file() and str(dependency) not in values:
                    values.append(str(dependency))
    return values


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cffi", required=True)
    parser.add_argument("--chthollyc", required=True)
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--provider", required=True)
    parser.add_argument("--real-provider", required=True)
    parser.add_argument("--sqlite-include", required=True)
    parser.add_argument("--sqlite-library", required=True)
    parser.add_argument("--zlib-library", required=True)
    parser.add_argument("--curl-library", required=True)
    parser.add_argument("--evidence-output", type=pathlib.Path)
    args = parser.parse_args()

    source = pathlib.Path(args.source_dir).resolve()
    provider = pathlib.Path(args.provider).resolve()
    real_provider = pathlib.Path(args.real_provider).resolve()
    sqlite_include = pathlib.Path(args.sqlite_include).resolve()
    sqlite_library = pathlib.Path(args.sqlite_library).resolve()
    zlib_library = pathlib.Path(args.zlib_library).resolve()
    curl_library = pathlib.Path(args.curl_library).resolve()
    fixture = source / "tests" / "fixtures" / "cffi"
    with tempfile.TemporaryDirectory(prefix="chtholly-cffi-") as temporary:
        root = pathlib.Path(temporary)
        fixture_config = root / "chtholly-cffi.toml"
        generated = root / "binding.cfdl"
        receipt_a = root / "binding-a.cffi-verify"
        receipt_b = root / "binding-b.cffi-verify"
        doctor = run([args.cffi, "doctor", "--output-format", "jsonl-v1"])
        doctor_events = [json.loads(line) for line in doctor.stdout.splitlines()]
        assert {event.get("name") for event in doctor_events} >= {
            "c-compiler", "c-sdk", "c-includes", "libclang", "cffi-probe",
            "doctor"}
        discovery_doctor = run([
            args.cffi, "doctor", "--output-format", "jsonl-v1"])
        discovery_events = [
            json.loads(line) for line in discovery_doctor.stdout.splitlines()]
        assert any(event.get("event") == "discovery" and
                   event.get("kind") == "trace"
                   for event in discovery_events)
        environment_event = next(
            event for event in discovery_events
            if event.get("event") == "environment")
        assert environment_event.get("target") == args.target
        assert environment_event.get("fingerprint")
        if "linux" in args.target:
            target_event = next(event for event in discovery_events
                                if event.get("event") == "target")
            assert "x86_64" in target_event.get("compiler", "")
            assert target_event.get("sysroot_mode") in {"host-root", "explicit"}
            assert any(event.get("event") == "component" and
                       event.get("status") == "ok"
                       for event in discovery_events)
            assert {event.get("value") for event in discovery_events
                    if event.get("event") == "header-probe"} >= {
                        "stddef.h=ok", "stdint.h=ok", "stdio.h=ok",
                        "stdlib.h=ok", "errno.h=ok"}
            assert any(event.get("event") == "runtime-file"
                       and "=ok:" in event.get("value", "")
                       for event in discovery_events)
        fixture_config.write_text(
            config(
                "fixture",
                args.target,
                "fixture.h",
                fixture,
                args.cc,
                provider,
                [
                    ("type", "chtholly_cffi_status"),
                    ("type", "chtholly_cffi_number"),
                    ("type", "chtholly_cffi_typedef_struct"),
                    ("type", "chtholly_cffi_typedef_union"),
                    ("function", "c_number_make"),
                    ("function", "c_number_sum"),
                    ("function", "c_status_is_ok"),
                    ("function", "c_set_callback"),
                    ("function", "c_errno_probe"),
                    ("constant", "CHTHOLLY_CFFI_LIMIT"),
                    ("constant", "CHTHOLLY_CFFI_CHAIN"),
                    ("constant", "EINVAL"),
                ],
            ),
            encoding="utf-8",
        )
        configured_doctor = run([args.chthollyc, "doctor", "--cffi-config",
                                 fixture_config])
        assert "cffi-probe\t" in configured_doctor.stdout
        broken_config = root / "broken-toolchain.toml"
        broken_config.write_text(
            fixture_config.read_text(encoding="utf-8").replace(
                quote(args.cc), quote(root / "missing-cc"), 1),
            encoding="utf-8")
        broken_doctor = run([args.chthollyc, "doctor", "--cffi-config",
                             broken_config], 1)
        assert "CFFI toolchain discovery failed" in broken_doctor.stderr
        if "linux" in args.target:
            missing_sysroot = root / "missing-sysroot.toml"
            missing_sysroot.write_text(
                fixture_config.read_text(encoding="utf-8").replace(
                    "[toolchain]\ncompiler =", "[toolchain]\nsysroot = \"/no/such/chtholly-sysroot\"\ncompiler =", 1),
                encoding="utf-8")
            missing_result = run([args.cffi, "doctor", "--config",
                                  missing_sysroot], 1)
            assert "linux.sysroot" in missing_result.stderr
            wrong_target = root / "wrong-target.toml"
            wrong_target.write_text(
                fixture_config.read_text(encoding="utf-8").replace(
                    args.target, "aarch64-unknown-linux-gnu", 1),
                encoding="utf-8")
            wrong_result = run([args.cffi, "doctor", "--config",
                                wrong_target], 1)
            assert ("native Tier-1 target" in wrong_result.stderr or
                    "native Tier-1 host" in wrong_result.stderr or
                    "aarch64" in wrong_result.stderr)
            sysroot = root / "linux-sysroot-fixture"
            (sysroot / "usr" / "include").mkdir(parents=True)
            (sysroot / "usr" / "lib").mkdir(parents=True)
            (sysroot / "lib").mkdir()
            fixture_sysroot = root / "sysroot-fixture.toml"
            fixture_sysroot.write_text(
                fixture_config.read_text(encoding="utf-8").replace(
                    "[toolchain]\ncompiler =",
                    f"[toolchain]\nsysroot = {quote(sysroot)}\ncompiler =", 1),
                encoding="utf-8")
            sysroot_result = run([args.cffi, "doctor", "--config",
                                  fixture_sysroot], 1)
            assert "linux.runtime" in sysroot_result.stderr
            missing_crt = sysroot / "usr" / "lib" / "crt1.o"
            missing_crt.write_bytes(b"not-an-elf")
            sysroot_result = run([args.cffi, "doctor", "--config",
                                  fixture_sysroot], 1)
            assert "runtime" in sysroot_result.stderr
            positive_sysroot = root / "linux-sysroot-positive"
            multiarch = copy_linux_sysroot(args.cc, positive_sysroot)
            assert multiarch
            positive_config = root / "positive-sysroot.toml"
            positive_config.write_text(
                fixture_config.read_text(encoding="utf-8").replace(
                    "[toolchain]\ncompiler =",
                    f"[toolchain]\nsysroot = {quote(positive_sysroot)}\ncompiler =",
                    1),
                encoding="utf-8")
            positive_doctor = run([args.cffi, "doctor", "--config",
                                   positive_config])
            assert positive_doctor.returncode == 0
            positive_cfdl = root / "positive.cfdl"
            positive_receipt = root / "positive.cffi-verify"
            run([args.cffi, "generate", "--config", positive_config, "-o",
                 positive_cfdl])
            run([args.cffi, "verify", "--config", positive_config,
                 positive_cfdl, "--receipt", positive_receipt])
            crt_candidates = list(positive_sysroot.rglob("crt1.o"))
            assert crt_candidates
            wrong_elf = bytearray(20)
            wrong_elf[0:4] = b"\x7fELF"
            wrong_elf[4] = 1  # ELF32
            wrong_elf[5] = 1  # little endian
            wrong_elf[6] = 1
            wrong_elf[18:20] = b"\x03\x00"  # EM_386
            crt_candidates[0].write_bytes(wrong_elf)
            wrong_arch_result = run([args.cffi, "doctor", "--config",
                                     positive_config], 1)
            assert "architecture-mismatch" in wrong_arch_result.stderr
        run([args.cffi, "doctor", "--config", fixture_config])
        run([args.cffi, "generate", "--config", fixture_config, "-o", generated])
        auto_config = root / "auto-cffi.toml"
        auto_config.write_text(
            config("auto", args.target, "fixture.h", fixture, "auto", provider,
                   [("function", "c_add_one")]),
            encoding="utf-8")
        auto_environment = os.environ.copy()
        for name in ("INCLUDE", "LIB", "LIBPATH"):
            auto_environment.pop(name, None)
        run([args.cffi, "generate", "--config", auto_config, "-o",
             root / "auto.cfdl"], env=auto_environment)
        run([args.cffi, "verify", "--config", auto_config, root / "auto.cfdl",
             "--receipt", root / "auto.cffi-verify"], env=auto_environment)
        v1_config = root / "v1-cffi.toml"
        v1_config.write_text(
            fixture_config.read_text(encoding="utf-8").replace(
                "version = 3", "version = 2", 1), encoding="utf-8")
        v1_result = run([args.cffi, "generate", "--config", v1_config, "-o",
                         root / "v1.cfdl"], 1)
        assert "version = 3" in v1_result.stderr
        generated_state = root / "binding.cffi-state"
        assert generated_state.read_text(encoding="utf-8").startswith("CHCFFIS5\n")
        same_output = root / "same.cfdl"
        same_path = run([args.cffi, "generate", "--config", fixture_config,
                         "-o", same_output, "--state", same_output], 1)
        assert "distinct CFDL and state paths" in same_path.stderr
        generated_copy = root / "binding-copy.cfdl"
        run([args.cffi, "generate", "--config", fixture_config, "-o",
             generated_copy])
        assert generated.read_bytes() == generated_copy.read_bytes()
        assert generated_state.read_bytes() == (
            root / "binding-copy.cffi-state").read_bytes()
        text = generated.read_text(encoding="utf-8")
        duplicate = root / "duplicate.cfdl"
        duplicate_line = next(
            line for line in text.splitlines()
            if line.startswith("foreign fn c_status_is_ok"))
        duplicate.write_text(text + "\n" + duplicate_line + "\n",
                             encoding="utf-8")
        duplicate_result = run([args.cffi, "regenerate", "--config",
                                fixture_config, duplicate], 1)
        assert "unique declaration keys" in duplicate_result.stderr
        rename_dir = root / "rename"
        rename_dir.mkdir()
        rename_header = rename_dir / "rename.h"
        rename_header.write_text(
            "#include <stdint.h>\n"
            "int32_t reorder(int32_t left, int32_t right);\n",
            encoding="utf-8")
        rename_config = rename_dir / "chtholly-cffi.toml"
        rename_config.write_text(
            config("rename", args.target, "rename.h", rename_dir, args.cc,
                   provider, [("function", "reorder")]),
            encoding="utf-8")
        rename_binding = rename_dir / "rename.cfdl"
        run([args.cffi, "generate", "--config", rename_config, "-o",
             rename_binding])
        rename_header.write_text(
            "#include <stdint.h>\n"
            "int32_t reorder(int32_t right, int32_t left);\n",
            encoding="utf-8")
        ambiguous_rename = run([args.cffi, "regenerate", "--config",
                                rename_config, rename_binding], 1)
        assert "ambiguous or reordered" in ambiguous_rename.stderr
        outcome_rename_dir = root / "outcome-rename"
        outcome_rename_dir.mkdir()
        outcome_rename_header = outcome_rename_dir / "outcome_rename.h"
        if "windows" in args.target:
            outcome_function = "c_win32_read_into"
            outcome_rename_header.write_text(
                "typedef void *read_handle;\n"
                "int c_win32_read_into(read_handle handle, void *buffer, "
                "unsigned long capacity, unsigned long *count, "
                "void *context);\n", encoding="utf-8")
        else:
            outcome_function = "c_posix_read_into"
            outcome_rename_header.write_text(
                "#include <stdint.h>\n"
                "int64_t c_posix_read_into(void *buffer, uint64_t capacity, "
                "int32_t mode);\n", encoding="utf-8")
        outcome_rename_config = outcome_rename_dir / "chtholly-cffi.toml"
        outcome_rename_config.write_text(
            config("outcome_rename", args.target, "outcome_rename.h",
                   outcome_rename_dir, args.cc, provider,
                   [("function", outcome_function)]), encoding="utf-8")
        outcome_rename_binding = outcome_rename_dir / "outcome_rename.cfdl"
        run([args.cffi, "generate", "--config", outcome_rename_config, "-o",
             outcome_rename_binding])
        outcome_rename_text = outcome_rename_binding.read_text(encoding="utf-8")
        outcome_rename_line = next(
            line for line in outcome_rename_text.splitlines()
            if line.startswith(f"foreign fn {outcome_function}"))
        if "windows" in args.target:
            annotated_outcome_line = annotate_win32_read(
                outcome_rename_line, "buffer", "capacity", "count", "context")
        else:
            annotated_outcome_line = (
                outcome_rename_line.replace(
                    "buffer: void*", "buffer: view_mut void*")[:-1] +
                " outcome posix_read<u8>(buffer, capacity)"
                " error errno when result == -1;")
        outcome_rename_binding.write_text(
            outcome_rename_text.replace(
                outcome_rename_line, annotated_outcome_line), encoding="utf-8")
        if "windows" in args.target:
            outcome_rename_header.write_text(
                "typedef void *read_handle;\n"
                "int c_win32_read_into(read_handle source, void *storage, "
                "unsigned long limit, unsigned long *transferred, "
                "void *operation);\n", encoding="utf-8")
        else:
            outcome_rename_header.write_text(
                "#include <stdint.h>\n"
                "int64_t c_posix_read_into(void *storage, uint64_t limit, "
                "int32_t mode);\n", encoding="utf-8")
        run([args.cffi, "regenerate", "--config", outcome_rename_config,
             outcome_rename_binding, "--write"])
        outcome_renamed = outcome_rename_binding.read_text(encoding="utf-8")
        assert "storage: view_mut void*" in outcome_renamed
        if "windows" in args.target:
            assert ("outcome win32_read<u8>(storage, limit, transferred, "
                    "operation)") in outcome_renamed
            assert "error win32 when result == 0" in outcome_renamed
        else:
            assert "outcome posix_read<u8>(storage, limit)" in outcome_renamed
            assert "error errno when result == -1" in outcome_renamed
        assert "foreign enum chtholly_cffi_status:" in text
        assert "foreign union chtholly_cffi_number" in text
        assert "foreign struct chtholly_cffi_typedef_struct" in text
        assert "foreign union chtholly_cffi_typedef_union" in text
        assert 'link "c_number_sum" call c' in text
        assert "c_fn c(c_int)->void" in text
        assert "foreign const CHTHOLLY_CFFI_LIMIT: c_int = 7;" in text
        assert "foreign const CHTHOLLY_CFFI_CHAIN: c_int = 7;" in text
        assert "foreign const EINVAL: c_int = " in text
        errno_line = next(line for line in text.splitlines()
                          if line.startswith("foreign fn c_errno_probe"))
        errno_binding = text
        errno_binding = errno_binding.replace(
            errno_line, errno_line[:-1] +
            " error errno when result == -1;")
        generated.write_text(errno_binding, encoding="utf-8")
        run([args.cffi, "regenerate", "--config", fixture_config, generated],
            (0, 3))
        regenerated_errno = generated.read_text(encoding="utf-8")
        assert "error errno when result == -1" in regenerated_errno
        text = regenerated_errno
        run([args.cffi, "generate", "--config", fixture_config, "-o", generated], 1)
        run([args.cffi, "verify", "--config", fixture_config, generated,
             "--receipt", receipt_a])
        run([args.cffi, "verify", "--config", fixture_config, generated,
             "--receipt", receipt_b])
        assert receipt_a.read_bytes() == receipt_b.read_bytes()
        assert receipt_a.read_text(encoding="utf-8").startswith("CHCFFI3\n")
        variant_config = root / "fixture-variant.toml"
        variant_binding = root / "fixture-variant.cfdl"
        variant_receipt = root / "fixture-variant.cffi-verify"
        variant_config.write_text(
            fixture_config.read_text(encoding="utf-8").replace(
                'module = "fixture"', 'module = "fixture_variant"', 1),
            encoding="utf-8")
        run([args.cffi, "generate", "--config", variant_config, "-o",
             variant_binding])
        run([args.cffi, "verify", "--config", variant_config,
             variant_binding, "--receipt", variant_receipt])
        assert variant_receipt.read_bytes() != receipt_a.read_bytes()

        tampered = root / "tampered.cfdl"
        tampered.write_text(text.replace('link "c_number_sum"',
                                         'link "wrong_symbol"'), encoding="utf-8")
        run([args.cffi, "verify", "--config", fixture_config, tampered,
             "--receipt", root / "tampered.receipt"], 1)

        for bad_root in [("type", "chtholly_cffi_bits"),
                         ("function", "c_variadic"),
                         ("constant", "CHTHOLLY_CFFI_FUNCTION"),
                         ("constant", "__LINE__")]:
            rejected_config = root / f"rejected-{bad_root[1]}.toml"
            rejected_output = root / f"rejected-{bad_root[1]}.cfdl"
            rejected_config.write_text(
                config("rejected", args.target, "fixture.h", fixture,
                       args.cc, provider, [bad_root]), encoding="utf-8")
            run([args.cffi, "generate", "--config", rejected_config, "-o",
                 rejected_output], 1)

        sqlite_config = root / "sqlite-cffi.toml"
        sqlite_generated = root / "sqlite.cfdl"
        sqlite_config.write_text(
            config(
                "sqlite",
                args.target,
                "sqlite3.h",
                sqlite_include,
                args.cc,
                sqlite_library,
                [
                    ("type", "sqlite3"),
                    ("function", "sqlite3_open"),
                    ("function", "sqlite3_close"),
                    ("function", "sqlite3_libversion_number"),
                ],
                [("sqlite3*", "Database", "void*")],
            ),
            encoding="utf-8",
        )
        run([args.cffi, "generate", "--config", sqlite_config, "-o",
             sqlite_generated])
        sqlite_text = sqlite_generated.read_text(encoding="utf-8")
        assert "foreign type sqlite3;" in sqlite_text
        assert "foreign type Database: void*;" in sqlite_text
        assert "Database*" in sqlite_text
        assert 'link "sqlite3_open" call c' in sqlite_text
        sqlite_text = sqlite_text.replace("ppDb: Database*", "ppDb: out Database")
        sqlite_text = sqlite_text.replace(
            'link "sqlite3_open" call c;',
            'link "sqlite3_open" call c where ppDb obliges close;')
        sqlite_text = sqlite_text.replace("arg0: Database", "arg0: move Database")
        sqlite_text = sqlite_text.replace(
            'link "sqlite3_close" call c;',
            'link "sqlite3_close" call c where arg0 discharges close;')
        sqlite_text = ("// preserved binding header\n" + sqlite_text.replace(
            "module sqlite;\n", "module sqlite;\nimport protocol_support;\n") +
            "\n// preserved manual declaration\nforeign type Manual: void*;\n")
        sqlite_generated.write_text(sqlite_text, encoding="utf-8")
        sqlite_config.write_text(
            config(
                "sqlite",
                args.target,
                "sqlite3.h",
                sqlite_include,
                args.cc,
                sqlite_library,
                [
                    ("type", "sqlite3"),
                    ("function", "sqlite3_open"),
                    ("function", "sqlite3_close"),
                    ("function", "sqlite3_libversion_number"),
                    ("function", "sqlite3_errcode"),
                ],
                [("sqlite3*", "Database", "void*")],
            ),
            encoding="utf-8",
        )
        before_regeneration = sqlite_generated.read_bytes()
        preview = run([args.cffi, "regenerate", "--config", sqlite_config,
                       sqlite_generated], 3)
        assert "semantic-preserved" in preview.stdout
        assert sqlite_generated.read_bytes() == before_regeneration
        run([args.cffi, "regenerate", "--config", sqlite_config,
             sqlite_generated, "--write"])
        regenerated_text = sqlite_generated.read_text(encoding="utf-8")
        assert "ppDb: out Database" in regenerated_text
        assert "ppDb obliges close" in regenerated_text
        assert "arg0: move Database" in regenerated_text
        assert "arg0 discharges close" in regenerated_text
        assert "sqlite3_errcode" in regenerated_text
        assert regenerated_text.startswith("// preserved binding header\n")
        assert "import protocol_support;" in regenerated_text
        assert "// preserved manual declaration\nforeign type Manual: void*;" in regenerated_text
        run([args.cffi, "regenerate", "--config", sqlite_config,
             sqlite_generated], 0)
        run([args.cffi, "verify", "--config", sqlite_config, sqlite_generated,
             "--receipt", root / "sqlite.cffi-verify"])

        sqlite_config.write_text(
            config(
                "sqlite", args.target, "sqlite3.h", sqlite_include, args.cc,
                sqlite_library,
                [("type", "sqlite3"), ("function", "sqlite3_close"),
                 ("function", "sqlite3_libversion_number")],
                [("sqlite3*", "Database", "void*")],
            ),
            encoding="utf-8",
        )
        removed_with_semantics = run(
            [args.cffi, "regenerate", "--config", sqlite_config,
             sqlite_generated], 1)
        assert "retains resource-flow semantics" in removed_with_semantics.stderr

        zlib_config = root / "zlib-cffi.toml"
        zlib_generated = root / "zlib.cfdl"
        zlib_config.write_text(
            config("zlib", args.target, "zlib.h", sqlite_include,
                   args.cc, zlib_library,
                   [("function", "compressBound"),
                    ("function", "zlibVersion")]),
            encoding="utf-8")
        run([args.cffi, "generate", "--config", zlib_config, "-o",
             zlib_generated])
        zlib_state = root / "zlib.cffi-state"
        previous_zlib_state = zlib_state.read_bytes()
        zlib_config.write_text(
            config("zlib", args.target, "zlib.h", sqlite_include,
                   args.cc, zlib_library, [("function", "compressBound")]),
            encoding="utf-8")
        run([args.cffi, "regenerate", "--config", zlib_config,
             zlib_generated], 3)
        run([args.cffi, "regenerate", "--config", zlib_config,
             zlib_generated, "--write"])
        assert "zlibVersion" not in zlib_generated.read_text(encoding="utf-8")
        zlib_state.write_bytes(previous_zlib_state)
        recovered = run([args.cffi, "regenerate", "--config", zlib_config,
                         zlib_generated], 3)
        assert "recovered-current-mechanical-model" in recovered.stdout
        run([args.cffi, "regenerate", "--config", zlib_config,
             zlib_generated, "--write"])
        zlib_state.unlink()
        bootstrap = run([args.cffi, "regenerate", "--config", zlib_config,
                         zlib_generated], 3)
        assert "state-bootstrap" in bootstrap.stdout
        run([args.cffi, "regenerate", "--config", zlib_config,
             zlib_generated, "--write"])
        run([args.cffi, "verify", "--config", zlib_config, zlib_generated,
             "--receipt", root / "zlib.cffi-verify"])
        valid_state = zlib_state.read_bytes()
        zlib_state.write_bytes(b"CHCFFIS5\n")
        corrupt_state = run([args.cffi, "regenerate", "--config", zlib_config,
                             zlib_generated], 1)
        assert "state is truncated" in corrupt_state.stderr
        zlib_state.write_bytes(valid_state)

        upgrade_root = root / "sqlite-upgrade"
        old_include = source / "tests" / "fixtures" / "cffi" / "upgrades" / "sqlite" / "3.40.1"
        new_include = source / "tests" / "fixtures" / "cffi" / "upgrades" / "sqlite" / "3.53.4"
        assert hashlib.sha256((old_include / "sqlite3.h").read_bytes()).hexdigest() == (
            "dc419c400665bd43b335d04d7562d78e1d5dcd464fa0d6150c9d5c3bc5d705f4")
        assert hashlib.sha256((new_include / "sqlite3.h").read_bytes()).hexdigest() == (
            "919e7f2e8ed1d8f56ac17b412b8971c76aa5d1a879752cc6058f75e7d5910e1d")
        provider_package = upgrade_root / "provider"
        consumer_package = upgrade_root / "consumer"
        generated_dir = provider_package / "generated"
        generated_dir.mkdir(parents=True)
        consumer_package.mkdir(parents=True)
        upgrade_config = provider_package / "chtholly-cffi.toml"
        upgrade_binding = generated_dir / "sqlite_upgrade.cfdl"
        upgrade_receipt = provider_package / "sqlite-upgrade.cffi-verify"
        old_roots = [
            ("type", "sqlite3"),
            ("function", "sqlite3_open"),
            ("function", "sqlite3_close"),
            ("function", "sqlite3_initialize"),
            ("function", "sqlite3_libversion_number"),
        ]
        mappings = [("sqlite3*", "Database", "void*")]
        upgrade_config.write_text(
            config("sqlite_upgrade", args.target, "sqlite3.h", old_include,
                   args.cc, sqlite_library, old_roots, mappings),
            encoding="utf-8")
        run([args.cffi, "generate", "--config", upgrade_config, "-o",
             upgrade_binding])
        upgrade_text = upgrade_binding.read_text(encoding="utf-8")
        upgrade_text = upgrade_text.replace("ppDb: Database*", "ppDb: out Database")
        upgrade_text = upgrade_text.replace(
            'link "sqlite3_open" call c;',
            'link "sqlite3_open" call c where ppDb obliges close;')
        upgrade_text = upgrade_text.replace("arg0: Database", "arg0: move Database")
        upgrade_text = upgrade_text.replace(
            'link "sqlite3_close" call c;',
            'link "sqlite3_close" call c where arg0 discharges close;')
        upgrade_text = upgrade_text.replace(
            'link "sqlite3_initialize" call c;',
            'link "sqlite3_initialize" call c error code when result != 0;')
        upgrade_binding.write_text(upgrade_text, encoding="utf-8")
        new_roots = old_roots + [("function", "sqlite3_is_interrupted")]
        upgrade_config.write_text(
            config("sqlite_upgrade", args.target, "sqlite3.h", new_include,
                   args.cc, sqlite_library, new_roots, mappings),
            encoding="utf-8")
        upgrade_preview = run([args.cffi, "regenerate", "--config",
                               upgrade_config, upgrade_binding], 3)
        assert "sqlite3_is_interrupted" in upgrade_preview.stdout
        assert "semantic-preserved" in upgrade_preview.stdout
        run([args.cffi, "regenerate", "--config", upgrade_config,
             upgrade_binding, "--write"])
        upgraded_text = upgrade_binding.read_text(encoding="utf-8")
        assert "sqlite3_is_interrupted" in upgraded_text
        assert "ppDb obliges close" in upgraded_text
        assert "arg0 discharges close" in upgraded_text
        assert "error code when result != 0" in upgraded_text
        run([args.cffi, "verify", "--config", upgrade_config, upgrade_binding,
             "--receipt", upgrade_receipt])
        (provider_package / "chtholly.toml").write_text(
            '[package]\nname = "sqlite_upgrade_provider"\nlanguage = "1.9"\n\n'
            '[build]\nmodule_paths = ["generated"]\n\n'
            f'[native]\nlink_libraries = [{quote(sqlite_library)}]\n\n'
            '[cffi]\nreceipt = "sqlite-upgrade.cffi-verify"\nrequired = true\n',
            encoding="utf-8")
        (consumer_package / "main.cns").write_text(
            "module main; import sqlite_upgrade; import std::result; "
            "fn main(): i32 { unsafe { "
            "if (sqlite_upgrade::sqlite3_libversion_number() <= 0) { return 1; } "
            "let initialized = sqlite_upgrade::sqlite3_initialize(); "
            "let initialize_status = switch (initialized) { "
            "std::result::Result<void, i32>::Ok { .. } => 0; "
            "std::result::Result<void, i32>::Err { .. } => 2; }; "
            "if (initialize_status != 0) { return initialize_status; } "
            "return 0; } }\n", encoding="utf-8")
        (consumer_package / "chtholly.toml").write_text(
            '[package]\nname = "sqlite_upgrade_consumer"\nlanguage = "1.9"\n\n'
            f'[target]\ntriple = {quote(args.target)}\n\n'
            '[build]\nentry = "main.cns"\nmodule_paths = ["."]\n\n'
            '[dependencies]\nsqlite_upgrade_provider = { path = "../provider" }\n',
            encoding="utf-8")
        run([args.chthollyc, "run", "--project", consumer_package])
        assert_missing_native_library(
            args.chthollyc, provider_package, consumer_package,
            sqlite_library, "sqlite")

        zlib_upgrade_root = root / "zlib-upgrade"
        zlib_old = source / "tests" / "fixtures" / "cffi" / "upgrades" / "zlib" / "1.2.11"
        zlib_new = source / "tests" / "fixtures" / "cffi" / "upgrades" / "zlib" / "1.3.1"
        zlib_provider = zlib_upgrade_root / "provider"
        zlib_consumer = zlib_upgrade_root / "consumer"
        zlib_generated_dir = zlib_provider / "generated"
        zlib_generated_dir.mkdir(parents=True)
        zlib_consumer.mkdir(parents=True)
        zlib_upgrade_config = zlib_provider / "chtholly-cffi.toml"
        zlib_upgrade_binding = zlib_generated_dir / "zlib_upgrade.cfdl"
        zlib_upgrade_receipt = zlib_provider / "zlib-upgrade.cffi-verify"
        zlib_old_roots = [("function", "zlibVersion"),
                          ("function", "compressBound"),
                          ("constant", "ZLIB_VERNUM")]
        zlib_upgrade_config.write_text(
            config("zlib_upgrade", args.target, "zlib.h", zlib_old,
                   args.cc, zlib_library, zlib_old_roots), encoding="utf-8")
        run([args.cffi, "generate", "--config", zlib_upgrade_config, "-o",
             zlib_upgrade_binding])
        zlib_upgrade_config.write_text(
            config("zlib_upgrade", args.target, "zlib.h", zlib_new,
                   args.cc, zlib_library,
                   zlib_old_roots + [("function", "zlibCompileFlags")]),
            encoding="utf-8")
        zlib_upgrade_preview = run(
            [args.cffi, "regenerate", "--config", zlib_upgrade_config,
             zlib_upgrade_binding], 3)
        assert "zlibCompileFlags" in zlib_upgrade_preview.stdout
        run([args.cffi, "regenerate", "--config", zlib_upgrade_config,
             zlib_upgrade_binding, "--write"])
        assert "zlibCompileFlags" in zlib_upgrade_binding.read_text(encoding="utf-8")
        run([args.cffi, "verify", "--config", zlib_upgrade_config,
             zlib_upgrade_binding, "--receipt", zlib_upgrade_receipt])
        (zlib_provider / "chtholly.toml").write_text(
            '[package]\nname = "zlib_upgrade_provider"\nlanguage = "1.9"\n\n'
            '[build]\nmodule_paths = ["generated"]\n\n'
            f'[native]\nlink_libraries = [{quote(zlib_library)}]\n\n'
            '[cffi]\nreceipt = "zlib-upgrade.cffi-verify"\nrequired = true\n',
            encoding="utf-8")
        (zlib_consumer / "main.cns").write_text(
            "module main; import zlib_upgrade; "
            "fn main(): i32 { unsafe { "
            "let version = zlib_upgrade::zlibVersion(); "
            "if (version == null) { return 1; } "
            "let bound = zlib_upgrade::compressBound(16); "
            "let flags = zlib_upgrade::zlibCompileFlags(); "
            "if (bound == 0) { return 2; } "
            "if (flags == 0) { return 3; } "
            "return 0; } }\n", encoding="utf-8")
        (zlib_consumer / "chtholly.toml").write_text(
            '[package]\nname = "zlib_upgrade_consumer"\nlanguage = "1.9"\n\n'
            f'[target]\ntriple = {quote(args.target)}\n\n'
            '[build]\nentry = "main.cns"\nmodule_paths = ["."]\n\n'
            '[dependencies]\nzlib_upgrade_provider = { path = "../provider" }\n',
            encoding="utf-8")
        run([args.chthollyc, "run", "--project", zlib_consumer])
        assert_missing_native_library(
            args.chthollyc, zlib_provider, zlib_consumer, zlib_library, "zlib")

        curl_root = root / "curl-upgrade"
        curl_old = source / "tests" / "fixtures" / "cffi" / "upgrades" / "curl" / "7.88.1"
        curl_new = source / "tests" / "fixtures" / "cffi" / "upgrades" / "curl" / "8.12.1"
        curl_provider = curl_root / "provider"
        curl_consumer = curl_root / "consumer"
        curl_generated_dir = curl_provider / "generated"
        curl_generated_dir.mkdir(parents=True)
        curl_consumer.mkdir(parents=True)
        curl_config = curl_provider / "chtholly-cffi.toml"
        curl_binding = curl_generated_dir / "curl_upgrade.cfdl"
        curl_receipt = curl_provider / "curl-upgrade.cffi-verify"
        curl_roots = [("type", "CURL"), ("function", "curl_easy_init"),
                      ("function", "curl_easy_cleanup"),
                      ("function", "curl_easy_strerror"),
                      ("function", "curl_easy_perform"),
                      ("constant", "LIBCURL_VERSION_NUM")]
        curl_config.write_text(config("curl_upgrade", args.target, "curl.h",
                                      curl_old, args.cc, curl_library,
                                      curl_roots, [("CURL*", "Easy", "void*")]),
                                encoding="utf-8")
        run([args.cffi, "generate", "--config", curl_config, "-o", curl_binding])
        curl_text = curl_binding.read_text(encoding="utf-8")
        curl_binding.write_text(curl_text, encoding="utf-8")
        curl_config.write_text(config(
            "curl_upgrade", args.target, "curl.h", curl_new, args.cc,
            curl_library, curl_roots + [("function", "curl_easy_pause")],
            [("CURL*", "Easy", "void*")]), encoding="utf-8")
        if "windows" in args.target:
            curl_config.write_text(
                curl_config.read_text(encoding="utf-8").replace(
                    "link_arguments = [\"bcrypt.lib\"]",
                    "link_arguments = [\"ws2_32.lib\", \"crypt32.lib\", \"advapi32.lib\", \"normaliz.lib\", \"wldap32.lib\", \"bcrypt.lib\", \"iphlpapi.lib\", \"secur32.lib\", " +
                    quote(zlib_library) + "]"),
                encoding="utf-8")
        curl_preview = run([args.cffi, "regenerate", "--config", curl_config,
                            curl_binding], 3)
        assert "curl_easy_pause" in curl_preview.stdout
        assert "curl_easy_pause" in curl_preview.stdout
        run([args.cffi, "regenerate", "--config", curl_config, curl_binding,
             "--write"])
        curl_text = curl_binding.read_text(encoding="utf-8")
        assert "curl_easy_pause" in curl_text
        run([args.cffi, "verify", "--config", curl_config, curl_binding,
             "--receipt", curl_receipt])
        curl_native_libraries = [curl_library]
        if "windows" in args.target:
            curl_native_libraries.extend(
                ["ws2_32.lib", "crypt32.lib", "advapi32.lib", "normaliz.lib",
                 "wldap32.lib", "bcrypt.lib", "iphlpapi.lib", "secur32.lib",
                 zlib_library])
        else:
            curl_native_libraries = native_link_libraries(
                args.target, [curl_library, zlib_library])
        (curl_provider / "chtholly.toml").write_text(
            '[package]\nname = "curl_upgrade_provider"\nlanguage = "1.9"\n\n'
            '[build]\nmodule_paths = ["generated"]\n\n'
            '[native]\nlink_libraries = [' +
            ", ".join(quote(value) for value in curl_native_libraries) +
            ']\n\n'
            '[cffi]\nreceipt = "curl-upgrade.cffi-verify"\nrequired = true\n',
            encoding="utf-8")
        (curl_consumer / "main.cns").write_text(
            "module main; import curl_upgrade; import std::ops; "
            "fn main(): i32 { unsafe { "
            "let handle = curl_upgrade::curl_easy_init(); "
            "let pause = curl_upgrade::curl_easy_pause(handle, 0); "
            "let message = curl_upgrade::curl_easy_strerror(0); "
            "curl_upgrade::curl_easy_cleanup(handle); return 0; } }\n",
            encoding="utf-8")
        (curl_consumer / "chtholly.toml").write_text(
            '[package]\nname = "curl_upgrade_consumer"\nlanguage = "1.9"\n\n'
            f'[target]\ntriple = {quote(args.target)}\n\n'
            '[build]\nentry = "main.cns"\nmodule_paths = ["."]\n\n'
            '[dependencies]\ncurl_upgrade_provider = { path = "../provider" }\n',
            encoding="utf-8")
        run([args.chthollyc, "run", "--project", curl_consumer])
        assert_missing_native_library(
            args.chthollyc, curl_provider, curl_consumer, curl_library, "curl")
        if "windows" in args.target:
            assert_missing_native_library(
                args.chthollyc, curl_provider, curl_consumer, "ws2_32.lib",
                "curl-windows-system")

        errno_root = root / "errno-cross-package"
        errno_provider = errno_root / "provider"
        errno_consumer = errno_root / "consumer"
        errno_generated = errno_provider / "generated"
        errno_generated.mkdir(parents=True)
        errno_consumer.mkdir(parents=True)
        errno_config = errno_provider / "chtholly-cffi.toml"
        errno_binding = errno_generated / "errno_api.cfdl"
        errno_receipt = errno_provider / "errno-api.cffi-verify"
        errno_roots = [("function", "c_errno_probe"),
                       ("function", "c_code_probe"),
                       ("function", "c_status_code"),
                       ("type", "chtholly_cffi_status"),
                       ("function", "c_errno_pointer"),
                       ("function", "c_fopen_missing"),
                       ("function", "c_code_set_probe"),
                       ("function", "c_code_allowed_probe"),
                       ("function", "c_unsigned_set_probe"),
                       ("function", "c_errno_sentinel"),
                       ("function", "c_sqlite_open_invalid"),
                       ("function", "c_zlib_corrupt"),
                       ("function", "c_curl_perform_invalid"),
                       ("function", "c_posix_read_probe"),
                       ("type", "chtholly_cffi_handle"),
                       ("constant", "CHTHOLLY_CFFI_SOFT_ERROR"),
                       ("constant", "CHTHOLLY_CFFI_HARD_BEGIN"),
                       ("constant", "CHTHOLLY_CFFI_HARD_END"),
                       ("constant", "EINVAL")]
        if "windows" in args.target:
            errno_roots.extend([
                ("function", "c_bcrypt_random"),
                ("function", "c_bcrypt_buffer"),
                ("function", "c_bcrypt_get_property"),
                ("function", "c_bcrypt_get_property_invalid"),
                ("function", "c_bcrypt_get_property_small"),
                ("function", "c_bcrypt_property_buffer"),
                ("function", "ReadFile"),
                ("function", "c_win32_probe"),
                ("function", "c_win32_handle"),
                ("function", "c_win32_close_handle"),
                ("function", "c_win32_read_data_handle"),
                ("function", "c_win32_read_eof_handle"),
                ("function", "c_win32_read_invalid_handle"),
                ("function", "c_win32_read_probe_handle"),
                ("function", "c_win32_read_buffer"),
                ("function", "c_win32_read_close"),
                ("function", "c_win32_read_contract_probe"),
            ])
        else:
            errno_roots.extend([
                ("function", "c_posix_buffer"),
                ("function", "c_posix_read_into"),
                ("function", "recv"),
                ("function", "c_posix_recv_data_socket"),
                ("function", "c_posix_recv_eof_socket"),
                ("function", "c_posix_recv_invalid_socket"),
                ("function", "c_posix_recv_buffer"),
                ("function", "c_posix_recv_close"),
                ("function", "fread"),
                ("function", "feof"),
                ("function", "ferror"),
                ("function", "c_fread_data_stream"),
                ("function", "c_fread_eof_stream"),
                ("function", "c_fread_error_stream"),
                ("function", "c_fread_buffer"),
                ("function", "c_fread_close"),
            ])
        errno_config.write_text(
            config("errno_api", args.target, "fixture.h", fixture, args.cc,
                   provider, errno_roots), encoding="utf-8")
        errno_link_values = (["bcrypt.lib", "ws2_32.lib", "crypt32.lib",
                              "advapi32.lib", "normaliz.lib", "wldap32.lib",
                              "iphlpapi.lib", "secur32.lib"]
                             if "windows" in args.target else []) + [
                                 provider, real_provider, sqlite_library,
                                 zlib_library, curl_library]
        errno_link_values = native_link_libraries(
            args.target, errno_link_values)
        errno_config_text = errno_config.read_text(encoding="utf-8")
        if "windows" in args.target:
            errno_config_text = errno_config_text.replace(
                "link_arguments = [\"bcrypt.lib\"]",
                "link_arguments = [" + ", ".join(
                    quote(value) for value in errno_link_values) + "]", 1)
        else:
            # GCC resolves static archives left-to-right. Keep the probe's
            # object before all provider archives by putting the complete
            # closure in `libraries`; link_arguments are reserved for flags.
            errno_config_text = re.sub(
                r"^libraries = \[[^\n]*\]$",
                "libraries = [" + ", ".join(
                    quote(value) for value in errno_link_values) + "]",
                errno_config_text, count=1, flags=re.MULTILINE)
        errno_config.write_text(errno_config_text, encoding="utf-8")
        run([args.cffi, "generate", "--config", errno_config, "-o",
             errno_binding])
        errno_text = errno_binding.read_text(encoding="utf-8")
        handle_type = next(
            line for line in errno_text.splitlines()
            if line.startswith("foreign type chtholly_cffi_handle"))
        errno_text = errno_text.replace(
            handle_type, handle_type[:-1] + " invalid -1;")
        errno_function = next(
            line for line in errno_text.splitlines()
            if line.startswith("foreign fn c_errno_probe"))
        errno_text = errno_text.replace(
            errno_function,
            errno_function[:-1] +
            " error errno when result == -1;")
        code_function = next(
            line for line in errno_text.splitlines()
            if line.startswith("foreign fn c_code_probe"))
        errno_text = errno_text.replace(
            code_function,
            code_function[:-1] +
            " error code when result != 0;")
        status_function = next(
            line for line in errno_text.splitlines()
            if line.startswith("foreign fn c_status_code"))
        errno_text = errno_text.replace(
            status_function,
            status_function[:-1] +
            " error code when result != 0;")
        pointer_function = next(
            line for line in errno_text.splitlines()
            if line.startswith("foreign fn c_errno_pointer"))
        errno_text = errno_text.replace(
            pointer_function,
            pointer_function[:-1] +
            " error errno when result == null;")
        fopen_function = next(
            line for line in errno_text.splitlines()
            if line.startswith("foreign fn c_fopen_missing"))
        errno_text = errno_text.replace(
            fopen_function,
            fopen_function[:-1] +
            " error errno when result == null;")
        set_function = next(
            line for line in errno_text.splitlines()
            if line.startswith("foreign fn c_code_set_probe"))
        errno_text = errno_text.replace(
            set_function,
            set_function[:-1] +
            " error code when result in { "
            "CHTHOLLY_CFFI_SOFT_ERROR, "
            "CHTHOLLY_CFFI_HARD_BEGIN through "
            "CHTHOLLY_CFFI_HARD_END };" )
        allowed_function = next(
            line for line in errno_text.splitlines()
            if line.startswith("foreign fn c_code_allowed_probe"))
        errno_text = errno_text.replace(
            allowed_function,
            allowed_function[:-1] +
            " error code when result not in { 0, 1 };")
        unsigned_function = next(
            line for line in errno_text.splitlines()
            if line.startswith("foreign fn c_unsigned_set_probe"))
        errno_text = errno_text.replace(
            unsigned_function,
            unsigned_function[:-1] +
            " error code when result in { "
            "4294967294 through 4294967295 };")
        sentinel_function = next(
            line for line in errno_text.splitlines()
            if line.startswith("foreign fn c_errno_sentinel"))
        errno_text = errno_text.replace(
            sentinel_function,
            sentinel_function[:-1] +
            " error errno when result == invalid;")
        read_function = next(
            line for line in errno_text.splitlines()
            if line.startswith("foreign fn c_posix_read_probe"))
        errno_text = errno_text.replace(
            read_function,
            read_function[:-1] +
            " error errno when result == -1;")
        for failure_name in ("c_sqlite_open_invalid", "c_zlib_corrupt",
                             "c_curl_perform_invalid"):
            failure_function = next(
                line for line in errno_text.splitlines()
                if line.startswith("foreign fn " + failure_name))
            errno_text = errno_text.replace(
                failure_function,
                failure_function[:-1] + " error code when result != 0;")
        if "windows" in args.target:
            bcrypt_function = next(
                line for line in errno_text.splitlines()
                if line.startswith("foreign fn c_bcrypt_random"))
            errno_text = errno_text.replace(
                bcrypt_function,
                bcrypt_function[:-1] +
                " error code when result != 0;")
            for property_name in ("c_bcrypt_get_property",
                                  "c_bcrypt_get_property_invalid",
                                  "c_bcrypt_get_property_small"):
                property_function = next(
                    line for line in errno_text.splitlines()
                    if line.startswith("foreign fn " + property_name))
                property_function = property_function.replace(
                    "buffer: void*", "buffer: view_mut void*")
                property_function = property_function.replace(
                    "result: c_ulong*", "result: out c_ulong")
                errno_text = errno_text.replace(
                    next(line for line in errno_text.splitlines()
                         if line.startswith("foreign fn " + property_name)),
                    property_function[:-1] + " error code when result != 0;")
            read_file_function = next(
                line for line in errno_text.splitlines()
                if line.startswith("foreign fn ReadFile"))
            errno_text = errno_text.replace(
                read_file_function,
                annotate_win32_read(
                    read_file_function, "lpBuffer", "nNumberOfBytesToRead",
                    "lpNumberOfBytesRead", "lpOverlapped"))
            read_contract_function = next(
                line for line in errno_text.splitlines()
                if line.startswith("foreign fn c_win32_read_contract_probe"))
            errno_text = errno_text.replace(
                read_contract_function,
                annotate_win32_read(
                    read_contract_function, "buffer", "capacity", "count",
                    "overlapped"))
            win32_function = next(
                line for line in errno_text.splitlines()
                if line.startswith("foreign fn c_win32_probe"))
            errno_text = errno_text.replace(
                win32_function,
                win32_function[:-1] +
                " error win32 when result == 0;")
            handle_function = next(
                line for line in errno_text.splitlines()
                if line.startswith("foreign fn c_win32_handle"))
            errno_text = errno_text.replace(
                handle_function,
                handle_function[:-1] +
                " error win32 when result == invalid;")
        else:
            read_into_function = next(
                line for line in errno_text.splitlines()
                if line.startswith("foreign fn c_posix_read_into"))
            read_into_function = read_into_function.replace(
                "buffer: void*", "buffer: view_mut void*")
            errno_text = errno_text.replace(
                next(line for line in errno_text.splitlines()
                     if line.startswith("foreign fn c_posix_read_into")),
                read_into_function[:-1] +
                " outcome posix_read<u8>(buffer, capacity)"
                " error errno when result == -1;")
            recv_function = next(
                line for line in errno_text.splitlines()
                if line.startswith("foreign fn recv"))
            recv_function = recv_function.replace(
                "buffer: void*", "buffer: view_mut void*")
            errno_text = errno_text.replace(
                next(line for line in errno_text.splitlines()
                     if line.startswith("foreign fn recv")),
                recv_function[:-1] +
                " outcome posix_read<u8>(buffer, capacity)"
                " error errno when result == -1;")
            fread_function = next(
                line for line in errno_text.splitlines()
                if line.startswith("foreign fn fread"))
            fread_function = fread_function.replace(
                "buffer: void*", "buffer: view_mut void*")
            errno_text = errno_text.replace(
                next(line for line in errno_text.splitlines()
                     if line.startswith("foreign fn fread")),
                fread_function[:-1] +
                " outcome fread<u8>(buffer, element_size, element_count, stream)"
                " eof \"feof\"(stream) ferror \"ferror\"(stream)"
                " error errno when ferror != 0;")
        errno_binding.write_text(errno_text, encoding="utf-8")
        errno_config.write_text(
            config("errno_api", args.target, "fixture.h", fixture, args.cc,
                   provider,
                   errno_roots + [("constant", "CHTHOLLY_CFFI_LIMIT")]),
            encoding="utf-8")
        errno_config_text = errno_config.read_text(encoding="utf-8")
        if "windows" in args.target:
            errno_config_text = errno_config_text.replace(
                "link_arguments = [\"bcrypt.lib\"]",
                "link_arguments = [" + ", ".join(
                    quote(value) for value in errno_link_values) + "]", 1)
        else:
            errno_config_text = re.sub(
                r"^libraries = \[[^\n]*\]$",
                "libraries = [" + ", ".join(
                    quote(value) for value in errno_link_values) + "]",
                errno_config_text, count=1, flags=re.MULTILINE)
        errno_config.write_text(errno_config_text, encoding="utf-8")
        run([args.cffi, "regenerate", "--config", errno_config,
             errno_binding, "--write"])
        errno_text = errno_binding.read_text(encoding="utf-8")
        assert "error errno when result == -1" in errno_text
        assert "error code when result != 0" in errno_text
        assert "foreign fn c_sqlite_open_invalid" in errno_text
        assert "foreign fn c_zlib_corrupt" in errno_text
        assert "foreign fn c_curl_perform_invalid" in errno_text
        assert "error errno when result == null" in errno_text
        assert "invalid -1" in errno_text
        assert "error code when result in { CHTHOLLY_CFFI_SOFT_ERROR" in (
            errno_text)
        assert "error code when result not in { 0, 1 }" in errno_text
        assert "4294967294 through 4294967295" in errno_text
        assert "error errno when result == invalid" in errno_text
        assert "foreign const CHTHOLLY_CFFI_LIMIT: c_int = 7;" in errno_text
        if "windows" in args.target:
            assert "foreign fn c_bcrypt_get_property" in errno_text
            assert "foreign fn c_bcrypt_get_property_invalid" in errno_text
            assert "foreign fn c_bcrypt_get_property_small" in errno_text
            assert "result: out c_ulong" in errno_text
            assert "outcome win32_read<u8>(lpBuffer, nNumberOfBytesToRead, " in (
                errno_text)
            assert "outcome win32_read<u8>(buffer, capacity, count, " in (
                errno_text)
        else:
            assert "foreign fn recv" in errno_text
            assert "outcome posix_read<u8>(buffer, capacity)" in errno_text
            assert "outcome fread<u8>(buffer, element_size, element_count, stream)" in errno_text
            assert 'eof "feof"(stream) ferror "ferror"(stream)' in errno_text
        if "windows" in args.target:
            original_config = errno_config.read_text(encoding="utf-8")
            missing_library = root / "missing-bcrypt.lib"
            errno_config.write_text(
                original_config.replace(
                    '"bcrypt.lib"', quote(missing_library), 1),
                encoding="utf-8")
            failed = run_nonzero([
                args.cffi, "verify", "--config", errno_config,
                errno_binding, "--receipt", errno_receipt])
            diagnostic = (failed.stderr + failed.stdout).lower()
            assert "link closure" in diagnostic
            assert missing_library.name.lower() in diagnostic
            errno_config.write_text(original_config, encoding="utf-8")
            conflict_dir = root / "conflicting-bcrypt"
            conflict_dir.mkdir()
            (conflict_dir / "bcrypt.lib").write_bytes(b"not a Windows import library")
            errno_config.write_text(
                original_config.replace(
                    "library_paths = []",
                    "library_paths = [" + quote(conflict_dir) + "]", 1),
                encoding="utf-8")
            failed = run_nonzero([
                args.cffi, "verify", "--config", errno_config,
                errno_binding, "--receipt", errno_receipt])
            diagnostic = (failed.stderr + failed.stdout).lower()
            assert "conflicting candidates" in diagnostic
            errno_config.write_text(original_config, encoding="utf-8")
        run([args.cffi, "verify", "--config", errno_config, errno_binding,
             "--receipt", errno_receipt])
        errno_native_libraries = [
            provider, real_provider, sqlite_library, zlib_library, curl_library]
        if "windows" in args.target:
            errno_native_libraries[1:1] = [
                "bcrypt.lib", "ws2_32.lib", "crypt32.lib", "advapi32.lib",
                "normaliz.lib", "wldap32.lib", "iphlpapi.lib", "secur32.lib"]
        else:
            errno_native_libraries = native_link_libraries(
                args.target, errno_native_libraries)
        (errno_provider / "chtholly.toml").write_text(
            '[package]\nname = "errno_provider"\nlanguage = "1.10"\n\n'
            '[build]\nmodule_paths = ["generated"]\n\n'
            '[native]\nlink_libraries = [' +
            ", ".join(quote(value) for value in errno_native_libraries) +
            ']\n\n'
            '[cffi]\nreceipt = "errno-api.cffi-verify"\nrequired = true\n',
            encoding="utf-8")
        library_failure_checks = (
            "let sqlite_failure = errno_api::c_sqlite_open_invalid(); "
            "let sqlite_failure_status = switch (sqlite_failure) { "
            "std::result::Result<void, i32>::Ok { .. } => 101; "
            "std::result::Result<void, i32>::Err { error = copy .0 } => { "
            "if (error == 0) { return 102; } 0 }; }; "
            "if (sqlite_failure_status != 0) { return sqlite_failure_status; } "
            "let zlib_failure = errno_api::c_zlib_corrupt(); "
            "let zlib_failure_status = switch (zlib_failure) { "
            "std::result::Result<void, i32>::Ok { .. } => 103; "
            "std::result::Result<void, i32>::Err { error = copy .0 } => { "
            "if (error == 0) { return 104; } 0 }; }; "
            "if (zlib_failure_status != 0) { return zlib_failure_status; } "
            "let curl_failure = errno_api::c_curl_perform_invalid(); "
            "let curl_failure_status = switch (curl_failure) { "
            "std::result::Result<void, i32>::Ok { .. } => 105; "
            "std::result::Result<void, i32>::Err { error = copy .0 } => { "
            "if (error == 0) { return 106; } 0 }; }; "
            "if (curl_failure_status != 0) { return curl_failure_status; } ")
        bcrypt_checks = (
            "let bcrypt_buffer = errno_api::c_bcrypt_buffer(); "
            "let bcrypt_result = errno_api::c_bcrypt_random(bcrypt_buffer, 16u32); "
            "let bcrypt_status = switch (bcrypt_result) { "
            "std::result::Result<void, i32>::Ok { .. } => 0; "
            "std::result::Result<void, i32>::Err { .. } => 91; }; "
            "if (bcrypt_status != 0) { return bcrypt_status; } "
            "let property_buffer = errno_api::c_bcrypt_property_buffer(); "
            "var property_length: u32; "
            "let property_result = errno_api::c_bcrypt_get_property("
            "property_buffer, 128u32, property_length); "
            "let property_status = switch (property_result) { "
            "std::result::Result<void, i32>::Ok { .. } => 0; "
            "std::result::Result<void, i32>::Err { .. } => 92; }; "
            "if (property_status != 0) { return property_status; } "
            "var invalid_property_length: u32; "
            "let invalid_property = errno_api::c_bcrypt_get_property_invalid("
            "property_buffer, 128u32, invalid_property_length); "
            "let invalid_property_status = switch (invalid_property) { "
            "std::result::Result<void, i32>::Ok { .. } => 93; "
            "std::result::Result<void, i32>::Err { error = copy .0 } => { "
            "if (error == 0) { return 94; } 0 }; }; "
            "if (invalid_property_status != 0) { return invalid_property_status; } "
            "var small_property_length: u32; "
            "let small_property = errno_api::c_bcrypt_get_property_small("
            "property_buffer, 1u32, small_property_length); "
            "let small_property_status = switch (small_property) { "
            "std::result::Result<void, i32>::Ok { .. } => 95; "
            "std::result::Result<void, i32>::Err { error = copy .0 } => { "
            "if (error == 0) { return 96; } "
            "if (small_property_length <= 1u32) { return 97; } 0 }; }; "
            "if (small_property_status != 0) { return small_property_status; } "
            if "windows" in args.target else "")
        win32_checks = (
            "let win_ok = errno_api::c_win32_probe(0); "
            "let win_ok_status = switch (win_ok) { "
            "std::result::Result<i32, u32>::Ok { .. } => 0; "
            "std::result::Result<i32, u32>::Err { .. } => 8; }; "
            "if (win_ok_status != 0) { return win_ok_status; } "
            "let win_fail = errno_api::c_win32_probe(1); "
            "let win_fail_status = switch (win_fail) { "
            "std::result::Result<i32, u32>::Ok { .. } => 9; "
            "std::result::Result<i32, u32>::Err { error = copy .0 } => { "
            "if (error != 5u32) { return 10; } 0 }; }; "
            "if (win_fail_status != 0) { return win_fail_status; } "
            "let handle_ok = errno_api::c_win32_handle(0); "
            "let handle_ok_status = switch (handle_ok) { "
            "std::result::Result<errno_api::chtholly_cffi_handle, u32>::Ok "
            "{ value = copy .0 } => { "
            "if (errno_api::c_win32_close_handle(value) == 0) { return 16; } "
            "0 }; "
            "std::result::Result<errno_api::chtholly_cffi_handle, u32>::Err "
            "{ .. } => 17; }; "
            "if (handle_ok_status != 0) { return handle_ok_status; } "
            "let handle_fail = errno_api::c_win32_handle(1); "
            "let handle_fail_status = switch (handle_fail) { "
            "std::result::Result<errno_api::chtholly_cffi_handle, u32>::Ok "
            "{ .. } => 18; "
            "std::result::Result<errno_api::chtholly_cffi_handle, u32>::Err "
            "{ error = copy .0 } => { if (error == 0u32) { return 19; } 0 }; }; "
            "if (handle_fail_status != 0) { return handle_fail_status; } "
            "let read_buffer = errno_api::c_win32_read_buffer(); "
            "let data_handle = errno_api::c_win32_read_data_handle(); "
            "let data_read = errno_api::ReadFile(data_handle, read_buffer, 8u32); "
            "let data_status = switch (data_read) { "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, u32>::Ok "
            "{ value = copy .0 } => switch (value) { "
            "std::io::ReadOutcome<slice<u8> >::Data { value = copy .0 } => { "
            "if (value.len != 3u64) { return 46; } "
            "if (value[0u64] != 49u8) { return 47; } 0 }; "
            "std::io::ReadOutcome<slice<u8> >::Eof => 48; }; "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, u32>::Err "
            "{ .. } => 49; }; "
            "if (data_status != 0) { return data_status; } "
            "if (errno_api::c_win32_read_close(data_handle) == 0) { return 50; } "
            "let eof_handle = errno_api::c_win32_read_eof_handle(); "
            "let eof_read = errno_api::ReadFile(eof_handle, read_buffer, 8u32); "
            "let win_eof_status = switch (eof_read) { "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, u32>::Ok "
            "{ value = copy .0 } => switch (value) { "
            "std::io::ReadOutcome<slice<u8> >::Data { .. } => 51; "
            "std::io::ReadOutcome<slice<u8> >::Eof => 0; }; "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, u32>::Err "
            "{ .. } => 52; }; "
            "if (win_eof_status != 0) { return win_eof_status; } "
            "if (errno_api::c_win32_read_close(eof_handle) == 0) { return 53; } "
            "let zero_handle = errno_api::c_win32_read_data_handle(); "
            "let zero_read = errno_api::ReadFile(zero_handle, null, 0u32); "
            "let zero_status = switch (zero_read) { "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, u32>::Ok "
            "{ value = copy .0 } => switch (value) { "
            "std::io::ReadOutcome<slice<u8> >::Data { value = copy .0 } => { "
            "if (value.len != 0u64) { return 54; } 0 }; "
            "std::io::ReadOutcome<slice<u8> >::Eof => 55; }; "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, u32>::Err "
            "{ .. } => 56; }; "
            "if (zero_status != 0) { return zero_status; } "
            "if (errno_api::c_win32_read_close(zero_handle) == 0) { return 57; } "
            "let invalid_handle = errno_api::c_win32_read_invalid_handle(); "
            "let invalid_read = errno_api::ReadFile(invalid_handle, read_buffer, 8u32); "
            "let invalid_status = switch (invalid_read) { "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, u32>::Ok "
            "{ .. } => 58; "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, u32>::Err "
            "{ error = copy .0 } => { if (error != 6u32) { return 59; } 0 }; }; "
            "if (invalid_status != 0) { return invalid_status; } "
            "let probe_failure = errno_api::c_win32_read_contract_probe("
            "errno_api::c_win32_read_probe_handle(3u32), read_buffer, 8u32); "
            "let probe_failure_status = switch (probe_failure) { "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, u32>::Ok "
            "{ .. } => 60; "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, u32>::Err "
            "{ error = copy .0 } => { if (error != 22u32) { return 61; } 0 }; }; "
            "if (probe_failure_status != 0) { return probe_failure_status; } "
            if "windows" in args.target else "")
        posix_checks = (
            "let buffer = errno_api::c_posix_buffer(); "
            "let projected_short = errno_api::c_posix_read_into(buffer, 8u64, 1); "
            "let short_status = switch (projected_short) { "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Ok "
            "{ value = copy .0 } => switch (value) { "
            "std::io::ReadOutcome<slice<u8> >::Data { value = copy .0 } => { "
            "if (value.len != 3u64) { return 35; } "
            "if (value[0u64] != 49u8) { return 36; } 0 }; "
            "std::io::ReadOutcome<slice<u8> >::Eof => 37; }; "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Err "
            "{ .. } => 38; }; "
            "if (short_status != 0) { return short_status; } "
            "let projected_eof = errno_api::c_posix_read_into(buffer, 8u64, 0); "
            "let eof_status = switch (projected_eof) { "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Ok "
            "{ value = copy .0 } => switch (value) { "
            "std::io::ReadOutcome<slice<u8> >::Data { .. } => 39; "
            "std::io::ReadOutcome<slice<u8> >::Eof => 0; }; "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Err "
            "{ .. } => 40; }; "
            "if (eof_status != 0) { return eof_status; } "
            "let empty = errno_api::c_posix_read_into(null, 0u64, 0); "
            "let empty_status = switch (empty) { "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Ok "
            "{ value = copy .0 } => switch (value) { "
            "std::io::ReadOutcome<slice<u8> >::Data { value = copy .0 } => { "
            "if (value.len != 0u64) { return 41; } 0 }; "
            "std::io::ReadOutcome<slice<u8> >::Eof => 42; }; "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Err "
            "{ .. } => 43; }; "
            "if (empty_status != 0) { return empty_status; } "
            "let read_error = errno_api::c_posix_read_into(buffer, 8u64, 2); "
            "let read_error_status = switch (read_error) { "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Ok "
            "{ .. } => 44; "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Err "
            "{ error = copy .0 } => { if (error == 0) { return 45; } 0 }; }; "
            "if (read_error_status != 0) { return read_error_status; } "
            if "windows" not in args.target else "")
        recv_checks = (
            "let recv_buffer = errno_api::c_posix_recv_buffer(); "
            "let recv_data_socket = errno_api::c_posix_recv_data_socket(); "
            "let recv_data = errno_api::recv(recv_data_socket, recv_buffer, 8u64, 0); "
            "let recv_data_status = switch (recv_data) { "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Ok "
            "{ value = copy .0 } => switch (value) { "
            "std::io::ReadOutcome<slice<u8> >::Data { value = copy .0 } => { "
            "if (value.len != 3u64) { return 62; } "
            "if (value[0u64] != 49u8) { return 63; } 0 }; "
            "std::io::ReadOutcome<slice<u8> >::Eof => 64; }; "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Err "
            "{ .. } => 65; }; "
            "if (recv_data_status != 0) { return recv_data_status; } "
            "if (errno_api::c_posix_recv_close(recv_data_socket) == 0) { return 66; } "
            "let recv_eof_socket = errno_api::c_posix_recv_eof_socket(); "
            "let recv_eof = errno_api::recv(recv_eof_socket, recv_buffer, 8u64, 0); "
            "let recv_eof_status = switch (recv_eof) { "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Ok "
            "{ value = copy .0 } => switch (value) { "
            "std::io::ReadOutcome<slice<u8> >::Data { .. } => 67; "
            "std::io::ReadOutcome<slice<u8> >::Eof => 0; }; "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Err "
            "{ .. } => 68; }; "
            "if (recv_eof_status != 0) { return recv_eof_status; } "
            "if (errno_api::c_posix_recv_close(recv_eof_socket) == 0) { return 69; } "
            "let recv_empty_socket = errno_api::c_posix_recv_data_socket(); "
            "let recv_empty = errno_api::recv(recv_empty_socket, null, 0u64, 0); "
            "let recv_empty_status = switch (recv_empty) { "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Ok "
            "{ value = copy .0 } => switch (value) { "
            "std::io::ReadOutcome<slice<u8> >::Data { value = copy .0 } => { "
            "if (value.len != 0u64) { return 70; } 0 }; "
            "std::io::ReadOutcome<slice<u8> >::Eof => 71; }; "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Err "
            "{ .. } => 72; }; "
            "if (recv_empty_status != 0) { return recv_empty_status; } "
            "if (errno_api::c_posix_recv_close(recv_empty_socket) == 0) { return 73; } "
            "let recv_invalid = errno_api::recv(-1, recv_buffer, 8u64, 0); "
            "let recv_invalid_status = switch (recv_invalid) { "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Ok "
            "{ .. } => 74; "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Err "
            "{ error = copy .0 } => { if (error == 0) { return 75; } 0 }; }; "
            "if (recv_invalid_status != 0) { return recv_invalid_status; } "
            if "windows" not in args.target else "")
        fread_checks = (
            "let fread_buffer = errno_api::c_fread_buffer(); "
            "let fread_data_stream = errno_api::c_fread_data_stream(); "
            "let fread_data = errno_api::fread(fread_buffer, 1u64, 8u64, "
            "fread_data_stream); "
            "let fread_data_status = switch (fread_data) { "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Ok "
            "{ value = copy .0 } => switch (value) { "
            "std::io::ReadOutcome<slice<u8> >::Data { value = copy .0 } => { "
            "if (value.len != 8u64) { return 76; } "
            "if (value[0u64] != 49u8) { return 77; } 0 }; "
            "std::io::ReadOutcome<slice<u8> >::Eof => 78; }; "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Err "
            "{ .. } => 79; }; "
            "if (fread_data_status != 0) { return fread_data_status; } "
            "if (errno_api::c_fread_close(fread_data_stream) == 0) { return 80; } "
            "let fread_eof_stream = errno_api::c_fread_eof_stream(); "
            "let fread_eof = errno_api::fread(fread_buffer, 1u64, 8u64, fread_eof_stream); "
            "let fread_eof_status = switch (fread_eof) { "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Ok "
            "{ value = copy .0 } => switch (value) { "
            "std::io::ReadOutcome<slice<u8> >::Data { .. } => 81; "
            "std::io::ReadOutcome<slice<u8> >::Eof => 0; }; "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Err "
            "{ .. } => 82; }; "
            "if (fread_eof_status != 0) { return fread_eof_status; } "
            "if (errno_api::c_fread_close(fread_eof_stream) == 0) { return 83; } "
            "let fread_empty_stream = errno_api::c_fread_data_stream(); "
            "let fread_empty = errno_api::fread(null, 0u64, 0u64, fread_empty_stream); "
            "let fread_empty_status = switch (fread_empty) { "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Ok "
            "{ value = copy .0 } => switch (value) { "
            "std::io::ReadOutcome<slice<u8> >::Data { value = copy .0 } => { "
            "if (value.len != 0u64) { return 84; } 0 }; "
            "std::io::ReadOutcome<slice<u8> >::Eof => 85; }; "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Err "
            "{ .. } => 86; }; "
            "if (fread_empty_status != 0) { return fread_empty_status; } "
            "if (errno_api::c_fread_close(fread_empty_stream) == 0) { return 87; } "
            "let fread_error_stream = errno_api::c_fread_error_stream(); "
            "let fread_error = errno_api::fread(fread_buffer, 1u64, 8u64, fread_error_stream); "
            "let fread_error_status = switch (fread_error) { "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Ok "
            "{ .. } => 88; "
            "std::result::Result<std::io::ReadOutcome<slice<u8> >, i32>::Err "
            "{ error = copy .0 } => { if (error == 0) { return 89; } 0 }; }; "
            "if (fread_error_status != 0) { return fread_error_status; } "
            "if (errno_api::c_fread_close(fread_error_stream) == 0) { return 90; } "
            if "windows" not in args.target else "")
        (errno_consumer / "main.cns").write_text(
            "module main; import errno_api; import std::result; import std::io; "
            "fn main(): i32 { unsafe { "
            "let ok = errno_api::c_errno_probe(0); "
            "let value = switch (ok) { "
            "std::result::Result<i32, i32>::Ok { value = copy .0 } => value; "
            "std::result::Result<i32, i32>::Err { .. } => 0; }; "
            "if (value != 41) { return 1; } "
            "let failed = errno_api::c_errno_probe(1); "
            "switch (failed) { "
            "std::result::Result<i32, i32>::Ok { .. } => 2; "
            "std::result::Result<i32, i32>::Err { error = copy .0 } => { "
            "if (error != errno_api::EINVAL) { return 3; } 0 }; }; "
            "let code_ok = errno_api::c_code_probe(0); "
            "let code_ok_status = switch (code_ok) { "
            "std::result::Result<void, i32>::Ok { .. } => 0; "
            "std::result::Result<void, i32>::Err { .. } => 4; }; "
            "if (code_ok_status != 0) { return code_ok_status; } "
            "let code_fail = errno_api::c_code_probe(-27); "
            "let code_fail_status = switch (code_fail) { "
            "std::result::Result<void, i32>::Ok { .. } => 5; "
            "std::result::Result<void, i32>::Err { error = copy .0 } => { "
            "if (error != -27) { return 6; } 0 }; }; "
            "if (code_fail_status != 0) { return code_fail_status; } "
            "let enum_fail = errno_api::c_status_code("
            "errno_api::CHTHOLLY_CFFI_FAILED); "
            "let enum_status = switch (enum_fail) { "
            "std::result::Result<void, i32>::Ok "
            "{ .. } => 14; "
            "std::result::Result<void, i32>::Err "
            "{ .. } => 0; }; "
            "if (enum_status != 0) { return enum_status; } "
            "let pointer_ok = errno_api::c_errno_pointer(0); "
            "let pointer_ok_status = switch (pointer_ok) { "
            "std::result::Result<void*, i32>::Ok { .. } => 0; "
            "std::result::Result<void*, i32>::Err { .. } => 11; }; "
            "if (pointer_ok_status != 0) { return pointer_ok_status; } "
            "let pointer_fail = errno_api::c_errno_pointer(1); "
            "let pointer_status = switch (pointer_fail) { "
            "std::result::Result<void*, i32>::Ok { .. } => 7; "
            "std::result::Result<void*, i32>::Err { error = copy .0 } => { "
            "if (error != errno_api::EINVAL) { return 7; } 0 }; }; "
            "if (pointer_status != 0) { return pointer_status; } " +
            "let fopen_fail = errno_api::c_fopen_missing(); "
            "let fopen_status = switch (fopen_fail) { "
            "std::result::Result<void*, i32>::Ok { .. } => 12; "
            "std::result::Result<void*, i32>::Err { error = copy .0 } => { "
            "if (error == 0) { return 13; } 0 }; }; "
            "if (fopen_status != 0) { return fopen_status; } " +
            "let set_ok = errno_api::c_code_set_probe(0); "
            "let set_ok_status = switch (set_ok) { "
            "std::result::Result<void, i32>::Ok { .. } => 0; "
            "std::result::Result<void, i32>::Err { .. } => 20; }; "
            "if (set_ok_status != 0) { return set_ok_status; } "
            "let set_fail = errno_api::c_code_set_probe(5); "
            "let set_fail_status = switch (set_fail) { "
            "std::result::Result<void, i32>::Ok { .. } => 21; "
            "std::result::Result<void, i32>::Err { error = copy .0 } => { "
            "if (error != 5) { return 22; } 0 }; }; "
            "if (set_fail_status != 0) { return set_fail_status; } "
            "let allowed_ok = errno_api::c_code_allowed_probe(1); "
            "let allowed_ok_status = switch (allowed_ok) { "
            "std::result::Result<void, i32>::Ok { .. } => 0; "
            "std::result::Result<void, i32>::Err { .. } => 23; }; "
            "if (allowed_ok_status != 0) { return allowed_ok_status; } "
            "let allowed_fail = errno_api::c_code_allowed_probe(3); "
            "let allowed_fail_status = switch (allowed_fail) { "
            "std::result::Result<void, i32>::Ok { .. } => 24; "
            "std::result::Result<void, i32>::Err { error = copy .0 } => { "
            "if (error != 3) { return 25; } 0 }; }; "
            "if (allowed_fail_status != 0) { return allowed_fail_status; } "
            "let unsigned_ok = errno_api::c_unsigned_set_probe(0u32); "
            "let unsigned_ok_status = switch (unsigned_ok) { "
            "std::result::Result<void, u32>::Ok { .. } => 0; "
            "std::result::Result<void, u32>::Err { .. } => 32; }; "
            "if (unsigned_ok_status != 0) { return unsigned_ok_status; } "
            "let unsigned_fail = "
            "errno_api::c_unsigned_set_probe(4294967295u32); "
            "let unsigned_fail_status = switch (unsigned_fail) { "
            "std::result::Result<void, u32>::Ok { .. } => 33; "
            "std::result::Result<void, u32>::Err { error = copy .0 } => { "
            "if (error != 4294967295u32) { return 34; } 0 }; }; "
            "if (unsigned_fail_status != 0) { return unsigned_fail_status; } "
            "let sentinel_fail = errno_api::c_errno_sentinel(); "
            "let sentinel_status = switch (sentinel_fail) { "
            "std::result::Result<errno_api::chtholly_cffi_handle, i32>::Ok "
            "{ .. } => 26; "
            "std::result::Result<errno_api::chtholly_cffi_handle, i32>::Err "
            "{ error = copy .0 } => { if (error == 0) { return 27; } 0 }; }; "
            "if (sentinel_status != 0) { return sentinel_status; } "
            "let eof = errno_api::c_posix_read_probe(0); "
            "let eof_value = switch (eof) { "
            "std::result::Result<i32, i32>::Ok { value = copy .0 } => value; "
            "std::result::Result<i32, i32>::Err { .. } => -2; }; "
            "if (eof_value != 0) { return 28; } "
            "let short_read = errno_api::c_posix_read_probe(1); "
            "let short_value = switch (short_read) { "
            "std::result::Result<i32, i32>::Ok { value = copy .0 } => value; "
            "std::result::Result<i32, i32>::Err { .. } => -2; }; "
            "if (short_value != 3) { return 29; } "
            "let read_fail = errno_api::c_posix_read_probe(2); "
            "let read_fail_status = switch (read_fail) { "
            "std::result::Result<i32, i32>::Ok { .. } => 30; "
            "std::result::Result<i32, i32>::Err { error = copy .0 } => { "
            "if (error == 0) { return 31; } 0 }; }; "
            "if (read_fail_status != 0) { return read_fail_status; } " +
            library_failure_checks + bcrypt_checks + win32_checks + posix_checks + recv_checks + fread_checks +
            "return 0; } }\n", encoding="utf-8")
        (errno_consumer / "chtholly.toml").write_text(
            '[package]\nname = "errno_consumer"\nlanguage = "1.10"\n\n'
            f'[target]\ntriple = {quote(args.target)}\n\n'
            '[build]\nentry = "main.cns"\nmodule_paths = ["."]\n\n'
            '[dependencies]\nerrno_provider = { path = "../provider" }\n',
            encoding="utf-8")
        run([args.chthollyc, "run", "--project", errno_consumer])
        if "windows" in args.target:
            assert_missing_native_library(
                args.chthollyc, errno_provider, errno_consumer,
                "bcrypt.lib", "bcrypt")

        if "windows" not in args.target:
            for name, call in (
                    ("count_exceeds_capacity",
                     "errno_api::c_posix_read_into(buffer, 8u64, 4)"),
                    ("unexpected_negative",
                     "errno_api::c_posix_read_into(buffer, 8u64, 5)"),
                    ("positive_null_buffer",
                     "errno_api::c_posix_read_into(null, 8u64, 6)"),
                    ("capacity_exceeds_result_domain",
                     "errno_api::c_posix_read_into(buffer, 9223372036854775808u64, 0)")):
                trap_consumer = errno_root / f"trap-{name}"
                trap_consumer.mkdir()
                (trap_consumer / "main.cns").write_text(
                    "module main; import errno_api; import std::result; "
                    "import std::io; fn main(): i32 { unsafe { "
                    "let buffer = errno_api::c_posix_buffer(); let ignored = "
                    f"{call}; return 0; }} }}\n", encoding="utf-8")
                (trap_consumer / "chtholly.toml").write_text(
                    f'[package]\nname = "trap_{name}"\nlanguage = "1.10"\n\n'
                    f'[target]\ntriple = {quote(args.target)}\n\n'
                    '[build]\nentry = "main.cns"\nmodule_paths = ["."]\n\n'
                    '[dependencies]\nerrno_provider = { path = "../provider" }\n',
                    encoding="utf-8")
                run_nonzero([args.chthollyc, "run", "--project",
                             trap_consumer])

            for name, imports in (
                    ("missing_io", "import std::result;"),
                    ("missing_result_for_outcome", "import std::io;")):
                missing_outcome = errno_root / name
                missing_outcome.mkdir()
                (missing_outcome / "main.cns").write_text(
                    f"module main; import errno_api; {imports} "
                    "fn main(): i32 { unsafe { let buffer = "
                    "errno_api::c_posix_buffer(); let ignored = "
                    "errno_api::c_posix_read_into(buffer, 8u64, 0); "
                    "return 0; } }\n", encoding="utf-8")
                (missing_outcome / "chtholly.toml").write_text(
                    f'[package]\nname = "{name}"\nlanguage = "1.10"\n\n'
                    f'[target]\ntriple = {quote(args.target)}\n\n'
                    '[build]\nentry = "main.cns"\nmodule_paths = ["."]\n\n'
                    '[dependencies]\nerrno_provider = { path = "../provider" }\n',
                    encoding="utf-8")
                rejected = run([args.chthollyc, "check", "--project",
                                missing_outcome], 1)
                assert "foreign POSIX outcome requires imports of std::io and std::result" in (
                    rejected.stderr)

            valid_read_line = next(
                line for line in errno_text.splitlines()
                if line.startswith("foreign fn c_posix_read_into"))
            invalid_outcomes = {
                "bad_buffer_lane": valid_read_line.replace(
                    "buffer: view_mut void*", "buffer: void*"),
                "bad_element": valid_read_line.replace(
                    "posix_read<u8>", "posix_read<u16>"),
                "bad_error_contract": valid_read_line.replace(
                    "result == -1", "result == -2"),
            }
            for name, invalid_line in invalid_outcomes.items():
                invalid_root = errno_root / name
                invalid_root.mkdir()
                (invalid_root / "errno_api.cfdl").write_text(
                    errno_text.replace(valid_read_line, invalid_line),
                    encoding="utf-8")
                (invalid_root / "main.cns").write_text(
                    "module main; import errno_api; fn main(): i32 { return 0; }\n",
                    encoding="utf-8")
                (invalid_root / "chtholly.toml").write_text(
                    f'[package]\nname = "{name}"\nlanguage = "1.10"\n\n'
                    f'[target]\ntriple = {quote(args.target)}\n\n'
                    '[build]\nentry = "main.cns"\nmodule_paths = ["."]\n',
                    encoding="utf-8")
                rejected = run([args.chthollyc, "check", "--project",
                                invalid_root], 1)
                assert "CFDL" in rejected.stderr

        if "windows" in args.target:
            for name, call in (
                    ("win32_count_exceeds_capacity",
                     "errno_api::c_win32_read_contract_probe("
                     "errno_api::c_win32_read_probe_handle(1u32), buffer, 8u32)"),
                    ("win32_positive_null_buffer",
                     "errno_api::c_win32_read_contract_probe("
                     "errno_api::c_win32_read_probe_handle(2u32), null, 8u32)")):
                trap_consumer = errno_root / f"trap-{name}"
                trap_consumer.mkdir()
                (trap_consumer / "main.cns").write_text(
                    "module main; import errno_api; import std::result; "
                    "import std::io; fn main(): i32 { unsafe { let buffer = "
                    "errno_api::c_win32_read_buffer(); let ignored = "
                    f"{call}; return 0; }} }}\n", encoding="utf-8")
                (trap_consumer / "chtholly.toml").write_text(
                    f'[package]\nname = "trap_{name}"\nlanguage = "1.10"\n\n'
                    f'[target]\ntriple = {quote(args.target)}\n\n'
                    '[build]\nentry = "main.cns"\nmodule_paths = ["."]\n\n'
                    '[dependencies]\nerrno_provider = { path = "../provider" }\n',
                    encoding="utf-8")
                run_nonzero([args.chthollyc, "run", "--project",
                             trap_consumer])

            valid_read_line = next(
                line for line in errno_text.splitlines()
                if line.startswith("foreign fn ReadFile"))
            invalid_win32_outcomes = {
                "win32_bad_buffer_lane": valid_read_line.replace(
                    "lpBuffer: view_mut void*", "lpBuffer: void*"),
                "win32_count_not_out": re.sub(
                    r"lpNumberOfBytesRead: out ([^,)]*)",
                    r"lpNumberOfBytesRead: \1*", valid_read_line),
                "win32_signed_count": re.sub(
                    r"lpNumberOfBytesRead: out [^,)]*",
                    "lpNumberOfBytesRead: out c_int", valid_read_line),
                "win32_wrong_capacity": re.sub(
                    r"nNumberOfBytesToRead: [^,)]*",
                    "nNumberOfBytesToRead: c_ushort", valid_read_line),
                "win32_wrong_context": re.sub(
                    r"lpOverlapped: [^,)]*", "lpOverlapped: c_uint",
                    valid_read_line),
                "win32_bad_error": valid_read_line.replace(
                    "error win32 when result == 0",
                    "error errno when result == 0"),
                "win32_duplicate_hidden": valid_read_line.replace(
                    "lpNumberOfBytesRead, lpOverlapped)",
                    "lpNumberOfBytesRead, lpNumberOfBytesRead)"),
            }
            for name, invalid_line in invalid_win32_outcomes.items():
                invalid_root = errno_root / name
                invalid_root.mkdir()
                (invalid_root / "errno_api.cfdl").write_text(
                    errno_text.replace(valid_read_line, invalid_line),
                    encoding="utf-8")
                (invalid_root / "main.cns").write_text(
                    "module main; import errno_api; fn main(): i32 { return 0; }\n",
                    encoding="utf-8")
                (invalid_root / "chtholly.toml").write_text(
                    f'[package]\nname = "{name}"\nlanguage = "1.10"\n\n'
                    f'[target]\ntriple = {quote(args.target)}\n\n'
                    '[build]\nentry = "main.cns"\nmodule_paths = ["."]\n',
                    encoding="utf-8")
                rejected = run([args.chthollyc, "check", "--project",
                                invalid_root], 1)
                assert "CFDL" in rejected.stderr

            linux_rejection = errno_root / "win32-linux-target"
            linux_rejection.mkdir()
            (linux_rejection / "errno_api.cfdl").write_text(
                errno_text, encoding="utf-8")
            (linux_rejection / "main.cns").write_text(
                "module main; import errno_api; fn main(): i32 { return 0; }\n",
                encoding="utf-8")
            (linux_rejection / "chtholly.toml").write_text(
                '[package]\nname = "win32_linux_target"\nlanguage = "1.10"\n\n'
                '[target]\ntriple = "x86_64-unknown-linux-gnu"\n\n'
                '[build]\nentry = "main.cns"\nmodule_paths = ["."]\n',
                encoding="utf-8")
            rejected = run([args.chthollyc, "check", "--project",
                            linux_rejection], 1)
            assert "CFDL" in rejected.stderr

        missing_result = errno_root / "missing-result-consumer"
        missing_result.mkdir()
        (missing_result / "main.cns").write_text(
            "module main; import errno_api; fn main(): i32 { unsafe { "
            "let ignored = errno_api::c_code_probe(0); return 0; } }\n",
            encoding="utf-8")
        (missing_result / "chtholly.toml").write_text(
            '[package]\nname = "missing_result_consumer"\nlanguage = "1.10"\n\n'
            f'[target]\ntriple = {quote(args.target)}\n\n'
            '[build]\nentry = "main.cns"\nmodule_paths = ["."]\n\n'
            '[dependencies]\nerrno_provider = { path = "../provider" }\n',
            encoding="utf-8")
        missing_result_run = run(
            [args.chthollyc, "run", "--project", missing_result], 1)
        assert "foreign error contract requires an import of std::result" in (
            missing_result_run.stderr)

        if "windows" not in args.target:
            rejected_root = root / "win32-error-rejected"
            rejected_provider = rejected_root / "provider"
            rejected_consumer = rejected_root / "consumer"
            rejected_provider.mkdir(parents=True)
            rejected_consumer.mkdir(parents=True)
            rejected_config = rejected_provider / "chtholly-cffi.toml"
            rejected_binding = rejected_provider / "win32_error.cfdl"
            rejected_config.write_text(
                config("win32_error", args.target, "fixture.h", fixture,
                       args.cc, provider,
                       [("function", "c_win32_probe")]), encoding="utf-8")
            run([args.cffi, "generate", "--config", rejected_config, "-o",
                 rejected_binding])
            rejected_text = rejected_binding.read_text(encoding="utf-8")
            rejected_function = next(
                line for line in rejected_text.splitlines()
                if line.startswith("foreign fn c_win32_probe"))
            rejected_binding.write_text(
                rejected_text.replace(
                    rejected_function, rejected_function[:-1] +
                    " error win32 when result == 0;"),
                encoding="utf-8")
            (rejected_provider / "chtholly.toml").write_text(
                '[package]\nname = "win32_error_provider"\nlanguage = "1.9"\n\n'
                '[build]\nmodule_paths = ["."]\n\n'
                f'[native]\nlink_libraries = [{quote(provider)}]\n',
                encoding="utf-8")
            (rejected_consumer / "main.cns").write_text(
                "module main; import win32_error; fn main(): i32 { return 0; }\n",
                encoding="utf-8")
            (rejected_consumer / "chtholly.toml").write_text(
                '[package]\nname = "win32_error_consumer"\nlanguage = "1.9"\n\n'
                f'[target]\ntriple = {quote(args.target)}\n\n'
                '[build]\nentry = "main.cns"\nmodule_paths = ["."]\n\n'
                '[dependencies]\nwin32_error_provider = { path = "../provider" }\n',
                encoding="utf-8")
            rejected = run([args.chthollyc, "run", "--project",
                            rejected_consumer], 1)
            assert "invalid CFDL foreign callable" in rejected.stderr

    if args.evidence_output:
        cases = [
            {"id": "sqlite-upgrade", "valid": True},
            {"id": "zlib-upgrade", "valid": True},
            {"id": "libcurl-upgrade", "valid": True},
            {"id": "windows-sdk-outcomes" if "windows" in args.target
             else "linux-posix-outcomes", "valid": True},
        ]
        evidence = {
            "schema": "chtholly-cffi-tier1-evidence-v1",
            "target": args.target,
            "phases": [
                "doctor", "generate", "regenerate", "verify",
                "independent-consumer", "native-execution",
                "native-failure", "warm-reuse",
            ],
            "cases": cases,
            "valid": all(case["valid"] for case in cases),
        }
        args.evidence_output.parent.mkdir(parents=True, exist_ok=True)
        args.evidence_output.write_text(
            json.dumps(evidence, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
