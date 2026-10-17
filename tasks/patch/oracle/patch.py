#!/usr/bin/env python3
"""Reference implementation of the JSON Patch engine (SPEC: tasks/patch/SPEC.md)."""
import copy
import json
import re
import sys

IDX_RE = re.compile(r"^(0|[1-9][0-9]*)$")


class PatchError(Exception):
    def __init__(self, code, index, message=""):
        super().__init__(code)
        self.code, self.index, self.message = code, index, message


def unescape(tok):
    out, i = [], 0
    while i < len(tok):
        if tok[i] == "~":
            if i + 1 >= len(tok) or tok[i + 1] not in "01":
                raise PatchError("INVALID_PATH", None, f"bad escape in {tok!r}")
            out.append("/" if tok[i + 1] == "1" else "~")
            i += 2
        else:
            out.append(tok[i]); i += 1
    return "".join(out)


def parse_pointer(path):
    if path == "":
        return []
    if not path.startswith("/"):
        raise PatchError("INVALID_PATH", None, "pointer must start with /")
    return [unescape(t) for t in path[1:].split("/")]


def arr_index(tok, allow_dash=False):
    if allow_dash and tok == "-":
        return "-"
    if not IDX_RE.match(tok):
        raise PatchError("INDEX_ERROR", None, f"bad array index {tok!r}")
    return int(tok)


def resolve(doc, tokens, index):
    cur = doc
    for t in tokens:
        if isinstance(cur, dict):
            if t not in cur:
                raise PatchError("PATH_NOT_FOUND", index, f"missing key {t!r}")
            cur = cur[t]
        elif isinstance(cur, list):
            i = arr_index(t)
            if i >= len(cur):
                raise PatchError("INDEX_ERROR", index, f"index {i} out of range")
            cur = cur[i]
        else:
            raise PatchError("PATH_NOT_FOUND", index, "path through a scalar")
    return cur


def parent_of(doc, tokens, index):
    return resolve(doc, tokens[:-1], index)


def do_add(doc, tokens, value, index):
    if not tokens:
        return copy.deepcopy(value)
    parent = parent_of(doc, tokens, index)
    last = tokens[-1]
    if isinstance(parent, dict):
        parent[last] = copy.deepcopy(value)
    elif isinstance(parent, list):
        if last == "-":
            parent.append(copy.deepcopy(value))
        else:
            i = arr_index(last)
            if i > len(parent):
                raise PatchError("INDEX_ERROR", index, f"index {i} > length {len(parent)}")
            parent.insert(i, copy.deepcopy(value))
    else:
        raise PatchError("PATH_NOT_FOUND", index, "parent is a scalar")
    return doc


def do_remove(doc, tokens, index):
    if not tokens:
        raise PatchError("INVALID_PATH", index, "cannot remove the root")
    parent = parent_of(doc, tokens, index)
    last = tokens[-1]
    if isinstance(parent, dict):
        if last not in parent:
            raise PatchError("PATH_NOT_FOUND", index, f"missing key {last!r}")
        del parent[last]
    elif isinstance(parent, list):
        i = arr_index(last)
        if i >= len(parent):
            raise PatchError("INDEX_ERROR", index, f"index {i} out of range")
        del parent[i]
    else:
        raise PatchError("PATH_NOT_FOUND", index, "parent is a scalar")
    return doc


def structural_eq(a, b):
    if isinstance(a, bool) != isinstance(b, bool):
        return False
    if (a is None) != (b is None):
        return False
    if isinstance(a, bool):
        return a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return a == b
    if isinstance(a, str) and isinstance(b, str):
        return a == b
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(structural_eq(x, y) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(structural_eq(a[k], b[k]) for k in a)
    return False


OP_FIELDS = {
    "add": {"op", "path", "value"},
    "remove": {"op", "path"},
    "replace": {"op", "path", "value"},
    "move": {"op", "path", "from"},
    "copy": {"op", "path", "from"},
    "test": {"op", "path", "value"},
}


def validate_request(req):
    if (not isinstance(req, dict) or set(req) != {"document", "patch"}
            or not isinstance(req["patch"], list)):
        raise PatchError("INVALID_REQUEST", None, "bad request envelope")
    for i, op in enumerate(req["patch"]):
        if not isinstance(op, dict) or not isinstance(op.get("op"), str):
            raise PatchError("INVALID_REQUEST", i, "op must be an object with a string op")
        name = op["op"]
        if name not in OP_FIELDS:
            raise PatchError("UNKNOWN_OP", i, f"unknown op {name!r}")
        if set(op) != OP_FIELDS[name]:
            raise PatchError("INVALID_REQUEST", i, f"bad fields for {name}")
        if not isinstance(op.get("path"), str):
            raise PatchError("INVALID_REQUEST", i, "path must be a string")
        if name in ("move", "copy") and not isinstance(op.get("from"), str):
            raise PatchError("INVALID_REQUEST", i, "from must be a string")
    return req["patch"]


def apply_patch(document, patch):
    doc = copy.deepcopy(document)
    for i, op in enumerate(patch):
        name = op["op"]
        try:
            tokens = parse_pointer(op["path"])
            if name == "add":
                doc = do_add(doc, tokens, op["value"], i)
            elif name == "remove":
                doc = do_remove(doc, tokens, i)
            elif name == "replace":
                if not tokens:
                    doc = copy.deepcopy(op["value"])
                else:
                    do_remove(doc, tokens, i)
                    doc = do_add(doc, tokens, op["value"], i)
            elif name == "move":
                src = parse_pointer(op["from"])
                resolve(doc, src, i)
                if src == tokens:
                    raise PatchError("INVALID_MOVE", i, "move to itself")
                if tokens[:len(src)] == src:
                    raise PatchError("INVALID_MOVE", i, "move into a descendant")
                value = resolve(doc, src, i)
                doc = do_remove(doc, src, i)
                doc = do_add(doc, tokens, value, i)
            elif name == "copy":
                src = parse_pointer(op["from"])
                value = resolve(doc, src, i)
                doc = do_add(doc, tokens, value, i)
            elif name == "test":
                value = resolve(doc, tokens, i)
                if not structural_eq(value, op["value"]):
                    raise PatchError("TEST_FAILED", i, "test failed")
        except PatchError as e:
            if e.index is None:
                e.index = i
            raise
    return doc


def main():
    argv = sys.argv[1:]
    if len(argv) != 2 or argv[0] != "apply":
        sys.stderr.write("usage: patch apply <request.json|->\n")
        sys.exit(2)
    try:
        raw = sys.stdin.read() if argv[1] == "-" else open(argv[1]).read()
    except OSError as e:
        print(json.dumps({"status": "error", "code": "INVALID_REQUEST", "index": None, "message": str(e)}))
        sys.exit(2)
    try:
        req = json.loads(raw)
    except json.JSONDecodeError as e:
        print(json.dumps({"status": "error", "code": "INVALID_REQUEST", "index": None, "message": str(e)}))
        sys.exit(2)
    try:
        patch = validate_request(req)
        document = apply_patch(req["document"], patch)
    except PatchError as e:
        print(json.dumps({"status": "error", "code": e.code, "index": e.index, "message": e.message}))
        sys.exit(2 if e.code == "INVALID_REQUEST" else 1)
    except Exception as e:
        print(json.dumps({"status": "error", "code": "INTERNAL", "index": None,
                          "message": f"{type(e).__name__}: {e}"}))
        sys.exit(3)
    print(json.dumps({"status": "ok", "document": document}, separators=(",", ":")))
    sys.exit(0)


if __name__ == "__main__":
    main()
