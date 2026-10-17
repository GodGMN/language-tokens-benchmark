#!/usr/bin/env python3
"""Build workflow implementations using their adapters.

Usage:
  python3 benchmark/build.py                     # default round
  python3 benchmark/build.py --round round-1
  python3 benchmark/build.py --language go --round round-1

Not-yet-implemented projects are reported as SKIP, not failure.
"""
import argparse
import subprocess
import sys

from common import (ROOT, default_round, load_adapters, load_round,
                    project_ready, resolve_project)


def build_one(name, adapter, round_cfg):
    proj = resolve_project(adapter, round_cfg)
    if not proj.is_dir():
        print(f"  {name}: MISSING project dir {proj}", file=sys.stderr)
        return "missing"
    if not project_ready(adapter, proj):
        missing = [r for r in adapter.get("requires", []) if not (proj / r).exists()]
        print(f"  {name}: SKIP (not implemented yet; missing {', '.join(missing)})")
        return "skipped"
    cmd = adapter.get("build")
    if cmd is None:
        print(f"  {name}: no build step (interpreted)")
        return "ok"
    print(f"  {name}: {' '.join(cmd)}  (in {proj.relative_to(ROOT)})")
    return "ok" if subprocess.run(cmd, cwd=proj).returncode == 0 else "failed"


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--language")
    g.add_argument("--all", action="store_true")
    ap.add_argument("--round", default=None, help="round id (default from config.json)")
    args = ap.parse_args()

    round_cfg = load_round(args.round or default_round())
    adapters = load_adapters()
    names = list(adapters) if args.all else [args.language]

    statuses = {}
    for n in names:
        if n not in adapters:
            print(f"unknown language: {n} (have: {', '.join(adapters)})", file=sys.stderr)
            statuses[n] = "failed"
            continue
        statuses[n] = build_one(n, adapters[n], round_cfg)

    built = [n for n, s in statuses.items() if s == "ok"]
    skipped = [n for n, s in statuses.items() if s == "skipped"]
    failed = [n for n, s in statuses.items() if s in ("failed", "missing")]
    print(f"\nbuilt: {built or '-'}  skipped: {skipped or '-'}  failed: {failed or '-'}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
