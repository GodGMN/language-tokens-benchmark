package main

import (
	"fmt"
	"math/big"
	"sort"
	"strings"
	"unicode/utf8"
)

// OpError is a runtime operation error. Code is one of the SPEC error codes.
type OpError struct {
	Code    string
	Message string
}

func (e *OpError) Error() string { return e.Code + ": " + e.Message }

func opErr(code, format string, args ...interface{}) *OpError {
	return &OpError{Code: code, Message: fmt.Sprintf(format, args...)}
}

func invalidArg(format string, args ...interface{}) *OpError {
	return opErr("INVALID_ARG", format, args...)
}

func checkAllowed(args map[string]Value, allowed ...string) *OpError {
	ok := make(map[string]bool, len(allowed))
	for _, a := range allowed {
		ok[a] = true
	}
	for k := range args {
		if !ok[k] {
			return invalidArg("unknown argument %q", k)
		}
	}
	return nil
}

func getArg(args map[string]Value, name string) (Value, *OpError) {
	v, ok := args[name]
	if !ok {
		return nil, invalidArg("missing required argument %q", name)
	}
	return v, nil
}

func getIntArg(args map[string]Value, name string) (int64, *OpError) {
	v, err := getArg(args, name)
	if err != nil {
		return 0, err
	}
	n, ok := asInt(v)
	if !ok {
		return 0, invalidArg("argument %q must be an integer", name)
	}
	return n, nil
}

func getStringArg(args map[string]Value, name string) (string, *OpError) {
	v, err := getArg(args, name)
	if err != nil {
		return "", err
	}
	s, ok := asString(v)
	if !ok {
		return "", invalidArg("argument %q must be a string", name)
	}
	return s, nil
}

func getArrayArg(args map[string]Value, name string) ([]Value, *OpError) {
	v, err := getArg(args, name)
	if err != nil {
		return nil, err
	}
	a, ok := asArray(v)
	if !ok {
		return nil, invalidArg("argument %q must be an array", name)
	}
	return a, nil
}

func getObjectArg(args map[string]Value, name string) (map[string]Value, *OpError) {
	v, err := getArg(args, name)
	if err != nil {
		return nil, err
	}
	m, ok := asObject(v)
	if !ok {
		return nil, invalidArg("argument %q must be an object", name)
	}
	return m, nil
}

func getBoolArg(args map[string]Value, name string) (bool, *OpError) {
	v, err := getArg(args, name)
	if err != nil {
		return false, err
	}
	b, ok := asBool(v)
	if !ok {
		return false, invalidArg("argument %q must be a boolean", name)
	}
	return b, nil
}

// ---- checked i64 arithmetic -------------------------------------------------

func addChecked(a, b int64) (int64, *OpError) {
	r := a + b
	if (b > 0 && r < a) || (b < 0 && r > a) {
		return 0, opErr("ARITHMETIC_OVERFLOW", "addition overflow")
	}
	return r, nil
}

func subChecked(a, b int64) (int64, *OpError) {
	r := a - b
	if (b < 0 && r < a) || (b > 0 && r > a) {
		return 0, opErr("ARITHMETIC_OVERFLOW", "subtraction overflow")
	}
	return r, nil
}

func mulChecked(a, b int64) (int64, *OpError) {
	if a == 0 || b == 0 {
		return 0, nil
	}
	if (a == minInt64 && b == -1) || (b == minInt64 && a == -1) {
		return 0, opErr("ARITHMETIC_OVERFLOW", "multiplication overflow")
	}
	r := a * b
	if r/b != a {
		return 0, opErr("ARITHMETIC_OVERFLOW", "multiplication overflow")
	}
	return r, nil
}

func divChecked(a, b int64) (int64, *OpError) {
	if b == 0 {
		return 0, opErr("DIVISION_BY_ZERO", "division by zero")
	}
	if a == minInt64 && b == -1 {
		return 0, opErr("ARITHMETIC_OVERFLOW", "division overflow")
	}
	return a / b, nil
}

func modChecked(a, b int64) (int64, *OpError) {
	if b == 0 {
		return 0, opErr("DIVISION_BY_ZERO", "modulo by zero")
	}
	if a == minInt64 && b == -1 {
		return 0, opErr("ARITHMETIC_OVERFLOW", "modulo overflow")
	}
	return a % b, nil
}

func applyBinaryMath(op string, a, b int64) (int64, *OpError) {
	switch op {
	case "add":
		return addChecked(a, b)
	case "subtract":
		return subChecked(a, b)
	case "multiply":
		return mulChecked(a, b)
	case "divide":
		return divChecked(a, b)
	case "mod":
		return modChecked(a, b)
	}
	return 0, invalidArg("unknown binary math operation %q", op)
}

func isArithmeticOp(op string) bool {
	switch op {
	case "add", "subtract", "multiply", "divide", "mod":
		return true
	}
	return false
}

// ---- operation dispatch -----------------------------------------------------

// executeOp runs one operation. attempt is the 1-based attempt number, used
// only by fail_until.
func executeOp(op string, args map[string]Value, attempt int) (Value, *OpError) {
	switch op {
	case "add", "subtract", "multiply", "divide", "mod":
		return opBinary(op, args)
	case "sum":
		return opSum(args)
	case "min", "max":
		return opMinMax(op, args)
	case "range":
		return opRange(args)

	case "concat":
		return opConcat(args)
	case "split":
		return opSplit(args)
	case "join":
		return opJoin(args)
	case "uppercase", "lowercase", "trim":
		return opStringUnary(op, args)
	case "replace":
		return opReplace(args)
	case "length":
		return opLength(args)

	case "map":
		return opMap(args)
	case "filter":
		return opFilter(args)
	case "reduce":
		return opReduce(args)
	case "sort":
		return opSort(args)
	case "reverse":
		return opReverse(args)
	case "unique":
		return opUnique(args)
	case "slice":
		return opSlice(args)
	case "flatten":
		return opFlatten(args)

	case "get":
		return opGet(args)
	case "set":
		return opSet(args)
	case "keys":
		return opKeys(args)
	case "values":
		return opValues(args)
	case "merge":
		return opMerge(args)
	case "pick":
		return opPick(args)
	case "omit":
		return opOmit(args)

	case "constant":
		return opConstant(args)
	case "equals":
		return opEquals(args)
	case "compare":
		return opCompare(args)
	case "and", "or":
		return opLogic(op, args)
	case "not":
		return opNot(args)
	case "type":
		return opType(args)

	case "fail_until":
		return opFailUntil(args, attempt)
	}
	return nil, invalidArg("unknown operation %q", op)
}

// ---- math -------------------------------------------------------------------

func opBinary(op string, args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "a", "b"); err != nil {
		return nil, err
	}
	a, err := getIntArg(args, "a")
	if err != nil {
		return nil, err
	}
	b, err := getIntArg(args, "b")
	if err != nil {
		return nil, err
	}
	r, err := applyBinaryMath(op, a, b)
	if err != nil {
		return nil, err
	}
	return r, nil
}

func opSum(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input"); err != nil {
		return nil, err
	}
	arr, err := getArrayArg(args, "input")
	if err != nil {
		return nil, err
	}
	total := new(big.Int)
	for _, v := range arr {
		n, ok := asInt(v)
		if !ok {
			return nil, invalidArg("sum input elements must be integers")
		}
		total.Add(total, big.NewInt(n))
	}
	if !total.IsInt64() {
		return nil, opErr("ARITHMETIC_OVERFLOW", "sum overflow")
	}
	return total.Int64(), nil
}

func opMinMax(op string, args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input"); err != nil {
		return nil, err
	}
	arr, err := getArrayArg(args, "input")
	if err != nil {
		return nil, err
	}
	if len(arr) == 0 {
		return nil, opErr("EMPTY_ARRAY", "%s of empty array", op)
	}
	first, ok := asInt(arr[0])
	if !ok {
		return nil, invalidArg("%s input elements must be integers", op)
	}
	best := first
	for _, v := range arr[1:] {
		n, ok := asInt(v)
		if !ok {
			return nil, invalidArg("%s input elements must be integers", op)
		}
		if (op == "min" && n < best) || (op == "max" && n > best) {
			best = n
		}
	}
	return best, nil
}

const maxRangeElements = 10_000_000

func opRange(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "start", "end"); err != nil {
		return nil, err
	}
	start, err := getIntArg(args, "start")
	if err != nil {
		return nil, err
	}
	end, err := getIntArg(args, "end")
	if err != nil {
		return nil, err
	}
	if start > end {
		return []Value{}, nil
	}
	diff := uint64(end) - uint64(start)
	if diff >= maxRangeElements {
		return nil, invalidArg("range is too large")
	}
	out := make([]Value, 0, int(diff)+1)
	for i := start; ; i++ {
		out = append(out, i)
		if i == end {
			break
		}
	}
	return out, nil
}

// ---- strings ----------------------------------------------------------------

func opConcat(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "a", "b"); err != nil {
		return nil, err
	}
	a, err := getStringArg(args, "a")
	if err != nil {
		return nil, err
	}
	b, err := getStringArg(args, "b")
	if err != nil {
		return nil, err
	}
	return a + b, nil
}

func opSplit(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input", "sep"); err != nil {
		return nil, err
	}
	input, err := getStringArg(args, "input")
	if err != nil {
		return nil, err
	}
	sep, err := getStringArg(args, "sep")
	if err != nil {
		return nil, err
	}
	out := []Value{}
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
}

func opJoin(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input", "sep"); err != nil {
		return nil, err
	}
	arr, err := getArrayArg(args, "input")
	if err != nil {
		return nil, err
	}
	sep, err := getStringArg(args, "sep")
	if err != nil {
		return nil, err
	}
	parts := make([]string, 0, len(arr))
	for _, v := range arr {
		s, ok := asString(v)
		if !ok {
			return nil, invalidArg("join input elements must be strings")
		}
		parts = append(parts, s)
	}
	return strings.Join(parts, sep), nil
}

func opStringUnary(op string, args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input"); err != nil {
		return nil, err
	}
	s, err := getStringArg(args, "input")
	if err != nil {
		return nil, err
	}
	switch op {
	case "uppercase":
		b := []byte(s)
		for i, c := range b {
			if c >= 'a' && c <= 'z' {
				b[i] = c - 32
			}
		}
		return string(b), nil
	case "lowercase":
		b := []byte(s)
		for i, c := range b {
			if c >= 'A' && c <= 'Z' {
				b[i] = c + 32
			}
		}
		return string(b), nil
	case "trim":
		return strings.Trim(s, " \t\n\r\f\v"), nil
	}
	return nil, invalidArg("unknown string operation %q", op)
}

func opReplace(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input", "search", "replace"); err != nil {
		return nil, err
	}
	input, err := getStringArg(args, "input")
	if err != nil {
		return nil, err
	}
	search, err := getStringArg(args, "search")
	if err != nil {
		return nil, err
	}
	replacement, err := getStringArg(args, "replace")
	if err != nil {
		return nil, err
	}
	if search == "" {
		return nil, invalidArg("replace search must not be empty")
	}
	return strings.ReplaceAll(input, search, replacement), nil
}

func opLength(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input"); err != nil {
		return nil, err
	}
	s, err := getStringArg(args, "input")
	if err != nil {
		return nil, err
	}
	return int64(utf8.RuneCountInString(s)), nil
}

// ---- arrays -----------------------------------------------------------------

func opMap(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input", "transform"); err != nil {
		return nil, err
	}
	arr, err := getArrayArg(args, "input")
	if err != nil {
		return nil, err
	}
	tv, err := getArg(args, "transform")
	if err != nil {
		return nil, err
	}
	transform, ok := asObject(tv)
	if !ok {
		return nil, invalidArg("map transform must be an object")
	}
	if verr := validateTransformDescriptor(transform); verr != nil {
		return nil, verr
	}
	out := make([]Value, 0, len(arr))
	for _, elem := range arr {
		res, terr := applyTransform(transform, elem)
		if terr != nil {
			return nil, terr
		}
		out = append(out, res)
	}
	return out, nil
}

func opFilter(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input", "predicate"); err != nil {
		return nil, err
	}
	arr, err := getArrayArg(args, "input")
	if err != nil {
		return nil, err
	}
	pv, err := getArg(args, "predicate")
	if err != nil {
		return nil, err
	}
	predicate, ok := asObject(pv)
	if !ok {
		return nil, invalidArg("filter predicate must be an object")
	}
	if verr := validatePredicateDescriptor(predicate); verr != nil {
		return nil, verr
	}
	out := []Value{}
	for _, elem := range arr {
		keep, perr := applyPredicate(predicate, elem)
		if perr != nil {
			return nil, perr
		}
		if keep {
			out = append(out, elem)
		}
	}
	return out, nil
}

func opReduce(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input", "op", "initial"); err != nil {
		return nil, err
	}
	arr, err := getArrayArg(args, "input")
	if err != nil {
		return nil, err
	}
	opName, err := getStringArg(args, "op")
	if err != nil {
		return nil, err
	}
	if opName != "add" && opName != "subtract" && opName != "multiply" {
		return nil, invalidArg("reduce op must be add, subtract or multiply")
	}
	var acc int64
	if _, present := args["initial"]; present {
		initial, ierr := getIntArg(args, "initial")
		if ierr != nil {
			return nil, ierr
		}
		acc = initial
	} else {
		switch opName {
		case "add", "subtract":
			acc = 0
		case "multiply":
			acc = 1
		}
	}
	for _, v := range arr {
		n, ok := asInt(v)
		if !ok {
			return nil, invalidArg("reduce input elements must be integers")
		}
		next, aerr := applyBinaryMath(opName, acc, n)
		if aerr != nil {
			return nil, aerr
		}
		acc = next
	}
	return acc, nil
}

func opSort(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input"); err != nil {
		return nil, err
	}
	arr, err := getArrayArg(args, "input")
	if err != nil {
		return nil, err
	}
	out := make([]Value, len(arr))
	copy(out, arr)
	sort.SliceStable(out, func(i, j int) bool {
		return compareValues(out[i], out[j]) < 0
	})
	return out, nil
}

func opReverse(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input"); err != nil {
		return nil, err
	}
	arr, err := getArrayArg(args, "input")
	if err != nil {
		return nil, err
	}
	out := make([]Value, len(arr))
	for i, v := range arr {
		out[len(arr)-1-i] = v
	}
	return out, nil
}

func opUnique(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input"); err != nil {
		return nil, err
	}
	arr, err := getArrayArg(args, "input")
	if err != nil {
		return nil, err
	}
	seen := make(map[string]bool, len(arr))
	out := make([]Value, 0, len(arr))
	for _, v := range arr {
		key := canonicalValue(v)
		if seen[key] {
			continue
		}
		seen[key] = true
		out = append(out, v)
	}
	return out, nil
}

func opSlice(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input", "start", "end"); err != nil {
		return nil, err
	}
	arr, err := getArrayArg(args, "input")
	if err != nil {
		return nil, err
	}
	start, err := getIntArg(args, "start")
	if err != nil {
		return nil, err
	}
	n := int64(len(arr))
	hasEnd := false
	var end int64
	if _, present := args["end"]; present {
		hasEnd = true
		end, err = getIntArg(args, "end")
		if err != nil {
			return nil, err
		}
	}
	if start < 0 {
		start += n
		if start < 0 {
			start = 0
		}
	}
	if start > n {
		start = n
	}
	if !hasEnd {
		end = n
	} else {
		if end < 0 {
			end += n
			if end < 0 {
				end = 0
			}
		}
		if end > n {
			end = n
		}
	}
	if start > end {
		return []Value{}, nil
	}
	out := make([]Value, 0, int(end-start))
	for i := start; i < end; i++ {
		out = append(out, arr[i])
	}
	return out, nil
}

func opFlatten(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input"); err != nil {
		return nil, err
	}
	arr, err := getArrayArg(args, "input")
	if err != nil {
		return nil, err
	}
	out := []Value{}
	for _, v := range arr {
		if inner, ok := asArray(v); ok {
			out = append(out, inner...)
		} else {
			out = append(out, v)
		}
	}
	return out, nil
}

// ---- objects ----------------------------------------------------------------

func opGet(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input", "key"); err != nil {
		return nil, err
	}
	obj, err := getObjectArg(args, "input")
	if err != nil {
		return nil, err
	}
	key, err := getStringArg(args, "key")
	if err != nil {
		return nil, err
	}
	if v, ok := obj[key]; ok {
		return v, nil
	}
	return nil, nil
}

func opSet(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input", "key", "value"); err != nil {
		return nil, err
	}
	obj, err := getObjectArg(args, "input")
	if err != nil {
		return nil, err
	}
	key, err := getStringArg(args, "key")
	if err != nil {
		return nil, err
	}
	value, err := getArg(args, "value")
	if err != nil {
		return nil, err
	}
	out := make(map[string]Value, len(obj)+1)
	for k, v := range obj {
		out[k] = v
	}
	out[key] = value
	return out, nil
}

func opKeys(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input"); err != nil {
		return nil, err
	}
	obj, err := getObjectArg(args, "input")
	if err != nil {
		return nil, err
	}
	keys := sortedKeys(obj)
	out := make([]Value, len(keys))
	for i, k := range keys {
		out[i] = k
	}
	return out, nil
}

func opValues(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input"); err != nil {
		return nil, err
	}
	obj, err := getObjectArg(args, "input")
	if err != nil {
		return nil, err
	}
	keys := sortedKeys(obj)
	out := make([]Value, len(keys))
	for i, k := range keys {
		out[i] = obj[k]
	}
	return out, nil
}

func opMerge(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "a", "b"); err != nil {
		return nil, err
	}
	a, err := getObjectArg(args, "a")
	if err != nil {
		return nil, err
	}
	b, err := getObjectArg(args, "b")
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
}

func getStringArrayArg(args map[string]Value, name string) ([]string, *OpError) {
	arr, err := getArrayArg(args, name)
	if err != nil {
		return nil, err
	}
	out := make([]string, 0, len(arr))
	for _, v := range arr {
		s, ok := asString(v)
		if !ok {
			return nil, invalidArg("argument %q elements must be strings", name)
		}
		out = append(out, s)
	}
	return out, nil
}

func opPick(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input", "keys"); err != nil {
		return nil, err
	}
	obj, err := getObjectArg(args, "input")
	if err != nil {
		return nil, err
	}
	keys, err := getStringArrayArg(args, "keys")
	if err != nil {
		return nil, err
	}
	out := make(map[string]Value, len(keys))
	for _, k := range keys {
		if v, ok := obj[k]; ok {
			out[k] = v
		} else {
			out[k] = nil
		}
	}
	return out, nil
}

func opOmit(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "input", "keys"); err != nil {
		return nil, err
	}
	obj, err := getObjectArg(args, "input")
	if err != nil {
		return nil, err
	}
	keys, err := getStringArrayArg(args, "keys")
	if err != nil {
		return nil, err
	}
	drop := make(map[string]bool, len(keys))
	for _, k := range keys {
		drop[k] = true
	}
	out := make(map[string]Value)
	for k, v := range obj {
		if !drop[k] {
			out[k] = v
		}
	}
	return out, nil
}

// ---- utilities --------------------------------------------------------------

func opConstant(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "value"); err != nil {
		return nil, err
	}
	v, err := getArg(args, "value")
	if err != nil {
		return nil, err
	}
	return v, nil
}

func opEquals(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "a", "b"); err != nil {
		return nil, err
	}
	a, err := getArg(args, "a")
	if err != nil {
		return nil, err
	}
	b, err := getArg(args, "b")
	if err != nil {
		return nil, err
	}
	return equalValues(a, b), nil
}

func opCompare(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "a", "b", "op"); err != nil {
		return nil, err
	}
	a, err := getArg(args, "a")
	if err != nil {
		return nil, err
	}
	b, err := getArg(args, "b")
	if err != nil {
		return nil, err
	}
	opName, err := getStringArg(args, "op")
	if err != nil {
		return nil, err
	}
	cmp := compareValues(a, b)
	switch opName {
	case "gt":
		return cmp > 0, nil
	case "gte":
		return cmp >= 0, nil
	case "lt":
		return cmp < 0, nil
	case "lte":
		return cmp <= 0, nil
	}
	return nil, invalidArg("compare op must be one of gt, gte, lt, lte")
}

func opLogic(op string, args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "a", "b"); err != nil {
		return nil, err
	}
	a, err := getBoolArg(args, "a")
	if err != nil {
		return nil, err
	}
	b, err := getBoolArg(args, "b")
	if err != nil {
		return nil, err
	}
	if op == "and" {
		return a && b, nil
	}
	return a || b, nil
}

func opNot(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "a"); err != nil {
		return nil, err
	}
	a, err := getBoolArg(args, "a")
	if err != nil {
		return nil, err
	}
	return !a, nil
}

func opType(args map[string]Value) (Value, *OpError) {
	if err := checkAllowed(args, "value"); err != nil {
		return nil, err
	}
	v, err := getArg(args, "value")
	if err != nil {
		return nil, err
	}
	return typeName(v), nil
}

func opFailUntil(args map[string]Value, attempt int) (Value, *OpError) {
	if err := checkAllowed(args, "failures", "value"); err != nil {
		return nil, err
	}
	failures, err := getIntArg(args, "failures")
	if err != nil {
		return nil, err
	}
	if failures < 0 || failures > 10 {
		return nil, invalidArg("failures must be between 0 and 10")
	}
	value, err := getArg(args, "value")
	if err != nil {
		return nil, err
	}
	if int64(attempt) <= failures {
		return nil, opErr("TRANSIENT_FAILURE", "transient failure on attempt %d", attempt)
	}
	return value, nil
}

// ---- map transformations ----------------------------------------------------

// validateTransformDescriptor checks everything about a transform that does not
// depend on an individual element, so that even an empty input array still
// rejects a malformed transform.
func validateTransformDescriptor(t map[string]Value) *OpError {
	tv, ok := t["type"]
	if !ok {
		return invalidArg("transform is missing type")
	}
	kind, ok := asString(tv)
	if !ok {
		return invalidArg("transform type must be a string")
	}
	switch kind {
	case "constant":
		if err := checkAllowed(t, "type", "value"); err != nil {
			return err
		}
		if _, err := getArg(t, "value"); err != nil {
			return err
		}
	case "add", "subtract", "multiply", "divide", "mod":
		if err := checkAllowed(t, "type", "value"); err != nil {
			return err
		}
		if _, err := getIntArg(t, "value"); err != nil {
			return err
		}
	case "negate", "abs", "stringify", "type":
		if err := checkAllowed(t, "type"); err != nil {
			return err
		}
	case "op":
		if err := checkAllowed(t, "type", "op", "args"); err != nil {
			return err
		}
		opName, err := getStringArg(t, "op")
		if err != nil {
			return err
		}
		if !isArithmeticOp(opName) {
			return invalidArg("transform op must be a two-number math operation")
		}
		argsValue, err := getArg(t, "args")
		if err != nil {
			return err
		}
		argsObj, ok := asObject(argsValue)
		if !ok {
			return invalidArg("transform args must be an object")
		}
		if len(argsObj) != 2 {
			return invalidArg("transform op args must contain a and b")
		}
		for _, name := range []string{"a", "b"} {
			v, ok := argsObj[name]
			if !ok {
				return invalidArg("transform op args are missing %q", name)
			}
			if _, ok := asInt(v); !ok && !isSelfRef(v) {
				return invalidArg("transform op arg %q must be an integer or $self", name)
			}
		}
		for k := range argsObj {
			if k != "a" && k != "b" {
				return invalidArg("unknown argument %q", k)
			}
		}
	default:
		return invalidArg("unknown transform type %q", kind)
	}
	return nil
}

func applyTransform(t map[string]Value, elem Value) (Value, *OpError) {
	tv, ok := t["type"]
	if !ok {
		return nil, invalidArg("transform is missing type")
	}
	kind, ok := asString(tv)
	if !ok {
		return nil, invalidArg("transform type must be a string")
	}
	switch kind {
	case "constant":
		if err := checkAllowed(t, "type", "value"); err != nil {
			return nil, err
		}
		v, err := getArg(t, "value")
		if err != nil {
			return nil, err
		}
		return v, nil
	case "add", "subtract", "multiply", "divide", "mod":
		if err := checkAllowed(t, "type", "value"); err != nil {
			return nil, err
		}
		value, err := getIntArg(t, "value")
		if err != nil {
			return nil, err
		}
		n, ok := asInt(elem)
		if !ok {
			return nil, invalidArg("transform %s requires an integer element", kind)
		}
		return applyBinaryMath(kind, n, value)
	case "negate":
		if err := checkAllowed(t, "type"); err != nil {
			return nil, err
		}
		n, ok := asInt(elem)
		if !ok {
			return nil, invalidArg("negate transform requires an integer element")
		}
		if n == minInt64 {
			return nil, opErr("ARITHMETIC_OVERFLOW", "negation overflow")
		}
		return -n, nil
	case "abs":
		if err := checkAllowed(t, "type"); err != nil {
			return nil, err
		}
		n, ok := asInt(elem)
		if !ok {
			return nil, invalidArg("abs transform requires an integer element")
		}
		if n == minInt64 {
			return nil, opErr("ARITHMETIC_OVERFLOW", "abs overflow")
		}
		if n < 0 {
			return -n, nil
		}
		return n, nil
	case "stringify":
		if err := checkAllowed(t, "type"); err != nil {
			return nil, err
		}
		s, err := compactJSON(elem)
		if err != nil {
			return nil, invalidArg("cannot stringify element")
		}
		return s, nil
	case "type":
		if err := checkAllowed(t, "type"); err != nil {
			return nil, err
		}
		return typeName(elem), nil
	case "op":
		if err := checkAllowed(t, "type", "op", "args"); err != nil {
			return nil, err
		}
		opName, err := getStringArg(t, "op")
		if err != nil {
			return nil, err
		}
		if !isArithmeticOp(opName) {
			return nil, invalidArg("transform op must be a two-number math operation")
		}
		argsValue, err := getArg(t, "args")
		if err != nil {
			return nil, err
		}
		argsObj, ok := asObject(argsValue)
		if !ok {
			return nil, invalidArg("transform args must be an object")
		}
		substituted := substituteSelf(argsObj, elem)
		subMap, ok := substituted.(map[string]Value)
		if !ok {
			return nil, invalidArg("transform args must be an object")
		}
		return executeOp(opName, subMap, 1)
	}
	return nil, invalidArg("unknown transform type %q", kind)
}

func isSelfRef(v Value) bool {
	m, ok := asObject(v)
	if !ok || len(m) != 1 {
		return false
	}
	sv, ok := m["$self"]
	if !ok {
		return false
	}
	b, ok := asBool(sv)
	return ok && b
}

func substituteSelf(v Value, elem Value) Value {
	if isSelfRef(v) {
		return elem
	}
	switch x := v.(type) {
	case []Value:
		out := make([]Value, len(x))
		for i := range x {
			out[i] = substituteSelf(x[i], elem)
		}
		return out
	case map[string]Value:
		out := make(map[string]Value, len(x))
		for k, vv := range x {
			out[k] = substituteSelf(vv, elem)
		}
		return out
	default:
		return v
	}
}

// ---- filter predicates ------------------------------------------------------

func predicateTypeTarget(p map[string]Value) (string, *OpError) {
	var target string
	found := false
	for _, name := range []string{"value", "type_name", "target"} {
		if v, ok := p[name]; ok {
			s, ok := asString(v)
			if !ok {
				return "", invalidArg("type_is %s must be a string", name)
			}
			target = s
			found = true
			break
		}
	}
	if !found {
		return "", invalidArg("type_is is missing its type parameter")
	}
	for k := range p {
		if k != "type" && k != "value" && k != "type_name" && k != "target" {
			return "", invalidArg("unknown predicate argument %q", k)
		}
	}
	switch target {
	case "null", "boolean", "number", "string", "array", "object":
	default:
		return "", invalidArg("unknown type name %q", target)
	}
	return target, nil
}

// validatePredicateDescriptor checks everything about a predicate that does not
// depend on an individual element.
func validatePredicateDescriptor(p map[string]Value) *OpError {
	pv, ok := p["type"]
	if !ok {
		return invalidArg("predicate is missing type")
	}
	kind, ok := asString(pv)
	if !ok {
		return invalidArg("predicate type must be a string")
	}
	switch kind {
	case "even", "odd", "truthy", "falsy":
		return checkAllowed(p, "type")
	case "gt", "gte", "lt", "lte", "eq", "ne":
		if err := checkAllowed(p, "type", "value"); err != nil {
			return err
		}
		if _, err := getIntArg(p, "value"); err != nil {
			return err
		}
		return nil
	case "type_is":
		_, err := predicateTypeTarget(p)
		return err
	case "string_contains":
		if err := checkAllowed(p, "type", "value"); err != nil {
			return err
		}
		_, err := getStringArg(p, "value")
		return err
	default:
		return invalidArg("unknown predicate type %q", kind)
	}
}

func applyPredicate(p map[string]Value, elem Value) (bool, *OpError) {
	pv, ok := p["type"]
	if !ok {
		return false, invalidArg("predicate is missing type")
	}
	kind, ok := asString(pv)
	if !ok {
		return false, invalidArg("predicate type must be a string")
	}
	switch kind {
	case "even", "odd":
		if err := checkAllowed(p, "type"); err != nil {
			return false, err
		}
		n, ok := asInt(elem)
		if !ok {
			return false, invalidArg("predicate %s requires an integer element", kind)
		}
		even := n%2 == 0
		if kind == "even" {
			return even, nil
		}
		return !even, nil
	case "gt", "gte", "lt", "lte", "eq", "ne":
		if err := checkAllowed(p, "type", "value"); err != nil {
			return false, err
		}
		n, err := getIntArg(p, "value")
		if err != nil {
			return false, err
		}
		value := Value(n)
		if kind == "eq" {
			return equalValues(elem, value), nil
		}
		if kind == "ne" {
			return !equalValues(elem, value), nil
		}
		cmp := compareValues(elem, value)
		switch kind {
		case "gt":
			return cmp > 0, nil
		case "gte":
			return cmp >= 0, nil
		case "lt":
			return cmp < 0, nil
		case "lte":
			return cmp <= 0, nil
		}
		return false, nil
	case "truthy", "falsy":
		if err := checkAllowed(p, "type"); err != nil {
			return false, err
		}
		if kind == "truthy" {
			return truthy(elem), nil
		}
		return !truthy(elem), nil
	case "type_is":
		// The spec describes the target as a "<type>" parameter. Accept the
		// likely spellings for that parameter.
		target, err := predicateTypeTarget(p)
		if err != nil {
			return false, err
		}
		return typeName(elem) == target, nil
	case "string_contains":
		if err := checkAllowed(p, "type", "value"); err != nil {
			return false, err
		}
		value, err := getStringArg(p, "value")
		if err != nil {
			return false, err
		}
		s, ok := asString(elem)
		if !ok {
			return false, invalidArg("string_contains requires a string element")
		}
		return strings.Contains(s, value), nil
	}
	return false, invalidArg("unknown predicate type %q", kind)
}
