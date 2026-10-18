#!/usr/bin/env python3
"""Collect exact token metrics from pi session files (JSONL).

pi records provider-reported usage on every assistant message:
  message.usage = {input, output, cacheRead, cacheWrite, reasoning?, totalTokens, cost{...}}
plus standalone `usage` entries (e.g. cache warm) and `compaction` entries.

Sessions live in:  ~/.pi/agent/sessions/--<cwd with / -> ->--/<timestamp>_<session-id>.jsonl

Usage:
  python3 benchmark/collect.py --round round-1
  # session mapping comes from benchmark/rounds/<round>/round.json:
  #   {"model": "...", "languages": {"go": "<path-or-session-id>", ...}}
  # A value may be a full path to a .jsonl, or a session id (globbed),
  # or a cwd-relative folder used to pick the right session dir.

Output: results/<round>/metrics.json
"""
import argparse
import json
import sys
from pathlib import Path

import common

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
RESULTS = common.RESULTS
PI_SESSIONS = Path.home() / ".pi" / "agent" / "sessions"


def resolve_session(value: str) -> Path:
    """Resolve a session reference to a .jsonl path.

    Accepts (in order): an existing file path, a bare session id, a session
    basename, or a cwd (matched against the --<cwd>-- session directories).
    """
    p = Path(value).expanduser()
    if p.is_file():
        return p
    if not PI_SESSIONS.is_dir():
        raise FileNotFoundError(f"no pi sessions under {PI_SESSIONS}")

    base = p.name
    stem = base[:-6] if base.endswith(".jsonl") else base
    # Session files are <timestamp>_<session-id>.jsonl inside --<cwd>-- dirs.
    patterns = [f"*/{base}", f"*/*_{stem}", f"*/*_{stem}.jsonl"]
    matches: list[Path] = []
    for pattern in patterns:
        matches.extend(PI_SESSIONS.glob(pattern))
    matches = sorted(set(matches))
    if len(matches) == 1:
        return matches[0]
    if len(matches) > 1:
        print(f"warning: multiple sessions for {p!r}; newest wins: {matches[-1]}", file=sys.stderr)
        return matches[-1]

    # Otherwise treat value as a cwd: newest session under the matching directory.
    suffix = "--" + str(value).lstrip("/").replace("/", "-").replace("\\", "-") + "--"
    dirs = sorted(d for d in PI_SESSIONS.iterdir() if d.is_dir() and d.name.endswith(suffix))
    if dirs:
        files = sorted(dirs[-1].glob("*.jsonl"))
        if files:
            return files[-1]
    raise FileNotFoundError(f"cannot resolve pi session from {value!r} under {PI_SESSIONS}")


def zero():
    return {"input": 0, "output": 0, "cacheRead": 0, "cacheWrite": 0,
            "reasoning": 0, "totalTokens": 0,
            "cost": {"input": 0.0, "output": 0.0, "cacheRead": 0.0, "cacheWrite": 0.0, "total": 0.0},
            "assistant_messages": 0, "tool_results": 0, "user_messages": 0,
            "compactions": 0, "models": set()}


def add_usage(t, u):
    if not u:
        return
    t["input"] += u.get("input", 0)
    t["output"] += u.get("output", 0)
    t["cacheRead"] += u.get("cacheRead", 0)
    t["cacheWrite"] += u.get("cacheWrite", 0)
    t["reasoning"] += u.get("reasoning", 0)
    t["totalTokens"] += u.get("totalTokens", 0)
    c = u.get("cost") or {}
    for k in t["cost"]:
        t["cost"][k] += c.get(k, 0.0)


def collect_session(path: Path) -> dict:
    t = zero()
    first = last = None
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            e = json.loads(line)
        except json.JSONDecodeError:
            continue
        etype = e.get("type")
        ts = e.get("timestamp")
        if first is None:
            first = ts
        last = ts
        if etype == "message":
            m = e.get("message", {})
            role = m.get("role")
            if role == "assistant":
                t["assistant_messages"] += 1
                add_usage(t, m.get("usage"))
                if m.get("model"):
                    t["models"].add(f"{m.get('provider', '?')}:{m['model']}")
            elif role == "user":
                t["user_messages"] += 1
            elif role == "toolResult":
                t["tool_results"] += 1
        elif etype == "usage":
            add_usage(t, e.get("usage"))
        elif etype == "compaction":
            t["compactions"] += 1
            add_usage(t, e.get("usage"))
    return {
        "session_file": str(path),
        "started": first, "ended": last,
        **{k: v for k, v in t.items() if k != "models"},
        "models": sorted(t["models"]),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--round", default=None, help="round id (default from config.json)")
    ap.add_argument("--session", action="append", default=[], metavar="LANG=PATH_OR_ID",
                    help="override/add a session mapping")
    args = ap.parse_args()

    cfg = common.load_round(args.round or common.default_round())
    round_id = cfg["id"]
    sessions = {lang: common.lang_entry(cfg, lang)["session"] for lang in cfg.get("languages", {})}
    for kv in args.session:
        lang, _, val = kv.partition("=")
        sessions[lang] = val
    sessions = {k: v for k, v in sessions.items() if v}

    if not sessions:
        print(f"no sessions configured for round {round_id}", file=sys.stderr)
        sys.exit(1)

    out = {"round": round_id, "model": cfg.get("model"), "languages": {}}
    missing = []
    for lang, val in sorted(sessions.items()):
        try:
            path = resolve_session(val)
        except FileNotFoundError as e:
            print(f"  {lang}: {e}", file=sys.stderr)
            missing.append(lang)
            continue
        data = collect_session(path)
        data["source"] = val
        out["languages"][lang] = data
        print(f"  {lang:12s} in={data['input']:>9d} out={data['output']:>9d} "
              f"total={data['totalTokens']:>10d} turns={data['user_messages']} "
              f"tools={data['tool_results']:>3d} cost=${data['cost']['total']:.4f}  [{path.name}]")

    if missing:
        sys.exit("Missing sessions; existing metrics were not changed: " + ", ".join(missing))

    outdir = RESULTS / round_id
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / "metrics.json").write_text(json.dumps(out, indent=2))
    print(f"\nwrote {outdir / 'metrics.json'}")


if __name__ == "__main__":
    main()
