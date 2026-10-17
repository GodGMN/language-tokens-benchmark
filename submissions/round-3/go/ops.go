package main

import (
	"fmt"
	"math"
	"sort"
	"strings"
	"unicode/utf8"
)

// ---------------------------------------------------------------------------
// Argument helpers
// ---------------------------------------------------------------------------

type argHelper struct {
	m    map[string]interface{}
	step string
}

func opErr(step, format string, a ...interface{}) *WErr {
	return newErr("INVALID_ARG", step, fmt.Sprintf(format, a...))
}

func makeArgs(step string, m map[string]interface{}, allowed ...string) (*argHelper, *WErr) {
	set := map[string]bool{}
	for _, k := range allowed {
		set[k] = true
	}
	for k := range m {
		if !set[k] {
			return nil, opErr(step, "unknown argument %q", k)
		}
	}
	return &argHelper{m: m, step: step}, nil
}

func (h *argHelper) get(name string) (interface{}, bool) {
	v, ok := h.m[name]
	return v, ok
}

func (h *argHelper) has(name string) bool {
	_, ok := h.m[name]
	return ok
}

func (h *argHelper) required(name string) (interface{}, *WErr) {
	v, ok := h.m[name]
	if !ok {
		return nil, opErr(h.step, "missing required argument %q", name)
	}
	return v, nil
}

func (h *argHelper) intVal(name string) (int64, *WErr) {
	v, ok := h.m[name]
	if !ok {
		return 0, opErr(h.step, "missing required argument %q", name)
	}
	n, ok := toIntLiteral(v)
	if !ok {
		return 0, opErr(h.step, "argument %q must be an i64 integer", name)
	}
	return n, nil
}

func (h *argHelper) optInt(name string) (int64, bool, *WErr) {
	v, ok := h.m[name]
	if !ok {
		return 0, false, nil
	}
	n, ok := toIntLiteral(v)
	if !ok {
		return 0, false, opErr(h.step, "argument %q must be an i64 integer", name)
	}
	return n, true, nil
}

func (h *argHelper) strVal(name string) (string, *WErr) {
	v, ok := h.m[name]
	if !ok {
		return "", opErr(h.step, "missing required argument %q", name)
	}
	s, ok := v.(string)
	if !ok {
		return "", opErr(h.step, "argument %q must be a string", name)
	}
	return s, nil
}

func (h *argHelper) boolVal(name string) (bool, *WErr) {
	v, ok := h.m[name]
	if !ok {
		return false, opErr(h.step, "missing required argument %q", name)
	}
	b, ok := v.(bool)
	if !ok {
		return false, opErr(h.step, "argument %q must be a boolean", name)
	}
	return b, nil
}

func (h *argHelper) arrVal(name string) ([]interface{}, *WErr) {
	v, ok := h.m[name]
	if !ok {
		return nil, opErr(h.step, "missing required argument %q", name)
	}
	a, ok := v.([]interface{})
	if !ok {
		return nil, opErr(h.step, "argument %q must be an array", name)
	}
	return a, nil
}

func (h *argHelper) objVal(name string) (map[string]interface{}, *WErr) {
	v, ok := h.m[name]
	if !ok {
		return nil, opErr(h.step, "missing required argument %q", name)
	}
	o, ok := v.(map[string]interface{})
	if !ok {
		return nil, opErr(h.step, "argument %q must be an object", name)
	}
	return o, nil
}

func (h *argHelper) strArray(name string) ([]string, *WErr) {
	arr, err := h.arrVal(name)
	if err != nil {
		return nil, err
	}
	out := make([]string, 0, len(arr))
	for _, e := range arr {
		s, ok := e.(string)
		if !ok {
			return nil, opErr(h.step, "argument %q must be an array of strings", name)
		}
		out = append(out, s)
	}
	return out, nil
}

// ---------------------------------------------------------------------------
// i64 arithmetic
// ---------------------------------------------------------------------------

func addInt(a, b int64) (int64, bool) {
	r := a + b
	if (b > 0 && r < a) || (b < 0 && r > a) {
		return 0, false
	}
	return r, true
}

func subInt(a, b int64) (int64, bool) {
	r := a - b
	if (b < 0 && r < a) || (b > 0 && r > a) {
		return 0, false
	}
	return r, true
}

func mulInt(a, b int64) (int64, bool) {
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

// arith applies one of the five binary i64 operations.
func arith(op string, a, b int64, step string) (int64, *WErr) {
	switch op {
	case "add":
		if r, ok := addInt(a, b); ok {
			return r, nil
		}
		return 0, newErr("ARITHMETIC_OVERFLOW", step, "addition overflow")
	case "subtract":
		if r, ok := subInt(a, b); ok {
			return r, nil
		}
		return 0, newErr("ARITHMETIC_OVERFLOW", step, "subtraction overflow")
	case "multiply":
		if r, ok := mulInt(a, b); ok {
			return r, nil
		}
		return 0, newErr("ARITHMETIC_OVERFLOW", step, "multiplication overflow")
	case "divide":
		if b == 0 {
			return 0, newErr("DIVISION_BY_ZERO", step, "division by zero")
		}
		if a == math.MinInt64 && b == -1 {
			return 0, newErr("ARITHMETIC_OVERFLOW", step, "division overflow")
		}
		return a / b, nil
	case "mod":
		if b == 0 {
			return 0, newErr("DIVISION_BY_ZERO", step, "modulo by zero")
		}
		if a == math.MinInt64 && b == -1 {
			return 0, newErr("ARITHMETIC_OVERFLOW", step, "modulo overflow")
		}
		return a % b, nil
	}
	return 0, opErr(step, "unknown arithmetic op %q", op)
}

// ---------------------------------------------------------------------------
// Operation dispatch
// ---------------------------------------------------------------------------

// runOp executes a step operation. attempt is the 1-based execution attempt
// counter for the step (only used by fail_until).
func runOp(op string, args map[string]interface{}, step string, attempt int) (interface{}, *WErr) {
	switch op {
	case "add", "subtract", "multiply", "divide", "mod":
		return opBinaryArith(op, args, step)
	case "sum":
		return opSum(args, step)
	case "min":
		return opMinMax(args, step, true)
	case "max":
		return opMinMax(args, step, false)
	case "range":
		return opRange(args, step)
	// strings
	case "concat":
		return opConcat(args, step)
	case "split":
		return opSplit(args, step)
	case "join":
		return opJoin(args, step)
	case "uppercase":
		return opUpperLower(args, step, true)
	case "lowercase":
		return opUpperLower(args, step, false)
	case "trim":
		return opTrim(args, step)
	case "replace":
		return opReplace(args, step)
	case "length":
		return opLength(args, step)
	// arrays
	case "map":
		return opMap(args, step)
	case "filter":
		return opFilter(args, step)
	case "reduce":
		return opReduce(args, step)
	case "sort":
		return opSort(args, step)
	case "reverse":
		return opReverse(args, step)
	case "unique":
		return opUnique(args, step)
	case "slice":
		return opSlice(args, step)
	case "flatten":
		return opFlatten(args, step)
	// objects
	case "get":
		return opGet(args, step)
	case "set":
		return opSet(args, step)
	case "keys":
		return opKeys(args, step)
	case "values":
		return opValues(args, step)
	case "merge":
		return opMerge(args, step)
	case "pick":
		return opPick(args, step)
	case "omit":
		return opOmit(args, step)
	// utilities
	case "constant":
		return opConstant(args, step)
	case "equals":
		return opEquals(args, step)
	case "compare":
		return opCompare(args, step)
	case "and":
		return opAnd(args, step)
	case "or":
		return opOr(args, step)
	case "not":
		return opNot(args, step)
	case "type":
		return opType(args, step)
	case "fail_until":
		return opFailUntil(args, step, attempt)
	}
	return nil, opErr(step, "unknown operation %q", op)
}

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------

func opBinaryArith(op string, args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "a", "b")
	if err != nil {
		return nil, err
	}
	a, err := h.intVal("a")
	if err != nil {
		return nil, err
	}
	b, err := h.intVal("b")
	if err != nil {
		return nil, err
	}
	r, err := arith(op, a, b, step)
	if err != nil {
		return nil, err
	}
	return r, nil
}

func opSum(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input")
	if err != nil {
		return nil, err
	}
	arr, err := h.arrVal("input")
	if err != nil {
		return nil, err
	}
	var total int64
	for _, e := range arr {
		n, ok := toIntLiteral(e)
		if !ok {
			return nil, opErr(step, "sum input must contain i64 integers")
		}
		r, ok := addInt(total, n)
		if !ok {
			return nil, newErr("ARITHMETIC_OVERFLOW", step, "sum overflow")
		}
		total = r
	}
	return total, nil
}

func opMinMax(args map[string]interface{}, step string, isMin bool) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input")
	if err != nil {
		return nil, err
	}
	arr, err := h.arrVal("input")
	if err != nil {
		return nil, err
	}
	if len(arr) == 0 {
		return nil, newErr("EMPTY_ARRAY", step, "min/max of empty array")
	}
	best, ok := toIntLiteral(arr[0])
	if !ok {
		return nil, opErr(step, "min/max input must contain i64 integers")
	}
	for _, e := range arr[1:] {
		n, ok := toIntLiteral(e)
		if !ok {
			return nil, opErr(step, "min/max input must contain i64 integers")
		}
		if (isMin && n < best) || (!isMin && n > best) {
			best = n
		}
	}
	return best, nil
}

func opRange(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "start", "end")
	if err != nil {
		return nil, err
	}
	start, err := h.intVal("start")
	if err != nil {
		return nil, err
	}
	end, err := h.intVal("end")
	if err != nil {
		return nil, err
	}
	out := []interface{}{}
	if start > end {
		return out, nil
	}
	for v := start; ; v++ {
		out = append(out, v)
		if v == end {
			break
		}
	}
	return out, nil
}

// ---------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------

func opConcat(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "a", "b")
	if err != nil {
		return nil, err
	}
	a, err := h.strVal("a")
	if err != nil {
		return nil, err
	}
	b, err := h.strVal("b")
	if err != nil {
		return nil, err
	}
	return a + b, nil
}

func opSplit(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input", "sep")
	if err != nil {
		return nil, err
	}
	input, err := h.strVal("input")
	if err != nil {
		return nil, err
	}
	sep, err := h.strVal("sep")
	if err != nil {
		return nil, err
	}
	out := []interface{}{}
	if sep == "" {
		for _, r := range input {
			out = append(out, string(r))
		}
		return out, nil
	}
	for _, p := range strings.Split(input, sep) {
		out = append(out, p)
	}
	return out, nil
}

func opJoin(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input", "sep")
	if err != nil {
		return nil, err
	}
	parts, err := h.strArray("input")
	if err != nil {
		return nil, err
	}
	sep, err := h.strVal("sep")
	if err != nil {
		return nil, err
	}
	return strings.Join(parts, sep), nil
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

func opUpperLower(args map[string]interface{}, step string, upper bool) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input")
	if err != nil {
		return nil, err
	}
	input, err := h.strVal("input")
	if err != nil {
		return nil, err
	}
	if upper {
		return asciiUpper(input), nil
	}
	return asciiLower(input), nil
}

const asciiWhitespace = " \t\n\r\f\v"

func opTrim(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input")
	if err != nil {
		return nil, err
	}
	input, err := h.strVal("input")
	if err != nil {
		return nil, err
	}
	return strings.Trim(input, asciiWhitespace), nil
}

func opReplace(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input", "search", "replace")
	if err != nil {
		return nil, err
	}
	input, err := h.strVal("input")
	if err != nil {
		return nil, err
	}
	search, err := h.strVal("search")
	if err != nil {
		return nil, err
	}
	repl, err := h.strVal("replace")
	if err != nil {
		return nil, err
	}
	if search == "" {
		return nil, opErr(step, "replace search must not be empty")
	}
	return strings.ReplaceAll(input, search, repl), nil
}

func opLength(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input")
	if err != nil {
		return nil, err
	}
	input, err := h.strVal("input")
	if err != nil {
		return nil, err
	}
	return int64(utf8.RuneCountInString(input)), nil
}

// ---------------------------------------------------------------------------
// Arrays
// ---------------------------------------------------------------------------

func opMap(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input", "transform")
	if err != nil {
		return nil, err
	}
	input, err := h.arrVal("input")
	if err != nil {
		return nil, err
	}
	tv, err := h.required("transform")
	if err != nil {
		return nil, err
	}
	out := make([]interface{}, 0, len(input))
	for _, el := range input {
		r, err := applyTransform(el, tv, step)
		if err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	return out, nil
}

func opFilter(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input", "predicate")
	if err != nil {
		return nil, err
	}
	input, err := h.arrVal("input")
	if err != nil {
		return nil, err
	}
	pv, err := h.required("predicate")
	if err != nil {
		return nil, err
	}
	out := []interface{}{}
	for _, el := range input {
		keep, err := applyPredicate(el, pv, step)
		if err != nil {
			return nil, err
		}
		if keep {
			out = append(out, el)
		}
	}
	return out, nil
}

func opReduce(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input", "op", "initial")
	if err != nil {
		return nil, err
	}
	input, err := h.arrVal("input")
	if err != nil {
		return nil, err
	}
	op, err := h.strVal("op")
	if err != nil {
		return nil, err
	}
	if op != "add" && op != "subtract" && op != "multiply" {
		return nil, opErr(step, "reduce op must be add, subtract or multiply")
	}
	initial, hasInitial, err := h.optInt("initial")
	if err != nil {
		return nil, err
	}
	if !hasInitial {
		if op == "multiply" {
			initial = 1
		} else {
			initial = 0
		}
	}
	acc := initial
	for _, el := range input {
		n, ok := toIntLiteral(el)
		if !ok {
			return nil, opErr(step, "reduce input must contain i64 integers")
		}
		r, err := arith(op, acc, n, step)
		if err != nil {
			return nil, err
		}
		acc = r
	}
	return acc, nil
}

func opSort(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input")
	if err != nil {
		return nil, err
	}
	input, err := h.arrVal("input")
	if err != nil {
		return nil, err
	}
	out := make([]interface{}, len(input))
	copy(out, input)
	sort.SliceStable(out, func(i, j int) bool {
		return compareValues(out[i], out[j]) < 0
	})
	return out, nil
}

func opReverse(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input")
	if err != nil {
		return nil, err
	}
	input, err := h.arrVal("input")
	if err != nil {
		return nil, err
	}
	out := make([]interface{}, len(input))
	for i, v := range input {
		out[len(input)-1-i] = v
	}
	return out, nil
}

func opUnique(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input")
	if err != nil {
		return nil, err
	}
	input, err := h.arrVal("input")
	if err != nil {
		return nil, err
	}
	out := []interface{}{}
	for _, el := range input {
		dup := false
		for _, seen := range out {
			if equalsValues(el, seen) {
				dup = true
				break
			}
		}
		if !dup {
			out = append(out, el)
		}
	}
	return out, nil
}

func normalizeIndex(i int64, n int64) int64 {
	if i < 0 {
		i += n
	}
	if i < 0 {
		return 0
	}
	if i > n {
		return n
	}
	return i
}

func opSlice(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input", "start", "end")
	if err != nil {
		return nil, err
	}
	input, err := h.arrVal("input")
	if err != nil {
		return nil, err
	}
	start, err := h.intVal("start")
	if err != nil {
		return nil, err
	}
	end, hasEnd, err := h.optInt("end")
	if err != nil {
		return nil, err
	}
	n := int64(len(input))
	if !hasEnd {
		end = n
	}
	start = normalizeIndex(start, n)
	end = normalizeIndex(end, n)
	if start > end {
		return []interface{}{}, nil
	}
	out := make([]interface{}, 0, end-start)
	out = append(out, input[start:end]...)
	return out, nil
}

func opFlatten(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input")
	if err != nil {
		return nil, err
	}
	input, err := h.arrVal("input")
	if err != nil {
		return nil, err
	}
	out := []interface{}{}
	for _, el := range input {
		if inner, ok := el.([]interface{}); ok {
			out = append(out, inner...)
		} else {
			out = append(out, el)
		}
	}
	return out, nil
}

// ---------------------------------------------------------------------------
// map transforms
// ---------------------------------------------------------------------------

func isSelfMarker(v interface{}) bool {
	m, ok := v.(map[string]interface{})
	if !ok || len(m) != 1 {
		return false
	}
	sv, ok := m["$self"]
	if !ok {
		return false
	}
	b, ok := sv.(bool)
	return ok && b
}

func applyTransform(el interface{}, tv interface{}, step string) (interface{}, *WErr) {
	m, ok := tv.(map[string]interface{})
	if !ok {
		return nil, opErr(step, "transform must be an object")
	}
	typv, ok := m["type"]
	if !ok {
		return nil, opErr(step, "transform is missing type")
	}
	typ, ok := typv.(string)
	if !ok {
		return nil, opErr(step, "transform type must be a string")
	}

	switch typ {
	case "constant":
		v, ok := m["value"]
		if !ok {
			return nil, opErr(step, "constant transform requires value")
		}
		return v, nil
	case "add", "subtract", "multiply", "divide", "mod":
		v, ok := m["value"]
		if !ok {
			return nil, opErr(step, "binary transform requires value")
		}
		vi, ok := toIntLiteral(v)
		if !ok {
			return nil, opErr(step, "transform value must be an i64 integer")
		}
		ei, ok := toIntLiteral(el)
		if !ok {
			return nil, opErr(step, "transform input element must be an i64 integer")
		}
		r, err := arith(typ, ei, vi, step)
		if err != nil {
			return nil, err
		}
		return r, nil
	case "negate":
		ei, ok := toIntLiteral(el)
		if !ok {
			return nil, opErr(step, "negate requires an i64 integer element")
		}
		if ei == math.MinInt64 {
			return nil, newErr("ARITHMETIC_OVERFLOW", step, "negate overflow")
		}
		return -ei, nil
	case "abs":
		ei, ok := toIntLiteral(el)
		if !ok {
			return nil, opErr(step, "abs requires an i64 integer element")
		}
		if ei == math.MinInt64 {
			return nil, newErr("ARITHMETIC_OVERFLOW", step, "abs overflow")
		}
		if ei < 0 {
			return -ei, nil
		}
		return ei, nil
	case "stringify":
		return stringifyValue(el), nil
	case "type":
		return typeName(el), nil
	case "op":
		opv, ok := m["op"]
		if !ok {
			return nil, opErr(step, "op transform requires op")
		}
		opName, ok := opv.(string)
		if !ok {
			return nil, opErr(step, "transform op must be a string")
		}
		switch opName {
		case "add", "subtract", "multiply", "divide", "mod":
		default:
			return nil, opErr(step, "unsupported transform op %q", opName)
		}
		argv, ok := m["args"]
		if !ok {
			return nil, opErr(step, "op transform requires args")
		}
		am, ok := argv.(map[string]interface{})
		if !ok {
			return nil, opErr(step, "op transform args must be an object")
		}
		h, err := makeArgs(step, am, "a", "b")
		if err != nil {
			return nil, err
		}
		av, err := h.required("a")
		if err != nil {
			return nil, err
		}
		bv, err := h.required("b")
		if err != nil {
			return nil, err
		}
		if isSelfMarker(av) {
			av = el
		}
		if isSelfMarker(bv) {
			bv = el
		}
		a, ok := toIntLiteral(av)
		if !ok {
			return nil, opErr(step, "transform operand a must be an i64 integer")
		}
		b, ok := toIntLiteral(bv)
		if !ok {
			return nil, opErr(step, "transform operand b must be an i64 integer")
		}
		r, err := arith(opName, a, b, step)
		if err != nil {
			return nil, err
		}
		return r, nil
	}
	return nil, opErr(step, "unknown transform type %q", typ)
}

// ---------------------------------------------------------------------------
// filter predicates
// ---------------------------------------------------------------------------

func isJSONTypeName(s string) bool {
	switch s {
	case "null", "boolean", "number", "string", "array", "object":
		return true
	}
	return false
}

func predicateTypeIsTarget(m map[string]interface{}) (string, bool) {
	for _, k := range []string{"value", "target", "type_name", "expected", "expected_type"} {
		if v, ok := m[k]; ok {
			if s, ok := v.(string); ok {
				return s, true
			}
		}
	}
	return "", false
}

func applyPredicate(el interface{}, pv interface{}, step string) (bool, *WErr) {
	m, ok := pv.(map[string]interface{})
	if !ok {
		return false, opErr(step, "predicate must be an object")
	}
	typv, ok := m["type"]
	if !ok {
		// Tolerate a {"type_is": "<type>"} spelling as an alternative to the
		// (otherwise unrepresentable) duplicate-key form in the SPEC.
		if tv, ok2 := m["type_is"].(string); ok2 {
			return typeName(el) == tv, nil
		}
		return false, opErr(step, "predicate is missing type")
	}
	typ, ok := typv.(string)
	if !ok {
		return false, opErr(step, "predicate type must be a string")
	}

	// The SPEC shows {"type": "type_is", "type": "<type>"} which is not a
	// representable JSON object; accept both that shorthand (a bare type name
	// in the discriminator) and explicit target fields.
	if typ != "type_is" && isJSONTypeName(typ) {
		return typeName(el) == typ, nil
	}

	switch typ {
	case "even", "odd":
		n, ok := toIntLiteral(el)
		if !ok {
			return false, opErr(step, "even/odd predicate requires an i64 integer element")
		}
		even := n%2 == 0
		if typ == "even" {
			return even, nil
		}
		return !even, nil
	case "gt", "gte", "lt", "lte":
		v, ok := m["value"]
		if !ok {
			return false, opErr(step, "predicate requires value")
		}
		c := compareValues(el, v)
		switch typ {
		case "gt":
			return c > 0, nil
		case "gte":
			return c >= 0, nil
		case "lt":
			return c < 0, nil
		default:
			return c <= 0, nil
		}
	case "eq", "ne":
		v, ok := m["value"]
		if !ok {
			return false, opErr(step, "predicate requires value")
		}
		eq := equalsValues(el, v)
		if typ == "eq" {
			return eq, nil
		}
		return !eq, nil
	case "truthy":
		return truthy(el), nil
	case "falsy":
		return !truthy(el), nil
	case "type_is":
		target, ok := predicateTypeIsTarget(m)
		if !ok {
			return false, opErr(step, "type_is predicate requires a target type")
		}
		return typeName(el) == target, nil
	case "string_contains":
		v, ok := m["value"]
		if !ok {
			return false, opErr(step, "predicate requires value")
		}
		s, ok := v.(string)
		if !ok {
			return false, opErr(step, "string_contains value must be a string")
		}
		es, ok := el.(string)
		if !ok {
			return false, nil
		}
		return strings.Contains(es, s), nil
	}
	return false, opErr(step, "unknown predicate type %q", typ)
}

// ---------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------

func opGet(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input", "key")
	if err != nil {
		return nil, err
	}
	input, err := h.objVal("input")
	if err != nil {
		return nil, err
	}
	key, err := h.strVal("key")
	if err != nil {
		return nil, err
	}
	if v, ok := input[key]; ok {
		return v, nil
	}
	return nil, nil
}

func opSet(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input", "key", "value")
	if err != nil {
		return nil, err
	}
	input, err := h.objVal("input")
	if err != nil {
		return nil, err
	}
	key, err := h.strVal("key")
	if err != nil {
		return nil, err
	}
	value, err := h.required("value")
	if err != nil {
		return nil, err
	}
	out := make(map[string]interface{}, len(input)+1)
	for k, v := range input {
		out[k] = v
	}
	out[key] = value
	return out, nil
}

func opKeys(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input")
	if err != nil {
		return nil, err
	}
	input, err := h.objVal("input")
	if err != nil {
		return nil, err
	}
	keys := sortedKeys(input)
	out := make([]interface{}, len(keys))
	for i, k := range keys {
		out[i] = k
	}
	return out, nil
}

func opValues(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input")
	if err != nil {
		return nil, err
	}
	input, err := h.objVal("input")
	if err != nil {
		return nil, err
	}
	keys := sortedKeys(input)
	out := make([]interface{}, len(keys))
	for i, k := range keys {
		out[i] = input[k]
	}
	return out, nil
}

func opMerge(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "a", "b")
	if err != nil {
		return nil, err
	}
	a, err := h.objVal("a")
	if err != nil {
		return nil, err
	}
	b, err := h.objVal("b")
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

func opPick(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input", "keys")
	if err != nil {
		return nil, err
	}
	input, err := h.objVal("input")
	if err != nil {
		return nil, err
	}
	keys, err := h.strArray("keys")
	if err != nil {
		return nil, err
	}
	out := map[string]interface{}{}
	for _, k := range keys {
		if v, ok := input[k]; ok {
			out[k] = v
		} else {
			out[k] = nil
		}
	}
	return out, nil
}

func opOmit(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "input", "keys")
	if err != nil {
		return nil, err
	}
	input, err := h.objVal("input")
	if err != nil {
		return nil, err
	}
	keys, err := h.strArray("keys")
	if err != nil {
		return nil, err
	}
	skip := map[string]bool{}
	for _, k := range keys {
		skip[k] = true
	}
	out := map[string]interface{}{}
	for k, v := range input {
		if !skip[k] {
			out[k] = v
		}
	}
	return out, nil
}

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

func opConstant(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "value")
	if err != nil {
		return nil, err
	}
	v, err := h.required("value")
	if err != nil {
		return nil, err
	}
	return v, nil
}

func opEquals(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "a", "b")
	if err != nil {
		return nil, err
	}
	a, err := h.required("a")
	if err != nil {
		return nil, err
	}
	b, err := h.required("b")
	if err != nil {
		return nil, err
	}
	return equalsValues(a, b), nil
}

func opCompare(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "a", "b", "op")
	if err != nil {
		return nil, err
	}
	a, err := h.required("a")
	if err != nil {
		return nil, err
	}
	b, err := h.required("b")
	if err != nil {
		return nil, err
	}
	op, err := h.strVal("op")
	if err != nil {
		return nil, err
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
	return nil, opErr(step, "compare op must be gt, gte, lt or lte")
}

func opAnd(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "a", "b")
	if err != nil {
		return nil, err
	}
	a, err := h.boolVal("a")
	if err != nil {
		return nil, err
	}
	b, err := h.boolVal("b")
	if err != nil {
		return nil, err
	}
	return a && b, nil
}

func opOr(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "a", "b")
	if err != nil {
		return nil, err
	}
	a, err := h.boolVal("a")
	if err != nil {
		return nil, err
	}
	b, err := h.boolVal("b")
	if err != nil {
		return nil, err
	}
	return a || b, nil
}

func opNot(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "a")
	if err != nil {
		return nil, err
	}
	a, err := h.boolVal("a")
	if err != nil {
		return nil, err
	}
	return !a, nil
}

func opType(args map[string]interface{}, step string) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "value")
	if err != nil {
		return nil, err
	}
	v, err := h.required("value")
	if err != nil {
		return nil, err
	}
	return typeName(v), nil
}

func opFailUntil(args map[string]interface{}, step string, attempt int) (interface{}, *WErr) {
	h, err := makeArgs(step, args, "failures", "value")
	if err != nil {
		return nil, err
	}
	failures, err := h.intVal("failures")
	if err != nil {
		return nil, err
	}
	if failures < 0 || failures > 10 {
		return nil, opErr(step, "failures must be in 0..10")
	}
	value, err := h.required("value")
	if err != nil {
		return nil, err
	}
	if int64(attempt) <= failures {
		return nil, newErr("TRANSIENT_FAILURE", step, "transient failure")
	}
	return value, nil
}

// (no external helpers)
