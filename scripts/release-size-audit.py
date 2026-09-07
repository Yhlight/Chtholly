import argparse, json, math, os, tempfile, zipfile
from pathlib import Path
import tomllib

SCHEMA = "chtholly-release-size-v1"
CLASSES = ("bin", "libexec", "runtime", "stdlib", "docs", "vscode", "support", "headers")
HOSTS = ("windows-x64", "linux-x64")

def category(path):
    parts=path.split("/")
    if len(parts) >= 3 and parts[:2] == ["share", "chtholly"] and parts[2] in CLASSES: return parts[2]
    if parts[0] == "include": return "headers"
    first = path.split("/", 1)[0]
    return first if first in CLASSES else "other"

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--archive", required=True); p.add_argument("--install-tree", required=True)
    p.add_argument("--budget", required=True); p.add_argument("--profile", choices=("minimal", "full"), required=True)
    p.add_argument("--host", choices=("windows-x64", "linux-x64"), required=True); p.add_argument("--output", required=True)
    p.add_argument("--check-budget", action="store_true")
    p.add_argument("--source-commit", default="")
    p.add_argument("--target", default="")
    a = p.parse_args()
    archive_path=Path(a.archive)
    with zipfile.ZipFile(archive_path) as z:
        archive_bytes = sum(i.compress_size for i in z.infolist())
        archive_uncompressed = sum(i.file_size for i in z.infolist())
    files=[]
    root=Path(a.install_tree)
    for f in root.rglob("*"):
        if f.is_symlink(): raise SystemExit("invalid-install-tree: symlink entry")
        if f.is_file(): files.append((f.relative_to(root).as_posix(), f.stat().st_size))
    if not files: raise SystemExit("invalid-install-tree: empty tree")
    files.sort(key=lambda x: (-x[1], x[0]))
    cats={}
    for path, size in files:
        c=category(path); cats[c]=cats.get(c, 0)+size
    report={"schema": SCHEMA, "archive_bytes": archive_path.stat().st_size, "archive_compressed_entry_bytes": archive_bytes, "archive_uncompressed_bytes": archive_uncompressed,
            "install_tree_bytes": sum(s for _,s in files), "file_count": len(files),
            "largest_files":[{"path":x,"bytes":s} for x,s in files[:10]], "categories":cats,
            "profile":a.profile, "host":a.host, "valid":True}
    if a.source_commit:
        report["source_commit"] = a.source_commit
    if a.target:
        report["target"] = a.target
    if a.check_budget:
        try: b=tomllib.loads(Path(a.budget).read_text(encoding="utf-8"))
        except Exception as e: raise SystemExit("invalid-budget: " + str(e))
        profiles = b.get("profiles")
        if b.get("schema") != "chtholly-release-size-budget-v1" or not isinstance(b.get("budget"), list):
            raise SystemExit("invalid-budget: schema")
        if (not isinstance(profiles, list) or not profiles or len(set(profiles)) != len(profiles)
                or any(x not in ("minimal", "full") for x in profiles)):
            raise SystemExit("invalid-budget: profiles")
        pairs=[(x.get("host"),x.get("profile")) if isinstance(x, dict) else (None, None) for x in b["budget"]]
        expected_pairs = {(h, q) for h in HOSTS for q in profiles}
        if (len(set(pairs)) != len(pairs)
                or any(h not in HOSTS or q not in profiles for h,q in pairs)
                or set(pairs) != expected_pairs
                or (a.host, a.profile) not in set(pairs)):
            raise SystemExit("invalid-budget: host/profile")
        base=Path(a.budget).parent / b.get("baseline", "")
        if not base.is_file(): raise SystemExit("missing baseline")
        try: data=json.loads(base.read_text()); records=data["records"]
        except Exception as e: raise SystemExit("invalid-baseline: " + str(e))
        if data.get("schema") != "chtholly-release-size-baseline-v1" or not isinstance(records,list): raise SystemExit("invalid-baseline: schema")
        if any(not isinstance(r, dict) or r.get("host") not in ("windows-x64", "linux-x64") or r.get("profile") not in ("minimal", "full") for r in records):
            raise SystemExit("invalid-baseline: host/profile")
        baseline_pairs = [(r["host"], r["profile"]) for r in records]
        if len(set(baseline_pairs)) != len(baseline_pairs):
            raise SystemExit("invalid-baseline: duplicate host/profile")
        rec=next((r for r in records if r["host"]==a.host and r["profile"]==a.profile), None)
        if rec is None: raise SystemExit("unknown host/profile")
        growth = b.get("max_growth_percent", 15)
        count_growth = b.get("max_file_count_growth", 64)
        if isinstance(growth, bool) or not isinstance(growth, (int, float)) or not math.isfinite(growth) or growth < 0:
            raise SystemExit("invalid-budget: numeric fields")
        if isinstance(count_growth, bool) or not isinstance(count_growth, int) or count_growth < 0:
            raise SystemExit("invalid-budget: numeric fields")
        status = rec.get("baseline_status", "measured")
        if status == "unmeasured":
            report["baseline_status"]="unmeasured"
        elif status == "measured":
            numeric = ("archive_bytes","archive_uncompressed_bytes","install_tree_bytes","file_count")
            if any(k not in rec or isinstance(rec[k], bool) or not isinstance(rec[k], int) or rec[k] < 0 for k in numeric): raise SystemExit("invalid-baseline: numeric fields")
            extras = [k for k in rec if k.endswith("_bytes") and k not in numeric]
            if any(isinstance(rec[k], bool) or not isinstance(rec[k], int) or rec[k] < 0 for k in extras): raise SystemExit("invalid-baseline: numeric fields")
            growth /= 100
            for key in ("archive_bytes","archive_uncompressed_bytes","install_tree_bytes"):
                if report[key] > rec[key]*(1+growth): report["valid"]=False
            if report["file_count"] > rec["file_count"] + count_growth: report["valid"]=False
        else: raise SystemExit("invalid-baseline: status")
        if not report["valid"]:
            largest_category = max(cats, key=cats.get) if cats else "other"
            largest_path = files[0][0] if files else "<none>"
            print("budget-exceeded: category=" + largest_category + " largest=" + largest_path)
    out=Path(a.output); out.parent.mkdir(parents=True, exist_ok=True)
    fd,tmp=tempfile.mkstemp(prefix=".release-size-", dir=out.parent); os.close(fd)
    Path(tmp).write_text(json.dumps(report, sort_keys=True, indent=2)+"\n", encoding="utf-8"); os.replace(tmp,out)
    raise SystemExit(0 if report["valid"] else 1)
if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception as exc:
        print("unexpected-error: " + type(exc).__name__ + ": " + str(exc))
        raise SystemExit(1)
