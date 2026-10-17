package main

import (
	"bytes"
	"encoding/json"
	"math/big"
	"sort"
	"strconv"
	"strings"
)

// JSON values are represented as:
//
//	nil                      -> null
//	bool                     -> boolean
//	json.Number              -> number (raw decimal text is preserved)
//	string                   -> string
//	[]interface{}            -> array
//	map[string]interface{}   -> object
//
// Numbers are compared exactly using math/big.Rat so that 1 and 1.0 are the
// same numeric value. Numeric operation parameters are extracted as i64.

func numFromInt(v int64) json.Number {
	return json.Number(strconv.FormatInt(v, 10))
}

func ratOf(v interface{}) (*big.Rat, bool) {
	n, ok := v.(json.Number)
	if !ok {
		return nil, false
	}
	r, ok := new(big.Rat).SetString(string(n))
	if !ok {
		return nil, false
	}
	return r, true
}

// asI64 accepts a JSON number with no fractional part that fits in i64.
// Values such as 1.0 and 1e2 are accepted (they have no fractional part).
func asI64(v interface{}) (int64, bool) {
	r, ok := ratOf(v)
	if !ok || !r.IsInt() {
		return 0, false
	}
	n := r.Num()
	if !n.IsInt64() {
		return 0, false
	}
	return n.Int64(), true
}

func typeRank(v interface{}) int {
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
	}
	return 6
}

func typeName(v interface{}) string {
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
	}
	return "unknown"
}

func sortedKeys(m map[string]interface{}) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}

// compareValues implements the total order from the spec:
// number < string < boolean < null < array < object.
func compareValues(a, b interface{}) int {
	ra, rb := typeRank(a), typeRank(b)
	if ra != rb {
		if ra < rb {
			return -1
		}
		return 1
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
		ar, _ := ratOf(av)
		br, _ := ratOf(b.(json.Number))
		return ar.Cmp(br)
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
		if len(av) < len(bv) {
			return -1
		}
		if len(av) > len(bv) {
			return 1
		}
		return 0
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
		if len(ak) < len(bk) {
			return -1
		}
		if len(ak) > len(bk) {
			return 1
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

// valuesEqual is structural equality. Numbers compare by numeric value.
func valuesEqual(a, b interface{}) bool {
	if typeRank(a) != typeRank(b) {
		return false
	}
	switch av := a.(type) {
	case nil:
		return true
	case bool:
		return av == b.(bool)
	case json.Number:
		ar, _ := ratOf(av)
		br, _ := ratOf(b.(json.Number))
		return ar.Cmp(br) == 0
	case string:
		return av == b.(string)
	case []interface{}:
		bv := b.([]interface{})
		if len(av) != len(bv) {
			return false
		}
		for i := range av {
			if !valuesEqual(av[i], bv[i]) {
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
			if !ok || !valuesEqual(v, ov) {
				return false
			}
		}
		return true
	}
	return false
}

// truthy implements the JSON truthiness used by filter predicates and
// the and/or/not utilities.
func truthy(v interface{}) bool {
	switch x := v.(type) {
	case nil:
		return false
	case bool:
		return x
	case json.Number:
		r, ok := ratOf(x)
		return !ok || r.Sign() != 0
	case string:
		return x != ""
	case []interface{}:
		return len(x) > 0
	case map[string]interface{}:
		return len(x) > 0
	}
	return false
}

// compactJSON renders a value as compact JSON text. HTML escaping is
// disabled so the resulting string is the plain JSON representation (the
// same as most JSON libraries' compact serialization).
func compactJSON(v interface{}) (string, bool) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(v); err != nil {
		return "", false
	}
	return strings.TrimSuffix(buf.String(), "\n"), true
}
