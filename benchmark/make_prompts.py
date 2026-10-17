#!/usr/bin/env python3
"""Render the per-language requirements prompts for a round.

Usage:
  python3 benchmark/make_prompts.py --round round-2

Reads benchmark/rounds/REQUIREMENTS.template.md and the round's round.json,
and writes benchmark/rounds/<round>/prompt-<lang>.md. Relative paths back to
the repo root are computed from each submission folder, so they are never
hardcoded wrong.
"""
import argparse
import json
import os
import sys
from pathlib import Path

import common

HERE = common.HERE
TEMPLATE = HERE / "rounds" / "REQUIREMENTS.template.md"


def commands_block(adapter, project_rel):
    lines = ["```bash", f"cd {project_rel}"]
    if adapter.get("build"):
        lines += ["# build", " ".join(adapter["build"])]
    else:
        lines += ["# (interpreted language — no build step)"]
    rendered = " ".join(adapter["run"]).replace("{input}", "myworkflow.json")
    lines += ["# run", rendered + "     # or: run -   (stdin)", "```"]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--round", default=None)
    args = ap.parse_args()
    round_cfg = common.load_round(args.round or common.default_round())
    adapters = common.load_adapters()
    template = TEMPLATE.read_text()
    outdir = HERE / "rounds" / round_cfg["id"]
    outdir.mkdir(parents=True, exist_ok=True)

    for lang in common.round_languages(round_cfg):
        entry = common.lang_entry(round_cfg, lang)
        project_rel = f"{round_cfg['submissions']}/{entry['path']}"
        project_abs = common.ROOT / project_rel
        rel_root = os.path.relpath(common.ROOT, project_abs)
        prompt = (template
                  .replace("{{LANGUAGE}}", "C" if lang == "c" else lang.capitalize())
                  .replace("{{PROJECT_DIR}}", project_rel)
                  .replace("{{REL_ROOT}}", rel_root)
                  .replace("{{ADAPTER_COMMANDS}}", commands_block(adapters[lang], project_rel)))
        (outdir / f"prompt-{lang}.md").write_text(prompt)
        print(f"wrote {outdir / f'prompt-{lang}.md'}")


if __name__ == "__main__":
    main()
