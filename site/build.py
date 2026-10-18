#!/usr/bin/env python3
"""Build the Token Golf site from results/dataset.json.

    python3 site/build.py            # writes site/dist/

No dependencies. Copy lives in src/index.html (English) and src/es/index.html
(Spanish); strings generated here live in STRINGS. Every number on the page is
computed here, so a new round only needs `benchmark/export.py` and a rebuild.
"""
import html
import json
import re
import shutil
import statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = Path(__file__).resolve().parent / "src"
DIST = Path(__file__).resolve().parent / "dist"
DATASET = ROOT / "results" / "dataset.json"
REPO = "https://github.com/GodGMN/language-tokens-benchmark"

NAMES = {"python": "Python", "javascript": "JavaScript", "go": "Go", "rust": "Rust", "c": "C"}
TASKS = ["workflow", "expr", "patch"]

# Generated text, per locale; the "" locale is English and builds into dist/.
STRINGS = {
    "": {
        "tasks": {"workflow": "Workflow engine", "expr": "Expression evaluator", "patch": "JSON Patch"},
        "runs": "median of {} runs",
        "run": "1 run",
        "output_tokens": "{} output tokens",
        "sample_tokens": "{} output · {} total",
        "sample_work": "{} turns · {} tool calls · {}",
        "median": "median",
        "spread": "spread",
        "cache": "Cached context re-read",
        "output": "Output (code, tool calls, reasoning)",
        "input": "Fresh input",
        "composition": "Token composition across all sessions",
        "tokens": "tokens",
        "columns": ["Round", "Task", "Language", "Tests", "Output", "Total", "Turns", "Cost"],
    },
    "es": {
        "tasks": {"workflow": "Motor de workflows", "expr": "Evaluador de expresiones", "patch": "JSON Patch"},
        "runs": "mediana de {} ejecuciones",
        "run": "1 ejecución",
        "output_tokens": "{} tokens de salida",
        "sample_tokens": "{} de salida · {} en total",
        "sample_work": "{} turnos · {} llamadas a herramientas · {}",
        "median": "mediana",
        "spread": "de dispersión",
        "cache": "Contexto en caché releído",
        "output": "Salida (código, llamadas a herramientas, razonamiento)",
        "input": "Entrada nueva",
        "composition": "Composición de tokens en todas las sesiones",
        "tokens": "tokens",
        "columns": ["Ronda", "Tarea", "Lenguaje", "Tests", "Salida", "Total", "Turnos", "Coste"],
    },
}
LOCALE = ""


def esc(value):
    return html.escape(str(value), quote=True)


def t(key):
    return STRINGS[LOCALE][key]


def num(text):
    """Swap decimal and thousands separators for Spanish: 4,360.5 -> 4.360,5."""
    return text.translate(str.maketrans(",.", ".,")) if LOCALE == "es" else text


def k(n):
    """Compact token count: 95022 -> 95.0k, 3760000 -> 3.76M."""
    if n >= 1_000_000:
        return num(f"{n / 1_000_000:.2f}M")
    return num(f"{n / 1_000:.1f}k")


def comma(n):
    return num(f"{round(n):,}")


def usd(n):
    return num(f"${n:.3f}")


def pct(x, lo, hi):
    return (x - lo) / (hi - lo) * 100


# ——— data ———


def load():
    samples = json.loads(DATASET.read_text())["samples"]
    for s in samples:
        s["name"] = NAMES[s["language"]]
    return samples


def workflow_v2(samples):
    return [s for s in samples if s["task"] == "workflow" and s["spec_version"] == 2]


def by_lang(rows):
    out = {}
    for s in rows:
        out.setdefault(s["language"], []).append(s)
    return out


def task_values(samples):
    """Per task: {lang: (output tokens, n)}; workflow uses the spec-v2 median."""
    result = {}
    for task in TASKS:
        rows = workflow_v2(samples) if task == "workflow" else [s for s in samples if s["task"] == task]
        result[task] = {
            lang: (statistics.median(r["output_tokens"] for r in group), len(group))
            for lang, group in by_lang(rows).items()
        }
    return result


# ——— fragments ———


def tip(*lines):
    return esc("\n".join(lines))


def icon(lang):
    """Inline Simple Icons glyph (CC0), tinted with currentColor."""
    d = re.search(r'<path d="([^"]+)"', (SRC / "icons" / f"{lang}.svg").read_text()).group(1)
    return f'<svg viewBox="0 0 24 24" aria-hidden="true"><path d="{d}"/></svg>'


def board(values):
    """Leaderboard: one ranked column per task, fewest output tokens first."""
    cols = []
    for task in TASKS:
        title = t("tasks")[task]
        ranked = sorted(values[task], key=lambda lang: values[task][lang][0])
        worst = values[task][ranked[-1]][0]
        n = values[task][ranked[0]][1]
        basis = t("runs").format(n) if n > 1 else t("run")
        rows = "".join(
            f'<li><button type="button" class="board-row" data-lang="{lang}" '
            f'data-tip="{tip(f"{NAMES[lang]} · {title}", t("output_tokens").format(comma(values[task][lang][0])), basis)}">'
            f'<span class="rank">{i}</span>'
            f'<span class="track"><span class="bar" style="width:calc((100% - 62px) * {values[task][lang][0] / worst:.3f})">'
            f'{icon(lang)}<span class="lang">{NAMES[lang]}</span></span>'
            f'<span class="val">{k(values[task][lang][0])}</span></span></button></li>'
            for i, lang in enumerate(ranked, 1)
        )
        cols.append(f'<div class="board-col"><h3>{title}</h3><p>{basis}</p><ol>{rows}</ol></div>')
    return f'<div class="board" data-highlight>{"".join(cols)}</div>'


def sample_tip(s):
    return tip(
        "{} · {}".format(s["name"], s["round"]),
        t("sample_tokens").format(comma(s["output_tokens"]), k(s["total_tokens"])),
        t("sample_work").format(s["turns"], s["tool_calls"], usd(s["cost_usd"])),
    )


def strip(samples, metric, lo, hi, ticks, fmt, tick):
    rows = by_lang(workflow_v2(samples))
    order = sorted(rows, key=lambda lang: statistics.median(s["output_tokens"] for s in rows[lang]))
    out = []
    for lang in order:
        group = rows[lang]
        vals = [s[metric] for s in group]
        med = statistics.median(vals)
        dots = "".join(
            f'<span class="strip-dot" style="left:{pct(s[metric], lo, hi):.2f}%" data-lang="{lang}" '
            f'data-tip="{sample_tip(s)}"></span>'
            for s in sorted(group, key=lambda s: s[metric])
        )
        out.append(f"""
    <div class="strip-row">
      <button type="button" class="strip-name" data-lang="{lang}">{NAMES[lang]}</button>
      <div class="strip-track">
        <span class="strip-range" style="left:{pct(min(vals), lo, hi):.2f}%;width:{pct(max(vals), lo, hi) - pct(min(vals), lo, hi):.2f}%"></span>
        <span class="strip-median" style="left:{pct(med, lo, hi):.2f}%"></span>
        {dots}
      </div>
      <div class="strip-sum"><span>{fmt(med)}</span> {t("median")} · <span>{num(format(max(vals) / min(vals), ".1f"))}×</span> {t("spread")}</div>
    </div>""")
    axis = "".join(f'<span style="left:{pct(t, lo, hi):.2f}%">{tick(t)}</span>' for t in ticks)
    return f"""
<div class="strip" data-highlight>
  {"".join(out)}
  <div class="strip-row strip-axis-row"><span></span><div class="strip-axis">{axis}</div><span></span></div>
</div>"""


def composition(samples):
    total = sum(s["total_tokens"] for s in samples)
    share = lambda n: num(f"{n / total * 100:.1f}")  # noqa: E731
    parts = [
        ("cache", t("cache"), sum(s["cache_read"] for s in samples)),
        ("output", t("output"), sum(s["output_tokens"] for s in samples)),
        ("input", t("input"), sum(s["input_tokens"] for s in samples)),
    ]
    bar = "".join(
        f'<span class="comp-seg is-{key}" style="flex-grow:{n}" '
        f'data-tip="{tip(label, comma(n) + " " + t("tokens") + " · " + share(n) + "%")}"></span>'
        for key, label, n in parts
    )
    legend = "".join(
        f'<li><i class="sw is-{key}"></i>{label}<b>{share(n)}%</b></li>' for key, label, n in parts
    )
    label = t("composition")
    return f'<div class="comp-bar" role="img" aria-label="{label}">{bar}</div><ul class="comp-legend">{legend}</ul>'


def sample_table(samples):
    rows = sorted(samples, key=lambda s: (s["task"], s["spec_version"], s["language"], s["round"]))
    body = "".join(
        f"<tr><td>{s['round']}</td><td>{s['task']} v{s['spec_version']}</td><td>{s['name']}</td>"
        f"<td>{s['passed']}/{s['total']}</td><td>{comma(s['output_tokens'])}</td><td>{comma(s['total_tokens'])}</td>"
        f"<td>{s['turns']}</td><td>{usd(s['cost_usd'])}</td></tr>"
        for s in rows
    )
    head = "".join(f"<th>{c}</th>" for c in t("columns"))
    return f'<table class="data"><thead><tr>{head}</tr></thead><tbody>{body}</tbody></table>'


# ——— page ———


def render(samples):
    """Fill the current locale's template; numbers are formatted for LOCALE."""
    values = task_values(samples)
    wf = workflow_v2(samples)
    total = sum(s["total_tokens"] for s in samples)
    cache_shares = [s["cache_read"] / s["total_tokens"] for s in samples]
    py = [s["output_tokens"] for s in wf if s["language"] == "python"]
    js_total = [s["total_tokens"] for s in wf if s["language"] == "javascript"]

    fill = {
        "sessions": str(len(samples)),
        "model": esc(samples[0]["model_version"]),
        "board": board(values),
        "strip_output": strip(samples, "output_tokens", 60_000, 180_000, range(60_000, 180_001, 30_000), k,
                              lambda v: f"{v // 1000}k"),
        "strip_total": strip(samples, "total_tokens", 0, 12_000_000, range(0, 12_000_001, 3_000_000), k,
                             lambda v: f"{v // 1_000_000}M" if v else "0"),
        "py_min": k(min(py)),
        "py_max": k(max(py)),
        "js_total_ratio": num(f"{max(js_total) / min(js_total):.1f}"),
        "cache_pct": f"{sum(s['cache_read'] for s in samples) / total * 100:.0f}",
        "cache_lo": f"{min(cache_shares) * 100:.0f}",
        "cache_hi": f"{max(cache_shares) * 100:.0f}",
        "reasoning_pct": f"{sum(s['reasoning'] for s in samples) / sum(s['output_tokens'] for s in samples) * 100:.0f}",
        "composition": composition(samples),
        "samples_table": sample_table(samples),
        "repo": REPO,
    }

    page = (SRC / LOCALE / "index.html").read_text()
    for key, value in fill.items():
        page = page.replace("{{" + key + "}}", value)
    if "{{" in page:
        raise SystemExit(f"unfilled placeholder near: {page[page.index('{{'):][:40]}")
    return page


def build():
    global LOCALE
    samples = load()

    if DIST.exists():
        shutil.rmtree(DIST)
    shutil.copytree(SRC, DIST, ignore=shutil.ignore_patterns("index.html", "icons"))
    for LOCALE in STRINGS:
        (DIST / LOCALE / "index.html").write_text(render(samples))
    for name in ("dataset.csv", "dataset.json"):
        shutil.copy(ROOT / "results" / name, DIST / name)
    print(f"wrote {DIST} ({', '.join(f'/{l}' for l in STRINGS)}; {len(samples)} samples)")


if __name__ == "__main__":
    build()
