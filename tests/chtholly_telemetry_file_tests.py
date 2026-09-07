"""Real file processing through the Chtholly telemetry application."""
import argparse
import pathlib
import shutil
import struct
import tempfile
from chtholly_test_support import run, single_native_executable

parser = argparse.ArgumentParser()
parser.add_argument("--chthollyc", required=True)
parser.add_argument("--source-dir", type=pathlib.Path, required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="chtholly-telemetry-file-") as raw:
    root = pathlib.Path(raw)
    workspace = root / "workspace"
    shutil.copytree(args.source_dir / "examples/telemetry-pipeline", workspace,
                    ignore=shutil.ignore_patterns(".chtholly", "chtholly.lock"))
    run([args.chthollyc, "build", "--workspace", workspace])
    executable = single_native_executable(workspace / ".chtholly/build", "telemetry")
    source, output = root / "input.bin", root / "output.bin"
    frames = b"".join(struct.pack("<4sHHIQiiQ", b"CHTM", 1, 0, 24,
                                   index + 10, index % 3, index - 30, index)
                       for index in range(70))
    source.write_bytes(frames)
    output.write_bytes(b"stale" * 1000)
    run([executable, source, output])
    if output.read_bytes() != frames:
        raise AssertionError(f"file output mismatch: sizes={len(output.read_bytes())}/{len(frames)} first={output.read_bytes()[:72].hex()}/{frames[:72].hex()}")
    source.write_bytes(b"")
    run([executable, source, output])
    if output.read_bytes():
        raise AssertionError("empty input produced output")
    source.write_bytes(frames + b"CHT")
    run([executable, source, output], expected=12)
    source.write_bytes(b"NOPE" + frames[4:])
    run([executable, source, output], expected=12)
    missing = root / "missing.bin"
    run([executable, missing, output], expected=10)
    if missing.exists():
        raise AssertionError("read-only open created the missing input")
    source.write_bytes(frames)
    run([executable, source, root / "missing-directory/output.bin"], expected=11)
    # Successful reuse after failures catches leaked exclusivity/file handles.
    run([executable, source, output])
    if output.read_bytes() != frames:
        raise AssertionError("recovery changed output")
