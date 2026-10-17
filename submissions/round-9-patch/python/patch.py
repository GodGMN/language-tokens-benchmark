#!/usr/bin/env python3
"""RFC-6902-style JSON Patch engine driven through a small CLI.

Usage:
    python3 patch.py apply <request.json>
    python3 patch.py apply -            # request read from stdin

The request envelope is::

    {"document": <any JSON>, "patch": [<operation>, ...]}

Exactly one JSON document is written to stdout.  Diagnostics, if any, go to
stderr.  See tasks/patch/SPEC.md for the full contract.
"""

from __future__ import annotations

import copy
import json
import math
import re
import sys

# A valid array index is exactly "0" or a non-zero digit followed by digits.
_ARRAY_INDEX_RE = re.compile(r"^(?:0|[1-9][0-9]*)$")

_KNOWN_OPS = {
    "add": frozenset({"value"}),
    "remove": frozenset(),
    "replace": frozenset({"value"}),
    "move": frozenset({"from"}),
    "copy": frozenset({"from"}),
    "test": frozenset({"value"}),
}

_EXIT_CODES = {
    "INVALID_REQUEST": 2,
    "INVALID_PATH": 1,
    "PATH_NOT_FOUND": 1,
    "INDEX_ERROR": 1,
    "UNKNOWN_OP": 1,
    "TEST_FAILED": 1,
    "INVALID_MOVE": 1,
    "INTERNAL": 3,
}


class PatchFailure(Exception):
    """A classified patch/envelope failure."""

    def __init__(self, code: str, message: str = "", index=None):
        super().__init__(message or code)
        self.code = code
        self.message = message or code
        self.index = index


# ---------------------------------------------------------------------------
# JSON pointer handling (RFC 6901)
# ---------------------------------------------------------------------------


def _unescape_segment(segment: str) -> str:
    """Unescape one pointer reference token.

    ``~1`` -> ``/`` and ``~0`` -> ``~``.  The scan is done left to right so
    that ``~01`` becomes ``~1`` (not ``/``), matching RFC 6901.  A ``~`` that
    is not followed by ``0`` or ``1`` makes the pointer malformed.
    """
    if "~" not in segment:
        return segment
    out = []
    i = 0
    length = len(segment)
    while i < length:
        ch = segment[i]
        if ch == "~":
            if i + 1 >= length or segment[i + 1] not in ("0", "1"):
                raise PatchFailure("INVALID_PATH", "invalid '~' escape in pointer")
            out.append("~" if segment[i + 1] == "0" else "/")
            i += 2
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def parse_pointer(path: str):
    """Return the list of unescaped reference tokens for *path*."""
    if not isinstance(path, str):
        raise PatchFailure("INVALID_REQUEST", "path must be a string")
    if path == "":
        return []
    if not path.startswith("/"):
        raise PatchFailure("INVALID_PATH", "non-empty pointer must start with '/'")
    return [_unescape_segment(part) for part in path[1:].split("/")]


def _parse_array_index(token: str) -> int:
    """Validate and convert an array reference token (never accepts '-')."""
    if not _ARRAY_INDEX_RE.match(token):
        raise PatchFailure("INDEX_ERROR", "invalid array index: %r" % (token,))
    # Any 19+ digit index is far beyond any possible list length in this
    # benchmark, and capping avoids huge int conversion costs.
    if len(token) > 18:
        return 10 ** 18
    return int(token)


# ---------------------------------------------------------------------------
# Structural equality (SPEC section 4)
# ---------------------------------------------------------------------------


def json_equal(left, right) -> bool:
    """Structural equality for parsed JSON values.

    Booleans are deliberately kept distinct from numbers even though Python
    bool is a subclass of int.  Integers and floats form one numeric category
    and compare by mathematical value.
    """
    left_is_bool = isinstance(left, bool)
    right_is_bool = isinstance(right, bool)
    if left_is_bool or right_is_bool:
        return left_is_bool and right_is_bool and left == right

    if left is None or right is None:
        return left is None and right is None

    left_is_num = isinstance(left, (int, float))
    right_is_num = isinstance(right, (int, float))
    if left_is_num or right_is_num:
        if not (left_is_num and right_is_num):
            return False
        return left == right

    if isinstance(left, str) or isinstance(right, str):
        return isinstance(left, str) and isinstance(right, str) and left == right

    if isinstance(left, list) or isinstance(right, list):
        if not (isinstance(left, list) and isinstance(right, list)):
            return False
        if len(left) != len(right):
            return False
        return all(json_equal(a, b) for a, b in zip(left, right))

    if isinstance(left, dict) or isinstance(right, dict):
        if not (isinstance(left, dict) and isinstance(right, dict)):
            return False
        if set(left.keys()) != set(right.keys()):
            return False
        return all(json_equal(left[key], right[key]) for key in left)

    return False


# ---------------------------------------------------------------------------
# Document traversal helpers
# ---------------------------------------------------------------------------


def _step_into(current, token):
    """Descend one level for traversal (token must already exist)."""
    if isinstance(current, dict):
        if token not in current:
            raise PatchFailure("PATH_NOT_FOUND", "member not found: %r" % (token,))
        return current[token]
    if isinstance(current, list):
        index = _parse_array_index(token)
        if index >= len(current):
            raise PatchFailure("INDEX_ERROR", "array index out of range: %s" % (index,))
        return current[index]
    raise PatchFailure("PATH_NOT_FOUND", "cannot traverse into a scalar")


def resolve_value(document, tokens):
    """Resolve a full pointer token list to an existing value."""
    current = document
    for token in tokens:
        current = _step_into(current, token)
    return current


def resolve_parent(document, tokens):
    """Resolve the parent container for a non-empty pointer.

    Returns ``(parent, last_token)``.  The last token is not interpreted yet:
    object keys and array indices are handled by each operation.
    """
    parent = document
    for token in tokens[:-1]:
        parent = _step_into(parent, token)
    return parent, tokens[-1]


# ---------------------------------------------------------------------------
# Operations
# ---------------------------------------------------------------------------


def op_add(document, tokens, value):
    if not tokens:
        # Replaces the whole document.
        return value

    parent, token = resolve_parent(document, tokens)
    if isinstance(parent, dict):
        parent[token] = value
        return document
    if isinstance(parent, list):
        if token == "-":
            parent.append(value)
            return document
        index = _parse_array_index(token)
        if index > len(parent):
            raise PatchFailure("INDEX_ERROR", "array index out of range: %s" % (index,))
        parent.insert(index, value)
        return document
    raise PatchFailure("PATH_NOT_FOUND", "parent is not a container")


def _remove_and_return(document, tokens):
    """Remove the value at *tokens* and return ``(document, removed_value)``."""
    if not tokens:
        raise PatchFailure("INVALID_PATH", "cannot remove the document root")

    parent, token = resolve_parent(document, tokens)
    if isinstance(parent, dict):
        if token not in parent:
            raise PatchFailure("PATH_NOT_FOUND", "member not found: %r" % (token,))
        return document, parent.pop(token)
    if isinstance(parent, list):
        index = _parse_array_index(token)
        if index >= len(parent):
            raise PatchFailure("INDEX_ERROR", "array index out of range: %s" % (index,))
        return document, parent.pop(index)
    raise PatchFailure("PATH_NOT_FOUND", "parent is not a container")


def op_remove(document, tokens):
    document, _ = _remove_and_return(document, tokens)
    return document


def op_replace(document, tokens, value):
    if not tokens:
        return value

    parent, token = resolve_parent(document, tokens)
    if isinstance(parent, dict):
        if token not in parent:
            raise PatchFailure("PATH_NOT_FOUND", "member not found: %r" % (token,))
        parent[token] = value
        return document
    if isinstance(parent, list):
        index = _parse_array_index(token)
        if index >= len(parent):
            raise PatchFailure("INDEX_ERROR", "array index out of range: %s" % (index,))
        parent[index] = value
        return document
    raise PatchFailure("PATH_NOT_FOUND", "parent is not a container")


def op_move(document, path_tokens, from_tokens):
    # `from` must address an existing value first.
    resolve_value(document, from_tokens)

    if path_tokens == from_tokens:
        raise PatchFailure("INVALID_MOVE", "cannot move a value onto itself")

    prefix_length = len(from_tokens)
    if (
        len(path_tokens) > prefix_length
        and path_tokens[:prefix_length] == from_tokens
    ):
        raise PatchFailure("INVALID_MOVE", "cannot move a value into its descendant")

    document, value = _remove_and_return(document, from_tokens)
    return op_add(document, path_tokens, value)


def op_copy(document, path_tokens, from_tokens):
    value = resolve_value(document, from_tokens)
    value = copy.deepcopy(value)
    return op_add(document, path_tokens, value)


def op_test(document, tokens, expected):
    actual = resolve_value(document, tokens)
    if not json_equal(actual, expected):
        raise PatchFailure("TEST_FAILED", "test value does not match")
    return document


# ---------------------------------------------------------------------------
# Operation validation / dispatch
# ---------------------------------------------------------------------------


def validate_operation(operation):
    """Validate an operation's shape and return its name.

    This performs only shape checks; pointer parsing and execution happen
    afterwards so that the correct error code is produced.
    """
    if not isinstance(operation, dict):
        raise PatchFailure("INVALID_REQUEST", "operation must be an object")

    if "op" not in operation or not isinstance(operation["op"], str):
        raise PatchFailure("INVALID_REQUEST", "operation requires a string 'op'")
    if "path" not in operation or not isinstance(operation["path"], str):
        raise PatchFailure("INVALID_REQUEST", "operation requires a string 'path'")

    name = operation["op"]
    if name not in _KNOWN_OPS:
        raise PatchFailure("UNKNOWN_OP", "unknown operation: %r" % (name,))

    extras = _KNOWN_OPS[name]
    allowed = {"op", "path"} | set(extras)
    for key in operation:
        if key not in allowed:
            raise PatchFailure(
                "INVALID_REQUEST", "unexpected field %r for op %s" % (key, name)
            )

    for required in extras:
        if required not in operation:
            raise PatchFailure(
                "INVALID_REQUEST", "missing field %r for op %s" % (required, name)
            )

    if name in ("move", "copy") and not isinstance(operation["from"], str):
        raise PatchFailure("INVALID_REQUEST", "'from' must be a string")

    return name


def apply_operation(document, operation):
    """Validate and apply one operation to *document* (may return a new root)."""
    name = validate_operation(operation)
    path_tokens = parse_pointer(operation["path"])

    if name == "add":
        return op_add(document, path_tokens, operation["value"])
    if name == "remove":
        return op_remove(document, path_tokens)
    if name == "replace":
        return op_replace(document, path_tokens, operation["value"])
    if name == "move":
        from_tokens = parse_pointer(operation["from"])
        return op_move(document, path_tokens, from_tokens)
    if name == "copy":
        from_tokens = parse_pointer(operation["from"])
        return op_copy(document, path_tokens, from_tokens)
    if name == "test":
        return op_test(document, path_tokens, operation["value"])

    # validate_operation already rejects unknown names.
    raise PatchFailure("UNKNOWN_OP", "unknown operation: %r" % (name,))


def apply_patch(document, patch):
    """Apply all operations atomically (the caller discards on failure)."""
    result = document
    for index, operation in enumerate(patch):
        try:
            result = apply_operation(result, operation)
        except PatchFailure as failure:
            if failure.index is None:
                failure.index = index
            raise
    return result


# ---------------------------------------------------------------------------
# Request parsing / envelope
# ---------------------------------------------------------------------------


def _reject_constant(token):
    raise ValueError("non-standard JSON constant: %s" % (token,))


def _parse_float(token):
    value = float(token)
    if not math.isfinite(value):
        raise ValueError("number out of range: %s" % (token,))
    return value


def parse_json(raw):
    """Parse a strict JSON body from bytes/str."""
    return json.loads(
        raw,
        parse_constant=_reject_constant,
        parse_float=_parse_float,
    )


def validate_envelope(request):
    """Return ``(document, patch)`` or raise INVALID_REQUEST."""
    if not isinstance(request, dict):
        raise PatchFailure("INVALID_REQUEST", "request must be a JSON object")

    for key in request:
        if key not in ("document", "patch"):
            raise PatchFailure(
                "INVALID_REQUEST", "unknown top-level field: %r" % (key,)
            )

    if "document" not in request:
        raise PatchFailure("INVALID_REQUEST", "missing 'document'")
    if "patch" not in request:
        raise PatchFailure("INVALID_REQUEST", "missing 'patch'")
    if not isinstance(request["patch"], list):
        raise PatchFailure("INVALID_REQUEST", "'patch' must be an array")

    return request["document"], request["patch"]


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _error_response(code, index, message):
    return {
        "status": "error",
        "code": code,
        "index": index,
        "message": message,
    }


def read_request_bytes(source):
    if source == "-":
        return sys.stdin.buffer.read()
    with open(source, "rb") as handle:
        return handle.read()


def process(argv):
    """Return ``(response_dict, exit_code)``."""
    if len(argv) != 2 or argv[0] != "apply":
        return (
            _error_response(
                "INVALID_REQUEST", None, "usage: patch apply <request.json|->"
            ),
            2,
        )

    try:
        raw = read_request_bytes(argv[1])
    except OSError as exc:
        return (
            _error_response("INVALID_REQUEST", None, "cannot read request: %s" % exc),
            2,
        )

    try:
        request = parse_json(raw)
    except (ValueError, UnicodeError, TypeError) as exc:
        return (
            _error_response("INVALID_REQUEST", None, "invalid JSON: %s" % exc),
            2,
        )

    try:
        document, patch = validate_envelope(request)
    except PatchFailure as failure:
        return (
            _error_response(failure.code, None, failure.message),
            _EXIT_CODES.get(failure.code, 3),
        )

    try:
        result = apply_patch(document, patch)
    except PatchFailure as failure:
        return (
            _error_response(failure.code, failure.index, failure.message),
            _EXIT_CODES.get(failure.code, 1),
        )

    return {"status": "ok", "document": result}, 0


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    try:
        response, exit_code = process(argv)
    except Exception as exc:  # pragma: no cover - defensive
        sys.stderr.write("internal error: %s\n" % (exc,))
        response = _error_response("INTERNAL", None, "internal error: %s" % (exc,))
        exit_code = 3

    try:
        encoded = json.dumps(
            response,
            ensure_ascii=True,
            allow_nan=False,
            separators=(",", ":"),
        )
    except Exception as exc:  # pragma: no cover - defensive
        sys.stderr.write("internal serialization error: %s\n" % (exc,))
        encoded = (
            '{"status":"error","code":"INTERNAL","index":null,'
            '"message":"serialization failure"}'
        )
        exit_code = 3

    sys.stdout.write(encoded + "\n")
    sys.stdout.flush()
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
