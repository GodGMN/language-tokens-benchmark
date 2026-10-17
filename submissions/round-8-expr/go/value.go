package main

import (
	"strconv"
	"strings"
)

// Kind enumerates the runtime value kinds allowed by the expression language.
type Kind int

const (
	KNull Kind = iota
	KBool
	KInt
	KStr
	KArr
)

// Value is a dynamically typed expression value.
type Value struct {
	Kind Kind
	Bool bool
	Int  int64
	Str  string
	Arr  []Value
}

func vNull() Value         { return Value{Kind: KNull} }
func vBool(b bool) Value   { return Value{Kind: KBool, Bool: b} }
func vInt(i int64) Value   { return Value{Kind: KInt, Int: i} }
func vStr(s string) Value  { return Value{Kind: KStr, Str: s} }
func vArr(a []Value) Value { return Value{Kind: KArr, Arr: a} }

func (v Value) typeName() string {
	switch v.Kind {
	case KNull:
		return "null"
	case KBool:
		return "boolean"
	case KInt:
		return "integer"
	case KStr:
		return "string"
	case KArr:
		return "array"
	}
	return "unknown"
}

const hexDigits = "0123456789abcdef"

// writeJSONString writes a JSON string literal using the minimal escaping
// required by JSON: quote, backslash and control characters. Non-ASCII text is
// emitted as raw UTF-8.
func writeJSONString(b *strings.Builder, s string) {
	b.WriteByte('"')
	for _, r := range s {
		switch r {
		case '"':
			b.WriteString("\\\"")
		case '\\':
			b.WriteString("\\\\")
		case '\b':
			b.WriteString("\\b")
		case '\f':
			b.WriteString("\\f")
		case '\n':
			b.WriteString("\\n")
		case '\r':
			b.WriteString("\\r")
		case '\t':
			b.WriteString("\\t")
		default:
			if r < 0x20 {
				b.WriteString("\\u00")
				b.WriteByte(hexDigits[(r>>4)&0xF])
				b.WriteByte(hexDigits[r&0xF])
			} else {
				b.WriteRune(r)
			}
		}
	}
	b.WriteByte('"')
}

// writeValue writes a compact JSON representation of v (no spaces).
func writeValue(b *strings.Builder, v Value) {
	switch v.Kind {
	case KNull:
		b.WriteString("null")
	case KBool:
		if v.Bool {
			b.WriteString("true")
		} else {
			b.WriteString("false")
		}
	case KInt:
		b.WriteString(strconv.FormatInt(v.Int, 10))
	case KStr:
		writeJSONString(b, v.Str)
	case KArr:
		b.WriteByte('[')
		for i, e := range v.Arr {
			if i > 0 {
				b.WriteByte(',')
			}
			writeValue(b, e)
		}
		b.WriteByte(']')
	}
}

func encodeValue(v Value) string {
	var b strings.Builder
	writeValue(&b, v)
	return b.String()
}

// valuesEqual implements structural equality.
func valuesEqual(a, b Value) bool {
	if a.Kind != b.Kind {
		return false
	}
	switch a.Kind {
	case KNull:
		return true
	case KBool:
		return a.Bool == b.Bool
	case KInt:
		return a.Int == b.Int
	case KStr:
		return a.Str == b.Str
	case KArr:
		if len(a.Arr) != len(b.Arr) {
			return false
		}
		for i := range a.Arr {
			if !valuesEqual(a.Arr[i], b.Arr[i]) {
				return false
			}
		}
		return true
	}
	return false
}
