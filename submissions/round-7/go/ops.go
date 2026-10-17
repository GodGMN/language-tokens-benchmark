package main

import (
	"encoding/json"
	"fmt"
	"math"
	"math/big"
	"sort"
	"strings"
	"unicode/utf8"
)

func sprintf(format string, a ...interface{}) string { return fmt.Sprintf(format, a...) }

// ---------- argument helpers ----------

func checkArgs(args map[string]Value, allowed ...string) *wfError {
	set := make(map[string]bool, len(allowed))
	for _, a := range allowed {
		set[a] = true
	}
	for k := range args {
		if !set[k] {
			return errf(ErrInvalidArg, "unknown argument %q", k)
		}
	}
	return nil
}

func reqArg(args map[string]Value, name string) (Value, *wfError) {
	v, ok := args[name]
	if !ok {
		return nil, errf(ErrInvalidArg, "missing required argument %q", name)
	}
	return v, nil
}

func argI64(args map[string]Value, name string) (int64, *wfError) {
	v, err := reqArg(args, name)
	if err != nil {
		return 0, err
	}
	n, ok := v.(json.Number)
	if !ok {
		return 0, errf(ErrInvalidArg, "argument %q must be a number", name)
	}
	i, ok := numToI64(n)
	if !ok {
		return 0, errf(ErrInvalidArg, "argument %q must be an integer literal fitting in i64", name)
	}
	return i, nil
}

func argStr(args map[string]Value, name string) (string, *wfError) {
	v, err := reqArg(args, name)
	if err != nil {
		return "", err
	}
	s, ok := v.(string)
	if !ok {
		return "", errf(ErrInvalidArg, "argument %q must be a string", name)
	}
	return s, nil
}

func argArr(args map[string]Value, name string) ([]interface{}, *wfError) {
	v, err := reqArg(args, name)
	if err != nil {
		return nil, err
	}
	a, ok := v.([]interface{})
	if !ok {
		return nil, errf(ErrInvalidArg, "argument %q must be an array", name)
	}
	return a, nil
}

func argObj(args map[string]Value, name string) (map[string]interface{}, *wfError) {
	v, err := reqArg(args, name)
	if err != nil {
		return nil, err
	}
	o, ok := v.(map[string]interface{})
	if !ok {
		return nil, errf(ErrInvalidArg, "argument %q must be an object", name)
	}
	return o, nil
}

func argBool(args map[string]Value, name string) (bool, *wfError) {
	v, err := reqArg(args, name)
	if err != nil {
		return false, err
	}
	b, ok := v.(bool)
	if !ok {
		return false, errf(ErrInvalidArg, "argument %q must be a boolean", name)
	}
	return b, nil
}

// argI64s requires an array whose elements are all i64 integer literals.
func argI64s(args map[string]Value, name string) ([]int64, *wfError) {
	arr, err := argArr(args, name)
	if err != nil {
		return nil, err
	}
	out := make([]int64, len(arr))
	for i, e := range arr {
		n, ok := e.(json.Number)
		if !ok {
			return nil, errf(ErrInvalidArg, "argument %q element %d must be a number", name, i)
		}
		v, ok := numToI64(n)
		if !ok {
			return nil, errf(ErrInvalidArg, "argument %q element %d must be an i64 integer literal", name, i)
		}
		out[i] = v
	}
	return out, nil
}

func argStrings(args map[string]Value, name string) ([]string, *wfError) {
	arr, err := argArr(args, name)
	if err != nil {
		return nil, err
	}
	out := make([]string, len(arr))
	for i, e := range arr {
		s, ok := e.(string)
		if !ok {
			return nil, errf(ErrInvalidArg, "argument %q element %d must be a string", name, i)
		}
		out[i] = s
	}
	return out, nil
}

// ---------- i64 arithmetic ----------

var minI64 = big.NewInt(math.MinInt64)
var maxI64 = big.NewInt(math.MaxInt64)

func bigToI64(b *big.Int) (int64, bool) {
	if !b.IsInt64() {
		return 0, false
	}
	return b.Int64(), true
}

func safeAdd(a, b int64) (int64, bool) {
	r := new(big.Int).Add(big.NewInt(a), big.NewInt(b))
	return bigToI64(r)
}

func safeSub(a, b int64) (int64, bool) {
	r := new(big.Int).Sub(big.NewInt(a), big.NewInt(b))
	return bigToI64(r)
}

func safeMul(a, b int64) (int64, bool) {
	r := new(big.Int).Mul(big.NewInt(a), big.NewInt(b))
	return bigToI64(r)
}

var overflowErr = func() *wfError { return errf(ErrArithmeticOverflow, "i64 overflow") }

// binaryNum applies a math op to two i64 operands.
func binaryNum(op string, a, b int64) (json.Number, *wfError) {
	switch op {
	case "add":
		r, ok := safeAdd(a, b)
		if !ok {
			return "", overflowErr()
		}
		return num(r), nil
	case "subtract":
		r, ok := safeSub(a, b)
		if !ok {
			return "", overflowErr()
		}
		return num(r), nil
	case "multiply":
		r, ok := safeMul(a, b)
		if !ok {
			return "", overflowErr()
		}
		return num(r), nil
	case "divide":
		if b == 0 {
			return "", errf(ErrDivisionByZero, "division by zero")
		}
		if a == math.MinInt64 && b == -1 {
			return "", overflowErr()
		}
		return num(a / b), nil
	case "mod":
		if b == 0 {
			return "", errf(ErrDivisionByZero, "modulo by zero")
		}
		if a == math.MinInt64 && b == -1 {
			return "", overflowErr()
		}
		return num(a % b), nil
	}
	return "", errf(ErrInvalidArg, "unknown math op %q", op)
}

func isMath2(op string) bool {
	switch op {
	case "add", "subtract", "multiply", "divide", "mod":
		return true
	}
	return false
}

func math2(op string) func(map[string]Value, *execCtx) (Value, *wfError) {
	return func(args map[string]Value, ctx *execCtx) (Value, *wfError) {
		if err := checkArgs(args, "a", "b"); err != nil {
			return nil, err
		}
		a, err := argI64(args, "a")
		if err != nil {
			return nil, err
		}
		b, err := argI64(args, "b")
		if err != nil {
			return nil, err
		}
		return binaryNum(op, a, b)
	}
}

// ---------- transforms (map) ----------

func applyTransform(elem Value, t Value) (Value, *wfError) {
	obj, ok := t.(map[string]interface{})
	if !ok {
		return nil, errf(ErrInvalidArg, "transform must be an object")
	}
	tv, ok := obj["type"]
	if !ok {
		return nil, errf(ErrInvalidArg, "transform requires a type")
	}
	tname, ok := tv.(string)
	if !ok {
		return nil, errf(ErrInvalidArg, "transform type must be a string")
	}
	switch tname {
	case "constant":
		v, ok := obj["value"]
		if !ok {
			return nil, errf(ErrInvalidArg, "constant transform requires value")
		}
		return v, nil
	case "add", "subtract", "multiply", "divide", "mod":
		n, ok := elem.(json.Number)
		if !ok {
			return nil, errf(ErrInvalidArg, "transform %q requires a numeric element", tname)
		}
		e, ok := numToI64(n)
		if !ok {
			return nil, errf(ErrInvalidArg, "element must be an i64 integer literal")
		}
		vv, ok := obj["value"]
		if !ok {
			return nil, errf(ErrInvalidArg, "transform %q requires value", tname)
		}
		vn, ok := vv.(json.Number)
		if !ok {
			return nil, errf(ErrInvalidArg, "transform value must be a number")
		}
		operand, ok := numToI64(vn)
		if !ok {
			return nil, errf(ErrInvalidArg, "transform value must be an i64 integer literal")
		}
		return binaryNum(tname, e, operand)
	case "negate":
		e, err := elemI64(elem, "negate")
		if err != nil {
			return nil, err
		}
		if e == math.MinInt64 {
			return nil, overflowErr()
		}
		return num(-e), nil
	case "abs":
		e, err := elemI64(elem, "abs")
		if err != nil {
			return nil, err
		}
		if e == math.MinInt64 {
			return nil, overflowErr()
		}
		if e < 0 {
			e = -e
		}
		return num(e), nil
	case "stringify":
		s, err := compactJSON(elem)
		if err != nil {
			return nil, errf(ErrInvalidArg, "cannot stringify element")
		}
		return s, nil
	case "type":
		return typeName(elem), nil
	case "op":
		opv, ok := obj["op"]
		if !ok {
			return nil, errf(ErrInvalidArg, "op transform requires op")
		}
		op, ok := opv.(string)
		if !ok || !isMath2(op) {
			return nil, errf(ErrInvalidArg, "op transform requires a known math op")
		}
		av, ok := obj["args"]
		if !ok {
			return nil, errf(ErrInvalidArg, "op transform requires args")
		}
		am, ok := av.(map[string]interface{})
		if !ok {
			return nil, errf(ErrInvalidArg, "op transform args must be an object")
		}
		if err := checkArgs(am, "a", "b"); err != nil {
			return nil, err
		}
		getOperand := func(name string) (int64, *wfError) {
			raw, ok := am[name]
			if !ok {
				return 0, errf(ErrInvalidArg, "missing required argument %q", name)
			}
			if isSelfMarker(raw) {
				return elemI64(elem, "op")
			}
			n, ok := raw.(json.Number)
			if !ok {
				return 0, errf(ErrInvalidArg, "argument %q must be a number or {\"$self\": true}", name)
			}
			i, ok := numToI64(n)
			if !ok {
				return 0, errf(ErrInvalidArg, "argument %q must be an i64 integer literal", name)
			}
			return i, nil
		}
		a, err := getOperand("a")
		if err != nil {
			return nil, err
		}
		b, err := getOperand("b")
		if err != nil {
			return nil, err
		}
		return binaryNum(op, a, b)
	}
	return nil, errf(ErrInvalidArg, "unknown transform type %q", tname)
}

func elemI64(elem Value, opName string) (int64, *wfError) {
	n, ok := elem.(json.Number)
	if !ok {
		return 0, errf(ErrInvalidArg, "transform %q requires a numeric element", opName)
	}
	i, ok := numToI64(n)
	if !ok {
		return 0, errf(ErrInvalidArg, "element must be an i64 integer literal")
	}
	return i, nil
}

func isTypeName(s string) bool {
	switch s {
	case "null", "boolean", "number", "string", "array", "object":
		return true
	}
	return false
}

func isSelfMarker(v Value) bool {
	obj, ok := v.(map[string]interface{})
	if !ok || len(obj) != 1 {
		return false
	}
	sv, ok := obj["$self"]
	if !ok {
		return false
	}
	b, ok := sv.(bool)
	return ok && b
}

// ---------- predicates (filter) ----------

func applyPredicate(elem Value, p Value) (bool, *wfError) {
	obj, ok := p.(map[string]interface{})
	if !ok {
		return false, errf(ErrInvalidArg, "predicate must be an object")
	}
	tv, ok := obj["type"]
	if !ok {
		return false, errf(ErrInvalidArg, "predicate requires a type")
	}
	tname, ok := tv.(string)
	if !ok {
		return false, errf(ErrInvalidArg, "predicate type must be a string")
	}
	// Defensive: a predicate written as {"type": "type_is", "type": "string"}
	// would collapse to {"type": "string"} when parsed, so treat a lone
	// type-name discriminator as a type_is predicate.
	if isTypeName(tname) && len(obj) == 1 {
		return typeName(elem) == tname, nil
	}
	switch tname {
	case "even", "odd":
		e, err := elemI64(elem, tname)
		if err != nil {
			return false, err
		}
		isEven := e%2 == 0
		if tname == "even" {
			return isEven, nil
		}
		return !isEven, nil
	case "gt", "gte", "lt", "lte":
		e, err := elemI64(elem, tname)
		if err != nil {
			return false, err
		}
		vv, ok := obj["value"]
		if !ok {
			return false, errf(ErrInvalidArg, "predicate %q requires value", tname)
		}
		vn, ok := vv.(json.Number)
		if !ok {
			return false, errf(ErrInvalidArg, "predicate value must be a number")
		}
		v, ok := numToI64(vn)
		if !ok {
			return false, errf(ErrInvalidArg, "predicate value must be an i64 integer literal")
		}
		switch tname {
		case "gt":
			return e > v, nil
		case "gte":
			return e >= v, nil
		case "lt":
			return e < v, nil
		default:
			return e <= v, nil
		}
	case "eq", "ne":
		vv, ok := obj["value"]
		if !ok {
			return false, errf(ErrInvalidArg, "predicate %q requires value", tname)
		}
		eq := structEqual(elem, vv)
		if tname == "eq" {
			return eq, nil
		}
		return !eq, nil
	case "truthy":
		return truthy(elem), nil
	case "falsy":
		return !truthy(elem), nil
	case "type_is":
		// The spec writes this predicate as {"type": "type_is", "type": "<type>"}
		// which cannot be represented in JSON, so the target type name may be
		// supplied under any of the obvious companion keys.
		target := ""
		for _, k := range []string{"value", "type_name", "name", "target"} {
			if s, ok := obj[k].(string); ok {
				target = s
				break
			}
		}
		if target == "" {
			// Fall back to any other string-valued key besides the
			// discriminator itself (sorted for determinism).
			for _, k := range sortedKeys(obj) {
				if k == "type" {
					continue
				}
				if s, ok := obj[k].(string); ok {
					target = s
					break
				}
			}
		}
		if target == "" {
			return false, errf(ErrInvalidArg, "type_is predicate requires a type name")
		}
		return typeName(elem) == target, nil
	case "string_contains":
		s, ok := elem.(string)
		if !ok {
			return false, errf(ErrInvalidArg, "string_contains requires a string element")
		}
		vv, ok := obj["value"]
		if !ok {
			return false, errf(ErrInvalidArg, "string_contains requires value")
		}
		sub, ok := vv.(string)
		if !ok {
			return false, errf(ErrInvalidArg, "string_contains value must be a string")
		}
		return strings.Contains(s, sub), nil
	}
	return false, errf(ErrInvalidArg, "unknown predicate type %q", tname)
}

// ---------- operations ----------

type opFn func(map[string]Value, *execCtx) (Value, *wfError)

var opTable = map[string]opFn{
	"add":        math2("add"),
	"subtract":   math2("subtract"),
	"multiply":   math2("multiply"),
	"divide":     math2("divide"),
	"mod":        math2("mod"),
	"sum":        opSum,
	"min":        opMin,
	"max":        opMax,
	"range":      opRange,
	"concat":     opConcat,
	"split":      opSplit,
	"join":       opJoin,
	"uppercase":  opUppercase,
	"lowercase":  opLowercase,
	"trim":       opTrim,
	"replace":    opReplace,
	"length":     opLength,
	"map":        opMap,
	"filter":     opFilter,
	"reduce":     opReduce,
	"sort":       opSort,
	"reverse":    opReverse,
	"unique":     opUnique,
	"slice":      opSlice,
	"flatten":    opFlatten,
	"get":        opGet,
	"set":        opSet,
	"keys":       opKeys,
	"values":     opValues,
	"merge":      opMerge,
	"pick":       opPick,
	"omit":       opOmit,
	"constant":   opConstant,
	"equals":     opEquals,
	"compare":    opCompare,
	"and":        opAnd,
	"or":         opOr,
	"not":        opNot,
	"type":       opType,
	"fail_until": opFailUntil,
}

var opArgs = map[string][]string{
	"add":        {"a", "b"},
	"subtract":   {"a", "b"},
	"multiply":   {"a", "b"},
	"divide":     {"a", "b"},
	"mod":        {"a", "b"},
	"sum":        {"input"},
	"min":        {"input"},
	"max":        {"input"},
	"range":      {"start", "end"},
	"concat":     {"a", "b"},
	"split":      {"input", "sep"},
	"join":       {"input", "sep"},
	"uppercase":  {"input"},
	"lowercase":  {"input"},
	"trim":       {"input"},
	"replace":    {"input", "search", "replace"},
	"length":     {"input"},
	"map":        {"input", "transform"},
	"filter":     {"input", "predicate"},
	"reduce":     {"input", "op", "initial"},
	"sort":       {"input"},
	"reverse":    {"input"},
	"unique":     {"input"},
	"slice":      {"input", "start", "end"},
	"flatten":    {"input"},
	"get":        {"input", "key"},
	"set":        {"input", "key", "value"},
	"keys":       {"input"},
	"values":     {"input"},
	"merge":      {"a", "b"},
	"pick":       {"input", "keys"},
	"omit":       {"input", "keys"},
	"constant":   {"value"},
	"equals":     {"a", "b"},
	"compare":    {"a", "b", "op"},
	"and":        {"a", "b"},
	"or":         {"a", "b"},
	"not":        {"a"},
	"type":       {"value"},
	"fail_until": {"failures", "value"},
}

func applyOp(name string, args map[string]Value, ctx *execCtx) (Value, *wfError) {
	fn, ok := opTable[name]
	if !ok {
		return nil, errf(ErrInvalidWorkflow, "unknown op %q", name)
	}
	if err := checkArgs(args, opArgs[name]...); err != nil {
		return nil, err
	}
	return fn(args, ctx)
}

// --- math ---

func opSum(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	xs, err := argI64s(args, "input")
	if err != nil {
		return nil, err
	}
	acc := new(big.Int)
	for _, x := range xs {
		acc.Add(acc, big.NewInt(x))
	}
	v, ok := bigToI64(acc)
	if !ok {
		return nil, overflowErr()
	}
	return num(v), nil
}

func opMin(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	xs, err := argI64s(args, "input")
	if err != nil {
		return nil, err
	}
	if len(xs) == 0 {
		return nil, errf(ErrEmptyArray, "min of empty array")
	}
	m := xs[0]
	for _, x := range xs[1:] {
		if x < m {
			m = x
		}
	}
	return num(m), nil
}

func opMax(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	xs, err := argI64s(args, "input")
	if err != nil {
		return nil, err
	}
	if len(xs) == 0 {
		return nil, errf(ErrEmptyArray, "max of empty array")
	}
	m := xs[0]
	for _, x := range xs[1:] {
		if x > m {
			m = x
		}
	}
	return num(m), nil
}

func opRange(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	start, err := argI64(args, "start")
	if err != nil {
		return nil, err
	}
	end, err := argI64(args, "end")
	if err != nil {
		return nil, err
	}
	out := []interface{}{}
	if start > end {
		return out, nil
	}
	// Defensive guard: a range spanning billions of elements cannot be
	// materialised; fail cleanly instead of exhausting memory.
	if uint64(end)-uint64(start) > 5_000_000 {
		return nil, errf(ErrInvalidArg, "range too large")
	}
	for i := start; ; i++ {
		out = append(out, num(i))
		if i == end {
			break
		}
	}
	return out, nil
}

// --- strings ---

func opConcat(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	a, err := argStr(args, "a")
	if err != nil {
		return nil, err
	}
	b, err := argStr(args, "b")
	if err != nil {
		return nil, err
	}
	return a + b, nil
}

func opSplit(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	in, err := argStr(args, "input")
	if err != nil {
		return nil, err
	}
	sep, err := argStr(args, "sep")
	if err != nil {
		return nil, err
	}
	out := []interface{}{}
	if sep == "" {
		for _, r := range in {
			out = append(out, string(r))
		}
		return out, nil
	}
	for _, part := range strings.Split(in, sep) {
		out = append(out, part)
	}
	return out, nil
}

func opJoin(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	parts, err := argStrings(args, "input")
	if err != nil {
		return nil, err
	}
	sep, err := argStr(args, "sep")
	if err != nil {
		return nil, err
	}
	return strings.Join(parts, sep), nil
}

func opUppercase(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	in, err := argStr(args, "input")
	if err != nil {
		return nil, err
	}
	var b strings.Builder
	b.Grow(len(in))
	for _, r := range in {
		if r >= 'a' && r <= 'z' {
			r = r - 'a' + 'A'
		}
		b.WriteRune(r)
	}
	return b.String(), nil
}

func opLowercase(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	in, err := argStr(args, "input")
	if err != nil {
		return nil, err
	}
	var b strings.Builder
	b.Grow(len(in))
	for _, r := range in {
		if r >= 'A' && r <= 'Z' {
			r = r - 'A' + 'a'
		}
		b.WriteRune(r)
	}
	return b.String(), nil
}

func opTrim(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	in, err := argStr(args, "input")
	if err != nil {
		return nil, err
	}
	return strings.Trim(in, " \t\n\r\f\v"), nil
}

func opReplace(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	in, err := argStr(args, "input")
	if err != nil {
		return nil, err
	}
	search, err := argStr(args, "search")
	if err != nil {
		return nil, err
	}
	rep, err := argStr(args, "replace")
	if err != nil {
		return nil, err
	}
	if search == "" {
		return nil, errf(ErrInvalidArg, "replace search must not be empty")
	}
	return strings.ReplaceAll(in, search, rep), nil
}

func opLength(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	in, err := argStr(args, "input")
	if err != nil {
		return nil, err
	}
	return num(int64(utf8.RuneCountInString(in))), nil
}

// --- arrays ---

func opMap(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	in, err := argArr(args, "input")
	if err != nil {
		return nil, err
	}
	tv, err := reqArg(args, "transform")
	if err != nil {
		return nil, err
	}
	out := make([]interface{}, len(in))
	for i, e := range in {
		v, err := applyTransform(e, tv)
		if err != nil {
			return nil, err
		}
		out[i] = v
	}
	return out, nil
}

func opFilter(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	in, err := argArr(args, "input")
	if err != nil {
		return nil, err
	}
	pv, err := reqArg(args, "predicate")
	if err != nil {
		return nil, err
	}
	out := []interface{}{}
	for _, e := range in {
		ok, err := applyPredicate(e, pv)
		if err != nil {
			return nil, err
		}
		if ok {
			out = append(out, e)
		}
	}
	return out, nil
}

func opReduce(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	xs, err := argI64s(args, "input")
	if err != nil {
		return nil, err
	}
	op, err := argStr(args, "op")
	if err != nil {
		return nil, err
	}
	if op != "add" && op != "subtract" && op != "multiply" {
		return nil, errf(ErrInvalidArg, "reduce op must be add, subtract or multiply")
	}
	var acc int64
	if _, present := args["initial"]; present {
		acc, err = argI64(args, "initial")
		if err != nil {
			return nil, err
		}
	} else {
		if op == "multiply" {
			acc = 1
		} else {
			acc = 0
		}
	}
	for _, x := range xs {
		var e *wfError
		var r json.Number
		r, e = binaryNum(op, acc, x)
		if e != nil {
			return nil, e
		}
		acc, _ = numToI64(r)
	}
	return num(acc), nil
}

func opSort(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	in, err := argArr(args, "input")
	if err != nil {
		return nil, err
	}
	out := make([]interface{}, len(in))
	copy(out, in)
	sort.SliceStable(out, func(i, j int) bool {
		return compareValues(out[i], out[j]) < 0
	})
	return out, nil
}

func opReverse(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	in, err := argArr(args, "input")
	if err != nil {
		return nil, err
	}
	out := make([]interface{}, len(in))
	for i := range in {
		out[len(in)-1-i] = in[i]
	}
	return out, nil
}

func opUnique(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	in, err := argArr(args, "input")
	if err != nil {
		return nil, err
	}
	out := []interface{}{}
	for _, e := range in {
		dup := false
		for _, kept := range out {
			if structEqual(kept, e) {
				dup = true
				break
			}
		}
		if !dup {
			out = append(out, e)
		}
	}
	return out, nil
}

func clampIndex(i int64, n int) int {
	var idx int
	if i < 0 {
		// negative: count from end
		if i < -int64(n) {
			return 0
		}
		idx = n + int(i)
	} else {
		if i > int64(n) {
			return n
		}
		idx = int(i)
	}
	if idx < 0 {
		return 0
	}
	if idx > n {
		return n
	}
	return idx
}

func opSlice(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	in, err := argArr(args, "input")
	if err != nil {
		return nil, err
	}
	start, err := argI64(args, "start")
	if err != nil {
		return nil, err
	}
	end := int64(len(in))
	if _, present := args["end"]; present {
		end, err = argI64(args, "end")
		if err != nil {
			return nil, err
		}
	}
	n := len(in)
	s := clampIndex(start, n)
	e := clampIndex(end, n)
	out := []interface{}{}
	if s >= e {
		return out, nil
	}
	for i := s; i < e; i++ {
		out = append(out, in[i])
	}
	return out, nil
}

func opFlatten(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	in, err := argArr(args, "input")
	if err != nil {
		return nil, err
	}
	out := []interface{}{}
	for _, e := range in {
		if sub, ok := e.([]interface{}); ok {
			out = append(out, sub...)
		} else {
			out = append(out, e)
		}
	}
	return out, nil
}

// --- objects ---

func opGet(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	obj, err := argObj(args, "input")
	if err != nil {
		return nil, err
	}
	key, err := argStr(args, "key")
	if err != nil {
		return nil, err
	}
	if v, ok := obj[key]; ok {
		return v, nil
	}
	return nil, nil
}

func opSet(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	obj, err := argObj(args, "input")
	if err != nil {
		return nil, err
	}
	key, err := argStr(args, "key")
	if err != nil {
		return nil, err
	}
	val, err2 := reqArg(args, "value")
	if err2 != nil {
		return nil, err2
	}
	out := make(map[string]interface{}, len(obj)+1)
	for k, v := range obj {
		out[k] = v
	}
	out[key] = val
	return out, nil
}

func opKeys(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	obj, err := argObj(args, "input")
	if err != nil {
		return nil, err
	}
	out := []interface{}{}
	for _, k := range sortedKeys(obj) {
		out = append(out, k)
	}
	return out, nil
}

func opValues(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	obj, err := argObj(args, "input")
	if err != nil {
		return nil, err
	}
	out := []interface{}{}
	for _, k := range sortedKeys(obj) {
		out = append(out, obj[k])
	}
	return out, nil
}

func opMerge(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	a, err := argObj(args, "a")
	if err != nil {
		return nil, err
	}
	b, err := argObj(args, "b")
	if err != nil {
		return nil, err
	}
	out := make(map[string]interface{}, len(a)+len(b))
	for k, v := range a {
		out[k] = v
	}
	for k, v := range b {
		out[k] = v
	}
	return out, nil
}

func opPick(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	obj, err := argObj(args, "input")
	if err != nil {
		return nil, err
	}
	keys, err := argStrings(args, "keys")
	if err != nil {
		return nil, err
	}
	out := make(map[string]interface{}, len(keys))
	for _, k := range keys {
		out[k] = obj[k]
	}
	return out, nil
}

func opOmit(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	obj, err := argObj(args, "input")
	if err != nil {
		return nil, err
	}
	keys, err := argStrings(args, "keys")
	if err != nil {
		return nil, err
	}
	drop := make(map[string]bool, len(keys))
	for _, k := range keys {
		drop[k] = true
	}
	out := make(map[string]interface{}, len(obj))
	for k, v := range obj {
		if !drop[k] {
			out[k] = v
		}
	}
	return out, nil
}

// --- utilities ---

func opConstant(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	v, err := reqArg(args, "value")
	if err != nil {
		return nil, err
	}
	return v, nil
}

func opEquals(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	a, err := reqArg(args, "a")
	if err != nil {
		return nil, err
	}
	b, err := reqArg(args, "b")
	if err != nil {
		return nil, err
	}
	return structEqual(a, b), nil
}

func opCompare(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	a, err := reqArg(args, "a")
	if err != nil {
		return nil, err
	}
	b, err := reqArg(args, "b")
	if err != nil {
		return nil, err
	}
	op, err2 := argStr(args, "op")
	if err2 != nil {
		return nil, err2
	}
	c := compareValues(a, b)
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
	return nil, errf(ErrInvalidArg, "compare op must be gt, gte, lt or lte")
}

func opAnd(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	a, err := argBool(args, "a")
	if err != nil {
		return nil, err
	}
	b, err := argBool(args, "b")
	if err != nil {
		return nil, err
	}
	return a && b, nil
}

func opOr(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	a, err := argBool(args, "a")
	if err != nil {
		return nil, err
	}
	b, err := argBool(args, "b")
	if err != nil {
		return nil, err
	}
	return a || b, nil
}

func opNot(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	a, err := argBool(args, "a")
	if err != nil {
		return nil, err
	}
	return !a, nil
}

func opType(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	v, err := reqArg(args, "value")
	if err != nil {
		return nil, err
	}
	return typeName(v), nil
}

func opFailUntil(args map[string]Value, ctx *execCtx) (Value, *wfError) {
	failures, err := argI64(args, "failures")
	if err != nil {
		return nil, err
	}
	if failures < 0 || failures > 10 {
		return nil, errf(ErrInvalidArg, "failures must be between 0 and 10")
	}
	val, err2 := reqArg(args, "value")
	if err2 != nil {
		return nil, err2
	}
	attempt := ctx.attempts[ctx.stepID] + 1
	ctx.attempts[ctx.stepID] = attempt
	if int64(attempt) <= failures {
		return nil, errf(ErrTransientFailure, "transient failure on attempt %d", attempt)
	}
	return val, nil
}
