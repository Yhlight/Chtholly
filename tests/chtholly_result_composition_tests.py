"""Exercise result mapping, temporaries and dynamic array writes natively."""
import argparse
import pathlib
import tempfile
from chtholly_test_support import run, single_native_executable

parser = argparse.ArgumentParser()
parser.add_argument("--chthollyc", required=True)
args = parser.parse_args()
SOURCE = '''module main;
import std::result;
import std::callable;
struct Mapper { add: i32; }
impl std::callable::InvokeOnce<(i32,)> for Mapper {
 alias Output = i32;
 fn invoke(self: Self, args: (i32,)): i32 { return args.0 + self.add; }
}
fn mapped(): std::result::Result<void, i32> {
  let result = std::result::Result<void, i32>::Err { 4 };
  std::result::map_error(move result, Mapper { .add = 1 })?;
  return std::result::Result<void, i32>::Ok {};
}
fn main(): i32 {
  let result = mapped();
  let error = switch (move result) {
    std::result::Result<void,i32>::Ok { .. } => 0;
    std::result::Result<void,i32>::Err { error = move .0 } => error;
  };
  if (error != 5) { return 1; }
  var values: i32[4] = [0,0,0,0];
  for (var index = 0usize; index < 4usize; index += 1usize) {
    values[index] = (index as i32) + 10;
  }
  if (values[0] != 10 || values[1] != 11 || values[2] != 12 || values[3] != 13) { return 2; }
  let mapped = std::result::map_error(std::result::Result<i32,i32>::Ok { 7 }, Mapper { .add = 3 });
  return switch (move mapped) {
    std::result::Result<i32,i32>::Ok { value = move .0 } => if (value == 7) { 0 } else { 3 };
    std::result::Result<i32,i32>::Err { .. } => 4;
  };
}
'''
with tempfile.TemporaryDirectory(prefix="chtholly-result-compose-") as raw:
    root = pathlib.Path(raw)
    (root / "src").mkdir()
    (root / "chtholly.toml").write_text('[package]\nname="result_composition"\nlanguage="1.10"\n[build]\nentry="src/main.cns"\nmodule_paths=["src"]\n')
    (root / "src/main.cns").write_text(SOURCE)
    for iteration in range(2):
        run([args.chthollyc, "build", "--project", root])
        run([single_native_executable(root / ".chtholly/build", "result")])
