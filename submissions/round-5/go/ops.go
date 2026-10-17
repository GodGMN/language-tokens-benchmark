package main

import (
	"fmt"
	"math"
	"sort"
	"strings"
	"unicode/utf8"
)

type OpError struct {
	Code string
	Msg  string
}

func newOpError(code, format string, a ...interface{}) *OpError {
	return &OpError{Code: code, Msg: fmt.Sprintf(format, a...)}
}

func argErr(format string, a ...interface{}) *OpError {
	return newOpError("INVALID_ARG", format, a...)
}

func overflowErr() *OpError {
	return newOpError("ARITHMETIC_OVERFLOW", "signed 64-bit integer overflow")
}

// ---------------------------------------------------------------------------
// Argument helpers
// ---------------------------------------------------------------------------

func allowedArgs(args map[string]Value, allowed ...string) *OpError {
	ok := make(map[string]bool, len(allowed))
	for _, a := range allowed {
		ok[a] = true
	}
	for _, k := range sortedKeys(args) {
		if !ok[k] {
			return argErr("unknown argument %q", k)
		}
	}
	return nil
}

func intArg(args map[string]Value, name string) (int64, *OpError) {
	v, ok := args[name]
	if !ok {
		return 0, argErr("missing required argument %q", name)
	}
	return intValue(v, fmt.Sprintf("argument %q", name))
}

func optIntArg(args map[string]Value, name string) (int64, bool, *OpError) {
	v, ok := args[name]
	if !ok {
		return 0, false, nil
	}
	i, err := intValue(v, fmt.Sprintf("argument %q", name))
	if err != nil {
		return 0, false, err
	}
	return i, true, nil
}

func stringArg(args map[string]Value, name string) (string, *OpError) {
	v, ok := args[name]
	if !ok {
		return "", argErr("missing required argument %q", name)
	}
	return stringValue(v, fmt.Sprintf("argument %q", name))
}

func arrayArg(args map[string]Value, name string) ([]Value, *OpError) {
	v, ok := args[name]
	if !ok {
		return nil, argErr("missing required argument %q", name)
	}
	arr, ok := v.([]Value)
	if !ok {
		return nil, argErr("argument %q must be an array", name)
	}
	return arr, nil
}

func objectArg(args map[string]Value, name string) (map[string]Value, *OpError) {
	v, ok := args[name]
	if !ok {
		return nil, argErr("missing required argument %q", name)
	}
	obj, ok := v.(map[string]Value)
	if !ok {
		return nil, argErr("argument %q must be an object", name)
	}
	return obj, nil
}

func intValue(v Value, ctx string) (int64, *OpError) {
	n, ok := v.(Number)
	if !ok {
		return 0, argErr("%s must be an integer", ctx)
	}
	i, ok := intLiteral(n)
	if !ok {
		return 0, argErr("%s must be an integer literal within the i64 range", ctx)
	}
	return i, nil
}

func stringValue(v Value, ctx string) (string, *OpError) {
	s, ok := v.(string)
	if !ok {
		return "", argErr("%s must be a string", ctx)
	}
	return s, nil
}

// ---------------------------------------------------------------------------
// Checked i64 arithmetic
// ---------------------------------------------------------------------------

func checkedAdd(a, b int64) (int64, *OpError) {
	s := a + b
	if (a > 0 && b > 0 && s < 0) || (a < 0 && b < 0 && s >= 0) {
		return 0, overflowErr()
	}
	return s, nil
}

func checkedSub(a, b int64) (int64, *OpError) {
	d := a - b
	if (a >= 0 && b < 0 && d < 0) || (a < 0 && b > 0 && d >= 0) {
		return 0, overflowErr()
	}
	return d, nil
}

func checkedMul(a, b int64) (int64, *OpError) {
	if a == 0 || b == 0 {
		return 0, nil
	}
	if (a == math.MinInt64 && b == -1) || (b == math.MinInt64 && a == -1) {
		return 0, overflowErr()
	}
	c := a * b
	if c/b != a {
		return 0, overflowErr()
	}
	return c, nil
}

func checkedNeg(a int64) (int64, *OpError) {
	if a == math.MinInt64 {
		return 0, overflowErr()
	}
	return -a, nil
}

func checkedAbs(a int64) (int64, *OpError) {
	if a == math.MinInt64 {
		return 0, overflowErr()
	}
	if a < 0 {
		return -a, nil
	}
	return a, nil
}

func applyBinary(op string, a, b int64) (int64, *OpError) {
	switch op {
	case "add":
		return checkedAdd(a, b)
	case "subtract":
		return checkedSub(a, b)
	case "multiply":
		return checkedMul(a, b)
	case "divide":
		if b == 0 {
			return 0, newOpError("DIVISION_BY_ZERO", "division by zero")
		}
		if a == math.MinInt64 && b == -1 {
			return 0, overflowErr()
		}
		return a / b, nil
	case "mod":
		if b == 0 {
			return 0, newOpError("DIVISION_BY_ZERO", "modulo by zero")
		}
		if a == math.MinInt64 && b == -1 {
			return 0, overflowErr()
		}
		return a % b, nil
	}
	return 0, argErr("unsupported binary operation %q", op)
}

// ---------------------------------------------------------------------------
// Op dispatch
// ---------------------------------------------------------------------------

func (e *Engine) executeOp(st *Step, args map[string]Value) (Value, *OpError) {
	switch st.Op {
	case "add", "subtract", "multiply", "divide", "mod":
		return opMathBinary(st.Op, args)
	case "sum":
		return opSum(args)
	case "min", "max":
		return opMinMax(st.Op, args)
	case "range":
		return opRange(args)
	case "concat":
		if err := allowedArgs(args, "a", "b"); err != nil {
			return nil, err
		}
		a, err := stringArg(args, "a")
		if err != nil {
			return nil, err
		}
		b, err := stringArg(args, "b")
		if err != nil {
			return nil, err
		}
		return a + b, nil
	case "split":
		return opSplit(args)
	case "join":
		return opJoin(args)
	case "uppercase", "lowercase":
		if err := allowedArgs(args, "input"); err != nil {
			return nil, err
		}
		s, err := stringArg(args, "input")
		if err != nil {
			return nil, err
		}
		if st.Op == "uppercase" {
			return asciiUpper(s), nil
		}
		return asciiLower(s), nil
	case "trim":
		if err := allowedArgs(args, "input"); err != nil {
			return nil, err
		}
		s, err := stringArg(args, "input")
		if err != nil {
			return nil, err
		}
		return strings.Trim(s, " \t\n\r\f\v"), nil
	case "replace":
		if err := allowedArgs(args, "input", "search", "replace"); err != nil {
			return nil, err
		}
		input, err := stringArg(args, "input")
		if err != nil {
			return nil, err
		}
		search, err := stringArg(args, "search")
		if err != nil {
			return nil, err
		}
		repl, err := stringArg(args, "replace")
		if err != nil {
			return nil, err
		}
		if search == "" {
			return nil, argErr("replace search must not be empty")
		}
		return strings.ReplaceAll(input, search, repl), nil
	case "length":
		if err := allowedArgs(args, "input"); err != nil {
			return nil, err
		}
		s, err := stringArg(args, "input")
		if err != nil {
			return nil, err
		}
		return num(int64(utf8.RuneCountInString(s))), nil
	case "map":
		return opMap(args)
	case "filter":
		return opFilter(args)
	case "reduce":
		return opReduce(args)
	case "sort":
		if err := allowedArgs(args, "input"); err != nil {
			return nil, err
		}
		arr, err := arrayArg(args, "input")
		if err != nil {
			return nil, err
		}
		out := make([]Value, len(arr))
		copy(out, arr)
		sort.SliceStable(out, func(i, j int) bool {
			return compareValues(out[i], out[j]) < 0
		})
		return out, nil
	case "reverse":
		if err := allowedArgs(args, "input"); err != nil {
			return nil, err
		}
		arr, err := arrayArg(args, "input")
		if err != nil {
			return nil, err
		}
		out := make([]Value, len(arr))
		for i := range arr {
			out[len(arr)-1-i] = arr[i]
		}
		return out, nil
	case "unique":
		if err := allowedArgs(args, "input"); err != nil {
			return nil, err
		}
		arr, err := arrayArg(args, "input")
		if err != nil {
			return nil, err
		}
		out := make([]Value, 0, len(arr))
		for _, el := range arr {
			dup := false
			for _, prev := range out {
				if equalValues(prev, el) {
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
		return opSlice(args)
	case "flatten":
		if err := allowedArgs(args, "input"); err != nil {
			return nil, err
		}
		arr, err := arrayArg(args, "input")
		if err != nil {
			return nil, err
		}
		out := make([]Value, 0, len(arr))
		for _, el := range arr {
			if sub, ok := el.([]Value); ok {
				out = append(out, sub...)
			} else {
				out = append(out, el)
			}
		}
		return out, nil
	case "get":
		if err := allowedArgs(args, "input", "key"); err != nil {
			return nil, err
		}
		obj, err := objectArg(args, "input")
		if err != nil {
			return nil, err
		}
		key, err := stringArg(args, "key")
		if err != nil {
			return nil, err
		}
		v, ok := obj[key]
		if !ok {
			return nil, nil
		}
		return v, nil
	case "set":
		if err := allowedArgs(args, "input", "key", "value"); err != nil {
			return nil, err
		}
		obj, err := objectArg(args, "input")
		if err != nil {
			return nil, err
		}
		key, err := stringArg(args, "key")
		if err != nil {
			return nil, err
		}
		val, ok := args["value"]
		if !ok {
			return nil, argErr("missing required argument \"value\"")
		}
		out := make(map[string]Value, len(obj)+1)
		for k, v := range obj {
			out[k] = v
		}
		out[key] = val
		return out, nil
	case "keys":
		if err := allowedArgs(args, "input"); err != nil {
			return nil, err
		}
		obj, err := objectArg(args, "input")
		if err != nil {
			return nil, err
		}
		keys := sortedKeys(obj)
		out := make([]Value, len(keys))
		for i, k := range keys {
			out[i] = k
		}
		return out, nil
	case "values":
		if err := allowedArgs(args, "input"); err != nil {
			return nil, err
		}
		obj, err := objectArg(args, "input")
		if err != nil {
			return nil, err
		}
		keys := sortedKeys(obj)
		out := make([]Value, len(keys))
		for i, k := range keys {
			out[i] = obj[k]
		}
		return out, nil
	case "merge":
		if err := allowedArgs(args, "a", "b"); err != nil {
			return nil, err
		}
		a, err := objectArg(args, "a")
		if err != nil {
			return nil, err
		}
		b, err := objectArg(args, "b")
		if err != nil {
			return nil, err
		}
		out := make(map[string]Value, len(a)+len(b))
		for k, v := range a {
			out[k] = v
		}
		for k, v := range b {
			out[k] = v
		}
		return out, nil
	case "pick":
		return opPick(args, true)
	case "omit":
		return opPick(args, false)
	case "constant":
		if err := allowedArgs(args, "value"); err != nil {
			return nil, err
		}
		v, ok := args["value"]
		if !ok {
			return nil, argErr("missing required argument \"value\"")
		}
		return v, nil
	case "equals":
		if err := allowedArgs(args, "a", "b"); err != nil {
			return nil, err
		}
		a, ok := args["a"]
		if !ok {
			return nil, argErr("missing required argument \"a\"")
		}
		b, ok := args["b"]
		if !ok {
			return nil, argErr("missing required argument \"b\"")
		}
		return equalValues(a, b), nil
	case "compare":
		if err := allowedArgs(args, "a", "b", "op"); err != nil {
			return nil, err
		}
		a, ok := args["a"]
		if !ok {
			return nil, argErr("missing required argument \"a\"")
		}
		b, ok := args["b"]
		if !ok {
			return nil, argErr("missing required argument \"b\"")
		}
		cmpOp, err := stringArg(args, "op")
		if err != nil {
			return nil, err
		}
		switch cmpOp {
		case "gt", "gte", "lt", "lte":
		default:
			return nil, argErr("compare op must be one of gt, gte, lt, lte")
		}
		c := compareValues(a, b)
		switch cmpOp {
		case "gt":
			return c > 0, nil
		case "gte":
			return c >= 0, nil
		case "lt":
			return c < 0, nil
		default:
			return c <= 0, nil
		}
	case "and":
		if err := allowedArgs(args, "a", "b"); err != nil {
			return nil, err
		}
		a, ok := args["a"]
		if !ok {
			return nil, argErr("missing required argument \"a\"")
		}
		b, ok := args["b"]
		if !ok {
			return nil, argErr("missing required argument \"b\"")
		}
		return truthy(a) && truthy(b), nil
	case "or":
		if err := allowedArgs(args, "a", "b"); err != nil {
			return nil, err
		}
		a, ok := args["a"]
		if !ok {
			return nil, argErr("missing required argument \"a\"")
		}
		b, ok := args["b"]
		if !ok {
			return nil, argErr("missing required argument \"b\"")
		}
		return truthy(a) || truthy(b), nil
	case "not":
		if err := allowedArgs(args, "a"); err != nil {
			return nil, err
		}
		a, ok := args["a"]
		if !ok {
			return nil, argErr("missing required argument \"a\"")
		}
		return !truthy(a), nil
	case "type":
		if err := allowedArgs(args, "value"); err != nil {
			return nil, err
		}
		v, ok := args["value"]
		if !ok {
			return nil, argErr("missing required argument \"value\"")
		}
		return typeName(v), nil
	case "fail_until":
		return e.opFailUntil(st, args)
	}
	return nil, newOpError("INVALID_ARG", "unknown operation %q", st.Op)
}

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------

func opMathBinary(op string, args map[string]Value) (Value, *OpError) {
	if err := allowedArgs(args, "a", "b"); err != nil {
		return nil, err
	}
	a, err := intArg(args, "a")
	if err != nil {
		return nil, err
	}
	b, err := intArg(args, "b")
	if err != nil {
		return nil, err
	}
	r, err := applyBinary(op, a, b)
	if err != nil {
		return nil, err
	}
	return num(r), nil
}

func opSum(args map[string]Value) (Value, *OpError) {
	if err := allowedArgs(args, "input"); err != nil {
		return nil, err
	}
	arr, err := arrayArg(args, "input")
	if err != nil {
		return nil, err
	}
	var acc int64
	for i, el := range arr {
		x, err := intValue(el, fmt.Sprintf("input[%d]", i))
		if err != nil {
			return nil, err
		}
		acc, err = checkedAdd(acc, x)
		if err != nil {
			return nil, err
		}
	}
	return num(acc), nil
}

func opMinMax(op string, args map[string]Value) (Value, *OpError) {
	if err := allowedArgs(args, "input"); err != nil {
		return nil, err
	}
	arr, err := arrayArg(args, "input")
	if err != nil {
		return nil, err
	}
	if len(arr) == 0 {
		return nil, newOpError("EMPTY_ARRAY", "%s requires a non-empty array", op)
	}
	best, err := intValue(arr[0], "input[0]")
	if err != nil {
		return nil, err
	}
	for i := 1; i < len(arr); i++ {
		x, err := intValue(arr[i], fmt.Sprintf("input[%d]", i))
		if err != nil {
			return nil, err
		}
		if (op == "min" && x < best) || (op == "max" && x > best) {
			best = x
		}
	}
	return num(best), nil
}

func opRange(args map[string]Value) (Value, *OpError) {
	if err := allowedArgs(args, "start", "end"); err != nil {
		return nil, err
	}
	start, err := intArg(args, "start")
	if err != nil {
		return nil, err
	}
	end, err := intArg(args, "end")
	if err != nil {
		return nil, err
	}
	out := make([]Value, 0)
	if start > end {
		return out, nil
	}
	for i := start; ; i++ {
		out = append(out, num(i))
		if i == end {
			break
		}
	}
	return out, nil
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

func asciiUpper(s string) string {
	var b strings.Builder
	b.Grow(len(s))
	for _, r := range s {
		if r >= 'a' && r <= 'z' {
			r -= 'a' - 'A'
		}
		b.WriteRune(r)
	}
	return b.String()
}

func asciiLower(s string) string {
	var b strings.Builder
	b.Grow(len(s))
	for _, r := range s {
		if r >= 'A' && r <= 'Z' {
			r += 'a' - 'A'
		}
		b.WriteRune(r)
	}
	return b.String()
}

func opSplit(args map[string]Value) (Value, *OpError) {
	if err := allowedArgs(args, "input", "sep"); err != nil {
		return nil, err
	}
	input, err := stringArg(args, "input")
	if err != nil {
		return nil, err
	}
	sep, err := stringArg(args, "sep")
	if err != nil {
		return nil, err
	}
	var parts []string
	if sep == "" {
		for _, r := range input {
			parts = append(parts, string(r))
		}
	} else {
		parts = strings.Split(input, sep)
	}
	out := make([]Value, len(parts))
	for i, p := range parts {
		out[i] = p
	}
	return out, nil
}

func opJoin(args map[string]Value) (Value, *OpError) {
	if err := allowedArgs(args, "input", "sep"); err != nil {
		return nil, err
	}
	arr, err := arrayArg(args, "input")
	if err != nil {
		return nil, err
	}
	sep, err := stringArg(args, "sep")
	if err != nil {
		return nil, err
	}
	parts := make([]string, len(arr))
	for i, el := range arr {
		s, err := stringValue(el, fmt.Sprintf("input[%d]", i))
		if err != nil {
			return nil, err
		}
		parts[i] = s
	}
	return strings.Join(parts, sep), nil
}

// ---------------------------------------------------------------------------
// Arrays: map / filter / reduce / slice
// ---------------------------------------------------------------------------

func opMap(args map[string]Value) (Value, *OpError) {
	if err := allowedArgs(args, "input", "transform"); err != nil {
		return nil, err
	}
	arr, err := arrayArg(args, "input")
	if err != nil {
		return nil, err
	}
	tv, ok := args["transform"]
	if !ok {
		return nil, argErr("missing required argument \"transform\"")
	}
	transform, ok := tv.(map[string]Value)
	if !ok {
		return nil, argErr("transform must be an object")
	}
	out := make([]Value, len(arr))
	for i, el := range arr {
		r, err := applyTransform(transform, el)
		if err != nil {
			return nil, err
		}
		out[i] = r
	}
	return out, nil
}

func opFilter(args map[string]Value) (Value, *OpError) {
	if err := allowedArgs(args, "input", "predicate"); err != nil {
		return nil, err
	}
	arr, err := arrayArg(args, "input")
	if err != nil {
		return nil, err
	}
	pv, ok := args["predicate"]
	if !ok {
		return nil, argErr("missing required argument \"predicate\"")
	}
	predicate, ok := pv.(map[string]Value)
	if !ok {
		return nil, argErr("predicate must be an object")
	}
	out := make([]Value, 0, len(arr))
	for _, el := range arr {
		keep, err := applyPredicate(predicate, el)
		if err != nil {
			return nil, err
		}
		if keep {
			out = append(out, el)
		}
	}
	return out, nil
}

func opReduce(args map[string]Value) (Value, *OpError) {
	if err := allowedArgs(args, "input", "op", "initial"); err != nil {
		return nil, err
	}
	arr, err := arrayArg(args, "input")
	if err != nil {
		return nil, err
	}
	op, err := stringArg(args, "op")
	if err != nil {
		return nil, err
	}
	if op != "add" && op != "subtract" && op != "multiply" {
		return nil, argErr("reduce op must be one of add, subtract, multiply")
	}
	var acc int64
	if op == "multiply" {
		acc = 1
	}
	if initial, ok, err := optIntArg(args, "initial"); err != nil {
		return nil, err
	} else if ok {
		acc = initial
	}
	for i, el := range arr {
		x, err := intValue(el, fmt.Sprintf("input[%d]", i))
		if err != nil {
			return nil, err
		}
		acc, err = applyBinary(op, acc, x)
		if err != nil {
			return nil, err
		}
	}
	return num(acc), nil
}

func normalizeIndex(i, n int64) int64 {
	if i < 0 {
		if i < -n {
			return 0
		}
		i += n
		if i < 0 {
			return 0
		}
		return i
	}
	if i > n {
		return n
	}
	return i
}

func opSlice(args map[string]Value) (Value, *OpError) {
	if err := allowedArgs(args, "input", "start", "end"); err != nil {
		return nil, err
	}
	arr, err := arrayArg(args, "input")
	if err != nil {
		return nil, err
	}
	start, err := intArg(args, "start")
	if err != nil {
		return nil, err
	}
	n := int64(len(arr))
	end := n
	if endVal, ok := args["end"]; ok {
		e, err := intValue(endVal, "argument \"end\"")
		if err != nil {
			return nil, err
		}
		end = e
	}
	start = normalizeIndex(start, n)
	end = normalizeIndex(end, n)
	out := make([]Value, 0)
	if start >= end {
		return out, nil
	}
	out = append(out, arr[start:end]...)
	return out, nil
}

// ---------------------------------------------------------------------------
// map.transform / filter.predicate
// ---------------------------------------------------------------------------

func transformTarget(transform map[string]Value) (string, *OpError) {
	tv, ok := transform["type"]
	if !ok {
		return "", argErr("transform is missing required field \"type\"")
	}
	ts, ok := tv.(string)
	if !ok {
		return "", argErr("transform type must be a string")
	}
	return ts, nil
}

func transformIntField(transform map[string]Value) (int64, *OpError) {
	v, ok := transform["value"]
	if !ok {
		return 0, argErr("transform is missing required field \"value\"")
	}
	return intValue(v, "transform value")
}

func applyTransform(transform map[string]Value, elem Value) (Value, *OpError) {
	ts, err := transformTarget(transform)
	if err != nil {
		return nil, err
	}
	switch ts {
	case "constant":
		if err := allowedArgs(transform, "type", "value"); err != nil {
			return nil, err
		}
		v, ok := transform["value"]
		if !ok {
			return nil, argErr("transform is missing required field \"value\"")
		}
		return v, nil
	case "add", "subtract", "multiply", "divide", "mod":
		if err := allowedArgs(transform, "type", "value"); err != nil {
			return nil, err
		}
		val, err := transformIntField(transform)
		if err != nil {
			return nil, err
		}
		x, err := intValue(elem, "element")
		if err != nil {
			return nil, err
		}
		r, err := applyBinary(ts, x, val)
		if err != nil {
			return nil, err
		}
		return num(r), nil
	case "negate":
		if err := allowedArgs(transform, "type"); err != nil {
			return nil, err
		}
		x, err := intValue(elem, "element")
		if err != nil {
			return nil, err
		}
		r, err := checkedNeg(x)
		if err != nil {
			return nil, err
		}
		return num(r), nil
	case "abs":
		if err := allowedArgs(transform, "type"); err != nil {
			return nil, err
		}
		x, err := intValue(elem, "element")
		if err != nil {
			return nil, err
		}
		r, err := checkedAbs(x)
		if err != nil {
			return nil, err
		}
		return num(r), nil
	case "stringify":
		if err := allowedArgs(transform, "type"); err != nil {
			return nil, err
		}
		s, cerr := compactJSON(elem)
		if cerr != nil {
			return nil, newOpError("INVALID_ARG", "cannot stringify element")
		}
		return s, nil
	case "type":
		if err := allowedArgs(transform, "type"); err != nil {
			return nil, err
		}
		return typeName(elem), nil
	case "op":
		if err := allowedArgs(transform, "type", "op", "args"); err != nil {
			return nil, err
		}
		op, err := stringArg(transform, "op")
		if err != nil {
			return nil, err
		}
		switch op {
		case "add", "subtract", "multiply", "divide", "mod":
		default:
			return nil, argErr("transform op must be one of add, subtract, multiply, divide, mod")
		}
		av, ok := transform["args"]
		if !ok {
			return nil, argErr("transform op form requires \"args\"")
		}
		argMap, ok := av.(map[string]Value)
		if !ok {
			return nil, argErr("transform args must be an object")
		}
		replaced, ok := replaceSelf(argMap, elem).(map[string]Value)
		if !ok {
			return nil, argErr("transform args must be an object")
		}
		if err := allowedArgs(replaced, "a", "b"); err != nil {
			return nil, err
		}
		a, err := intArg(replaced, "a")
		if err != nil {
			return nil, err
		}
		b, err := intArg(replaced, "b")
		if err != nil {
			return nil, err
		}
		r, err := applyBinary(op, a, b)
		if err != nil {
			return nil, err
		}
		return num(r), nil
	}
	return nil, argErr("unknown transform type %q", ts)
}

// replaceSelf replaces {"$self": true} markers with the element.
func replaceSelf(v Value, elem Value) Value {
	switch x := v.(type) {
	case map[string]Value:
		if len(x) == 1 {
			if marker, ok := x["$self"]; ok {
				if b, ok := marker.(bool); ok && b {
					return elem
				}
			}
		}
		out := make(map[string]Value, len(x))
		for k, val := range x {
			out[k] = replaceSelf(val, elem)
		}
		return out
	case []Value:
		out := make([]Value, len(x))
		for i, val := range x {
			out[i] = replaceSelf(val, elem)
		}
		return out
	}
	return v
}

func applyPredicate(predicate map[string]Value, elem Value) (bool, *OpError) {
	ts, err := transformTarget(predicate)
	if err != nil {
		return false, err
	}
	switch ts {
	case "even", "odd":
		if err := allowedArgs(predicate, "type"); err != nil {
			return false, err
		}
		x, err := intValue(elem, "element")
		if err != nil {
			return false, err
		}
		even := x%2 == 0
		if ts == "even" {
			return even, nil
		}
		return !even, nil
	case "gt", "gte", "lt", "lte", "eq", "ne":
		if err := allowedArgs(predicate, "type", "value"); err != nil {
			return false, err
		}
		v, ok := predicate["value"]
		if !ok {
			return false, argErr("predicate is missing required field \"value\"")
		}
		val, err := intValue(v, "predicate value")
		if err != nil {
			return false, err
		}
		cmp := compareValues(elem, num(val))
		switch ts {
		case "gt":
			return cmp > 0, nil
		case "gte":
			return cmp >= 0, nil
		case "lt":
			return cmp < 0, nil
		case "lte":
			return cmp <= 0, nil
		case "eq":
			return cmp == 0, nil
		default:
			return cmp != 0, nil
		}
	case "truthy":
		if err := allowedArgs(predicate, "type"); err != nil {
			return false, err
		}
		return truthy(elem), nil
	case "falsy":
		if err := allowedArgs(predicate, "type"); err != nil {
			return false, err
		}
		return !truthy(elem), nil
	case "type_is":
		if err := allowedArgs(predicate, "type", "value", "target_type", "type_name", "target", "type_arg", "expected", "typeName", "is", "of"); err != nil {
			return false, err
		}
		var target string
		found := false
		for _, k := range []string{"value", "target_type", "type_name", "target", "type_arg", "expected", "typeName", "is", "of"} {
			if pv, ok := predicate[k]; ok {
				target, err = stringValue(pv, fmt.Sprintf("predicate %q", k))
				if err != nil {
					return false, err
				}
				found = true
				break
			}
		}
		if !found {
			return false, argErr("type_is predicate requires a target type")
		}
		return typeName(elem) == target, nil
	case "string_contains":
		if err := allowedArgs(predicate, "type", "value"); err != nil {
			return false, err
		}
		v, ok := predicate["value"]
		if !ok {
			return false, argErr("predicate is missing required field \"value\"")
		}
		needle, err := stringValue(v, "predicate value")
		if err != nil {
			return false, err
		}
		haystack, ok := elem.(string)
		if !ok {
			return false, argErr("string_contains predicate requires string elements")
		}
		return strings.Contains(haystack, needle), nil
	}
	return false, argErr("unknown predicate type %q", ts)
}

// ---------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------

func opPick(args map[string]Value, keep bool) (Value, *OpError) {
	if err := allowedArgs(args, "input", "keys"); err != nil {
		return nil, err
	}
	obj, err := objectArg(args, "input")
	if err != nil {
		return nil, err
	}
	keysArr, err := arrayArg(args, "keys")
	if err != nil {
		return nil, err
	}
	selected := make(map[string]bool, len(keysArr))
	for i, el := range keysArr {
		k, err := stringValue(el, fmt.Sprintf("keys[%d]", i))
		if err != nil {
			return nil, err
		}
		selected[k] = true
	}
	out := make(map[string]Value)
	if keep {
		for _, k := range sortedKeys(obj) {
			if selected[k] {
				out[k] = obj[k]
			}
		}
		for k := range selected {
			if _, ok := out[k]; !ok {
				out[k] = nil
			}
		}
	} else {
		for k, v := range obj {
			if !selected[k] {
				out[k] = v
			}
		}
	}
	return out, nil
}

// ---------------------------------------------------------------------------
// fail_until
// ---------------------------------------------------------------------------

func (e *Engine) opFailUntil(st *Step, args map[string]Value) (Value, *OpError) {
	if err := allowedArgs(args, "failures", "value"); err != nil {
		return nil, err
	}
	failures, err := intArg(args, "failures")
	if err != nil {
		return nil, err
	}
	if failures < 0 || failures > 10 {
		return nil, argErr("failures must be between 0 and 10")
	}
	v, ok := args["value"]
	if !ok {
		return nil, argErr("missing required argument \"value\"")
	}
	e.failCounts[st.ID]++
	if int64(e.failCounts[st.ID]) <= failures {
		return nil, newOpError("TRANSIENT_FAILURE", "transient failure on attempt %d", e.failCounts[st.ID])
	}
	return v, nil
}
