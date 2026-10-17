#!/usr/bin/env python3
"""Render REQUIREMENTS.template.md into per-language prompt files.

Usage:
  python3 benchmark/rounds/round-1/make_prompts.py

Reads the round's round.json + the adapters, and computes the correct relative
path from each submission folder back to the repo root (so `../../SPEC.md`
depth is never hardcoded wrong).
"""
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BENCH = HERE.parent.parent
ROOT = BENCH.parent

sys.path.insert(0, str(BENCH))
import common  # noqa: E402

TEMPLATE = (HERE / "REQUIREMENTS.template.md").read_text()


def commands_block(adapter: dict, project_rel: str) -> str:
    lines = ["```bash", f"cd {project_rel}"]
    if adapter.get("build"):
        lines.append("# build")
        lines.append(" ".join(adapter["build"]))
    else:
        lines.append("# (interpreted language — no build step)")
    run = adapter["run"]
    example = [c for c in run]
    # Show a friendly example instead of the raw {input} placeholder.
    rendered = " ".join(example).replace("{input}", "myworkflow.json")
    lines.append("# run")
    lines.append(rendered + "     # or: run -   (stdin)")
    lines.append("```")
    return "\n".join(lines)


def main() -> None:
    round_cfg = common.load_round(HERE.name)
    adapters = common.load_adapters()
    submissions = round_cfg["submissions"]

    for lang in common.round_languages(round_cfg):
        adapter = adapters[lang]
        entry = common.lang_entry(round_cfg, lang)
        project_rel = f"{submissions}/{entry['path']}"
        project_abs = ROOT / project_rel
        rel_root = os.path.relpath(ROOT, project_abs)  # e.g. ../../..

        prompt = (
            TEMPLATE
            .replace("{{LANGUAGE}}", "C" if lang == "c" else lang.capitalize())
            .replace("{{PROJECT_DIR}}", project_rel)
            .replace("{{REL_ROOT}}", rel_root)
            .replace("{{ADAPTER_COMMANDS}}", commands_block(adapter, project_rel))
        )
        out = HERE / f"prompt-{lang}.md"
        out.write_text(prompt)
        print(f"wrote {out}")


if __name__ == "__main__":
    main()
