#!/usr/bin/env python3

import argparse
import json
import pathlib


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", required=True, type=pathlib.Path)
    args = parser.parse_args()
    extension = args.source_dir / "editors" / "vscode"
    package = json.loads((extension / "package.json").read_text(encoding="utf-8"))
    contributes = package["contributes"]

    commands = {entry["command"] for entry in contributes["commands"]}
    expected = {
        "chtholly.check",
        "chtholly.build",
        "chtholly.run",
        "chtholly.doctor",
    }
    if commands != expected:
        raise AssertionError(f"unexpected VS Code command surface: {commands!r}")
    if "chtholly.compiler.path" not in contributes["configuration"]["properties"]:
        raise AssertionError("compiler path configuration is missing")
    matcher = contributes["problemMatchers"][0]
    if matcher["name"] != "chtholly" or matcher["pattern"]["code"] != 6:
        raise AssertionError(f"invalid Chtholly problem matcher: {matcher!r}")

    source = (extension / "extension.js").read_text(encoding="utf-8")
    for command in expected:
        action = command.split(".", 1)[1]
        if f'"{action}"' not in source:
            raise AssertionError(f"extension does not register {command}")
    if "ProcessExecution" not in source or '"$chtholly"' not in source:
        raise AssertionError("extension commands do not use the compiler task contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
