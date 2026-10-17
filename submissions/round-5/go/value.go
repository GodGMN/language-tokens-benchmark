package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"math/big"
	"sort"
	"strconv"
	"strings"
)

// Value represents any JSON value. Concrete dynamic types used:
//
//	nil, bool, string, Number, []Value, map[string]Value
type Value interface{}

// Number preserves the original JSON number literal. This lets us distinguish
// integer literals ("42") from non-integer literals ("42.0", "4e1") which the
// spec requires for numeric parameters, while still preserving the literal for
// output and comparing numbers by numeric value.
type Number struct {
	Raw string
}

func (n Number) MarshalJSON() ([]byte, error) {
	if n.Raw == "" {
		return []byte("0"), nil
	}
	return []byte(n.Raw), nil
}

func num(i int64) Value { return Number{Raw: strconv.FormatInt(i, 10)} }

// decodeJSON parses exactly one JSON document from data.
func decodeJSON(data []byte) (Value, error) {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.UseNumber()
	var raw interface{}
	if err := dec.Decode(&raw); err != nil {
		return nil, err
	}
	// Reject trailing garbage after the document.
	var extra interface{}
	if err := dec.Decode(&extra); err != io.EOF {
		return nil, errors.New("trailing data after JSON document")
	}
	return convertJSON(raw), nil
}

func convertJSON(raw interface{}) Value {
	switch x := raw.(type) {
	case nil:
		return nil
	case bool:
		return x
	case string:
		return x
	case json.Number:
		return Number{Raw: string(x)}
	case []interface{}:
		out := make([]Value, len(x))
		for i, e := range x {
			out[i] = convertJSON(e)
		}
		return out
	case map[string]interface{}:
		out := make(map[string]Value, len(x))
		for k, e := range x {
			out[k] = convertJSON(e)
		}
		return out
	}
	return nil
}

func typeName(v Value) string {
	switch v.(type) {
	case nil:
		return "null"
	case bool:
		return "boolean"
	case Number:
		return "number"
	case string:
		return "string"
	case []Value:
		return "array"
	case map[string]Value:
		return "object"
	}
	return "null"
}

func sortedKeys(m map[string]Value) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}

// intLiteral reports whether the number is a plain JSON integer literal that
// fits in a signed 64-bit integer.
func intLiteral(n Number) (int64, bool) {
	s := n.Raw
	if s == "" {
		return 0, false
	}
	i := 0
	if s[0] == '-' {
		i = 1
	}
	if i >= len(s) {
		return 0, false
	}
	for j := i; j < len(s); j++ {
		if s[j] < '0' || s[j] > '9' {
			return 0, false
		}
	}
	v, err := strconv.ParseInt(s, 10, 64)
	if err != nil {
		return 0, false
	}
	return v, true
}

func ratOf(n Number) *big.Rat {
	r, ok := new(big.Rat).SetString(n.Raw)
	if !ok {
		return new(big.Rat)
	}
	return r
}

// typeRank implements the total order's first criterion:
// number < string < boolean < null < array < object.
func typeRank(v Value) int {
	switch v.(type) {
	case Number:
		return 0
	case string:
		return 1
	case bool:
		return 2
	case nil:
		return 3
	case []Value:
		return 4
	case map[string]Value:
		return 5
	}
	return 3
}

func compareBool(a, b bool) int {
	if a == b {
		return 0
	}
	if !a {
		return -1
	}
	return 1
}

// compareValues implements the total order from SPEC §3.
func compareValues(a, b Value) int {
	ra, rb := typeRank(a), typeRank(b)
	if ra != rb {
		if ra < rb {
			return -1
		}
		return 1
	}
	switch av := a.(type) {
	case Number:
		return ratOf(av).Cmp(ratOf(b.(Number)))
	case string:
		return strings.Compare(av, b.(string))
	case bool:
		return compareBool(av, b.(bool))
	case nil:
		return 0
	case []Value:
		bv := b.([]Value)
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
	case map[string]Value:
		bv := b.(map[string]Value)
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
		for i := range ak {
			if c := compareValues(av[ak[i]], bv[bk[i]]); c != 0 {
				return c
			}
		}
		return 0
	}
	return 0
}

func equalValues(a, b Value) bool { return compareValues(a, b) == 0 }

// truthy implements the JSON truthiness used by filter predicates and boolean ops:
// false, null, "", 0, [] and {} are falsy.
func truthy(v Value) bool {
	switch x := v.(type) {
	case nil:
		return false
	case bool:
		return x
	case string:
		return x != ""
	case Number:
		return ratOf(x).Sign() != 0
	case []Value:
		return len(x) > 0
	case map[string]Value:
		return len(x) > 0
	}
	return false
}

// compactJSON renders a value as compact JSON without HTML escaping.
func compactJSON(v Value) (string, error) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(v); err != nil {
		return "", err
	}
	return strings.TrimSuffix(buf.String(), "\n"), nil
}
