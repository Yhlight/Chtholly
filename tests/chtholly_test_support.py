"""Shared Python helpers for tests executed by chtholly-test."""

from __future__ import annotations

import os
import pathlib
import subprocess
from typing import Iterable, Mapping


def target_triple() -> str:
    """Return the native target used by the current test host."""
    if os.name == "nt":
        return "x86_64-pc-windows-msvc"
    if os.uname().sysname == "Linux":
        return "x86_64-unknown-linux-gnu"
    if os.uname().sysname == "Darwin":
        return "aarch64-apple-darwin"
    raise RuntimeError(f"unsupported test host: {os.name}")


def executable_suffix() -> str:
    return ".exe" if os.name == "nt" else ""


def native_exit_code(code: int) -> int:
    """Normalize a process result to the host's exit-code width."""
    return code if os.name == "nt" else code & 0xFF


def native_executables(root: pathlib.Path) -> list[pathlib.Path]:
    """Find native program artifacts without assuming a Windows suffix."""
    if os.name == "nt":
        return sorted(path for path in root.rglob("*.exe") if path.is_file())
    return sorted(
        path for path in root.rglob("*")
        if path.is_file() and os.access(path, os.X_OK)
    )


def single_native_executable(
    root: pathlib.Path, description: str = "project"
) -> pathlib.Path:
    candidates = native_executables(root)
    if len(candidates) != 1:
        raise AssertionError(
            f"{description} produced {len(candidates)} native executables: "
            f"{candidates}")
    return candidates[0]


def run(
    command: Iterable[str | pathlib.Path],
    expected: int | Iterable[int] = 0,
    *,
    env: Mapping[str, str] | None = None,
    cwd: pathlib.Path | None = None,
) -> subprocess.CompletedProcess[str]:
    command_list = [str(argument) for argument in command]
    expected_codes = (expected,) if isinstance(expected, int) else tuple(expected)
    result = subprocess.run(
        command_list, cwd=cwd, env=env, text=True, encoding="utf-8",
        errors="replace", stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode not in expected_codes:
        raise AssertionError(
            f"command returned {result.returncode}, expected {expected_codes}: "
            f"{command_list!r}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def run_nonzero(
    command: Iterable[str | pathlib.Path],
    *,
    env: Mapping[str, str] | None = None,
    cwd: pathlib.Path | None = None,
) -> subprocess.CompletedProcess[str]:
    command_list = [str(argument) for argument in command]
    result = subprocess.run(
        command_list, cwd=cwd, env=env, text=True, encoding="utf-8",
        errors="replace", stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode == 0:
        raise AssertionError(
            f"command unexpectedly succeeded: {command_list!r}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result
