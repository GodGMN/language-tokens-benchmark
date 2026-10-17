package main

import (
	"bytes"
	"encoding/json"
	"math/big"
	"sort"
	"strconv"
	"strings"
)

// Value is any JSON value produced by encoding/json with UseNumber:
// nil, bool, json.Number, string, []interface{}, map[string]interface{}.
type Value = interface{}

// ---------- error type ----------

type wfError struct {
	Code   string
	Msg    string
	stepID *string // optional step attribution for validation errors
}

func (e *wfError) Error() string { return e.Code + ": " + e.Msg }

func errf(code, format string, a ...interface{}) *wfError {
	return &wfError{Code: code, Msg: sprintf(format, a...)}
}

// ---------- numbers ----------

func num(i int64) json.Number { return json.Number(strconv.FormatInt(i, 10)) }

// numToI64 returns the value of a JSON integer literal that fits in i64.
// Literals with a fraction or exponent are rejected.
func numToI64(n json.Number) (int64, bool) {
	s := n.String()
	if strings.ContainsAny(s, ".eE+") {
		return 0, false
	}
	i, err := strconv.ParseInt(s, 10, 64)
	if err != nil {
		return 0, false
	}
	return i, true
}

func numRat(n json.Number) *big.Rat {
	r := new(big.Rat)
	if _, ok := r.SetString(n.String()); ok {
		return r
	}
	f, _ := n.Float64()
	r.SetFloat64(f)
	return r
}

func numEq(a, b json.Number) bool {
	ai, aok := numToI64(a)
	bi, bok := numToI64(b)
	if aok && bok {
		return ai == bi
	}
	return numRat(a).Cmp(numRat(b)) == 0
}

func numCmp(a, b json.Number) int {
	ai, aok := numToI64(a)
	bi, bok := numToI64(b)
	if aok && bok {
		switch {
		case ai < bi:
			return -1
		case ai > bi:
			return 1
		default:
			return 0
		}
	}
	return numRat(a).Cmp(numRat(b))
}

func numIsZero(n json.Number) bool {
	if i, ok := numToI64(n); ok {
		return i == 0
	}
	return numRat(n).Sign() == 0
}

// ---------- type names and ordering ----------

func typeName(v Value) string {
	switch v.(type) {
	case nil:
		return "null"
	case bool:
		return "boolean"
	case json.Number:
		return "number"
	case string:
		return "string"
	case []interface{}:
		return "array"
	case map[string]interface{}:
		return "object"
	default:
		return "null"
	}
}

func typeRank(v Value) int {
	switch v.(type) {
	case json.Number:
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
	default:
		return 3
	}
}

func sortedKeys(m map[string]interface{}) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}

func cmpInt(a, b int) int {
	switch {
	case a < b:
		return -1
	case a > b:
		return 1
	default:
		return 0
	}
}

// compareValues implements the total order from SPEC §3.
func compareValues(a, b Value) int {
	ra, rb := typeRank(a), typeRank(b)
	if ra != rb {
		return cmpInt(ra, rb)
	}
	switch av := a.(type) {
	case nil:
		return 0
	case bool:
		bv := b.(bool)
		if av == bv {
			return 0
		}
		if !av {
			return -1
		}
		return 1
	case json.Number:
		return numCmp(av, b.(json.Number))
	case string:
		return strings.Compare(av, b.(string))
	case []interface{}:
		bv := b.([]interface{})
		n := len(av)
		if len(bv) < n {
			n = len(bv)
		}
		for i := 0; i < n; i++ {
			if c := compareValues(av[i], bv[i]); c != 0 {
				return c
			}
		}
		return cmpInt(len(av), len(bv))
	case map[string]interface{}:
		bv := b.(map[string]interface{})
		ak := sortedKeys(av)
		bk := sortedKeys(bv)
		n := len(ak)
		if len(bk) < n {
			n = len(bk)
		}
		for i := 0; i < n; i++ {
			if c := strings.Compare(ak[i], bk[i]); c != 0 {
				return c
			}
		}
		if c := cmpInt(len(ak), len(bk)); c != 0 {
			return c
		}
		for _, k := range ak {
			if c := compareValues(av[k], bv[k]); c != 0 {
				return c
			}
		}
		return 0
	}
	return 0
}

// structEqual implements structural equality from SPEC §3.
func structEqual(a, b Value) bool {
	if typeRank(a) != typeRank(b) {
		return false
	}
	switch av := a.(type) {
	case nil:
		return true
	case bool:
		return av == b.(bool)
	case json.Number:
		return numEq(av, b.(json.Number))
	case string:
		return av == b.(string)
	case []interface{}:
		bv := b.([]interface{})
		if len(av) != len(bv) {
			return false
		}
		for i := range av {
			if !structEqual(av[i], bv[i]) {
				return false
			}
		}
		return true
	case map[string]interface{}:
		bv := b.(map[string]interface{})
		if len(av) != len(bv) {
			return false
		}
		for k, v := range av {
			ov, ok := bv[k]
			if !ok || !structEqual(v, ov) {
				return false
			}
		}
		return true
	}
	return false
}

func truthy(v Value) bool {
	switch t := v.(type) {
	case nil:
		return false
	case bool:
		return t
	case json.Number:
		return !numIsZero(t)
	case string:
		return t != ""
	case []interface{}:
		return len(t) > 0
	case map[string]interface{}:
		return len(t) > 0
	}
	return false
}

// ---------- reference detection ----------

// asRef reports whether v is a {"$ref": "<string>"} object.
func asRef(v Value) (string, bool) {
	obj, ok := v.(map[string]interface{})
	if !ok || len(obj) != 1 {
		return "", false
	}
	rv, ok := obj["$ref"]
	if !ok {
		return "", false
	}
	s, ok := rv.(string)
	if !ok {
		return "", false
	}
	return s, true
}

// ---------- JSON encoding ----------

// compactJSON encodes v as compact JSON without HTML escaping.
func compactJSON(v Value) (string, error) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(v); err != nil {
		return "", err
	}
	return strings.TrimSuffix(buf.String(), "\n"), nil
}
