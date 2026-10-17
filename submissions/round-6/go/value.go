package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"math/big"
	"sort"
	"strconv"
	"strings"
)

// OpError is an error produced while executing an operation or condition.
type OpError struct {
	Code    string
	Message string
}

func opErr(code, format string, a ...any) *OpError {
	return &OpError{Code: code, Message: fmt.Sprintf(format, a...)}
}

func num(i int64) json.Number {
	return json.Number(strconv.FormatInt(i, 10))
}

// asI64 accepts only JSON integer literals that fit into an i64.
func asI64(v any) (int64, bool) {
	n, ok := v.(json.Number)
	if !ok {
		return 0, false
	}
	s := n.String()
	if strings.ContainsAny(s, ".eE") {
		return 0, false
	}
	i, err := strconv.ParseInt(s, 10, 64)
	if err != nil {
		return 0, false
	}
	return i, true
}

func asString(v any) (string, bool) {
	s, ok := v.(string)
	return s, ok
}

func asBool(v any) (bool, bool) {
	b, ok := v.(bool)
	return b, ok
}

func asArray(v any) ([]any, bool) {
	a, ok := v.([]any)
	return a, ok
}

func asObject(v any) (map[string]any, bool) {
	m, ok := v.(map[string]any)
	return m, ok
}

func numberRat(v any) (*big.Rat, bool) {
	n, ok := v.(json.Number)
	if !ok {
		return nil, false
	}
	r, ok := new(big.Rat).SetString(n.String())
	if !ok {
		return nil, false
	}
	return r, true
}

// jsonTypeName returns the spec type name of a JSON value.
func jsonTypeName(v any) string {
	switch v.(type) {
	case nil:
		return "null"
	case bool:
		return "boolean"
	case json.Number, int64, int, float64:
		return "number"
	case string:
		return "string"
	case []any:
		return "array"
	case map[string]any:
		return "object"
	default:
		return "null"
	}
}

func typeRank(v any) int {
	switch v.(type) {
	case json.Number, int64, int, float64:
		return 0
	case string:
		return 1
	case bool:
		return 2
	case nil:
		return 3
	case []any:
		return 4
	case map[string]any:
		return 5
	default:
		return 3
	}
}

// valuesEqual implements the structural equality of SPEC section 3.
func valuesEqual(a, b any) bool {
	ra, rb := typeRank(a), typeRank(b)
	if ra != rb {
		return false
	}
	switch ra {
	case 0:
		na, oka := numberRat(a)
		nb, okb := numberRat(b)
		if oka && okb {
			return na.Cmp(nb) == 0
		}
		return fmt.Sprint(a) == fmt.Sprint(b)
	case 1:
		return a.(string) == b.(string)
	case 2:
		return a.(bool) == b.(bool)
	case 3:
		return true
	case 4:
		aa := a.([]any)
		bb := b.([]any)
		if len(aa) != len(bb) {
			return false
		}
		for i := range aa {
			if !valuesEqual(aa[i], bb[i]) {
				return false
			}
		}
		return true
	case 5:
		ma := a.(map[string]any)
		mb := b.(map[string]any)
		if len(ma) != len(mb) {
			return false
		}
		for k, va := range ma {
			vb, ok := mb[k]
			if !ok || !valuesEqual(va, vb) {
				return false
			}
		}
		return true
	}
	return false
}

func sortedKeys(m map[string]any) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}

// compareValues implements the total order from SPEC section 3.
// It returns -1, 0 or 1.
func compareValues(a, b any) int {
	ra, rb := typeRank(a), typeRank(b)
	if ra < rb {
		return -1
	}
	if ra > rb {
		return 1
	}
	switch ra {
	case 0:
		na, oka := numberRat(a)
		nb, okb := numberRat(b)
		if oka && okb {
			return na.Cmp(nb)
		}
		return strings.Compare(fmt.Sprint(a), fmt.Sprint(b))
	case 1:
		return strings.Compare(a.(string), b.(string))
	case 2:
		ba := a.(bool)
		bb := b.(bool)
		if ba == bb {
			return 0
		}
		if !ba {
			return -1
		}
		return 1
	case 3:
		return 0
	case 4:
		aa := a.([]any)
		bb := b.([]any)
		n := len(aa)
		if len(bb) < n {
			n = len(bb)
		}
		for i := 0; i < n; i++ {
			if c := compareValues(aa[i], bb[i]); c != 0 {
				return c
			}
		}
		if len(aa) < len(bb) {
			return -1
		}
		if len(aa) > len(bb) {
			return 1
		}
		return 0
	case 5:
		ma := a.(map[string]any)
		mb := b.(map[string]any)
		ka := sortedKeys(ma)
		kb := sortedKeys(mb)
		n := len(ka)
		if len(kb) < n {
			n = len(kb)
		}
		for i := 0; i < n; i++ {
			if c := strings.Compare(ka[i], kb[i]); c != 0 {
				return c
			}
		}
		if len(ka) < len(kb) {
			return -1
		}
		if len(ka) > len(kb) {
			return 1
		}
		for i := range ka {
			if c := compareValues(ma[ka[i]], mb[ka[i]]); c != 0 {
				return c
			}
		}
		return 0
	}
	return 0
}

// truthy implements the JSON truthiness used by filter predicates and logic ops.
func truthy(v any) bool {
	switch x := v.(type) {
	case nil:
		return false
	case bool:
		return x
	case json.Number, int64, int, float64:
		r, ok := numberRat(v)
		if ok {
			return r.Sign() != 0
		}
		return true
	case string:
		return x != ""
	case []any:
		return len(x) != 0
	case map[string]any:
		return len(x) != 0
	}
	return false
}

// marshalCompact emits compact JSON with no HTML escaping and no trailing newline.
func marshalCompact(v any) ([]byte, error) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(v); err != nil {
		return nil, err
	}
	return bytes.TrimRight(buf.Bytes(), "\n"), nil
}
