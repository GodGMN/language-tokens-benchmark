package main

import (
	"math"
	"sort"
	"strings"
	"unicode/utf8"
)

// checkArgs validates exact argument names. Every key must be in
// required ∪ optional and every required key must be present.
func checkArgs(args map[string]interface{}, required []string, optional []string) bool {
	allowed := map[string]bool{}
	for _, k := range required {
		allowed[k] = true
		if _, ok := args[k]; !ok {
			return false
		}
	}
	for _, k := range optional {
		allowed[k] = true
	}
	for k := range args {
		if !allowed[k] {
			return false
		}
	}
	return true
}

func addI(a, b int64) (int64, bool) {
	if b > 0 && a > math.MaxInt64-b {
		return 0, false
	}
	if b < 0 && a < math.MinInt64-b {
		return 0, false
	}
	return a + b, true
}

func subI(a, b int64) (int64, bool) {
	if b > 0 && a < math.MinInt64+b {
		return 0, false
	}
	if b < 0 && a > math.MaxInt64+b {
		return 0, false
	}
	return a - b, true
}

func mulI(a, b int64) (int64, bool) {
	if a == 0 || b == 0 {
		return 0, true
	}
	if (a == math.MinInt64 && b == -1) || (b == math.MinInt64 && a == -1) {
		return 0, false
	}
	r := a * b
	if r/b != a {
		return 0, false
	}
	return r, true
}

func divI(a, b int64) (int64, string) {
	if b == 0 {
		return 0, "DIVISION_BY_ZERO"
	}
	if a == math.MinInt64 && b == -1 {
		return 0, "ARITHMETIC_OVERFLOW"
	}
	return a / b, ""
}

func modI(a, b int64) (int64, string) {
	if b == 0 {
		return 0, "DIVISION_BY_ZERO"
	}
	if a == math.MinInt64 && b == -1 {
		return 0, ""
	}
	return a % b, ""
}

// binaryMath applies one of the five integer binary operations.
// The returned string is empty on success or an error code.
func binaryMath(op string, a, b int64) (int64, string) {
	switch op {
	case "add":
		r, ok := addI(a, b)
		if !ok {
			return 0, "ARITHMETIC_OVERFLOW"
		}
		return r, ""
	case "subtract":
		r, ok := subI(a, b)
		if !ok {
			return 0, "ARITHMETIC_OVERFLOW"
		}
		return r, ""
	case "multiply":
		r, ok := mulI(a, b)
		if !ok {
			return 0, "ARITHMETIC_OVERFLOW"
		}
		return r, ""
	case "divide":
		return divI(a, b)
	case "mod":
		return modI(a, b)
	}
	return 0, "INVALID_ARG"
}

func (e *engine) runOp(s *Step, args map[string]interface{}) (interface{}, *Err) {
	inv := func(msg string) *Err {
		return &Err{Code: "INVALID_ARG", Step: &s.ID, Message: msg}
	}
	overflow := func() *Err {
		return &Err{Code: "ARITHMETIC_OVERFLOW", Step: &s.ID, Message: "i64 arithmetic overflow"}
	}
	fromCode := func(code string) *Err {
		if code == "DIVISION_BY_ZERO" {
			return &Err{Code: code, Step: &s.ID, Message: "division by zero"}
		}
		return &Err{Code: code, Step: &s.ID, Message: code}
	}

	switch s.Op {

	// ---------------------------------------------------------------- math
	case "add", "subtract", "multiply", "divide", "mod":
		if !checkArgs(args, []string{"a", "b"}, nil) {
			return nil, inv(s.Op + " expects args a and b")
		}
		a, ok1 := asI64(args["a"])
		b, ok2 := asI64(args["b"])
		if !ok1 || !ok2 {
			return nil, inv(s.Op + " arguments must be i64 integers")
		}
		r, code := binaryMath(s.Op, a, b)
		if code != "" {
			return nil, fromCode(code)
		}
		return numFromInt(r), nil

	case "sum":
		if !checkArgs(args, []string{"input"}, nil) {
			return nil, inv("sum expects arg input")
		}
		arr, ok := args["input"].([]interface{})
		if !ok {
			return nil, inv("sum input must be an array")
		}
		var acc int64
		for _, el := range arr {
			v, ok := asI64(el)
			if !ok {
				return nil, inv("sum input must contain only integers")
			}
			var good bool
			acc, good = addI(acc, v)
			if !good {
				return nil, overflow()
			}
		}
		return numFromInt(acc), nil

	case "min", "max":
		if !checkArgs(args, []string{"input"}, nil) {
			return nil, inv(s.Op + " expects arg input")
		}
		arr, ok := args["input"].([]interface{})
		if !ok {
			return nil, inv(s.Op + " input must be an array")
		}
		if len(arr) == 0 {
			return nil, &Err{Code: "EMPTY_ARRAY", Step: &s.ID, Message: s.Op + " on empty array"}
		}
		best, ok := asI64(arr[0])
		if !ok {
			return nil, inv(s.Op + " input must contain only integers")
		}
		for _, el := range arr[1:] {
			v, ok := asI64(el)
			if !ok {
				return nil, inv(s.Op + " input must contain only integers")
			}
			if (s.Op == "min" && v < best) || (s.Op == "max" && v > best) {
				best = v
			}
		}
		return numFromInt(best), nil

	case "range":
		if !checkArgs(args, []string{"start", "end"}, nil) {
			return nil, inv("range expects args start and end")
		}
		start, ok1 := asI64(args["start"])
		end, ok2 := asI64(args["end"])
		if !ok1 || !ok2 {
			return nil, inv("range start and end must be i64 integers")
		}
		out := []interface{}{}
		if start > end {
			return out, nil
		}
		for i := start; ; i++ {
			out = append(out, numFromInt(i))
			if i == end {
				break
			}
		}
		return out, nil

	// -------------------------------------------------------------- strings
	case "concat":
		if !checkArgs(args, []string{"a", "b"}, nil) {
			return nil, inv("concat expects args a and b")
		}
		a, ok1 := args["a"].(string)
		b, ok2 := args["b"].(string)
		if !ok1 || !ok2 {
			return nil, inv("concat arguments must be strings")
		}
		return a + b, nil

	case "split":
		if !checkArgs(args, []string{"input", "sep"}, nil) {
			return nil, inv("split expects args input and sep")
		}
		input, ok1 := args["input"].(string)
		sep, ok2 := args["sep"].(string)
		if !ok1 || !ok2 {
			return nil, inv("split input and sep must be strings")
		}
		out := []interface{}{}
		if sep == "" {
			for _, r := range input {
				out = append(out, string(r))
			}
			return out, nil
		}
		for _, part := range strings.Split(input, sep) {
			out = append(out, part)
		}
		return out, nil

	case "join":
		if !checkArgs(args, []string{"input", "sep"}, nil) {
			return nil, inv("join expects args input and sep")
		}
		arr, ok1 := args["input"].([]interface{})
		sep, ok2 := args["sep"].(string)
		if !ok1 || !ok2 {
			return nil, inv("join expects an array input and a string sep")
		}
		parts := make([]string, len(arr))
		for i, el := range arr {
			sv, ok := el.(string)
			if !ok {
				return nil, inv("join input must contain only strings")
			}
			parts[i] = sv
		}
		return strings.Join(parts, sep), nil

	case "uppercase", "lowercase":
		if !checkArgs(args, []string{"input"}, nil) {
			return nil, inv(s.Op + " expects arg input")
		}
		input, ok := args["input"].(string)
		if !ok {
			return nil, inv(s.Op + " input must be a string")
		}
		var b strings.Builder
		b.Grow(len(input))
		for _, r := range input {
			if s.Op == "uppercase" && r >= 'a' && r <= 'z' {
				r -= 'a' - 'A'
			} else if s.Op == "lowercase" && r >= 'A' && r <= 'Z' {
				r += 'a' - 'A'
			}
			b.WriteRune(r)
		}
		return b.String(), nil

	case "trim":
		if !checkArgs(args, []string{"input"}, nil) {
			return nil, inv("trim expects arg input")
		}
		input, ok := args["input"].(string)
		if !ok {
			return nil, inv("trim input must be a string")
		}
		return strings.Trim(input, " \t\n\r\f\v"), nil

	case "replace":
		if !checkArgs(args, []string{"input", "search", "replace"}, nil) {
			return nil, inv("replace expects args input, search and replace")
		}
		input, ok1 := args["input"].(string)
		search, ok2 := args["search"].(string)
		repl, ok3 := args["replace"].(string)
		if !ok1 || !ok2 || !ok3 {
			return nil, inv("replace arguments must be strings")
		}
		return strings.ReplaceAll(input, search, repl), nil

	case "length":
		if !checkArgs(args, []string{"input"}, nil) {
			return nil, inv("length expects arg input")
		}
		input, ok := args["input"].(string)
		if !ok {
			return nil, inv("length input must be a string")
		}
		return numFromInt(int64(utf8.RuneCountInString(input))), nil

	// --------------------------------------------------------------- arrays
	case "map":
		if !checkArgs(args, []string{"input", "transform"}, nil) {
			return nil, inv("map expects args input and transform")
		}
		arr, ok := args["input"].([]interface{})
		if !ok {
			return nil, inv("map input must be an array")
		}
		tr, err := parseTransform(args["transform"], s)
		if err != nil {
			return nil, err
		}
		out := make([]interface{}, 0, len(arr))
		for _, el := range arr {
			v, err := applyTransform(tr, el, s)
			if err != nil {
				return nil, err
			}
			out = append(out, v)
		}
		return out, nil

	case "filter":
		if !checkArgs(args, []string{"input", "predicate"}, nil) {
			return nil, inv("filter expects args input and predicate")
		}
		arr, ok := args["input"].([]interface{})
		if !ok {
			return nil, inv("filter input must be an array")
		}
		p, err := parsePredicate(args["predicate"], s)
		if err != nil {
			return nil, err
		}
		out := make([]interface{}, 0, len(arr))
		for _, el := range arr {
			keep, err := applyPredicate(p, el, s)
			if err != nil {
				return nil, err
			}
			if keep {
				out = append(out, el)
			}
		}
		return out, nil

	case "reduce":
		if !checkArgs(args, []string{"input", "op"}, []string{"initial"}) {
			return nil, inv("reduce expects args input, op and optional initial")
		}
		arr, ok := args["input"].([]interface{})
		if !ok {
			return nil, inv("reduce input must be an array")
		}
		rop, ok := args["op"].(string)
		if !ok || (rop != "add" && rop != "subtract" && rop != "multiply") {
			return nil, inv("reduce op must be add, subtract or multiply")
		}
		var acc int64
		switch rop {
		case "add", "subtract":
			acc = 0
		case "multiply":
			acc = 1
		}
		if iv, present := args["initial"]; present {
			v, ok := asI64(iv)
			if !ok {
				return nil, inv("reduce initial must be an i64 integer")
			}
			acc = v
		}
		for _, el := range arr {
			v, ok := asI64(el)
			if !ok {
				return nil, inv("reduce input must contain only integers")
			}
			var code string
			acc, code = binaryMath(rop, acc, v)
			if code != "" {
				return nil, fromCode(code)
			}
		}
		return numFromInt(acc), nil

	case "sort":
		if !checkArgs(args, []string{"input"}, nil) {
			return nil, inv("sort expects arg input")
		}
		arr, ok := args["input"].([]interface{})
		if !ok {
			return nil, inv("sort input must be an array")
		}
		out := append([]interface{}{}, arr...)
		sort.SliceStable(out, func(i, j int) bool {
			return compareValues(out[i], out[j]) < 0
		})
		return out, nil

	case "reverse":
		if !checkArgs(args, []string{"input"}, nil) {
			return nil, inv("reverse expects arg input")
		}
		arr, ok := args["input"].([]interface{})
		if !ok {
			return nil, inv("reverse input must be an array")
		}
		out := make([]interface{}, len(arr))
		for i := range arr {
			out[len(arr)-1-i] = arr[i]
		}
		return out, nil

	case "unique":
		if !checkArgs(args, []string{"input"}, nil) {
			return nil, inv("unique expects arg input")
		}
		arr, ok := args["input"].([]interface{})
		if !ok {
			return nil, inv("unique input must be an array")
		}
		out := []interface{}{}
		for _, el := range arr {
			dup := false
			for _, kept := range out {
				if valuesEqual(kept, el) {
					dup = true
					break
				}
			}
			if !dup {
				out = append(out, el)
			}
		}
		return out, nil

	case "slice":
		if !checkArgs(args, []string{"input", "start"}, []string{"end"}) {
			return nil, inv("slice expects args input, start and optional end")
		}
		arr, ok := args["input"].([]interface{})
		if !ok {
			return nil, inv("slice input must be an array")
		}
		start, ok := asI64(args["start"])
		if !ok {
			return nil, inv("slice start must be an i64 integer")
		}
		n := int64(len(arr))
		start = normalizeSliceIndex(start, n)
		end := n
		if ev, present := args["end"]; present {
			v, ok := asI64(ev)
			if !ok {
				return nil, inv("slice end must be an i64 integer")
			}
			end = normalizeSliceIndex(v, n)
		}
		if end < start {
			end = start
		}
		out := make([]interface{}, 0, end-start)
		out = append(out, arr[start:end]...)
		return out, nil

	case "flatten":
		if !checkArgs(args, []string{"input"}, nil) {
			return nil, inv("flatten expects arg input")
		}
		arr, ok := args["input"].([]interface{})
		if !ok {
			return nil, inv("flatten input must be an array")
		}
		out := []interface{}{}
		for _, el := range arr {
			if nested, ok := el.([]interface{}); ok {
				out = append(out, nested...)
			} else {
				out = append(out, el)
			}
		}
		return out, nil

	// -------------------------------------------------------------- objects
	case "get":
		if !checkArgs(args, []string{"input", "key"}, nil) {
			return nil, inv("get expects args input and key")
		}
		obj, ok1 := args["input"].(map[string]interface{})
		key, ok2 := args["key"].(string)
		if !ok1 || !ok2 {
			return nil, inv("get expects an object input and a string key")
		}
		return obj[key], nil

	case "set":
		if !checkArgs(args, []string{"input", "key", "value"}, nil) {
			return nil, inv("set expects args input, key and value")
		}
		obj, ok1 := args["input"].(map[string]interface{})
		key, ok2 := args["key"].(string)
		if !ok1 || !ok2 {
			return nil, inv("set expects an object input and a string key")
		}
		out := make(map[string]interface{}, len(obj)+1)
		for k, v := range obj {
			out[k] = v
		}
		out[key] = args["value"]
		return out, nil

	case "keys":
		if !checkArgs(args, []string{"input"}, nil) {
			return nil, inv("keys expects arg input")
		}
		obj, ok := args["input"].(map[string]interface{})
		if !ok {
			return nil, inv("keys input must be an object")
		}
		out := []interface{}{}
		for _, k := range sortedKeys(obj) {
			out = append(out, k)
		}
		return out, nil

	case "values":
		if !checkArgs(args, []string{"input"}, nil) {
			return nil, inv("values expects arg input")
		}
		obj, ok := args["input"].(map[string]interface{})
		if !ok {
			return nil, inv("values input must be an object")
		}
		out := []interface{}{}
		for _, k := range sortedKeys(obj) {
			out = append(out, obj[k])
		}
		return out, nil

	case "merge":
		if !checkArgs(args, []string{"a", "b"}, nil) {
			return nil, inv("merge expects args a and b")
		}
		a, ok1 := args["a"].(map[string]interface{})
		b, ok2 := args["b"].(map[string]interface{})
		if !ok1 || !ok2 {
			return nil, inv("merge arguments must be objects")
		}
		out := make(map[string]interface{}, len(a)+len(b))
		for k, v := range a {
			out[k] = v
		}
		for k, v := range b {
			out[k] = v
		}
		return out, nil

	case "pick":
		if !checkArgs(args, []string{"input", "keys"}, nil) {
			return nil, inv("pick expects args input and keys")
		}
		obj, ok1 := args["input"].(map[string]interface{})
		keyArr, ok2 := args["keys"].([]interface{})
		if !ok1 || !ok2 {
			return nil, inv("pick expects an object input and an array of keys")
		}
		out := map[string]interface{}{}
		for _, el := range keyArr {
			key, ok := el.(string)
			if !ok {
				return nil, inv("pick keys must be strings")
			}
			out[key] = obj[key]
		}
		return out, nil

	case "omit":
		if !checkArgs(args, []string{"input", "keys"}, nil) {
			return nil, inv("omit expects args input and keys")
		}
		obj, ok1 := args["input"].(map[string]interface{})
		keyArr, ok2 := args["keys"].([]interface{})
		if !ok1 || !ok2 {
			return nil, inv("omit expects an object input and an array of keys")
		}
		drop := map[string]bool{}
		for _, el := range keyArr {
			key, ok := el.(string)
			if !ok {
				return nil, inv("omit keys must be strings")
			}
			drop[key] = true
		}
		out := make(map[string]interface{}, len(obj))
		for k, v := range obj {
			if !drop[k] {
				out[k] = v
			}
		}
		return out, nil

	// ------------------------------------------------------------ utilities
	case "constant":
		if !checkArgs(args, []string{"value"}, nil) {
			return nil, inv("constant expects arg value")
		}
		return args["value"], nil

	case "equals":
		if !checkArgs(args, []string{"a", "b"}, nil) {
			return nil, inv("equals expects args a and b")
		}
		return valuesEqual(args["a"], args["b"]), nil

	case "compare":
		if !checkArgs(args, []string{"a", "b", "op"}, nil) {
			return nil, inv("compare expects args a, b and op")
		}
		op, ok := args["op"].(string)
		if !ok || (op != "gt" && op != "gte" && op != "lt" && op != "lte") {
			return nil, inv("compare op must be gt, gte, lt or lte")
		}
		c := compareValues(args["a"], args["b"])
		switch op {
		case "gt":
			return c > 0, nil
		case "gte":
			return c >= 0, nil
		case "lt":
			return c < 0, nil
		case "lte":
			return c <= 0, nil
		}
		return nil, inv("invalid compare op")

	case "and":
		if !checkArgs(args, []string{"a", "b"}, nil) {
			return nil, inv("and expects args a and b")
		}
		return truthy(args["a"]) && truthy(args["b"]), nil

	case "or":
		if !checkArgs(args, []string{"a", "b"}, nil) {
			return nil, inv("or expects args a and b")
		}
		return truthy(args["a"]) || truthy(args["b"]), nil

	case "not":
		if !checkArgs(args, []string{"a"}, nil) {
			return nil, inv("not expects arg a")
		}
		return !truthy(args["a"]), nil

	case "type":
		if !checkArgs(args, []string{"value"}, nil) {
			return nil, inv("type expects arg value")
		}
		return typeName(args["value"]), nil

	// ---------------------------------------------------------------- retry
	case "fail_until":
		if !checkArgs(args, []string{"failures", "value"}, nil) {
			return nil, inv("fail_until expects args failures and value")
		}
		failures, ok := asI64(args["failures"])
		if !ok || failures < 0 || failures > 10 {
			return nil, inv("fail_until failures must be an integer from 0 to 10")
		}
		used := e.failCounts[s.ID]
		if used < failures {
			e.failCounts[s.ID] = used + 1
			return nil, &Err{Code: "TRANSIENT_FAILURE", Step: &s.ID, Message: "transient failure"}
		}
		return args["value"], nil
	}

	return nil, inv("unknown operation: " + s.Op)
}

func normalizeSliceIndex(i, n int64) int64 {
	if i < 0 {
		if i < -n {
			return 0
		}
		return i + n
	}
	if i > n {
		return n
	}
	return i
}

// ---------------------------------------------------------------- transforms

type transform struct {
	kind   string
	value  interface{}
	valueI int64
	op     string
	aSelf  bool
	bSelf  bool
	a      interface{}
	b      interface{}
}

func isSelfMarker(v interface{}) bool {
	m, ok := v.(map[string]interface{})
	if !ok || len(m) != 1 {
		return false
	}
	_, ok = m["$self"]
	return ok
}

func parseTransform(v interface{}, s *Step) (*transform, *Err) {
	inv := func(msg string) *Err {
		return &Err{Code: "INVALID_ARG", Step: &s.ID, Message: msg}
	}
	m, ok := v.(map[string]interface{})
	if !ok {
		return nil, inv("map transform must be an object")
	}
	kindV, ok := m["type"]
	if !ok {
		return nil, inv("map transform is missing type")
	}
	kind, ok := kindV.(string)
	if !ok {
		return nil, inv("map transform type must be a string")
	}
	switch kind {
	case "constant":
		if !checkArgs(m, []string{"type", "value"}, nil) {
			return nil, inv("constant transform expects type and value")
		}
		return &transform{kind: "constant", value: m["value"]}, nil
	case "add", "subtract", "multiply", "divide", "mod":
		if !checkArgs(m, []string{"type", "value"}, nil) {
			return nil, inv(kind + " transform expects type and value")
		}
		val, ok := asI64(m["value"])
		if !ok {
			return nil, inv("transform value must be an i64 integer")
		}
		return &transform{kind: kind, valueI: val}, nil
	case "negate", "abs", "stringify", "type":
		if !checkArgs(m, []string{"type"}, nil) {
			return nil, inv(kind + " transform takes no extra fields")
		}
		return &transform{kind: kind}, nil
	case "op":
		if !checkArgs(m, []string{"type", "op", "args"}, nil) {
			return nil, inv("op transform expects type, op and args")
		}
		op, ok := m["op"].(string)
		if !ok || (op != "add" && op != "subtract" && op != "multiply" && op != "divide" && op != "mod") {
			return nil, inv("op transform op must be a 2-number operation")
		}
		am, ok := m["args"].(map[string]interface{})
		if !ok {
			return nil, inv("op transform args must be an object")
		}
		if !checkArgs(am, []string{"a", "b"}, nil) {
			return nil, inv("op transform args must be a and b")
		}
		aSelf := isSelfMarker(am["a"])
		bSelf := isSelfMarker(am["b"])
		if !aSelf {
			if _, ok := asI64(am["a"]); !ok {
				return nil, inv("op transform a must be $self or an i64")
			}
		}
		if !bSelf {
			if _, ok := asI64(am["b"]); !ok {
				return nil, inv("op transform b must be $self or an i64")
			}
		}
		return &transform{kind: "op", op: op, a: am["a"], b: am["b"], aSelf: aSelf, bSelf: bSelf}, nil
	}
	return nil, inv("unknown map transform type: " + kind)
}

func applyTransform(t *transform, el interface{}, s *Step) (interface{}, *Err) {
	inv := func(msg string) *Err {
		return &Err{Code: "INVALID_ARG", Step: &s.ID, Message: msg}
	}
	fromCode := func(code string) *Err {
		if code == "DIVISION_BY_ZERO" {
			return &Err{Code: code, Step: &s.ID, Message: "division by zero"}
		}
		return &Err{Code: code, Step: &s.ID, Message: code}
	}

	switch t.kind {
	case "constant":
		return t.value, nil
	case "stringify":
		sval, ok := compactJSON(el)
		if !ok {
			return nil, inv("could not stringify element")
		}
		return sval, nil
	case "type":
		return typeName(el), nil
	}

	x, ok := asI64(el)
	if !ok {
		return nil, inv("map element must be an i64 integer")
	}

	switch t.kind {
	case "negate":
		if x == math.MinInt64 {
			return nil, fromCode("ARITHMETIC_OVERFLOW")
		}
		return numFromInt(-x), nil
	case "abs":
		if x == math.MinInt64 {
			return nil, fromCode("ARITHMETIC_OVERFLOW")
		}
		if x < 0 {
			x = -x
		}
		return numFromInt(x), nil
	case "op":
		var a, b int64
		if t.aSelf {
			a = x
		} else {
			a, _ = asI64(t.a)
		}
		if t.bSelf {
			b = x
		} else {
			b, _ = asI64(t.b)
		}
		r, code := binaryMath(t.op, a, b)
		if code != "" {
			return nil, fromCode(code)
		}
		return numFromInt(r), nil
	default:
		r, code := binaryMath(t.kind, x, t.valueI)
		if code != "" {
			return nil, fromCode(code)
		}
		return numFromInt(r), nil
	}
}

// --------------------------------------------------------------- predicates

type predicate struct {
	kind       string
	valueI     int64
	valueS     string
	targetType string
}

var typeNames = map[string]bool{
	"null": true, "boolean": true, "number": true,
	"string": true, "array": true, "object": true,
}

func parsePredicate(v interface{}, s *Step) (*predicate, *Err) {
	inv := func(msg string) *Err {
		return &Err{Code: "INVALID_ARG", Step: &s.ID, Message: msg}
	}
	m, ok := v.(map[string]interface{})
	if !ok {
		return nil, inv("filter predicate must be an object")
	}
	kindV, ok := m["type"]
	if !ok {
		return nil, inv("filter predicate is missing type")
	}
	kind, ok := kindV.(string)
	if !ok {
		return nil, inv("filter predicate type must be a string")
	}
	switch kind {
	case "even", "odd", "truthy", "falsy":
		if !checkArgs(m, []string{"type"}, nil) {
			return nil, inv(kind + " predicate takes no extra fields")
		}
		return &predicate{kind: kind}, nil
	case "gt", "gte", "lt", "lte", "eq", "ne":
		if !checkArgs(m, []string{"type", "value"}, nil) {
			return nil, inv(kind + " predicate expects type and value")
		}
		val, ok := asI64(m["value"])
		if !ok {
			return nil, inv("predicate value must be an i64 integer")
		}
		return &predicate{kind: kind, valueI: val}, nil
	case "type_is":
		for k := range m {
			switch k {
			case "type", "value", "expected", "type_name", "target", "target_type", "of":
			default:
				return nil, inv("unknown type_is predicate field: " + k)
			}
		}
		var target string
		found := false
		for _, k := range []string{"value", "expected", "type_name", "target", "target_type", "of"} {
			if tv, ok := m[k]; ok {
				sv, ok := tv.(string)
				if !ok {
					return nil, inv("type_is target type must be a string")
				}
				target = sv
				found = true
				break
			}
		}
		if !found {
			return nil, inv("type_is predicate expects a target type")
		}
		return &predicate{kind: "type_is", targetType: target}, nil
	case "string_contains":
		if !checkArgs(m, []string{"type", "value"}, nil) {
			return nil, inv("string_contains predicate expects type and value")
		}
		sv, ok := m["value"].(string)
		if !ok {
			return nil, inv("string_contains value must be a string")
		}
		return &predicate{kind: "string_contains", valueS: sv}, nil
	}

	// Tolerate the degenerate {"type": "<type name>"} spelling that results
	// from writing {"type": "type_is", "type": "<type>"} as a JSON object.
	if typeNames[kind] && len(m) == 1 {
		return &predicate{kind: "type_is", targetType: kind}, nil
	}
	return nil, inv("unknown filter predicate type: " + kind)
}

func applyPredicate(p *predicate, el interface{}, s *Step) (bool, *Err) {
	inv := func(msg string) *Err {
		return &Err{Code: "INVALID_ARG", Step: &s.ID, Message: msg}
	}
	switch p.kind {
	case "truthy":
		return truthy(el), nil
	case "falsy":
		return !truthy(el), nil
	case "type_is":
		return typeName(el) == p.targetType, nil
	case "string_contains":
		str, ok := el.(string)
		if !ok {
			return false, inv("string_contains element must be a string")
		}
		return strings.Contains(str, p.valueS), nil
	}

	x, ok := asI64(el)
	if !ok {
		return false, inv("filter element must be an i64 integer")
	}
	switch p.kind {
	case "even":
		return x%2 == 0, nil
	case "odd":
		return x%2 != 0, nil
	case "gt":
		return x > p.valueI, nil
	case "gte":
		return x >= p.valueI, nil
	case "lt":
		return x < p.valueI, nil
	case "lte":
		return x <= p.valueI, nil
	case "eq":
		return x == p.valueI, nil
	case "ne":
		return x != p.valueI, nil
	}
	return false, inv("unknown filter predicate")
}
