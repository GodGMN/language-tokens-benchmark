package main

import (
	"math"
	"math/big"
	"sort"
	"strings"
)

type opErr struct {
	code string
	msg  string
}

func errCode(code, msg string) *opErr { return &opErr{code: code, msg: msg} }
func errArg(msg string) *opErr        { return errCode("INVALID_ARG", msg) }
func errCond(msg string) *opErr       { return errCode("INVALID_CONDITION", msg) }

// ---------------------------------------------------------------------------
// Checked i64 arithmetic

func checkedAdd(a, b int64) (int64, *opErr) {
	z := new(big.Int).Add(big.NewInt(a), big.NewInt(b))
	if !z.IsInt64() {
		return 0, errCode("ARITHMETIC_OVERFLOW", "addition overflow")
	}
	return z.Int64(), nil
}

func checkedSub(a, b int64) (int64, *opErr) {
	z := new(big.Int).Sub(big.NewInt(a), big.NewInt(b))
	if !z.IsInt64() {
		return 0, errCode("ARITHMETIC_OVERFLOW", "subtraction overflow")
	}
	return z.Int64(), nil
}

func checkedMul(a, b int64) (int64, *opErr) {
	z := new(big.Int).Mul(big.NewInt(a), big.NewInt(b))
	if !z.IsInt64() {
		return 0, errCode("ARITHMETIC_OVERFLOW", "multiplication overflow")
	}
	return z.Int64(), nil
}

func checkedNeg(a int64) (int64, *opErr) {
	if a == math.MinInt64 {
		return 0, errCode("ARITHMETIC_OVERFLOW", "negation overflow")
	}
	return -a, nil
}

func checkedAbs(a int64) (int64, *opErr) {
	if a < 0 {
		return checkedNeg(a)
	}
	return a, nil
}

func checkedDiv(a, b int64) (int64, *opErr) {
	if b == 0 {
		return 0, errCode("DIVISION_BY_ZERO", "division by zero")
	}
	if a == math.MinInt64 && b == -1 {
		return 0, errCode("ARITHMETIC_OVERFLOW", "division overflow")
	}
	return a / b, nil
}

func checkedMod(a, b int64) (int64, *opErr) {
	if b == 0 {
		return 0, errCode("DIVISION_BY_ZERO", "modulo by zero")
	}
	if a == math.MinInt64 && b == -1 {
		return 0, errCode("ARITHMETIC_OVERFLOW", "modulo overflow")
	}
	return a % b, nil
}

// ---------------------------------------------------------------------------
// Argument access helpers

type opArgs struct {
	m map[string]Value
}

func newArgs(v Value) (*opArgs, *opErr) {
	if v.Kind != KObj {
		return nil, errArg("args must be an object")
	}
	return &opArgs{m: v.O}, nil
}

func (a *opArgs) only(names ...string) *opErr {
	allowed := make(map[string]bool, len(names))
	for _, n := range names {
		allowed[n] = true
	}
	for k := range a.m {
		if !allowed[k] {
			return errArg("unknown argument: " + k)
		}
	}
	return nil
}

func (a *opArgs) req(name string) (Value, *opErr) {
	v, ok := a.m[name]
	if !ok {
		return Value{}, errArg("missing argument: " + name)
	}
	return v, nil
}

func (a *opArgs) intArg(name string) (int64, *opErr) {
	v, ok := a.m[name]
	if !ok {
		return 0, errArg("missing argument: " + name)
	}
	iv, ok := intValue(v)
	if !ok {
		return 0, errArg("argument " + name + " must be a signed 64-bit integer literal")
	}
	return iv, nil
}

func (a *opArgs) optIntArg(name string) (int64, bool, *opErr) {
	v, ok := a.m[name]
	if !ok {
		return 0, false, nil
	}
	iv, ok := intValue(v)
	if !ok {
		return 0, false, errArg("argument " + name + " must be a signed 64-bit integer literal")
	}
	return iv, true, nil
}

func (a *opArgs) strArg(name string) (string, *opErr) {
	v, ok := a.m[name]
	if !ok {
		return "", errArg("missing argument: " + name)
	}
	if v.Kind != KStr {
		return "", errArg("argument " + name + " must be a string")
	}
	return v.S, nil
}

func (a *opArgs) boolArg(name string) (bool, *opErr) {
	v, ok := a.m[name]
	if !ok {
		return false, errArg("missing argument: " + name)
	}
	if v.Kind != KBool {
		return false, errArg("argument " + name + " must be a boolean")
	}
	return v.B, nil
}

func (a *opArgs) arrArg(name string) ([]Value, *opErr) {
	v, ok := a.m[name]
	if !ok {
		return nil, errArg("missing argument: " + name)
	}
	if v.Kind != KArr {
		return nil, errArg("argument " + name + " must be an array")
	}
	return v.A, nil
}

func (a *opArgs) objArg(name string) (map[string]Value, *opErr) {
	v, ok := a.m[name]
	if !ok {
		return nil, errArg("missing argument: " + name)
	}
	if v.Kind != KObj {
		return nil, errArg("argument " + name + " must be an object")
	}
	return v.O, nil
}

func (a *opArgs) anyArg(name string) (Value, *opErr) {
	v, ok := a.m[name]
	if !ok {
		return Value{}, errArg("missing argument: " + name)
	}
	return v, nil
}

func onlyKeys(m map[string]Value, names ...string) *opErr {
	allowed := make(map[string]bool, len(names))
	for _, n := range names {
		allowed[n] = true
	}
	for k := range m {
		if !allowed[k] {
			return errArg("unknown field: " + k)
		}
	}
	return nil
}

// ---------------------------------------------------------------------------
// Operation dispatch

func (e *engine) runOp(st *Step, argsV Value) (Value, *opErr) {
	a, err := newArgs(argsV)
	if err != nil {
		return Value{}, err
	}
	switch st.Op {
	// ---- math ----
	case "add", "subtract", "multiply", "divide", "mod":
		return applyMath2(st.Op, a)

	case "sum":
		if err := a.only("input"); err != nil {
			return Value{}, err
		}
		arr, err := a.arrArg("input")
		if err != nil {
			return Value{}, err
		}
		acc := int64(0)
		for _, el := range arr {
			iv, ok := intValue(el)
			if !ok {
				return Value{}, errArg("sum input must contain integers")
			}
			acc, err = checkedAdd(acc, iv)
			if err != nil {
				return Value{}, err
			}
		}
		return Int(acc), nil

	case "min", "max":
		if err := a.only("input"); err != nil {
			return Value{}, err
		}
		arr, err := a.arrArg("input")
		if err != nil {
			return Value{}, err
		}
		if len(arr) == 0 {
			return Value{}, errCode("EMPTY_ARRAY", st.Op+" requires a non-empty array")
		}
		best, ok := intValue(arr[0])
		if !ok {
			return Value{}, errArg(st.Op + " input must contain integers")
		}
		for _, el := range arr[1:] {
			iv, ok := intValue(el)
			if !ok {
				return Value{}, errArg(st.Op + " input must contain integers")
			}
			if (st.Op == "min" && iv < best) || (st.Op == "max" && iv > best) {
				best = iv
			}
		}
		return Int(best), nil

	case "range":
		if err := a.only("start", "end"); err != nil {
			return Value{}, err
		}
		start, err := a.intArg("start")
		if err != nil {
			return Value{}, err
		}
		end, err := a.intArg("end")
		if err != nil {
			return Value{}, err
		}
		out := []Value{}
		if start <= end {
			for i := start; ; i++ {
				out = append(out, Int(i))
				if i == end {
					break
				}
			}
		}
		return Arr(out), nil

	// ---- strings ----
	case "concat":
		if err := a.only("a", "b"); err != nil {
			return Value{}, err
		}
		x, err := a.strArg("a")
		if err != nil {
			return Value{}, err
		}
		y, err := a.strArg("b")
		if err != nil {
			return Value{}, err
		}
		return Str(x + y), nil

	case "split":
		if err := a.only("input", "sep"); err != nil {
			return Value{}, err
		}
		input, err := a.strArg("input")
		if err != nil {
			return Value{}, err
		}
		sep, err := a.strArg("sep")
		if err != nil {
			return Value{}, err
		}
		parts := strings.Split(input, sep)
		out := make([]Value, len(parts))
		for i, p := range parts {
			out[i] = Str(p)
		}
		return Arr(out), nil

	case "join":
		if err := a.only("input", "sep"); err != nil {
			return Value{}, err
		}
		arr, err := a.arrArg("input")
		if err != nil {
			return Value{}, err
		}
		sep, err := a.strArg("sep")
		if err != nil {
			return Value{}, err
		}
		parts := make([]string, len(arr))
		for i, el := range arr {
			if el.Kind != KStr {
				return Value{}, errArg("join input must contain strings")
			}
			parts[i] = el.S
		}
		return Str(strings.Join(parts, sep)), nil

	case "uppercase":
		if err := a.only("input"); err != nil {
			return Value{}, err
		}
		s, err := a.strArg("input")
		if err != nil {
			return Value{}, err
		}
		return Str(asciiUpper(s)), nil

	case "lowercase":
		if err := a.only("input"); err != nil {
			return Value{}, err
		}
		s, err := a.strArg("input")
		if err != nil {
			return Value{}, err
		}
		return Str(asciiLower(s)), nil

	case "trim":
		if err := a.only("input"); err != nil {
			return Value{}, err
		}
		s, err := a.strArg("input")
		if err != nil {
			return Value{}, err
		}
		return Str(strings.Trim(s, " \t\n\r\f\v")), nil

	case "replace":
		if err := a.only("input", "search", "replace"); err != nil {
			return Value{}, err
		}
		input, err := a.strArg("input")
		if err != nil {
			return Value{}, err
		}
		search, err := a.strArg("search")
		if err != nil {
			return Value{}, err
		}
		repl, err := a.strArg("replace")
		if err != nil {
			return Value{}, err
		}
		if search == "" {
			return Value{}, errArg("replace search must not be empty")
		}
		return Str(strings.ReplaceAll(input, search, repl)), nil

	case "length":
		if err := a.only("input"); err != nil {
			return Value{}, err
		}
		s, err := a.strArg("input")
		if err != nil {
			return Value{}, err
		}
		return Int(int64(len([]rune(s)))), nil

	// ---- arrays ----
	case "map":
		if err := a.only("input", "transform"); err != nil {
			return Value{}, err
		}
		arr, err := a.arrArg("input")
		if err != nil {
			return Value{}, err
		}
		tr, err := a.req("transform")
		if err != nil {
			return Value{}, err
		}
		out := make([]Value, len(arr))
		for i, el := range arr {
			rv, terr := applyTransform(tr, el)
			if terr != nil {
				return Value{}, terr
			}
			out[i] = rv
		}
		return Arr(out), nil

	case "filter":
		if err := a.only("input", "predicate"); err != nil {
			return Value{}, err
		}
		arr, err := a.arrArg("input")
		if err != nil {
			return Value{}, err
		}
		pr, err := a.req("predicate")
		if err != nil {
			return Value{}, err
		}
		out := make([]Value, 0, len(arr))
		for _, el := range arr {
			keep, perr := applyPredicate(pr, el)
			if perr != nil {
				return Value{}, perr
			}
			if keep {
				out = append(out, el)
			}
		}
		return Arr(out), nil

	case "reduce":
		if err := a.only("input", "op", "initial"); err != nil {
			return Value{}, err
		}
		arr, err := a.arrArg("input")
		if err != nil {
			return Value{}, err
		}
		opName, err := a.strArg("op")
		if err != nil {
			return Value{}, err
		}
		var combine func(int64, int64) (int64, *opErr)
		identity := int64(0)
		switch opName {
		case "add":
			combine = checkedAdd
		case "subtract":
			combine = checkedSub
		case "multiply":
			combine = checkedMul
			identity = 1
		default:
			return Value{}, errArg("reduce op must be add, subtract or multiply")
		}
		acc, has, err := a.optIntArg("initial")
		if err != nil {
			return Value{}, err
		}
		if !has {
			acc = identity
		}
		for _, el := range arr {
			iv, ok := intValue(el)
			if !ok {
				return Value{}, errArg("reduce input must contain integers")
			}
			acc, err = combine(acc, iv)
			if err != nil {
				return Value{}, err
			}
		}
		return Int(acc), nil

	case "sort":
		if err := a.only("input"); err != nil {
			return Value{}, err
		}
		arr, err := a.arrArg("input")
		if err != nil {
			return Value{}, err
		}
		out := make([]Value, len(arr))
		copy(out, arr)
		sort.SliceStable(out, func(i, j int) bool {
			return compareValues(out[i], out[j]) < 0
		})
		return Arr(out), nil

	case "reverse":
		if err := a.only("input"); err != nil {
			return Value{}, err
		}
		arr, err := a.arrArg("input")
		if err != nil {
			return Value{}, err
		}
		out := make([]Value, len(arr))
		for i := range arr {
			out[i] = arr[len(arr)-1-i]
		}
		return Arr(out), nil

	case "unique":
		if err := a.only("input"); err != nil {
			return Value{}, err
		}
		arr, err := a.arrArg("input")
		if err != nil {
			return Value{}, err
		}
		out := make([]Value, 0, len(arr))
		for _, el := range arr {
			dup := false
			for _, kept := range out {
				if equalValues(el, kept) {
					dup = true
					break
				}
			}
			if !dup {
				out = append(out, el)
			}
		}
		return Arr(out), nil

	case "slice":
		if err := a.only("input", "start", "end"); err != nil {
			return Value{}, err
		}
		arr, err := a.arrArg("input")
		if err != nil {
			return Value{}, err
		}
		start, err := a.intArg("start")
		if err != nil {
			return Value{}, err
		}
		end, hasEnd, err := a.optIntArg("end")
		if err != nil {
			return Value{}, err
		}
		n := int64(len(arr))
		norm := func(x int64) int64 {
			if x < 0 {
				x += n
			}
			if x < 0 {
				return 0
			}
			if x > n {
				return n
			}
			return x
		}
		s := norm(start)
		var eIdx int64
		if hasEnd {
			eIdx = norm(end)
		} else {
			eIdx = n
		}
		if s > eIdx {
			return Arr([]Value{}), nil
		}
		out := make([]Value, eIdx-s)
		copy(out, arr[s:eIdx])
		return Arr(out), nil

	case "flatten":
		if err := a.only("input"); err != nil {
			return Value{}, err
		}
		arr, err := a.arrArg("input")
		if err != nil {
			return Value{}, err
		}
		out := make([]Value, 0, len(arr))
		for _, el := range arr {
			if el.Kind == KArr {
				out = append(out, el.A...)
			} else {
				out = append(out, el)
			}
		}
		return Arr(out), nil

	// ---- objects ----
	case "get":
		if err := a.only("input", "key"); err != nil {
			return Value{}, err
		}
		obj, err := a.objArg("input")
		if err != nil {
			return Value{}, err
		}
		key, err := a.strArg("key")
		if err != nil {
			return Value{}, err
		}
		if v, ok := obj[key]; ok {
			return v, nil
		}
		return Null(), nil

	case "set":
		if err := a.only("input", "key", "value"); err != nil {
			return Value{}, err
		}
		obj, err := a.objArg("input")
		if err != nil {
			return Value{}, err
		}
		key, err := a.strArg("key")
		if err != nil {
			return Value{}, err
		}
		val, err := a.anyArg("value")
		if err != nil {
			return Value{}, err
		}
		out := make(map[string]Value, len(obj)+1)
		for k, v := range obj {
			out[k] = v
		}
		out[key] = val
		return Obj(out), nil

	case "keys":
		if err := a.only("input"); err != nil {
			return Value{}, err
		}
		obj, err := a.objArg("input")
		if err != nil {
			return Value{}, err
		}
		keys := sortedKeys(obj)
		out := make([]Value, len(keys))
		for i, k := range keys {
			out[i] = Str(k)
		}
		return Arr(out), nil

	case "values":
		if err := a.only("input"); err != nil {
			return Value{}, err
		}
		obj, err := a.objArg("input")
		if err != nil {
			return Value{}, err
		}
		keys := sortedKeys(obj)
		out := make([]Value, len(keys))
		for i, k := range keys {
			out[i] = obj[k]
		}
		return Arr(out), nil

	case "merge":
		if err := a.only("a", "b"); err != nil {
			return Value{}, err
		}
		ao, err := a.objArg("a")
		if err != nil {
			return Value{}, err
		}
		bo, err := a.objArg("b")
		if err != nil {
			return Value{}, err
		}
		out := make(map[string]Value, len(ao)+len(bo))
		for k, v := range ao {
			out[k] = v
		}
		for k, v := range bo {
			out[k] = v
		}
		return Obj(out), nil

	case "pick":
		if err := a.only("input", "keys"); err != nil {
			return Value{}, err
		}
		obj, err := a.objArg("input")
		if err != nil {
			return Value{}, err
		}
		keyVals, err := a.arrArg("keys")
		if err != nil {
			return Value{}, err
		}
		out := make(map[string]Value, len(keyVals))
		for _, kv := range keyVals {
			if kv.Kind != KStr {
				return Value{}, errArg("pick keys must be strings")
			}
			if v, ok := obj[kv.S]; ok {
				out[kv.S] = v
			} else {
				out[kv.S] = Null()
			}
		}
		return Obj(out), nil

	case "omit":
		if err := a.only("input", "keys"); err != nil {
			return Value{}, err
		}
		obj, err := a.objArg("input")
		if err != nil {
			return Value{}, err
		}
		keyVals, err := a.arrArg("keys")
		if err != nil {
			return Value{}, err
		}
		skip := make(map[string]bool, len(keyVals))
		for _, kv := range keyVals {
			if kv.Kind != KStr {
				return Value{}, errArg("omit keys must be strings")
			}
			skip[kv.S] = true
		}
		out := make(map[string]Value, len(obj))
		for k, v := range obj {
			if !skip[k] {
				out[k] = v
			}
		}
		return Obj(out), nil

	// ---- utilities ----
	case "constant":
		if err := a.only("value"); err != nil {
			return Value{}, err
		}
		return a.anyArg("value")

	case "equals":
		if err := a.only("a", "b"); err != nil {
			return Value{}, err
		}
		x, err := a.anyArg("a")
		if err != nil {
			return Value{}, err
		}
		y, err := a.anyArg("b")
		if err != nil {
			return Value{}, err
		}
		return Bool(equalValues(x, y)), nil

	case "compare":
		if err := a.only("a", "b", "op"); err != nil {
			return Value{}, err
		}
		x, err := a.anyArg("a")
		if err != nil {
			return Value{}, err
		}
		y, err := a.anyArg("b")
		if err != nil {
			return Value{}, err
		}
		cmpOp, err := a.strArg("op")
		if err != nil {
			return Value{}, err
		}
		c := compareValues(x, y)
		switch cmpOp {
		case "gt":
			return Bool(c > 0), nil
		case "gte":
			return Bool(c >= 0), nil
		case "lt":
			return Bool(c < 0), nil
		case "lte":
			return Bool(c <= 0), nil
		}
		return Value{}, errArg("compare op must be gt, gte, lt or lte")

	case "and":
		if err := a.only("a", "b"); err != nil {
			return Value{}, err
		}
		x, err := a.boolArg("a")
		if err != nil {
			return Value{}, err
		}
		y, err := a.boolArg("b")
		if err != nil {
			return Value{}, err
		}
		return Bool(x && y), nil

	case "or":
		if err := a.only("a", "b"); err != nil {
			return Value{}, err
		}
		x, err := a.boolArg("a")
		if err != nil {
			return Value{}, err
		}
		y, err := a.boolArg("b")
		if err != nil {
			return Value{}, err
		}
		return Bool(x || y), nil

	case "not":
		if err := a.only("a"); err != nil {
			return Value{}, err
		}
		x, err := a.boolArg("a")
		if err != nil {
			return Value{}, err
		}
		return Bool(!x), nil

	case "type":
		if err := a.only("value"); err != nil {
			return Value{}, err
		}
		x, err := a.anyArg("value")
		if err != nil {
			return Value{}, err
		}
		return Str(typeName(x)), nil

	case "fail_until":
		if err := a.only("failures", "value"); err != nil {
			return Value{}, err
		}
		failures, err := a.intArg("failures")
		if err != nil {
			return Value{}, err
		}
		if failures < 0 || failures > 10 {
			return Value{}, errArg("failures must be between 0 and 10")
		}
		val, err := a.anyArg("value")
		if err != nil {
			return Value{}, err
		}
		e.failCount[st.ID]++
		if e.failCount[st.ID] <= int(failures) {
			return Value{}, errCode("TRANSIENT_FAILURE", "transient failure")
		}
		return val, nil
	}
	return Value{}, errArg("unknown operation: " + st.Op)
}

func applyMath2(op string, a *opArgs) (Value, *opErr) {
	if err := a.only("a", "b"); err != nil {
		return Value{}, err
	}
	x, err := a.intArg("a")
	if err != nil {
		return Value{}, err
	}
	y, err := a.intArg("b")
	if err != nil {
		return Value{}, err
	}
	var r int64
	switch op {
	case "add":
		r, err = checkedAdd(x, y)
	case "subtract":
		r, err = checkedSub(x, y)
	case "multiply":
		r, err = checkedMul(x, y)
	case "divide":
		r, err = checkedDiv(x, y)
	case "mod":
		r, err = checkedMod(x, y)
	default:
		return Value{}, errArg("unsupported math op: " + op)
	}
	if err != nil {
		return Value{}, err
	}
	return Int(r), nil
}

func asciiUpper(s string) string {
	b := []byte(s)
	for i := 0; i < len(b); i++ {
		if b[i] >= 'a' && b[i] <= 'z' {
			b[i] -= 'a' - 'A'
		}
	}
	return string(b)
}

func asciiLower(s string) string {
	b := []byte(s)
	for i := 0; i < len(b); i++ {
		if b[i] >= 'A' && b[i] <= 'Z' {
			b[i] += 'a' - 'A'
		}
	}
	return string(b)
}

// ---------------------------------------------------------------------------
// map/filter declarative transforms and predicates

func applyTransform(t Value, elem Value) (Value, *opErr) {
	if t.Kind != KObj {
		return Value{}, errArg("transform must be an object")
	}
	tv, ok := t.O["type"]
	if !ok || tv.Kind != KStr {
		return Value{}, errArg("transform must have a string type")
	}
	switch tv.S {
	case "constant":
		if err := onlyKeys(t.O, "type", "value"); err != nil {
			return Value{}, err
		}
		v, ok := t.O["value"]
		if !ok {
			return Value{}, errArg("constant transform requires value")
		}
		return v, nil

	case "add", "subtract", "multiply", "divide", "mod":
		if err := onlyKeys(t.O, "type", "value"); err != nil {
			return Value{}, err
		}
		vv, ok := t.O["value"]
		if !ok {
			return Value{}, errArg("transform requires value")
		}
		y, ok := intValue(vv)
		if !ok {
			return Value{}, errArg("transform value must be an integer")
		}
		x, ok := intValue(elem)
		if !ok {
			return Value{}, errArg("transform element must be an integer")
		}
		var r int64
		var err *opErr
		switch tv.S {
		case "add":
			r, err = checkedAdd(x, y)
		case "subtract":
			r, err = checkedSub(x, y)
		case "multiply":
			r, err = checkedMul(x, y)
		case "divide":
			r, err = checkedDiv(x, y)
		case "mod":
			r, err = checkedMod(x, y)
		}
		if err != nil {
			return Value{}, err
		}
		return Int(r), nil

	case "negate":
		if err := onlyKeys(t.O, "type"); err != nil {
			return Value{}, err
		}
		x, ok := intValue(elem)
		if !ok {
			return Value{}, errArg("negate element must be an integer")
		}
		r, err := checkedNeg(x)
		if err != nil {
			return Value{}, err
		}
		return Int(r), nil

	case "abs":
		if err := onlyKeys(t.O, "type"); err != nil {
			return Value{}, err
		}
		x, ok := intValue(elem)
		if !ok {
			return Value{}, errArg("abs element must be an integer")
		}
		r, err := checkedAbs(x)
		if err != nil {
			return Value{}, err
		}
		return Int(r), nil

	case "stringify":
		if err := onlyKeys(t.O, "type"); err != nil {
			return Value{}, err
		}
		return Str(compactJSON(elem)), nil

	case "type":
		if err := onlyKeys(t.O, "type"); err != nil {
			return Value{}, err
		}
		return Str(typeName(elem)), nil

	case "op":
		if err := onlyKeys(t.O, "type", "op", "args"); err != nil {
			return Value{}, err
		}
		opv, ok := t.O["op"]
		if !ok || opv.Kind != KStr {
			return Value{}, errArg("op transform requires a string op")
		}
		switch opv.S {
		case "add", "subtract", "multiply", "divide", "mod":
		default:
			return Value{}, errArg("unsupported transform op: " + opv.S)
		}
		argsV := Obj(nil)
		if av, ok := t.O["args"]; ok {
			if av.Kind != KObj {
				return Value{}, errArg("op transform args must be an object")
			}
			argsV = av
		}
		resolved, err := replaceSelf(argsV, elem)
		if err != nil {
			return Value{}, err
		}
		a, err := newArgs(resolved)
		if err != nil {
			return Value{}, err
		}
		return applyMath2(opv.S, a)
	}
	return Value{}, errArg("unknown transform type: " + tv.S)
}

func replaceSelf(v Value, elem Value) (Value, *opErr) {
	switch v.Kind {
	case KObj:
		if len(v.O) == 1 {
			if s, ok := v.O["$self"]; ok && s.Kind == KBool && s.B {
				return elem, nil
			}
		}
		out := make(map[string]Value, len(v.O))
		for k, val := range v.O {
			rv, err := replaceSelf(val, elem)
			if err != nil {
				return Value{}, err
			}
			out[k] = rv
		}
		return Obj(out), nil
	case KArr:
		out := make([]Value, len(v.A))
		for i, val := range v.A {
			rv, err := replaceSelf(val, elem)
			if err != nil {
				return Value{}, err
			}
			out[i] = rv
		}
		return Arr(out), nil
	default:
		return v, nil
	}
}

func applyPredicate(p Value, elem Value) (bool, *opErr) {
	if p.Kind != KObj {
		return false, errArg("predicate must be an object")
	}
	tv, ok := p.O["type"]
	if !ok || tv.Kind != KStr {
		return false, errArg("predicate must have a string type")
	}

	// Support the type_is form even if a JSON document used a duplicate "type"
	// key (last-wins leaves only the target type name).
	if tv.S == "type_is" || isTypeName(tv.S) {
		if tv.S == "type_is" {
			target := ""
			found := false
			for _, name := range []string{"value", "type_name", "type_value", "target", "target_type", "arg", "name", "of", "expected"} {
				if v, ok := p.O[name]; ok {
					if v.Kind != KStr || !isTypeName(v.S) {
						return false, errArg("type_is requires a valid type name")
					}
					target = v.S
					found = true
					break
				}
			}
			if !found {
				// Fall back to any single extra field carrying a valid type name.
				for k, v := range p.O {
					if k == "type" {
						continue
					}
					if v.Kind == KStr && isTypeName(v.S) {
						target = v.S
						found = true
						break
					}
				}
			}
			if !found {
				return false, errArg("type_is requires a type name")
			}
			return typeName(elem) == target, nil
		}
		// Collapsed duplicate-key form: {"type": "<type name>"}.
		if len(p.O) != 1 {
			return false, errArg("invalid type_is predicate")
		}
		return typeName(elem) == tv.S, nil
	}

	switch tv.S {
	case "even", "odd":
		if err := onlyKeys(p.O, "type"); err != nil {
			return false, err
		}
		x, ok := intValue(elem)
		if !ok {
			return false, errArg("even/odd predicate requires an integer element")
		}
		if tv.S == "even" {
			return x%2 == 0, nil
		}
		return x%2 != 0, nil

	case "gt", "gte", "lt", "lte":
		if err := onlyKeys(p.O, "type", "value"); err != nil {
			return false, err
		}
		vv, ok := p.O["value"]
		if !ok {
			return false, errArg("predicate requires value")
		}
		y, ok := intValue(vv)
		if !ok {
			return false, errArg("predicate value must be an integer")
		}
		c := compareValues(elem, Int(y))
		switch tv.S {
		case "gt":
			return c > 0, nil
		case "gte":
			return c >= 0, nil
		case "lt":
			return c < 0, nil
		case "lte":
			return c <= 0, nil
		}

	case "eq", "ne":
		if err := onlyKeys(p.O, "type", "value"); err != nil {
			return false, err
		}
		vv, ok := p.O["value"]
		if !ok {
			return false, errArg("predicate requires value")
		}
		eq := equalValues(elem, vv)
		if tv.S == "eq" {
			return eq, nil
		}
		return !eq, nil

	case "truthy":
		if err := onlyKeys(p.O, "type"); err != nil {
			return false, err
		}
		return truthy(elem), nil

	case "falsy":
		if err := onlyKeys(p.O, "type"); err != nil {
			return false, err
		}
		return !truthy(elem), nil

	case "string_contains":
		if err := onlyKeys(p.O, "type", "value"); err != nil {
			return false, err
		}
		vv, ok := p.O["value"]
		if !ok || vv.Kind != KStr {
			return false, errArg("string_contains requires a string value")
		}
		if elem.Kind != KStr {
			return false, errArg("string_contains requires a string element")
		}
		return strings.Contains(elem.S, vv.S), nil
	}
	return false, errArg("unknown predicate type: " + tv.S)
}
