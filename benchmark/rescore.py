#!/usr/bin/env python3
"""Build and rescore committed submissions without launching or billing a model.

Usage: python3 benchmark/rescore.py [--round ROUND]
Existing metrics.json files are reused unchanged. Logs go to ignored results
files. The script exits nonzero if any build or scored conformance test fails.
"""
import argparse
import hashlib
import json
import platform
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

import common


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--round")
    args = parser.parse_args()
    configs = [common.ROUNDS / args.round / "round.json"] if args.round else sorted(common.ROUNDS.glob("*/round.json"))
    failures = []
    for config in configs:
        cfg = json.loads(config.read_text())
        rid = cfg["id"]
        out = common.RESULTS / rid
        out.mkdir(parents=True, exist_ok=True)
        log_path = out / "rescore.log"
        with log_path.open("w") as log:
            build = subprocess.run([sys.executable, str(common.HERE / "build.py"), "--all", "--round", rid], stdout=log, stderr=subprocess.STDOUT)
            if build.returncode:
                failures.append(rid)
                print(f"{rid}: BUILD FAILED; see {log_path}", flush=True)
                continue
            tests = subprocess.run([sys.executable, str(common.HERE / "runner.py"), "--all", "--round", rid], stdout=log, stderr=subprocess.STDOUT)
            evaluation = {
                "checkout_base_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=common.ROOT, text=True).strip(),
                "grader_sha256": hashlib.sha256((common.HERE / "runner.py").read_bytes()).hexdigest(),
                "grader": "json-comparison-v2", "evaluated_at": datetime.now(timezone.utc).isoformat(),
                "python": platform.python_version(), "platform": platform.platform(),
                "usage": "historical metrics.json retained; no new model calls",
                "test_exit_code": tests.returncode,
            }
            (out / "evaluation.json").write_text(json.dumps(evaluation, indent=2) + "\n")
            subprocess.run([sys.executable, str(common.HERE / "report.py"), "--round", rid], stdout=log, stderr=subprocess.STDOUT, check=True)
            if tests.returncode:
                failures.append(rid)
        summary = json.loads((out / "summary.json").read_text())
        samples = [r for r in summary["all"] if not r["reference"]]
        print(f"{rid}: " + ", ".join(f"{r['language']} {r['passed']}/{r['total']}" for r in samples), flush=True)
    for script in ["aggregate.py", "compare.py", "export.py", "site.py"]:
        subprocess.run([sys.executable, str(common.HERE / script)], check=True)
    if failures:
        sys.exit("Rounds with build or conformance failures: " + ", ".join(failures))


if __name__ == "__main__":
    main()
