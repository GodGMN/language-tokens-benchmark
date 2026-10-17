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

// Value is any JSON value. Numbers decoded from an integer literal that fits
// in an i64 are represented as int64. Every other JSON number is kept as a
// json.Number so its literal spelling (and precision) is preserved.
type Value = interface{}

const (
	maxInt64 = int64(^uint64(0) >> 1)
	minInt64 = -maxInt64 - 1
)

// decodeDocument decodes exactly one JSON document, preserving number literals.
func decodeDocument(data []byte) (Value, error) {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.UseNumber()
	var v interface{}
	if err := dec.Decode(&v); err != nil {
		return nil, err
	}
	var extra interface{}
	if err := dec.Decode(&extra); err != io.EOF {
		if err == nil {
			return nil, errors.New("trailing data after JSON document")
		}
		return nil, err
	}
	return normalize(v), nil
}

func normalize(v Value) Value {
	switch x := v.(type) {
	case json.Number:
		s := string(x)
		if !strings.ContainsAny(s, ".eE") {
			if n, err := strconv.ParseInt(s, 10, 64); err == nil {
				return n
			}
		}
		return x
	case []Value:
		for i := range x {
			x[i] = normalize(x[i])
		}
		return x
	case map[string]Value:
		for k, vv := range x {
			x[k] = normalize(vv)
		}
		return x
	default:
		return v
	}
}

func typeName(v Value) string {
	switch v.(type) {
	case nil:
		return "null"
	case bool:
		return "boolean"
	case int64, json.Number:
		return "number"
	case string:
		return "string"
	case []Value:
		return "array"
	case map[string]Value:
		return "object"
	default:
		return "null"
	}
}

func isNumber(v Value) bool {
	switch v.(type) {
	case int64, json.Number:
		return true
	}
	return false
}

func numberRat(v Value) (*big.Rat, bool) {
	switch x := v.(type) {
	case int64:
		return new(big.Rat).SetInt64(x), true
	case json.Number:
		r := new(big.Rat)
		if _, ok := r.SetString(string(x)); !ok {
			// Very defensive fallback; json.Number normally holds valid syntax.
			f, err := strconv.ParseFloat(string(x), 64)
			if err != nil {
				return nil, false
			}
			r.SetFloat64(f)
		}
		return r, true
	}
	return nil, false
}

func numbersEqual(a, b Value) bool {
	ai, aok := a.(int64)
	bi, bok := b.(int64)
	if aok && bok {
		return ai == bi
	}
	ar, ok := numberRat(a)
	if !ok {
		return false
	}
	br, ok := numberRat(b)
	if !ok {
		return false
	}
	return ar.Cmp(br) == 0
}

func compareNumbers(a, b Value) int {
	ai, aok := a.(int64)
	bi, bok := b.(int64)
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
	ar, _ := numberRat(a)
	br, _ := numberRat(b)
	return ar.Cmp(br)
}

func isNumericZero(v Value) bool {
	return isNumber(v) && compareNumbers(v, int64(0)) == 0
}

func asInt(v Value) (int64, bool) {
	n, ok := v.(int64)
	return n, ok
}

func asString(v Value) (string, bool) {
	s, ok := v.(string)
	return s, ok
}

func asBool(v Value) (bool, bool) {
	b, ok := v.(bool)
	return b, ok
}

func asArray(v Value) ([]Value, bool) {
	a, ok := v.([]Value)
	return a, ok
}

func asObject(v Value) (map[string]Value, bool) {
	m, ok := v.(map[string]Value)
	return m, ok
}

func sortedKeys(m map[string]Value) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}

// equalValues implements structural equality.
func equalValues(a, b Value) bool {
	if a == nil || b == nil {
		return a == nil && b == nil
	}
	switch av := a.(type) {
	case bool:
		bv, ok := b.(bool)
		return ok && av == bv
	case string:
		bv, ok := b.(string)
		return ok && av == bv
	case int64, json.Number:
		return isNumber(b) && numbersEqual(a, b)
	case []Value:
		bv, ok := b.([]Value)
		if !ok || len(av) != len(bv) {
			return false
		}
		for i := range av {
			if !equalValues(av[i], bv[i]) {
				return false
			}
		}
		return true
	case map[string]Value:
		bv, ok := b.(map[string]Value)
		if !ok || len(av) != len(bv) {
			return false
		}
		for k, x := range av {
			y, present := bv[k]
			if !present || !equalValues(x, y) {
				return false
			}
		}
		return true
	default:
		return false
	}
}

func typeRank(v Value) int {
	switch v.(type) {
	case int64, json.Number:
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
	default:
		return 3
	}
}

// compareValues is the total order used by sort and compare.
func compareValues(a, b Value) int {
	ra, rb := typeRank(a), typeRank(b)
	if ra != rb {
		if ra < rb {
			return -1
		}
		return 1
	}
	switch av := a.(type) {
	case int64, json.Number:
		return compareNumbers(a, b)
	case string:
		bv, _ := b.(string)
		return strings.Compare(av, bv)
	case bool:
		bv, _ := b.(bool)
		switch {
		case av == bv:
			return 0
		case !av:
			return -1
		default:
			return 1
		}
	case nil:
		return 0
	case []Value:
		bv, _ := b.([]Value)
		n := len(av)
		if len(bv) < n {
			n = len(bv)
		}
		for i := 0; i < n; i++ {
			if c := compareValues(av[i], bv[i]); c != 0 {
				return c
			}
		}
		switch {
		case len(av) < len(bv):
			return -1
		case len(av) > len(bv):
			return 1
		default:
			return 0
		}
	case map[string]Value:
		bv, _ := b.(map[string]Value)
		ka, kb := sortedKeys(av), sortedKeys(bv)
		n := len(ka)
		if len(kb) < n {
			n = len(kb)
		}
		for i := 0; i < n; i++ {
			if c := strings.Compare(ka[i], kb[i]); c != 0 {
				return c
			}
		}
		switch {
		case len(ka) < len(kb):
			return -1
		case len(ka) > len(kb):
			return 1
		}
		// Same key sets: compare the corresponding values.
		for _, k := range ka {
			if c := compareValues(av[k], bv[k]); c != 0 {
				return c
			}
		}
		return 0
	default:
		return 0
	}
}

func truthy(v Value) bool {
	switch x := v.(type) {
	case nil:
		return false
	case bool:
		return x
	case string:
		return x != ""
	case int64, json.Number:
		return !isNumericZero(v)
	case []Value:
		return len(x) > 0
	case map[string]Value:
		return len(x) > 0
	default:
		return false
	}
}

// canonicalValue produces a collision-free canonical encoding used by unique.
func canonicalValue(v Value) string {
	var b strings.Builder
	writeCanonical(&b, v)
	return b.String()
}

func writeCanonical(b *strings.Builder, v Value) {
	switch x := v.(type) {
	case nil:
		b.WriteString("z")
	case bool:
		if x {
			b.WriteString("t")
		} else {
			b.WriteString("f")
		}
	case string:
		b.WriteString("s")
		b.WriteString(strconv.Itoa(len(x)))
		b.WriteByte(':')
		b.WriteString(x)
	case int64, json.Number:
		b.WriteString("d")
		if r, ok := numberRat(v); ok {
			b.WriteString(r.RatString())
		} else {
			b.WriteString("<bad>")
		}
	case []Value:
		b.WriteString("a")
		b.WriteString(strconv.Itoa(len(x)))
		b.WriteByte('[')
		for _, e := range x {
			part := canonicalValue(e)
			b.WriteString(strconv.Itoa(len(part)))
			b.WriteByte(':')
			b.WriteString(part)
		}
		b.WriteByte(']')
	case map[string]Value:
		keys := sortedKeys(x)
		b.WriteString("o")
		b.WriteString(strconv.Itoa(len(keys)))
		b.WriteByte('{')
		for _, k := range keys {
			b.WriteString(strconv.Itoa(len(k)))
			b.WriteByte(':')
			b.WriteString(k)
			part := canonicalValue(x[k])
			b.WriteString(strconv.Itoa(len(part)))
			b.WriteByte(':')
			b.WriteString(part)
		}
		b.WriteByte('}')
	default:
		b.WriteString("?")
	}
}

// compactJSON renders a value as compact JSON (used by stringify). It does
// not HTML-escape characters, matching the usual compact JSON encoding.
func compactJSON(v Value) (string, error) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(v); err != nil {
		return "", err
	}
	return strings.TrimSuffix(buf.String(), "\n"), nil
}
