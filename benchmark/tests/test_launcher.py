import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest


class LauncherTests(unittest.TestCase):
    def test_agent_runs_in_its_submission_directory(self):
        source = Path(__file__).resolve().parents[2] / "scripts" / "launch-round.sh"
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "scripts").mkdir()
            shutil.copy(source, root / "scripts" / source.name)
            config = root / "benchmark/rounds/example"
            config.mkdir(parents=True)
            (config / "round.json").write_text(json.dumps({
                "id": "example", "submissions": "submissions/example",
                "languages": {"python": {"session": "test-python"}},
            }))
            (config / "prompt-python.md").write_text("test prompt")
            submission = root / "submissions/example/python"
            submission.mkdir(parents=True)
            (root / "bin").mkdir()
            pi = root / "bin/pi"
            pi.write_text("#!/bin/sh\npwd\n")
            pi.chmod(0o755)
            env = {**os.environ, "PATH": str(root / "bin") + os.pathsep + os.environ["PATH"]}
            subprocess.run(["bash", str(root / "scripts/launch-round.sh"), "example"],
                           env=env, check=True, capture_output=True)
            log = root / "results/example/python/agent.log"
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if log.exists() and log.read_text().strip():
                    break
                time.sleep(0.05)
            self.assertEqual(log.read_text().strip(), str(submission))


if __name__ == "__main__":
    unittest.main()
