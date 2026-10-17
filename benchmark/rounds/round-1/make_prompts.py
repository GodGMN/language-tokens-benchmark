#!/usr/bin/env python3
"""Render REQUIREMENTS.template.md into per-language prompt files.

Usage: python3 make_prompts.py
Reads ../adapters/*.json and writes prompt-<language>.md next to itself.
Copy the content of prompt-<language>.md into the pi session running in cli-<language>/.
"""
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
ADAPTERS = HERE.parent.parent / "adapters"
TEMPLATE = (HERE / "REQUIREMENTS.template.md").read_text()


def commands_block(adapter: dict) -> str:
    lines = ["```bash", f"cd {adapter['project_dir']}"]
    if adapter.get("build"):
        lines.append("# build")
        lines.append(" ".join(adapter["build"]))
    else:
        lines.append("# (interpreted language — no build step)")
    lines.append("# run")
    lines.append(" ".join(adapter["run"][:2] + ["myworkflow.json", "  # or: run -  (stdin)"]))
    lines.append("```")
    return "\n".join(lines)


def main() -> None:
    for f in sorted(ADAPTERS.glob("*.json")):
        a = json.loads(f.read_text())
        lang, proj = a["language"], a["project_dir"]
        if lang == "oracle":
            continue  # reference implementation, not one of the benchmarked languages
        prompt = (
            TEMPLATE.replace("{{LANGUAGE}}", lang.capitalize() if lang != "c" else "C")
            .replace("{{ADAPTER_COMMANDS}}", commands_block(a))
            .replace("{{PROJECT_DIR}}", proj)
        )
        out = HERE / f"prompt-{lang}.md"
        out.write_text(prompt)
        print(f"wrote {out}")


if __name__ == "__main__":
    main()
