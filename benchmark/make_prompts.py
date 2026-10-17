#!/usr/bin/env python3
"""Render per-language requirements prompts for a round.

Usage: python3 benchmark/make_prompts.py --round round-8-expr

Uses the round's task prompt template and adapters. Relative paths back to the
task spec are computed per submission folder, so they are never hardcoded.
"""
import argparse
import os
import sys
from pathlib import Path

import common


def commands_block(adapter, project_rel):
    lines = ["```bash", f"cd {project_rel}"]
    if adapter.get("build"):
        lines += ["# build", " ".join(adapter["build"])]
    else:
        lines += ["# (interpreted language — no build step)"]
    rendered = " ".join(adapter["run"]).replace("{input}", "request.json")
    lines += ["# run", rendered + "     # or: - for stdin", "```"]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--round", default=None)
    args = ap.parse_args()
    round_cfg = common.load_round(args.round or common.default_round())
    tp = common.task_paths(round_cfg)
    adapters = common.load_adapters(tp["adapters"])
    template = Path(tp["prompt"]).read_text()
    outdir = common.HERE / "rounds" / round_cfg["id"]
    outdir.mkdir(parents=True, exist_ok=True)

    for lang in common.round_languages(round_cfg):
        entry = common.lang_entry(round_cfg, lang)
        project_rel = f"{round_cfg['submissions']}/{entry['path']}"
        project_abs = common.ROOT / project_rel
        rel_root = os.path.relpath(common.ROOT, project_abs)
        spec_path = os.path.relpath(tp["spec"], project_abs)
        prd_path = os.path.relpath(common.ROOT / "PRD.md", project_abs)
        prompt = (template
                  .replace("{{LANGUAGE}}", "C" if lang == "c" else lang.capitalize())
                  .replace("{{PROJECT_DIR}}", project_rel)
                  .replace("{{REL_ROOT}}", rel_root)
                  .replace("{{SPEC_PATH}}", spec_path)
                  .replace("{{PRD_PATH}}", prd_path)
                  .replace("{{ADAPTER_COMMANDS}}", commands_block(adapters[lang], project_rel)))
        (outdir / f"prompt-{lang}.md").write_text(prompt)
        print(f"wrote {outdir / f'prompt-{lang}.md'}")


if __name__ == "__main__":
    main()
