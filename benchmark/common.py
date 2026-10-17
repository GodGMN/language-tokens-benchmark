"""Shared helpers for the harness: rounds, adapters, project resolution.

A *round* is one benchmark run (one model, one set of sessions). Its config lives
in `benchmark/rounds/<round-id>/round.json` and points at a submissions dir:

    submissions/<round-id>/<language>/...

The oracle is special: its adapter carries an explicit `project_dir`.
"""
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
ADAPTERS = HERE / "adapters"
ROUNDS = HERE / "rounds"
RESULTS = ROOT / "results"
TASKS = ROOT / "tasks"


def default_round() -> str:
    cfg = HERE / "config.json"
    if cfg.exists():
        return json.loads(cfg.read_text()).get("default_round", "round-1")
    return "round-1"


def task_paths(round_cfg: dict) -> dict:
    """Resolve the task a round targets.

    The original `workflow` task lives at the repo root (back-compat). Any other
    task is self-contained under `tasks/<task>/`.
    """
    task = round_cfg.get("task", "workflow")
    if task == "workflow":
        return {"id": task, "envelope": "workflow", "spec": ROOT / "SPEC.md",
                "prompt": HERE / "rounds" / "REQUIREMENTS.template.md",
                "adapters": ADAPTERS, "fixtures": HERE / "fixtures",
                "holdout": HERE / "holdout", "oracle": HERE / "oracle"}
    base = TASKS / task
    return {"id": task, "envelope": "generic", "spec": base / "SPEC.md",
            "prompt": base / "PROMPT.template.md",
            "adapters": base / "adapters", "fixtures": base / "fixtures",
            "holdout": base / "holdout", "oracle": base / "oracle"}


def load_adapters(adapters_dir=None) -> dict:
    d = adapters_dir or ADAPTERS
    return {p.stem: json.loads(p.read_text()) for p in sorted(Path(d).glob("*.json"))}


def load_round(round_id: str | None = None) -> dict:
    round_id = round_id or default_round()
    path = ROUNDS / round_id / "round.json"
    if not path.exists():
        raise FileNotFoundError(f"no round config at {path}")
    cfg = json.loads(path.read_text())
    cfg.setdefault("id", round_id)
    return cfg


def lang_entry(round_cfg: dict, lang: str) -> dict:
    """Normalize a language entry to {"path", "session"}.

    Accepts either a bare session id string or a full object.
    """
    entry = round_cfg.get("languages", {}).get(lang)
    if isinstance(entry, str):
        return {"path": lang, "session": entry}
    if isinstance(entry, dict):
        return {"path": entry.get("path", lang), "session": entry.get("session")}
    return {"path": lang, "session": None}


def resolve_project(adapter: dict, round_cfg: dict) -> Path:
    """Where this language's implementation lives for the given round."""
    if adapter.get("project_dir"):
        return ROOT / adapter["project_dir"]
    lang = adapter["language"]
    return ROOT / round_cfg["submissions"] / lang_entry(round_cfg, lang)["path"]


def project_ready(adapter: dict, project: Path) -> bool:
    return all((project / r).exists() for r in adapter.get("requires", []))


def session_for(round_cfg: dict, lang: str):
    return lang_entry(round_cfg, lang).get("session")


def round_languages(round_cfg: dict) -> list[str]:
    return list(round_cfg.get("languages", {}).keys())
