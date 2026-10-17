package main

import (
	"bytes"
	"encoding/json"
	"math/big"
	"sort"
	"strconv"
	"strings"
)

// Kind enumerates the JSON value kinds.
type Kind int

const (
	KNull Kind = iota
	KBool
	KNum
	KStr
	KArr
	KObj
)

// Num holds a JSON number. IsInt is true only for integer literals that fit
// in a signed 64-bit integer. Rat is kept for exact structural comparison.
type Num struct {
	IsInt bool
	I     int64
	Rat   *big.Rat
	Raw   string
}

// Value is a JSON value.
type Value struct {
	Kind Kind
	B    bool
	N    Num
	S    string
	A    []Value
	O    map[string]Value
}

func Null() Value { return Value{Kind: KNull} }
func Bool(b bool) Value {
	return Value{Kind: KBool, B: b}
}
func Str(s string) Value { return Value{Kind: KStr, S: s} }
func Int(i int64) Value {
	return Value{Kind: KNum, N: Num{IsInt: true, I: i, Rat: new(big.Rat).SetInt64(i), Raw: strconv.FormatInt(i, 10)}}
}
func Arr(a []Value) Value {
	if a == nil {
		a = []Value{}
	}
	return Value{Kind: KArr, A: a}
}
func Obj(m map[string]Value) Value {
	if m == nil {
		m = map[string]Value{}
	}
	return Value{Kind: KObj, O: m}
}

func typeName(v Value) string {
	switch v.Kind {
	case KNull:
		return "null"
	case KBool:
		return "boolean"
	case KNum:
		return "number"
	case KStr:
		return "string"
	case KArr:
		return "array"
	case KObj:
		return "object"
	}
	return "null"
}

func isTypeName(s string) bool {
	switch s {
	case "null", "boolean", "number", "string", "array", "object":
		return true
	}
	return false
}

func numberIsZero(n Num) bool {
	if n.IsInt {
		return n.I == 0
	}
	if n.Rat != nil {
		return n.Rat.Sign() == 0
	}
	return false
}

func truthy(v Value) bool {
	switch v.Kind {
	case KNull:
		return false
	case KBool:
		return v.B
	case KNum:
		return !numberIsZero(v.N)
	case KStr:
		return v.S != ""
	case KArr:
		return len(v.A) > 0
	case KObj:
		return len(v.O) > 0
	}
	return false
}

func intValue(v Value) (int64, bool) {
	if v.Kind == KNum && v.N.IsInt {
		return v.N.I, true
	}
	return 0, false
}

func sortedKeys(m map[string]Value) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys) // byte-wise lexicographic for UTF-8 strings
	return keys
}

// ---------------------------------------------------------------------------
// JSON decoding

func fromJSON(raw interface{}) (Value, error) {
	switch x := raw.(type) {
	case nil:
		return Null(), nil
	case bool:
		return Bool(x), nil
	case string:
		return Str(x), nil
	case json.Number:
		return fromNumber(x), nil
	case []interface{}:
		out := make([]Value, len(x))
		for i, e := range x {
			v, err := fromJSON(e)
			if err != nil {
				return Value{}, err
			}
			out[i] = v
		}
		return Arr(out), nil
	case map[string]interface{}:
		out := make(map[string]Value, len(x))
		for k, e := range x {
			v, err := fromJSON(e)
			if err != nil {
				return Value{}, err
			}
			out[k] = v
		}
		return Obj(out), nil
	}
	return Null(), nil
}

func fromNumber(n json.Number) Value {
	raw := n.String()
	num := Num{Raw: raw}
	if !strings.ContainsAny(raw, ".eE") {
		if i, err := strconv.ParseInt(raw, 10, 64); err == nil {
			num.IsInt = true
			num.I = i
		}
	}
	if r, ok := new(big.Rat).SetString(raw); ok {
		num.Rat = r
	} else if f, err := strconv.ParseFloat(raw, 64); err == nil {
		if r := new(big.Rat).SetFloat64(f); r != nil {
			num.Rat = r
		}
	}
	if num.Rat == nil {
		num.Rat = new(big.Rat)
	}
	return Value{Kind: KNum, N: num}
}

// ---------------------------------------------------------------------------
// JSON encoding

func (v Value) marshalTo(b *strings.Builder) {
	switch v.Kind {
	case KNull:
		b.WriteString("null")
	case KBool:
		if v.B {
			b.WriteString("true")
		} else {
			b.WriteString("false")
		}
	case KNum:
		if v.N.IsInt {
			b.WriteString(strconv.FormatInt(v.N.I, 10))
		} else if v.N.Raw != "" {
			b.WriteString(v.N.Raw)
		} else if v.N.Rat != nil {
			b.WriteString(v.N.Rat.FloatString(10))
		} else {
			b.WriteString("0")
		}
	case KStr:
		b.WriteString(quoteJSONString(v.S))
	case KArr:
		b.WriteByte('[')
		for i, e := range v.A {
			if i > 0 {
				b.WriteByte(',')
			}
			e.marshalTo(b)
		}
		b.WriteByte(']')
	case KObj:
		b.WriteByte('{')
		for i, k := range sortedKeys(v.O) {
			if i > 0 {
				b.WriteByte(',')
			}
			b.WriteString(quoteJSONString(k))
			b.WriteByte(':')
			v.O[k].marshalTo(b)
		}
		b.WriteByte('}')
	}
}

func (v Value) MarshalJSON() ([]byte, error) {
	var b strings.Builder
	v.marshalTo(&b)
	return []byte(b.String()), nil
}

// quoteJSONString encodes s as a JSON string without HTML escaping. This keeps
// the result of stringify identical to canonical JSON serializers.
func quoteJSONString(s string) string {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	_ = enc.Encode(s)
	data := buf.Bytes()
	if len(data) > 0 && data[len(data)-1] == '\n' {
		data = data[:len(data)-1]
	}
	return string(data)
}

func compactJSON(v Value) string {
	var b strings.Builder
	v.marshalTo(&b)
	return b.String()
}

// ---------------------------------------------------------------------------
// Equality and total ordering

func numCmp(a, b Num) int {
	if a.IsInt && b.IsInt {
		if a.I < b.I {
			return -1
		}
		if a.I > b.I {
			return 1
		}
		return 0
	}
	if a.Rat != nil && b.Rat != nil {
		return a.Rat.Cmp(b.Rat)
	}
	// Unreachable in practice (Rat is always populated).
	af, bf := 0.0, 0.0
	if a.IsInt {
		af = float64(a.I)
	} else if a.Rat != nil {
		af, _ = a.Rat.Float64()
	}
	if b.IsInt {
		bf = float64(b.I)
	} else if b.Rat != nil {
		bf, _ = b.Rat.Float64()
	}
	if af < bf {
		return -1
	}
	if af > bf {
		return 1
	}
	return 0
}

func equalValues(a, b Value) bool {
	if a.Kind != b.Kind {
		return false
	}
	switch a.Kind {
	case KNull:
		return true
	case KBool:
		return a.B == b.B
	case KNum:
		return numCmp(a.N, b.N) == 0
	case KStr:
		return a.S == b.S
	case KArr:
		if len(a.A) != len(b.A) {
			return false
		}
		for i := range a.A {
			if !equalValues(a.A[i], b.A[i]) {
				return false
			}
		}
		return true
	case KObj:
		if len(a.O) != len(b.O) {
			return false
		}
		for k, av := range a.O {
			bv, ok := b.O[k]
			if !ok || !equalValues(av, bv) {
				return false
			}
		}
		return true
	}
	return false
}

func typeRank(v Value) int {
	switch v.Kind {
	case KNum:
		return 0
	case KStr:
		return 1
	case KBool:
		return 2
	case KNull:
		return 3
	case KArr:
		return 4
	case KObj:
		return 5
	}
	return 6
}

// compareValues implements the total order from SPEC section 3.
func compareValues(a, b Value) int {
	ra, rb := typeRank(a), typeRank(b)
	if ra != rb {
		if ra < rb {
			return -1
		}
		return 1
	}
	switch a.Kind {
	case KNum:
		return numCmp(a.N, b.N)
	case KStr:
		return strings.Compare(a.S, b.S)
	case KBool:
		if a.B == b.B {
			return 0
		}
		if !a.B {
			return -1
		}
		return 1
	case KNull:
		return 0
	case KArr:
		n := len(a.A)
		if len(b.A) < n {
			n = len(b.A)
		}
		for i := 0; i < n; i++ {
			if c := compareValues(a.A[i], b.A[i]); c != 0 {
				return c
			}
		}
		if len(a.A) < len(b.A) {
			return -1
		}
		if len(a.A) > len(b.A) {
			return 1
		}
		return 0
	case KObj:
		ka, kb := sortedKeys(a.O), sortedKeys(b.O)
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
			if c := compareValues(a.O[ka[i]], b.O[ka[i]]); c != 0 {
				return c
			}
		}
		return 0
	}
	return 0
}

// isRefObject reports whether v is a {"$ref": "<string>"} object.
func isRefObject(v Value) bool {
	if v.Kind != KObj || len(v.O) != 1 {
		return false
	}
	r, ok := v.O["$ref"]
	return ok && r.Kind == KStr
}
