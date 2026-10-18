import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runner import check


class GradingTests(unittest.TestCase):
    def grade(self, expected, actual, envelope="generic"):
        return check(
            {"expected_exit_code": 0, "expected_output": expected},
            {"exit_code": 0, "stdout_valid_json": True, "stdout_json": actual},
            envelope,
        )

    def test_boolean_is_not_a_number_even_inside_arrays(self):
        for expected, actual in [(True, 1), (False, 0), ([True], [1])]:
            with self.subTest(expected=expected):
                self.assertEqual(self.grade({"value": expected}, {"value": actual}), ["output mismatch"])

    def test_user_message_fields_are_compared(self):
        self.assertEqual(self.grade(
            {"status": "ok", "document": {"message": "hello"}},
            {"status": "ok", "document": {"message": "wrong"}},
        ), ["output mismatch"])

    def test_generic_error_message_is_free_form_but_index_is_compared(self):
        expected = {"status": "error", "code": "BAD", "index": 0, "message": "first"}
        self.assertEqual(self.grade(expected, {**expected, "message": "second"}), [])
        self.assertEqual(self.grade(expected, {**expected, "index": 1}), ["output mismatch"])

    def test_workflow_only_ignores_error_message(self):
        expected = {"status": "failed", "results": {"a": {"message": "data"}},
                    "execution_order": ["a"], "skipped": [],
                    "error": {"code": "BAD", "step": "b", "message": "first"}}
        actual = {**expected, "error": {**expected["error"], "message": "second"}}
        self.assertEqual(self.grade(expected, actual, "workflow"), [])
        actual["results"] = {"a": {"message": "changed"}}
        self.assertEqual(self.grade(expected, actual, "workflow"), ["output mismatch"])

    def test_scalar_response_fails_without_crashing_grader(self):
        self.assertEqual(self.grade({"status": "ok", "value": 1}, 1), ["output mismatch"])

    def test_json_object_order_does_not_matter(self):
        self.assertEqual(self.grade({"a": 1, "b": [2, 3]}, {"b": [2, 3], "a": 1}), [])
        self.assertEqual(self.grade({"a": [2, 3]}, {"a": [3, 2]}), ["output mismatch"])


if __name__ == "__main__":
    unittest.main()
