#!/usr/bin/env python3
import argparse, hashlib, pathlib, subprocess, tempfile

def run(tool, *args, expect=0):
    p = subprocess.run([str(tool), *map(str, args)], text=True, capture_output=True)
    if p.returncode != expect:
        raise AssertionError(f"{args}: {p.returncode}\n{p.stdout}\n{p.stderr}")
    return p

def manifest(root, version, payload):
    contract = root / f"contract-{version}.bin"; contract.write_bytes(payload)
    library = root / f"component-{version}.dll"; library.write_bytes(b"library-" + payload)
    digest = hashlib.sha256(payload).hexdigest()
    path = root / f"manifest-{version}.toml"
    path.write_text('[component]\nidentity = "org.example.component"\nversion = "' + version +
                    '"\ntarget = "x86_64-pc-windows-msvc"\nruntime = "v1"\nlibrary = "' + library.name +
                    '"\ncontract = "' + contract.name + '"\ncontract_digest = "' + digest + '"\n', encoding='utf-8')
    return path, version + '-' + digest

def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--tool', required=True); args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='chtholly-component-deploy-') as d:
        root = pathlib.Path(d); m0, id0 = manifest(root, '0.1.0', b'contract-v0')
        m1, id1 = manifest(root, '0.1.1', b'contract-v1')
        run(args.tool, 'install', root / 'tree', m0); run(args.tool, 'activate', root / 'tree', id0)
        if '0.1.0' not in run(args.tool, 'active', root / 'tree').stdout: raise AssertionError('v0.1.0 not active')
        run(args.tool, 'install', root / 'tree', m1); run(args.tool, 'activate', root / 'tree', id1)
        if '0.1.1' not in run(args.tool, 'active', root / 'tree').stdout: raise AssertionError('v0.1.1 not active')
        run(args.tool, 'rollback', root / 'tree')
        if '0.1.0' not in run(args.tool, 'active', root / 'tree').stdout: raise AssertionError('rollback failed')
        run(args.tool, 'remove', root / 'tree', id1)
        run(args.tool, 'remove', root / 'tree', id0, expect=1)
    return 0
if __name__ == '__main__': raise SystemExit(main())
