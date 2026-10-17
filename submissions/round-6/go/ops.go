package main

import (
	"math/big"
	"sort"
	"strings"
	"unicode/utf8"
)

func checkArgs(args map[string]any, required, optional []string) *OpError {
	allowed := make(map[string]bool, len(required)+len(optional))
	for _, k := range required {
		allowed[k] = true
	}
	for _, k := range optional {
		allowed[k] = true
	}
	for k := range args {
		if !allowed[k] {
			return opErr("INVALID_ARG", "unknown argument %q", k)
		}
	}
	for _, k := range required {
		if _, ok := args[k]; !ok {
			return opErr("INVALID_ARG", "missing argument %q", k)
		}
	}
	return nil
}

func requireI64(args map[string]any, key string) (int64, *OpError) {
	v, ok := asI64(args[key])
	if !ok {
		return 0, opErr("INVALID_ARG", "argument %q must be an i64 integer", key)
	}
	return v, nil
}

func requireString(args map[string]any, key string) (string, *OpError) {
	v, ok := asString(args[key])
	if !ok {
		return "", opErr("INVALID_ARG", "argument %q must be a string", key)
	}
	return v, nil
}

func requireArray(args map[string]any, key string) ([]any, *OpError) {
	v, ok := asArray(args[key])
	if !ok {
		return nil, opErr("INVALID_ARG", "argument %q must be an array", key)
	}
	return v, nil
}

func requireObject(args map[string]any, key string) (map[string]any, *OpError) {
	v, ok := asObject(args[key])
	if !ok {
		return nil, opErr("INVALID_ARG", "argument %q must be an object", key)
	}
	return v, nil
}

func requireStringArray(args map[string]any, key string) ([]string, *OpError) {
	arr, err := requireArray(args, key)
	if err != nil {
		return nil, err
	}
	out := make([]string, 0, len(arr))
	for _, v := range arr {
		s, ok := v.(string)
		if !ok {
			return nil, opErr("INVALID_ARG", "argument %q must contain strings", key)
		}
		out = append(out, s)
	}
	return out, nil
}

func binaryMath(op string, a, b int64) (int64, *OpError) {
	if (op == "divide" || op == "mod") && b == 0 {
		return 0, opErr("DIVISION_BY_ZERO", "division by zero")
	}
	if (op == "divide" || op == "mod") && a == -1<<63 && b == -1 {
		return 0, opErr("ARITHMETIC_OVERFLOW", "integer overflow")
	}
	ba := big.NewInt(a)
	bb := big.NewInt(b)
	z := new(big.Int)
	switch op {
	case "add":
		z.Add(ba, bb)
	case "subtract":
		z.Sub(ba, bb)
	case "multiply":
		z.Mul(ba, bb)
	case "divide":
		z.Quo(ba, bb) // truncates toward zero
	case "mod":
		z.Rem(ba, bb) // sign of the dividend
	default:
		return 0, opErr("INVALID_ARG", "unknown binary math op %q", op)
	}
	if !z.IsInt64() {
		return 0, opErr("ARITHMETIC_OVERFLOW", "integer overflow")
	}
	return z.Int64(), nil
}

func addAll(arr []any) (any, *OpError) {
	z := new(big.Int)
	for _, v := range arr {
		i, ok := asI64(v)
		if !ok {
			return nil, opErr("INVALID_ARG", "sum input must contain i64 integers")
		}
		z.Add(z, big.NewInt(i))
	}
	if !z.IsInt64() {
		return nil, opErr("ARITHMETIC_OVERFLOW", "integer overflow")
	}
	return num(z.Int64()), nil
}

func minMax(arr []any, wantMin bool) (any, *OpError) {
	if len(arr) == 0 {
		return nil, opErr("EMPTY_ARRAY", "min/max of empty array")
	}
	best, ok := asI64(arr[0])
	if !ok {
		return nil, opErr("INVALID_ARG", "min/max input must contain i64 integers")
	}
	for _, v := range arr[1:] {
		i, ok := asI64(v)
		if !ok {
			return nil, opErr("INVALID_ARG", "min/max input must contain i64 integers")
		}
		if (wantMin && i < best) || (!wantMin && i > best) {
			best = i
		}
	}
	return num(best), nil
}

func asciiUpper(s string) string {
	b := []byte(s)
	for i := range b {
		if b[i] >= 'a' && b[i] <= 'z' {
			b[i] -= 'a' - 'A'
		}
	}
	return string(b)
}

func asciiLower(s string) string {
	b := []byte(s)
	for i := range b {
		if b[i] >= 'A' && b[i] <= 'Z' {
			b[i] += 'a' - 'A'
		}
	}
	return string(b)
}

// substituteSelf replaces {"$self": true} placeholders with the current element.
func substituteSelf(v, el any) any {
	switch x := v.(type) {
	case map[string]any:
		if len(x) == 1 {
			if b, ok := x["$self"].(bool); ok && b {
				return el
			}
		}
		out := make(map[string]any, len(x))
		for k, val := range x {
			out[k] = substituteSelf(val, el)
		}
		return out
	case []any:
		out := make([]any, len(x))
		for i, val := range x {
			out[i] = substituteSelf(val, el)
		}
		return out
	default:
		return v
	}
}

func applyTransform(el, tr any) (any, *OpError) {
	m, ok := tr.(map[string]any)
	if !ok {
		return nil, opErr("INVALID_ARG", "transform must be an object")
	}
	tv, ok := m["type"].(string)
	if !ok {
		return nil, opErr("INVALID_ARG", "transform type must be a string")
	}

	switch tv {
	case "constant":
		if !onlyKeys(m, "type", "value") {
			return nil, opErr("INVALID_ARG", "unknown field in transform")
		}
		v, ok := m["value"]
		if !ok {
			return nil, opErr("INVALID_ARG", "constant transform requires value")
		}
		return v, nil
	case "add", "subtract", "multiply", "divide", "mod":
		if !onlyKeys(m, "type", "value") {
			return nil, opErr("INVALID_ARG", "unknown field in transform")
		}
		v, ok := asI64(m["value"])
		if !ok {
			return nil, opErr("INVALID_ARG", "transform value must be an i64 integer")
		}
		a, ok := asI64(el)
		if !ok {
			return nil, opErr("INVALID_ARG", "map element must be an i64 integer for %s", tv)
		}
		r, err := binaryMath(tv, a, v)
		if err != nil {
			return nil, err
		}
		return num(r), nil
	case "negate", "abs":
		if !onlyKeys(m, "type") {
			return nil, opErr("INVALID_ARG", "unknown field in transform")
		}
		a, ok := asI64(el)
		if !ok {
			return nil, opErr("INVALID_ARG", "map element must be an i64 integer for %s", tv)
		}
		z := new(big.Int).SetInt64(a)
		if tv == "negate" {
			z.Neg(z)
		} else {
			z.Abs(z)
		}
		if !z.IsInt64() {
			return nil, opErr("ARITHMETIC_OVERFLOW", "integer overflow")
		}
		return num(z.Int64()), nil
	case "stringify":
		if !onlyKeys(m, "type") {
			return nil, opErr("INVALID_ARG", "unknown field in transform")
		}
		b, err := marshalCompact(el)
		if err != nil {
			return nil, opErr("INVALID_ARG", "cannot stringify element")
		}
		return string(b), nil
	case "type":
		if !onlyKeys(m, "type") {
			return nil, opErr("INVALID_ARG", "unknown field in transform")
		}
		return jsonTypeName(el), nil
	case "op":
		if !onlyKeys(m, "type", "op", "args") {
			return nil, opErr("INVALID_ARG", "unknown field in transform")
		}
		opv, ok := m["op"].(string)
		if !ok {
			return nil, opErr("INVALID_ARG", "op transform requires a string op")
		}
		switch opv {
		case "add", "subtract", "multiply", "divide", "mod":
		default:
			return nil, opErr("INVALID_ARG", "unsupported op transform %q", opv)
		}
		am, ok := m["args"].(map[string]any)
		if !ok {
			return nil, opErr("INVALID_ARG", "op transform requires args object")
		}
		if err := checkArgs(am, []string{"a", "b"}, nil); err != nil {
			return nil, err
		}
		if _, ok := am["a"]; !ok {
			return nil, opErr("INVALID_ARG", "missing argument \"a\"")
		}
		if _, ok := am["b"]; !ok {
			return nil, opErr("INVALID_ARG", "missing argument \"b\"")
		}
		av := substituteSelf(am["a"], el)
		bv := substituteSelf(am["b"], el)
		a, ok := asI64(av)
		if !ok {
			return nil, opErr("INVALID_ARG", "op transform argument a must be i64")
		}
		b, ok := asI64(bv)
		if !ok {
			return nil, opErr("INVALID_ARG", "op transform argument b must be i64")
		}
		r, err := binaryMath(opv, a, b)
		if err != nil {
			return nil, err
		}
		return num(r), nil
	default:
		return nil, opErr("INVALID_ARG", "unknown transform type %q", tv)
	}
}

func applyPredicate(el, pr any) (bool, *OpError) {
	m, ok := pr.(map[string]any)
	if !ok {
		return false, opErr("INVALID_ARG", "predicate must be an object")
	}
	tv, ok := m["type"].(string)
	if !ok {
		return false, opErr("INVALID_ARG", "predicate type must be a string")
	}

	typeIs := func(target string) (bool, *OpError) {
		switch target {
		case "null", "boolean", "number", "string", "array", "object":
		default:
			return false, opErr("INVALID_ARG", "unknown type name %q", target)
		}
		return jsonTypeName(el) == target, nil
	}

	switch tv {
	case "even", "odd":
		if !onlyKeys(m, "type") {
			return false, opErr("INVALID_ARG", "unknown field in predicate")
		}
		n, ok := asI64(el)
		if !ok {
			return false, opErr("INVALID_ARG", "even/odd predicate requires numeric elements")
		}
		even := n%2 == 0
		if tv == "even" {
			return even, nil
		}
		return !even, nil
	case "gt", "gte", "lt", "lte":
		if !onlyKeys(m, "type", "value") {
			return false, opErr("INVALID_ARG", "unknown field in predicate")
		}
		v, ok := asI64(m["value"])
		if !ok {
			return false, opErr("INVALID_ARG", "predicate value must be an i64 integer")
		}
		n, ok := asI64(el)
		if !ok {
			return false, opErr("INVALID_ARG", "comparison predicate requires numeric elements")
		}
		switch tv {
		case "gt":
			return n > v, nil
		case "gte":
			return n >= v, nil
		case "lt":
			return n < v, nil
		default:
			return n <= v, nil
		}
	case "eq", "ne":
		if !onlyKeys(m, "type", "value") {
			return false, opErr("INVALID_ARG", "unknown field in predicate")
		}
		v, ok := m["value"]
		if !ok {
			return false, opErr("INVALID_ARG", "eq/ne predicate requires value")
		}
		if tv == "eq" {
			return valuesEqual(el, v), nil
		}
		return !valuesEqual(el, v), nil
	case "truthy", "falsy":
		if !onlyKeys(m, "type") {
			return false, opErr("INVALID_ARG", "unknown field in predicate")
		}
		t := truthy(el)
		if tv == "truthy" {
			return t, nil
		}
		return !t, nil
	case "type_is":
		target := ""
		found := 0
		for k, v := range m {
			if k == "type" {
				continue
			}
			s, ok := v.(string)
			if !ok {
				return false, opErr("INVALID_ARG", "type_is predicate target must be a string")
			}
			target = s
			found++
		}
		if found != 1 {
			return false, opErr("INVALID_ARG", "type_is predicate requires exactly one type name")
		}
		return typeIs(target)
	case "string_contains":
		if !onlyKeys(m, "type", "value") {
			return false, opErr("INVALID_ARG", "unknown field in predicate")
		}
		needle, ok := m["value"].(string)
		if !ok {
			return false, opErr("INVALID_ARG", "string_contains value must be a string")
		}
		s, ok := el.(string)
		if !ok {
			return false, opErr("INVALID_ARG", "string_contains predicate requires string elements")
		}
		return strings.Contains(s, needle), nil
	case "null", "boolean", "number", "string", "array", "object":
		// Fallback for the ambiguous {"type": "type_is", "type": "<type>"} form:
		// a JSON parser keeps the last duplicate key, so only the type name remains.
		if !onlyKeys(m, "type") {
			return false, opErr("INVALID_ARG", "ambiguous type_is predicate")
		}
		return typeIs(tv)
	default:
		return false, opErr("INVALID_ARG", "unknown predicate type %q", tv)
	}
}

func opSlice(args map[string]any) (any, *OpError) {
	if err := checkArgs(args, []string{"input", "start"}, []string{"end"}); err != nil {
		return nil, err
	}
	arr, err := requireArray(args, "input")
	if err != nil {
		return nil, err
	}
	start, err := requireI64(args, "start")
	if err != nil {
		return nil, err
	}
	n := int64(len(arr))
	norm := func(i int64) int64 {
		if i < 0 {
			i += n
			if i < 0 {
				i = 0
			}
		} else if i > n {
			i = n
		}
		return i
	}
	start = norm(start)
	end := n
	if ev, ok := args["end"]; ok {
		e, ok := asI64(ev)
		if !ok {
			return nil, opErr("INVALID_ARG", "argument \"end\" must be an i64 integer")
		}
		end = norm(e)
	}
	out := []any{}
	if start < end {
		out = append(out, arr[start:end]...)
	}
	return out, nil
}

func (ex *Executor) executeOp(step *Step, args map[string]any) (any, *OpError) {
	switch step.Op {
	case "add", "subtract", "multiply", "divide", "mod":
		if err := checkArgs(args, []string{"a", "b"}, nil); err != nil {
			return nil, err
		}
		a, err := requireI64(args, "a")
		if err != nil {
			return nil, err
		}
		b, err := requireI64(args, "b")
		if err != nil {
			return nil, err
		}
		r, e := binaryMath(step.Op, a, b)
		if e != nil {
			return nil, e
		}
		return num(r), nil

	case "sum":
		if err := checkArgs(args, []string{"input"}, nil); err != nil {
			return nil, err
		}
		arr, err := requireArray(args, "input")
		if err != nil {
			return nil, err
		}
		return addAll(arr)

	case "min", "max":
		if err := checkArgs(args, []string{"input"}, nil); err != nil {
			return nil, err
		}
		arr, err := requireArray(args, "input")
		if err != nil {
			return nil, err
		}
		return minMax(arr, step.Op == "min")

	case "range":
		if err := checkArgs(args, []string{"start", "end"}, nil); err != nil {
			return nil, err
		}
		start, err := requireI64(args, "start")
		if err != nil {
			return nil, err
		}
		end, err := requireI64(args, "end")
		if err != nil {
			return nil, err
		}
		out := []any{}
		if start > end {
			return out, nil
		}
		for v := start; ; v++ {
			out = append(out, num(v))
			if v == end {
				break
			}
		}
		return out, nil

	case "concat":
		if err := checkArgs(args, []string{"a", "b"}, nil); err != nil {
			return nil, err
		}
		a, err := requireString(args, "a")
		if err != nil {
			return nil, err
		}
		b, err := requireString(args, "b")
		if err != nil {
			return nil, err
		}
		return a + b, nil

	case "split":
		if err := checkArgs(args, []string{"input", "sep"}, nil); err != nil {
			return nil, err
		}
		input, err := requireString(args, "input")
		if err != nil {
			return nil, err
		}
		sep, err := requireString(args, "sep")
		if err != nil {
			return nil, err
		}
		out := []any{}
		if sep == "" {
			for _, r := range input {
				out = append(out, string(r))
			}
		} else {
			for _, part := range strings.Split(input, sep) {
				out = append(out, part)
			}
		}
		return out, nil

	case "join":
		if err := checkArgs(args, []string{"input", "sep"}, nil); err != nil {
			return nil, err
		}
		arr, err := requireStringArray(args, "input")
		if err != nil {
			return nil, err
		}
		sep, err := requireString(args, "sep")
		if err != nil {
			return nil, err
		}
		return strings.Join(arr, sep), nil

	case "uppercase":
		if err := checkArgs(args, []string{"input"}, nil); err != nil {
			return nil, err
		}
		s, err := requireString(args, "input")
		if err != nil {
			return nil, err
		}
		return asciiUpper(s), nil

	case "lowercase":
		if err := checkArgs(args, []string{"input"}, nil); err != nil {
			return nil, err
		}
		s, err := requireString(args, "input")
		if err != nil {
			return nil, err
		}
		return asciiLower(s), nil

	case "trim":
		if err := checkArgs(args, []string{"input"}, nil); err != nil {
			return nil, err
		}
		s, err := requireString(args, "input")
		if err != nil {
			return nil, err
		}
		return strings.Trim(s, " \t\n\r\f\v"), nil

	case "replace":
		if err := checkArgs(args, []string{"input", "search", "replace"}, nil); err != nil {
			return nil, err
		}
		input, err := requireString(args, "input")
		if err != nil {
			return nil, err
		}
		search, err := requireString(args, "search")
		if err != nil {
			return nil, err
		}
		repl, err := requireString(args, "replace")
		if err != nil {
			return nil, err
		}
		if search == "" {
			return nil, opErr("INVALID_ARG", "replace search must not be empty")
		}
		return strings.ReplaceAll(input, search, repl), nil

	case "length":
		if err := checkArgs(args, []string{"input"}, nil); err != nil {
			return nil, err
		}
		s, err := requireString(args, "input")
		if err != nil {
			return nil, err
		}
		return num(int64(utf8.RuneCountInString(s))), nil

	case "map":
		if err := checkArgs(args, []string{"input", "transform"}, nil); err != nil {
			return nil, err
		}
		arr, err := requireArray(args, "input")
		if err != nil {
			return nil, err
		}
		tr := args["transform"]
		out := make([]any, 0, len(arr))
		for _, el := range arr {
			r, e := applyTransform(el, tr)
			if e != nil {
				return nil, e
			}
			out = append(out, r)
		}
		return out, nil

	case "filter":
		if err := checkArgs(args, []string{"input", "predicate"}, nil); err != nil {
			return nil, err
		}
		arr, err := requireArray(args, "input")
		if err != nil {
			return nil, err
		}
		pr := args["predicate"]
		out := make([]any, 0, len(arr))
		for _, el := range arr {
			ok, e := applyPredicate(el, pr)
			if e != nil {
				return nil, e
			}
			if ok {
				out = append(out, el)
			}
		}
		return out, nil

	case "reduce":
		if err := checkArgs(args, []string{"input", "op"}, []string{"initial"}); err != nil {
			return nil, err
		}
		arr, err := requireArray(args, "input")
		if err != nil {
			return nil, err
		}
		op, err := requireString(args, "op")
		if err != nil {
			return nil, err
		}
		var acc int64
		switch op {
		case "add", "subtract":
			acc = 0
		case "multiply":
			acc = 1
		default:
			return nil, opErr("INVALID_ARG", "unsupported reduce op %q", op)
		}
		if iv, ok := args["initial"]; ok {
			i, ok := asI64(iv)
			if !ok {
				return nil, opErr("INVALID_ARG", "argument \"initial\" must be an i64 integer")
			}
			acc = i
		}
		for _, el := range arr {
			i, ok := asI64(el)
			if !ok {
				return nil, opErr("INVALID_ARG", "reduce input must contain i64 integers")
			}
			r, e := binaryMath(op, acc, i)
			if e != nil {
				return nil, e
			}
			acc = r
		}
		return num(acc), nil

	case "sort":
		if err := checkArgs(args, []string{"input"}, nil); err != nil {
			return nil, err
		}
		arr, err := requireArray(args, "input")
		if err != nil {
			return nil, err
		}
		out := make([]any, len(arr))
		copy(out, arr)
		sort.SliceStable(out, func(i, j int) bool {
			return compareValues(out[i], out[j]) < 0
		})
		return out, nil

	case "reverse":
		if err := checkArgs(args, []string{"input"}, nil); err != nil {
			return nil, err
		}
		arr, err := requireArray(args, "input")
		if err != nil {
			return nil, err
		}
		out := make([]any, len(arr))
		for i, v := range arr {
			out[len(arr)-1-i] = v
		}
		return out, nil

	case "unique":
		if err := checkArgs(args, []string{"input"}, nil); err != nil {
			return nil, err
		}
		arr, err := requireArray(args, "input")
		if err != nil {
			return nil, err
		}
		out := make([]any, 0, len(arr))
		for _, v := range arr {
			seen := false
			for _, u := range out {
				if valuesEqual(v, u) {
					seen = true
					break
				}
			}
			if !seen {
				out = append(out, v)
			}
		}
		return out, nil

	case "slice":
		return opSlice(args)

	case "flatten":
		if err := checkArgs(args, []string{"input"}, nil); err != nil {
			return nil, err
		}
		arr, err := requireArray(args, "input")
		if err != nil {
			return nil, err
		}
		out := []any{}
		for _, v := range arr {
			if inner, ok := v.([]any); ok {
				out = append(out, inner...)
			} else {
				out = append(out, v)
			}
		}
		return out, nil

	case "get":
		if err := checkArgs(args, []string{"input", "key"}, nil); err != nil {
			return nil, err
		}
		obj, err := requireObject(args, "input")
		if err != nil {
			return nil, err
		}
		key, err := requireString(args, "key")
		if err != nil {
			return nil, err
		}
		if v, ok := obj[key]; ok {
			return v, nil
		}
		return nil, nil

	case "set":
		if err := checkArgs(args, []string{"input", "key", "value"}, nil); err != nil {
			return nil, err
		}
		obj, err := requireObject(args, "input")
		if err != nil {
			return nil, err
		}
		key, err := requireString(args, "key")
		if err != nil {
			return nil, err
		}
		out := make(map[string]any, len(obj)+1)
		for k, v := range obj {
			out[k] = v
		}
		out[key] = args["value"]
		return out, nil

	case "keys":
		if err := checkArgs(args, []string{"input"}, nil); err != nil {
			return nil, err
		}
		obj, err := requireObject(args, "input")
		if err != nil {
			return nil, err
		}
		out := make([]any, 0, len(obj))
		for _, k := range sortedKeys(obj) {
			out = append(out, k)
		}
		return out, nil

	case "values":
		if err := checkArgs(args, []string{"input"}, nil); err != nil {
			return nil, err
		}
		obj, err := requireObject(args, "input")
		if err != nil {
			return nil, err
		}
		out := make([]any, 0, len(obj))
		for _, k := range sortedKeys(obj) {
			out = append(out, obj[k])
		}
		return out, nil

	case "merge":
		if err := checkArgs(args, []string{"a", "b"}, nil); err != nil {
			return nil, err
		}
		a, err := requireObject(args, "a")
		if err != nil {
			return nil, err
		}
		b, err := requireObject(args, "b")
		if err != nil {
			return nil, err
		}
		out := make(map[string]any, len(a)+len(b))
		for k, v := range a {
			out[k] = v
		}
		for k, v := range b {
			out[k] = v
		}
		return out, nil

	case "pick":
		if err := checkArgs(args, []string{"input", "keys"}, nil); err != nil {
			return nil, err
		}
		obj, err := requireObject(args, "input")
		if err != nil {
			return nil, err
		}
		keys, err := requireStringArray(args, "keys")
		if err != nil {
			return nil, err
		}
		out := make(map[string]any, len(keys))
		for _, k := range keys {
			out[k] = obj[k] // missing keys become null
		}
		return out, nil

	case "omit":
		if err := checkArgs(args, []string{"input", "keys"}, nil); err != nil {
			return nil, err
		}
		obj, err := requireObject(args, "input")
		if err != nil {
			return nil, err
		}
		keys, err := requireStringArray(args, "keys")
		if err != nil {
			return nil, err
		}
		drop := make(map[string]bool, len(keys))
		for _, k := range keys {
			drop[k] = true
		}
		out := make(map[string]any, len(obj))
		for k, v := range obj {
			if !drop[k] {
				out[k] = v
			}
		}
		return out, nil

	case "constant":
		if err := checkArgs(args, []string{"value"}, nil); err != nil {
			return nil, err
		}
		return args["value"], nil

	case "equals":
		if err := checkArgs(args, []string{"a", "b"}, nil); err != nil {
			return nil, err
		}
		return valuesEqual(args["a"], args["b"]), nil

	case "compare":
		if err := checkArgs(args, []string{"a", "b", "op"}, nil); err != nil {
			return nil, err
		}
		op, err := requireString(args, "op")
		if err != nil {
			return nil, err
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
		default:
			return nil, opErr("INVALID_ARG", "unknown compare op %q", op)
		}

	case "and":
		if err := checkArgs(args, []string{"a", "b"}, nil); err != nil {
			return nil, err
		}
		return truthy(args["a"]) && truthy(args["b"]), nil

	case "or":
		if err := checkArgs(args, []string{"a", "b"}, nil); err != nil {
			return nil, err
		}
		return truthy(args["a"]) || truthy(args["b"]), nil

	case "not":
		if err := checkArgs(args, []string{"a"}, nil); err != nil {
			return nil, err
		}
		return !truthy(args["a"]), nil

	case "type":
		if err := checkArgs(args, []string{"value"}, nil); err != nil {
			return nil, err
		}
		return jsonTypeName(args["value"]), nil

	case "fail_until":
		if err := checkArgs(args, []string{"failures", "value"}, nil); err != nil {
			return nil, err
		}
		failures, err := requireI64(args, "failures")
		if err != nil {
			return nil, err
		}
		if failures < 0 || failures > 10 {
			return nil, opErr("INVALID_ARG", "failures must be an integer from 0 to 10")
		}
		ex.failAttempts[step.ID]++
		if int64(ex.failAttempts[step.ID]) <= failures {
			return nil, opErr("TRANSIENT_FAILURE", "transient failure %d of %d", ex.failAttempts[step.ID], failures)
		}
		return args["value"], nil
	}

	return nil, opErr("INVALID_ARG", "unsupported op %q", step.Op)
}
