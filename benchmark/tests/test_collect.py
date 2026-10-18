import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import collect


class CollectionTests(unittest.TestCase):
    def test_missing_session_preserves_published_metrics(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            out = root / "example"
            out.mkdir()
            published = out / "metrics.json"
            original = '{"published": "must survive"}\n'
            published.write_text(original)
            session = root / "session.jsonl"
            session.write_text(json.dumps({"type": "message", "message": {
                "role": "assistant", "usage": {"output": 42, "totalTokens": 42}}}) + "\n")
            cfg = {"id": "example", "languages": {
                "go": {"session": "missing"}, "python": {"session": str(session)}}}
            with patch.object(collect.common, "load_round", return_value=cfg), \
                 patch.object(collect, "RESULTS", root), \
                 patch.object(collect, "PI_SESSIONS", root / "absent"), \
                 patch.object(sys, "argv", ["collect.py", "--round", "example"]), \
                 contextlib.redirect_stdout(io.StringIO()), \
                 contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    collect.main()
            self.assertIn("Missing sessions", str(error.exception))
            self.assertEqual(published.read_text(), original)


if __name__ == "__main__":
    unittest.main()
