"""Native ownership checks for the value-returning channel API."""
import argparse
import pathlib
import tempfile
from chtholly_test_support import run, run_nonzero, single_native_executable

parser = argparse.ArgumentParser()
parser.add_argument("--chthollyc", required=True)
args = parser.parse_args()
PREFIX = '''module main;
import std::typed_channel;
import std::result;
import std::error;
import std::io;
lifecycle(copy = delete, move = default, drop = custom)
struct Owned { pub value: i32; }
impl Owned {
  fn drop(self: Owned&): void { std::io::write_stdout("drop\\n"); }
}
alias Channel = std::typed_channel::Channel<Owned>;
alias Sent = std::result::Result<void, std::typed_channel::SendError<Owned> >;
alias Received = std::result::Result<Owned, std::error::ErrorCode>;
'''
CASES = {
    "recover": ('''fn main(): i32 {
  var empty = std::typed_channel::Channel<Owned>::empty();
  let failed = empty.send(Owned { .value = 42 });
  let recovered = switch (failed) {
    std::result::Result<void, std::typed_channel::SendError<Owned> >::Ok { .. } => { return 1; };
    std::result::Result<void, std::typed_channel::SendError<Owned> >::Err { error = move .0 } => move error.value;
  };
  var channel = std::typed_channel::Channel<Owned>::empty();
  let initialized = channel.init(1u64);
  let sent = channel.send(move recovered);
  let received = channel.receive();
  return switch (received) {
    std::result::Result<Owned, std::error::ErrorCode>::Ok { value = move .0 } => if (value.value == 42) { 0 } else { 2 };
    std::result::Result<Owned, std::error::ErrorCode>::Err { .. } => 3;
  };
}''', "drop\n"),
    "discard_error": ('''fn main(): i32 {
  var channel = std::typed_channel::Channel<Owned>::empty();
  let failed = channel.send(Owned { .value = 7 });
  return 0;
}''', "drop\n"),
    "receive_error": ('''fn main(): i32 {
  var channel = std::typed_channel::Channel<Owned>::empty();
  let result = channel.receive();
  return switch (result) {
    std::result::Result<Owned, std::error::ErrorCode>::Ok { .. } => 1;
    std::result::Result<Owned, std::error::ErrorCode>::Err { .. } => 0;
  };
}''', ""),
    "queued_drop": ('''fn main(): i32 {
  var channel = std::typed_channel::Channel<Owned>::empty();
  let initialized = channel.init(1u64);
  let sent = channel.send(Owned { .value = 7 });
  let closed = (move channel).close();
  return 0;
}''', "drop\n"),
}
with tempfile.TemporaryDirectory(prefix="chtholly-channel-values-") as raw:
    for name, (source, output) in CASES.items():
        root = pathlib.Path(raw) / name
        (root / "src").mkdir(parents=True)
        (root / "chtholly.toml").write_text(
            f'[package]\nname = "{name}"\nlanguage = "1.10"\n'
            '[build]\nentry = "src/main.cns"\nmodule_paths = ["src"]\n')
        source_path = root / "src/main.cns"
        source_path.write_text(PREFIX + source, encoding="utf-8")
        run([args.chthollyc, "build", "--project", root])
        result = run([single_native_executable(root / ".chtholly/build", name)])
        if result.stdout != output:
            raise AssertionError(f"{name}: unexpected drops: {result.stdout!r}")
        # Replay the build from cached independent artifacts as well.
        run([args.chthollyc, "build", "--project", root])
        if run([single_native_executable(root / ".chtholly/build", name)]).stdout != output:
            raise AssertionError(f"{name}: cached build changed lifecycle")
    source_path.write_text(PREFIX + '''fn main(): i32 {
      var channel = std::typed_channel::Channel<Owned>::empty(); var output: Owned;
      let result = channel.receive(output); return output.value;
    }''', encoding="utf-8")
    run_nonzero([args.chthollyc, "check", "--project", root])
