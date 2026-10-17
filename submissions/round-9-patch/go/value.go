package main

import (
	"encoding/json"
	"math/big"
)

// deepCopy clones maps and arrays recursively. Scalars (including json.Number)
// are immutable and may be shared.
func deepCopy(v any) any {
	switch x := v.(type) {
	case map[string]any:
		m := make(map[string]any, len(x))
		for k, val := range x {
			m[k] = deepCopy(val)
		}
		return m
	case []any:
		a := make([]any, len(x))
		for i, val := range x {
			a[i] = deepCopy(val)
		}
		return a
	default:
		return v
	}
}

// jsonEqual implements structural equality from SPEC section 4.
func jsonEqual(a, b any) bool {
	switch x := a.(type) {
	case nil:
		return b == nil
	case bool:
		y, ok := b.(bool)
		return ok && x == y
	case string:
		y, ok := b.(string)
		return ok && x == y
	case json.Number:
		y, ok := b.(json.Number)
		return ok && numbersEqual(x, y)
	case []any:
		y, ok := b.([]any)
		if !ok || len(x) != len(y) {
			return false
		}
		for i := range x {
			if !jsonEqual(x[i], y[i]) {
				return false
			}
		}
		return true
	case map[string]any:
		y, ok := b.(map[string]any)
		if !ok || len(x) != len(y) {
			return false
		}
		for k, xv := range x {
			yv, ok := y[k]
			if !ok || !jsonEqual(xv, yv) {
				return false
			}
		}
		return true
	default:
		return false
	}
}

func numbersEqual(a, b json.Number) bool {
	ra, ok := parseNumber(a.String())
	if !ok {
		return a.String() == b.String()
	}
	rb, ok := parseNumber(b.String())
	if !ok {
		return a.String() == b.String()
	}
	return ra.Cmp(rb) == 0
}

func parseNumber(s string) (*big.Rat, bool) {
	r := new(big.Rat)
	if _, ok := r.SetString(s); ok {
		return r, true
	}
	return nil, false
}
