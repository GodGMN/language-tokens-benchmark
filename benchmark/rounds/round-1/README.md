# Round 1 — one-shot implementation

Five independent pi sessions, one per language. Same model, same prompt except the language, no shared context.

## 1. Generate the prompts

```bash
python3 benchmark/rounds/round-1/make_prompts.py
```

This writes `prompt-<language>.md` for every language in `round.json`.

## 2. Run one session per language

Start pi **inside `submissions/round-1/<language>`** and give it a fixed session id so the
token collector can find it later:

```bash
cd submissions/round-1/python     && pi --session-id round1-python
cd submissions/round-1/javascript && pi --session-id round1-javascript
cd submissions/round-1/go         && pi --session-id round1-go
cd submissions/round-1/rust       && pi --session-id round1-rust
cd submissions/round-1/c          && pi --session-id round1-c
```

Paste the matching `prompt-<language>.md` as the first message. Let each session
finish, then commit its result (or leave the working tree dirty — the collector
reads the session file, not git).

Run these sequentially if you want a fair comparison of wall-clock time, or in
parallel if you only care about token counts.

## 3. Verify the implementations

```bash
python3 benchmark/build.py --all
python3 benchmark/runner.py --all --public     # public suite only during the round
```

## 4. Freeze and measure

```bash
python3 benchmark/collect.py --round round-1
python3 benchmark/report.py  --round round-1
```

Or do all of it at once:

```bash
./scripts/benchmark.sh round-1
```

## Session ids

`round.json` maps each language to its pi session id. If you used different ids
(or resumed a session), override them without editing the file:

```bash
python3 benchmark/collect.py --round round-1 --session go=round1-go --session rust=/path/to/session.jsonl
```

`collect.py` accepts a session id, a `.jsonl` path, or a cwd.
