import argparse
import importlib.util
import json
import pathlib

parser = argparse.ArgumentParser()
parser.add_argument("--script", type=pathlib.Path, required=True)
args = parser.parse_args()
spec = importlib.util.spec_from_file_location("evidence", args.script)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
events = [dict(event="load", cycle=0, seed=17),
          dict(event="close", cycle=0, status=0),
          dict(event="joined", cycle=0, workers=4, calls=12),
          dict(event="unloaded", cycle=0)]
def encode(values):
    return "\n".join(json.dumps(x) for x in values)
if module.validate_events(encode(events), 1, 17) != (1, 12):
    raise AssertionError("valid transcript was not accepted")
for invalid in ([], events[:3], list(reversed(events)), events + events,
                [*events[:2], dict(event="joined", cycle=0, workers=4, calls=0), events[3]]):
    try:
        module.validate_events(encode(invalid), 1, 17)
    except ValueError:
        continue
    raise AssertionError(f"invalid transcript accepted: {invalid}")
