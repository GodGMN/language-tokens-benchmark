#!/usr/bin/env python3
"""Build workflow implementations using their adapters.

Usage:
  python3 benchmark/build.py --all
  python3 benchmark/build.py --language go
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ADAPTERS = Path(__file__).resolve().parent / "adapters"


def load_adapters():
    return {p.stem: json.loads(p.read_text()) for p in sorted(ADAPTERS.glob("*.json"))}


def project_ready(adapter):
    """A project is ready once its implementation entry files exist."""
    proj = ROOT / adapter["project_dir"]
    requires = adapter.get("requires", [])
    return all((proj / r).exists() for r in requires)


def build_one(name, adapter):
    proj = ROOT / adapter["project_dir"]
    if not proj.is_dir():
        print(f"  {name}: MISSING project dir {proj}", file=sys.stderr)
        return "missing"
    if not project_ready(adapter):
        missing = [r for r in adapter.get("requires", []) if not (proj / r).exists()]
        print(f"  {name}: SKIP (not implemented yet; missing {', '.join(missing)})")
        return "skipped"
    cmd = adapter.get("build")
    if cmd is None:
        print(f"  {name}: no build step (interpreted)")
        return "ok"
    print(f"  {name}: {' '.join(cmd)}  (in {adapter['project_dir']})")
    return "ok" if subprocess.run(cmd, cwd=proj).returncode == 0 else "failed"


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--language")
    g.add_argument("--all", action="store_true")
    args = ap.parse_args()

    adapters = load_adapters()
    names = list(adapters) if args.all else [args.language]
    statuses = {}
    for n in names:
        if n not in adapters:
            print(f"unknown language: {n} (have: {', '.join(adapters)})", file=sys.stderr)
            statuses[n] = "failed"
            continue
        statuses[n] = build_one(n, adapters[n])

    built = [n for n, s in statuses.items() if s == "ok"]
    skipped = [n for n, s in statuses.items() if s == "skipped"]
    failed = [n for n, s in statuses.items() if s in ("failed", "missing")]
    print(f"\nbuilt: {built or '-'}  skipped: {skipped or '-'}  failed: {failed or '-'}")
    # Not-yet-implemented projects are not an error; genuine build failures are.
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
