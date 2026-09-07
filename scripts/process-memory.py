#!/usr/bin/env python3
"""Report a child process' peak resident memory in a stable JSON shape.

The helper intentionally performs one observation per invocation.  The build
baseline driver imports :func:`measure_process` and samples a running child at
its fixed interval so the helper itself remains useful in small diagnostics
and shell scripts.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import re
import sys
from pathlib import Path
from typing import Final


SCHEMA: Final = "chtholly-process-memory-v1"
_KB: Final = 1024
_STATUS_VALUE = re.compile(r"^(VmHWM|VmRSS):\s+(\d+)\s+kB\s*$", re.MULTILINE)


def _linux_memory(pid: int) -> tuple[int, str] | None:
    """Return peak/current RSS from procfs, or None when procfs is absent."""

    status_path = Path("/proc") / str(pid) / "status"
    if not Path("/proc").is_dir():
        return None
    try:
        text = status_path.read_text(encoding="utf-8")
    except FileNotFoundError as exc:
        raise RuntimeError(f"process {pid} no longer exists") from exc
    except OSError as exc:
        raise RuntimeError(f"cannot read {status_path}: {exc}") from exc
    values = {name: int(value) * _KB for name, value in _STATUS_VALUE.findall(text)}
    # VmHWM is the kernel's peak resident set size.  A few procfs-compatible
    # environments expose only VmRSS; report that current value rather than
    # silently inventing a peak.
    value = values.get("VmHWM") or values.get("VmRSS")
    if value is None or value <= 0:
        raise RuntimeError(f"procfs status has no positive RSS for process {pid}")
    return value, "proc-status"


def _windows_memory(pid: int) -> tuple[int, str] | None:
    """Return PeakWorkingSetSize through the documented Windows API."""

    if os.name != "nt":
        return None
    try:
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
    except (AttributeError, OSError):
        return None

    process_query_limited_information = 0x1000
    kernel32.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel32.CloseHandle.restype = ctypes.c_int

    class ProcessMemoryCounters(ctypes.Structure):
        _fields_ = [
            ("cb", ctypes.c_uint32),
            ("PageFaultCount", ctypes.c_uint32),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
        ]

    psapi.GetProcessMemoryInfo.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ProcessMemoryCounters),
        ctypes.c_uint32,
    ]
    psapi.GetProcessMemoryInfo.restype = ctypes.c_int
    handle = kernel32.OpenProcess(process_query_limited_information, 0, pid)
    if not handle:
        error = ctypes.get_last_error()
        raise RuntimeError(f"OpenProcess({pid}) failed with Win32 error {error}")
    try:
        counters = ProcessMemoryCounters()
        counters.cb = ctypes.sizeof(counters)
        if not psapi.GetProcessMemoryInfo(
            handle, ctypes.byref(counters), ctypes.sizeof(counters)
        ):
            error = ctypes.get_last_error()
            raise RuntimeError(
                f"GetProcessMemoryInfo({pid}) failed with Win32 error {error}"
            )
        value = int(counters.PeakWorkingSetSize)
        if value <= 0:
            raise RuntimeError(f"Windows process {pid} has no positive RSS")
        return value, "get-process-memory-info"
    finally:
        kernel32.CloseHandle(handle)


def measure_process(pid: int) -> tuple[int | None, str]:
    """Measure ``pid`` and return ``(bytes, source)``.

    ``unsupported`` is reserved for a host with no supported platform API. A
    missing process or malformed platform response is an operational error so
    callers do not mistake an invalid sample for valid evidence.
    """

    if pid <= 0:
        raise ValueError("pid must be positive")
    windows = _windows_memory(pid)
    if windows is not None:
        return windows
    linux = _linux_memory(pid)
    if linux is not None:
        return linux
    return None, "unsupported"


def main() -> int:
    parser = argparse.ArgumentParser(description="Measure process peak RSS")
    parser.add_argument("--pid", type=int, required=True)
    args = parser.parse_args()
    value, source = measure_process(args.pid)
    print(json.dumps({
        "schema": SCHEMA,
        "peak_rss_bytes": value,
        "source": source,
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"process memory measurement failed: {error}", file=sys.stderr)
        raise SystemExit(1)
