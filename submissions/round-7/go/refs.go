package main

import (
	"strconv"
	"strings"
)

// execCtx carries the state needed for reference resolution and step-local
// attempt counters.
type execCtx struct {
	stepID   string
	results  map[string]interface{}
	skipped  map[string]bool
	closure  map[string]bool
	attempts map[string]int
}

// resolveValue recursively replaces $ref objects inside v.
func resolveValue(v Value, ctx *execCtx) (Value, *wfError) {
	if ref, ok := asRef(v); ok {
		return resolveRef(ref, ctx)
	}
	switch t := v.(type) {
	case []interface{}:
		out := make([]interface{}, len(t))
		for i, e := range t {
			r, err := resolveValue(e, ctx)
			if err != nil {
				return nil, err
			}
			out[i] = r
		}
		return out, nil
	case map[string]interface{}:
		out := make(map[string]interface{}, len(t))
		for k, e := range t {
			r, err := resolveValue(e, ctx)
			if err != nil {
				return nil, err
			}
			out[k] = r
		}
		return out, nil
	default:
		return v, nil
	}
}

func resolveRef(ref string, ctx *execCtx) (Value, *wfError) {
	parts := strings.Split(ref, ".")
	id := parts[0]
	if !ctx.closure[id] {
		return nil, errf(ErrInvalidReference, "reference %q is not a dependency of step %q", ref, ctx.stepID)
	}
	if ctx.skipped[id] {
		return nil, errf(ErrSkippedReference, "reference %q targets skipped step %q", ref, id)
	}
	cur, ok := ctx.results[id]
	if !ok {
		return nil, errf(ErrInvalidReference, "reference %q targets step %q which has not run", ref, id)
	}
	for _, seg := range parts[1:] {
		switch c := cur.(type) {
		case map[string]interface{}:
			nv, ok := c[seg]
			if !ok {
				return nil, errf(ErrInvalidReference, "reference %q: missing key %q", ref, seg)
			}
			cur = nv
		case []interface{}:
			if seg == "" || !isDigits(seg) {
				return nil, errf(ErrInvalidReference, "reference %q: invalid array index %q", ref, seg)
			}
			idx, err := strconv.ParseInt(seg, 10, 64)
			if err != nil || idx < 0 || idx >= int64(len(c)) {
				return nil, errf(ErrInvalidReference, "reference %q: array index %q out of range", ref, seg)
			}
			cur = c[idx]
		default:
			return nil, errf(ErrInvalidReference, "reference %q: cannot descend into %s", ref, typeName(cur))
		}
	}
	return cur, nil
}

func isDigits(s string) bool {
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return false
		}
	}
	return len(s) > 0
}

// evalCondition evaluates a validated `when` condition.
func evalCondition(v interface{}, ctx *execCtx) (bool, *wfError) {
	if _, ok := asRef(v); ok {
		resolved, err := resolveValue(v, ctx)
		if err != nil {
			return false, err
		}
		b, ok := resolved.(bool)
		if !ok {
			return false, errf(ErrInvalidCondition, "condition reference did not evaluate to a boolean (got %s)", typeName(resolved))
		}
		return b, nil
	}
	if b, ok := v.(bool); ok {
		return b, nil
	}
	m, ok := v.(map[string]interface{})
	if !ok {
		return false, errf(ErrInvalidCondition, "condition must be an object")
	}
	opv, ok := m["op"]
	if !ok {
		return false, errf(ErrInvalidCondition, "condition is missing \"op\"")
	}
	op, ok := opv.(string)
	if !ok {
		return false, errf(ErrInvalidCondition, "condition \"op\" must be a string")
	}
	switch op {
	case "all":
		arr, ok := m["conditions"].([]interface{})
		if !ok {
			return false, errf(ErrInvalidCondition, "\"all\" requires a conditions array")
		}
		for _, c := range arr {
			b, err := evalCondition(c, ctx)
			if err != nil {
				return false, err
			}
			if !b {
				return false, nil
			}
		}
		return true, nil
	case "any":
		arr, ok := m["conditions"].([]interface{})
		if !ok {
			return false, errf(ErrInvalidCondition, "\"any\" requires a conditions array")
		}
		for _, c := range arr {
			b, err := evalCondition(c, ctx)
			if err != nil {
				return false, err
			}
			if b {
				return true, nil
			}
		}
		return false, nil
	case "not":
		c, ok := m["condition"]
		if !ok {
			return false, errf(ErrInvalidCondition, "\"not\" requires a condition")
		}
		b, err := evalCondition(c, ctx)
		if err != nil {
			return false, err
		}
		return !b, nil
	case "eq", "ne", "gt", "gte", "lt", "lte":
		leftRaw, ok := m["left"]
		if !ok {
			return false, errf(ErrInvalidCondition, "condition %q is missing \"left\"", op)
		}
		rightRaw, ok := m["right"]
		if !ok {
			return false, errf(ErrInvalidCondition, "condition %q is missing \"right\"", op)
		}
		left, err := resolveValue(leftRaw, ctx)
		if err != nil {
			return false, err
		}
		right, err := resolveValue(rightRaw, ctx)
		if err != nil {
			return false, err
		}
		switch op {
		case "eq":
			return structEqual(left, right), nil
		case "ne":
			return !structEqual(left, right), nil
		}
		c := compareValues(left, right)
		switch op {
		case "gt":
			return c > 0, nil
		case "gte":
			return c >= 0, nil
		case "lt":
			return c < 0, nil
		default:
			return c <= 0, nil
		}
	}
	return false, errf(ErrInvalidCondition, "unknown condition op %q", op)
}
