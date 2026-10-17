package main

import (
	"encoding/json"
	"math/big"
	"sort"
	"strconv"
)

// ---------------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------------

// isIntLiteral reports whether s is a plain (optionally negative) decimal
// integer literal, with no fraction or exponent part.
func isIntLiteral(s string) bool {
	if s == "" {
		return false
	}
	i := 0
	if s[0] == '-' {
		i = 1
		if len(s) == 1 {
			return false
		}
	}
	for ; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return false
		}
	}
	return true
}

func toIntLiteral(v interface{}) (int64, bool) {
	switch x := v.(type) {
	case int64:
		return x, true
	case json.Number:
		s := string(x)
		if !isIntLiteral(s) {
			return 0, false
		}
		n, err := strconv.ParseInt(s, 10, 64)
		if err != nil {
			return 0, false
		}
		return n, true
	}
	return 0, false
}

func isNumber(v interface{}) bool {
	switch v.(type) {
	case int64, json.Number, float64:
		return true
	}
	return false
}

func toRat(v interface{}) (*big.Rat, bool) {
	switch x := v.(type) {
	case int64:
		return new(big.Rat).SetInt64(x), true
	case json.Number:
		if r, ok := new(big.Rat).SetString(string(x)); ok {
			return r, true
		}
		if f, err := strconv.ParseFloat(string(x), 64); err == nil {
			r := new(big.Rat)
			if r.SetFloat64(f) != nil {
				return r, true
			}
		}
	case float64:
		r := new(big.Rat)
		if r.SetFloat64(x) != nil {
			return r, true
		}
	}
	return nil, false
}

// numCmp compares two numeric JSON values (-1, 0, 1).
func numCmp(a, b interface{}) int {
	ai, aok := a.(int64)
	bi, bok := b.(int64)
	if aok && bok {
		if ai < bi {
			return -1
		}
		if ai > bi {
			return 1
		}
		return 0
	}
	ar, ok1 := toRat(a)
	br, ok2 := toRat(b)
	if ok1 && ok2 {
		return ar.Cmp(br)
	}
	// Should not happen for values produced by the JSON decoder.
	af, _ := strconv.ParseFloat(stringifyValue(a), 64)
	bf, _ := strconv.ParseFloat(stringifyValue(b), 64)
	if af < bf {
		return -1
	}
	if af > bf {
		return 1
	}
	return 0
}

// ---------------------------------------------------------------------------
// Type names / total order
// ---------------------------------------------------------------------------

func typeName(v interface{}) string {
	switch v.(type) {
	case nil:
		return "null"
	case bool:
		return "boolean"
	case int64, json.Number, float64:
		return "number"
	case string:
		return "string"
	case []interface{}:
		return "array"
	case map[string]interface{}:
		return "object"
	}
	return "null"
}

func typeRank(v interface{}) int {
	switch v.(type) {
	case int64, json.Number, float64:
		return 0
	case string:
		return 1
	case bool:
		return 2
	case nil:
		return 3
	case []interface{}:
		return 4
	case map[string]interface{}:
		return 5
	}
	return 3
}

func sortedKeys(m map[string]interface{}) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys) // byte-wise for UTF-8 strings
	return keys
}

// compareValues implements the total order from SPEC section 3.
func compareValues(a, b interface{}) int {
	ra, rb := typeRank(a), typeRank(b)
	if ra != rb {
		if ra < rb {
			return -1
		}
		return 1
	}
	switch ra {
	case 0: // number
		return numCmp(a, b)
	case 1: // string
		x, y := a.(string), b.(string)
		if x < y {
			return -1
		}
		if x > y {
			return 1
		}
		return 0
	case 2: // boolean
		x, y := a.(bool), b.(bool)
		if x == y {
			return 0
		}
		if !x {
			return -1
		}
		return 1
	case 3: // null
		return 0
	case 4: // array
		x, y := a.([]interface{}), b.([]interface{})
		n := len(x)
		if len(y) < n {
			n = len(y)
		}
		for i := 0; i < n; i++ {
			if c := compareValues(x[i], y[i]); c != 0 {
				return c
			}
		}
		if len(x) < len(y) {
			return -1
		}
		if len(x) > len(y) {
			return 1
		}
		return 0
	case 5: // object
		x, y := a.(map[string]interface{}), b.(map[string]interface{})
		kx, ky := sortedKeys(x), sortedKeys(y)
		n := len(kx)
		if len(ky) < n {
			n = len(ky)
		}
		for i := 0; i < n; i++ {
			if kx[i] < ky[i] {
				return -1
			}
			if kx[i] > ky[i] {
				return 1
			}
		}
		if len(kx) < len(ky) {
			return -1
		}
		if len(kx) > len(ky) {
			return 1
		}
		for _, k := range kx {
			if c := compareValues(x[k], y[k]); c != 0 {
				return c
			}
		}
		return 0
	}
	return 0
}

// equalsValues implements structural equality from SPEC section 3.
func equalsValues(a, b interface{}) bool {
	if isNumber(a) && isNumber(b) {
		return numCmp(a, b) == 0
	}
	switch x := a.(type) {
	case nil:
		return b == nil
	case bool:
		y, ok := b.(bool)
		return ok && x == y
	case string:
		y, ok := b.(string)
		return ok && x == y
	case []interface{}:
		y, ok := b.([]interface{})
		if !ok || len(x) != len(y) {
			return false
		}
		for i := range x {
			if !equalsValues(x[i], y[i]) {
				return false
			}
		}
		return true
	case map[string]interface{}:
		y, ok := b.(map[string]interface{})
		if !ok || len(x) != len(y) {
			return false
		}
		for k, xv := range x {
			yv, ok := y[k]
			if !ok || !equalsValues(xv, yv) {
				return false
			}
		}
		return true
	}
	return false
}

// truthy implements JSON truthiness used by filter predicates.
func truthy(v interface{}) bool {
	switch x := v.(type) {
	case nil:
		return false
	case bool:
		return x
	case string:
		return x != ""
	case int64, json.Number, float64:
		return numCmp(x, int64(0)) != 0
	case []interface{}:
		return len(x) != 0
	case map[string]interface{}:
		return len(x) != 0
	}
	return false
}

// ---------------------------------------------------------------------------
// Stringify (compact JSON)
// ---------------------------------------------------------------------------

func stringifyValue(v interface{}) string {
	b, err := json.Marshal(v)
	if err != nil {
		return ""
	}
	return string(b)
}
