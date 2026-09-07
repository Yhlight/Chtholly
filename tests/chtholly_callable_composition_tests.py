"""Calls compose through values without duplicating callee evaluation."""
import argparse
import pathlib
import tempfile
from chtholly_test_support import run, run_nonzero, single_native_executable

parser = argparse.ArgumentParser()
parser.add_argument("--chthollyc", required=True)
args = parser.parse_args()
PROVIDER = '''module provider;
pub fn answer(value: i32): i32 { return value + 1; }
pub fn choose(count: i32&): fn(i32): i32 {
  count = count + 1; return answer;
}
pub struct Holder { pub operation: fn(i32): i32; }
impl Holder { pub fn make(): Holder { return Holder { .operation = answer }; } }
pub fn argument(count: i32&): i32 { return count; }
'''
MAIN = '''module main;
import provider;
alias Holder = provider::Holder;
enum Flag { Yes, No }
alias Selected = Flag;
fn main(): i32 {
  var calls = 0;
  if (provider::choose(&calls)(4) != 5 || calls != 1) { return 1; }
  if ((if (true) { provider::answer } else { provider::answer })(6) != 7) { return 2; }
  if ((fn [](value: i32): i32 { return value + 2; })(5) != 7) { return 3; }
  let holder = Holder::make();
  let selected = Selected::Yes {};
  let choice = switch (selected) { Selected::Yes => 1; Selected::No => 0; };
  if (choice != 1) { return 8; }
  if (holder.operation(8) != 9) { return 4; }
  let operations = [provider::answer, provider::answer];
  if (operations[1usize](9) != 10) { return 5; }
  calls = 0;
  if (provider::choose(&calls)(provider::argument(&calls)) != 2 || calls != 1) { return 6; }
  let offset = 3;
  if ((fn [copy offset](value: i32): i32 { return value + offset; })(4) != 7) { return 7; }
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="chtholly-call-compose-") as raw:
    root = pathlib.Path(raw)
    for name, content in (("provider", PROVIDER), ("consumer", MAIN)):
        project = root / name
        (project / "src").mkdir(parents=True)
        manifest = f'[package]\nname = "{name}"\nlanguage = "1.10"\n[build]\nmodule_paths = ["src"]\n'
        if name == "consumer":
            manifest += 'entry = "src/main.cns"\n[dependencies]\nprovider = { path = "../provider" }\n'
        (project / "chtholly.toml").write_text(manifest)
        (project / "src/main.cns").write_text(content)
    for iteration in range(2):
        run([args.chthollyc, "build", "--project", project])
        run([single_native_executable(project / ".chtholly/build", "calls")])
    (project / "src/main.cns").write_text('module main; fn main(): i32 { return (3)(4); }')
    result = run_nonzero([args.chthollyc, "check", "--project", project])
    if "not-callable" not in result.stderr:
        raise AssertionError(result.stderr)

    (project / "src/main.cns").write_text("""module main;
fn change(value: i32&): void { value = 2; }
fn main(): i32 {
  var value = 1;
  let borrowed = &value;
  let operation = change;
  operation(&value);
  return borrowed;
}
""")
    result = run_nonzero([args.chthollyc, "check", "--project", project])
    if "borrow" not in result.stderr:
        raise AssertionError(result.stderr)
