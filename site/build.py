#!/usr/bin/env python3
"""Build the Token Golf site from results/dataset.json.

    python3 site/build.py            # writes site/dist/

No dependencies. Copy lives in src/index.html; every number on the page is
computed here, so a new round only needs `benchmark/export.py` and a rebuild.
"""
import html
import json
import shutil
import statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = Path(__file__).resolve().parent / "src"
DIST = Path(__file__).resolve().parent / "dist"
DATASET = ROOT / "results" / "dataset.json"
REPO = "https://github.com/GodGMN/language-tokens-benchmark"

NAMES = {"python": "Python", "javascript": "JavaScript", "go": "Go", "rust": "Rust", "c": "C"}
TASKS = [
    ("workflow", "Workflow engine", "Workflow"),
    ("expr", "Expression evaluator", "Expr"),
    ("patch", "JSON Patch", "Patch"),
]


def esc(value):
    return html.escape(str(value), quote=True)


def k(n):
    """Compact token count: 95022 -> 95.0k, 3760000 -> 3.76M."""
    if n >= 1_000_000:
        return f"{n / 1_000_000:.2f}M"
    return f"{n / 1_000:.1f}k"


def comma(n):
    return f"{round(n):,}"


def usd(n):
    return f"${n:.3f}"


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
    for task, _, _ in TASKS:
        rows = workflow_v2(samples) if task == "workflow" else [s for s in samples if s["task"] == task]
        result[task] = {
            lang: (statistics.median(r["output_tokens"] for r in group), len(group))
            for lang, group in by_lang(rows).items()
        }
    return result


# ——— fragments ———


def tip(*lines):
    return esc("\n".join(lines))


def bump(values):
    """Rank-per-task chart: lines in an SVG, dots and labels in HTML."""
    xs = [7, 50, 93]
    ranks = {
        task: sorted(values[task], key=lambda lang: values[task][lang][0])
        for task, _, _ in TASKS
    }
    y = lambda r: (r + 0.5) / 5 * 100  # noqa: E731

    lines, dots = [], []
    for lang in NAMES:
        pts = [(xs[i], y(ranks[t].index(lang))) for i, (t, _, _) in enumerate(TASKS)]
        lines.append(
            f'<polyline data-lang="{lang}" points="{" ".join(f"{a},{b:.2f}" for a, b in pts)}" />'
        )
        for (x, yy), (task, title, _) in zip(pts, TASKS):
            tok, n = values[task][lang]
            rank = ranks[task].index(lang) + 1
            basis = f"median of {n} runs" if n > 1 else "single run"
            dots.append(
                f'<span class="bump-dot{" is-single" if n == 1 else ""}" data-lang="{lang}" '
                f'style="left:{x}%;top:{yy:.2f}%" '
                f'data-tip="{tip(f"{NAMES[lang]} · {title}", f"#{rank} · {comma(tok)} output tokens", basis)}"></span>'
            )

    def side(task, align):
        rows = []
        for lang in ranks[task]:
            tok, _ = values[task][lang]
            rows.append(
                f'<button type="button" class="bump-label is-{align}" data-lang="{lang}">'
                f'<span class="lang">{NAMES[lang]}</span><span class="val">{k(tok)}</span></button>'
            )
        return "".join(rows)

    def runs(task):
        n = values[task]["python"][1]
        return f"median of {n} runs" if n > 1 else "1 run"

    heads = "".join(
        f'<div class="bump-col" style="left:{x}%"><strong>{title}</strong><b>{short}</b><span>{runs(t)}</span></div>'
        for x, (t, title, short) in zip(xs, TASKS)
    )
    return f"""
<div class="bump" data-highlight>
  <div class="bump-heads"><div></div><div class="bump-heads-plot">{heads}</div><div></div></div>
  <div class="bump-grid">
    <div class="bump-side">{side("workflow", "left")}</div>
    <div class="bump-plot">
      <svg viewBox="0 0 100 100" preserveAspectRatio="none" aria-hidden="true">{"".join(lines)}</svg>
      {"".join(dots)}
    </div>
    <div class="bump-side">{side("patch", "right")}</div>
  </div>
  <p class="chart-note">Rank 1 = fewest session output tokens. Hollow dots are a single run. Middle-column values are in the table.</p>
</div>"""


def sample_tip(s):
    return tip(
        "{} · {}".format(s["name"], s["round"]),
        "{} output · {} total".format(comma(s["output_tokens"]), k(s["total_tokens"])),
        "{} turns · {} tool calls · {}".format(s["turns"], s["tool_calls"], usd(s["cost_usd"])),
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
      <div class="strip-sum"><span>{fmt(med)}</span> median · <span>{max(vals) / min(vals):.1f}×</span> spread</div>
    </div>""")
    axis = "".join(f'<span style="left:{pct(t, lo, hi):.2f}%">{tick(t)}</span>' for t in ticks)
    return f"""
<div class="strip" data-highlight>
  {"".join(out)}
  <div class="strip-row strip-axis-row"><span></span><div class="strip-axis">{axis}</div><span></span></div>
</div>"""


def composition(samples):
    total = sum(s["total_tokens"] for s in samples)
    parts = [
        ("cache", "Cached context re-read", sum(s["cache_read"] for s in samples)),
        ("output", "Output (code, tool calls, reasoning)", sum(s["output_tokens"] for s in samples)),
        ("input", "Fresh input", sum(s["input_tokens"] for s in samples)),
    ]
    bar = "".join(
        f'<span class="comp-seg is-{key}" style="flex-grow:{n}" '
        f'data-tip="{tip(label, f"{comma(n)} tokens · {n / total * 100:.1f}%")}"></span>'
        for key, label, n in parts
    )
    legend = "".join(
        f'<li><i class="sw is-{key}"></i>{label}<b>{n / total * 100:.1f}%</b></li>' for key, label, n in parts
    )
    return f'<div class="comp-bar" role="img" aria-label="Token composition across all sessions">{bar}</div><ul class="comp-legend">{legend}</ul>'


def awards(samples):
    rows = by_lang(workflow_v2(samples))

    def med(lang, key):
        return statistics.median(s[key] for s in rows[lang])

    def cv(lang):
        vals = [s["output_tokens"] for s in rows[lang]]
        return statistics.pstdev(vals) / statistics.mean(vals)

    cats = [
        ("Fewest tokens", "median session output", lambda l: med(l, "output_tokens"), comma),
        ("Cheapest", "median recorded cost", lambda l: med(l, "cost_usd"), usd),
        ("Fewest tool calls", "median per session", lambda l: med(l, "tool_calls"), lambda v: f"{v:g}"),
        ("Most consistent", "spread of output (CV)", cv, lambda v: f"{v * 100:.0f}%"),
    ]
    cards = []
    for title, sub, score, fmt in cats:
        best, second = sorted(rows, key=score)[:2]
        cards.append(f"""
    <article class="award">
      <span class="award-cat">{title}</span>
      <strong class="award-winner">{NAMES[best]}</strong>
      <span class="award-score">{fmt(score(best))} <em>{sub}</em></span>
      <span class="award-runner">runner-up {NAMES[second]} · {fmt(score(second))}</span>
    </article>""")
    return "".join(cards)


def table(samples, values):
    head = "".join(f"<th>{t}</th>" for _, t, _ in TASKS)
    body = []
    for lang in NAMES:
        cells = "".join(
            f"<td>{comma(values[t][lang][0])}<small> n={values[t][lang][1]}</small></td>" for t, _, _ in TASKS
        )
        body.append(f"<tr><th>{NAMES[lang]}</th>{cells}</tr>")
    return f'<table class="data"><thead><tr><th>Output tokens</th>{head}</tr></thead><tbody>{"".join(body)}</tbody></table>'


def sample_table(samples):
    rows = sorted(samples, key=lambda s: (s["task"], s["spec_version"], s["language"], s["round"]))
    body = "".join(
        f"<tr><td>{s['round']}</td><td>{s['task']} v{s['spec_version']}</td><td>{s['name']}</td>"
        f"<td>{s['passed']}/{s['total']}</td><td>{comma(s['output_tokens'])}</td><td>{comma(s['total_tokens'])}</td>"
        f"<td>{s['turns']}</td><td>{usd(s['cost_usd'])}</td></tr>"
        for s in rows
    )
    return (
        '<table class="data"><thead><tr><th>Round</th><th>Task</th><th>Language</th><th>Tests</th>'
        "<th>Output</th><th>Total</th><th>Turns</th><th>Cost</th></tr></thead>"
        f"<tbody>{body}</tbody></table>"
    )


# ——— page ———


def build():
    samples = load()
    values = task_values(samples)
    wf = workflow_v2(samples)
    total = sum(s["total_tokens"] for s in samples)
    cache_shares = [s["cache_read"] / s["total_tokens"] for s in samples]
    py = [s["output_tokens"] for s in wf if s["language"] == "python"]
    js_total = [s["total_tokens"] for s in wf if s["language"] == "javascript"]
    expr = sorted(v[0] for v in values["expr"].values())

    fill = {
        "sessions": str(len(samples)),
        "languages": str(len({s["language"] for s in samples})),
        "tasks": str(len({s["task"] for s in samples})),
        "tests_passed": comma(sum(s["passed"] for s in samples)),
        "total_tokens": f"{total / 1_000_000:.0f}M",
        "total_cost": f"${sum(s['cost_usd'] for s in samples):.2f}",
        "model": esc(samples[0]["model_version"]),
        "bump": bump(values),
        "rank_table": table(samples, values),
        "expr_gap": comma(expr[2] - expr[0]),
        "strip_output": strip(samples, "output_tokens", 60_000, 180_000, range(60_000, 180_001, 30_000), k,
                              lambda v: f"{v // 1000}k"),
        "strip_total": strip(samples, "total_tokens", 0, 12_000_000, range(0, 12_000_001, 3_000_000), k,
                             lambda v: f"{v // 1_000_000}M" if v else "0"),
        "py_min": k(min(py)),
        "py_max": k(max(py)),
        "js_total_ratio": f"{max(js_total) / min(js_total):.1f}",
        "cache_pct": f"{sum(s['cache_read'] for s in samples) / total * 100:.0f}",
        "cache_lo": f"{min(cache_shares) * 100:.0f}",
        "cache_hi": f"{max(cache_shares) * 100:.0f}",
        "reasoning_pct": f"{sum(s['reasoning'] for s in samples) / sum(s['output_tokens'] for s in samples) * 100:.0f}",
        "composition": composition(samples),
        "awards": awards(samples),
        "samples_table": sample_table(samples),
        "repo": REPO,
    }

    page = (SRC / "index.html").read_text()
    for key, value in fill.items():
        page = page.replace("{{" + key + "}}", value)
    if "{{" in page:
        raise SystemExit(f"unfilled placeholder near: {page[page.index('{{'):][:40]}")

    if DIST.exists():
        shutil.rmtree(DIST)
    shutil.copytree(SRC, DIST, ignore=shutil.ignore_patterns("index.html"))
    (DIST / "index.html").write_text(page)
    for name in ("dataset.csv", "dataset.json"):
        shutil.copy(ROOT / "results" / name, DIST / name)
    print(f"wrote {DIST}/index.html ({len(samples)} samples)")


if __name__ == "__main__":
    build()
