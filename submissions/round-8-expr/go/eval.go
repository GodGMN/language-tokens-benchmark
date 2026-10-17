package main

import (
	"math"
	"strconv"
	"strings"
	"unicode/utf8"
)

var arities = map[string]int{
	"len":         1,
	"sum":         1,
	"min":         1,
	"max":         1,
	"abs":         1,
	"neg":         1,
	"upper":       1,
	"lower":       1,
	"trim":        1,
	"contains":    2,
	"starts_with": 2,
	"ends_with":   2,
	"substr":      3,
	"index_of":    2,
	"join":        2,
	"split":       2,
	"type":        1,
	"to_string":   1,
}

func addInt64(a, b int64) (int64, bool) {
	if (b > 0 && a > math.MaxInt64-b) || (b < 0 && a < math.MinInt64-b) {
		return 0, true
	}
	return a + b, false
}

func subInt64(a, b int64) (int64, bool) {
	if (b < 0 && a > math.MaxInt64+b) || (b > 0 && a < math.MinInt64+b) {
		return 0, true
	}
	return a - b, false
}

func mulInt64(a, b int64) (int64, bool) {
	if a == 0 || b == 0 {
		return 0, false
	}
	if (a == math.MinInt64 && b == -1) || (b == math.MinInt64 && a == -1) {
		return 0, true
	}
	r := a * b
	if r/b != a {
		return 0, true
	}
	return r, false
}

func evalNode(n Node, vars map[string]Value) (Value, *ExprError) {
	switch x := n.(type) {
	case *IntLit:
		return vInt(x.Val), nil
	case *StrLit:
		return vStr(x.Val), nil
	case *BoolLit:
		return vBool(x.Val), nil
	case *NullLit:
		return vNull(), nil
	case *Var:
		v, ok := vars[x.Name]
		if !ok {
			return Value{}, newErr(codeUnknownVar, x.Pos, "unknown variable %q", x.Name)
		}
		return v, nil
	case *ArrayLit:
		elems := make([]Value, 0, len(x.Elems))
		for _, e := range x.Elems {
			v, err := evalNode(e, vars)
			if err != nil {
				return Value{}, err
			}
			elems = append(elems, v)
		}
		return vArr(elems), nil
	case *Unary:
		v, err := evalNode(x.Operand, vars)
		if err != nil {
			return Value{}, err
		}
		switch x.Op {
		case tMinus:
			if v.Kind != KInt {
				return Value{}, newErr(codeType, x.Pos, "unary '-' requires an integer")
			}
			if v.Int == math.MinInt64 {
				return Value{}, newErr(codeOverflow, x.Pos, "integer overflow in unary '-'")
			}
			return vInt(-v.Int), nil
		case tBang:
			if v.Kind != KBool {
				return Value{}, newErr(codeType, x.Pos, "unary '!' requires a boolean")
			}
			return vBool(!v.Bool), nil
		}
		return Value{}, newErr("INTERNAL", x.Pos, "bad unary operator")
	case *Binary:
		return evalBinary(x, vars)
	case *Ternary:
		cond, err := evalNode(x.Cond, vars)
		if err != nil {
			return Value{}, err
		}
		if cond.Kind != KBool {
			return Value{}, newErr(codeType, x.Pos, "ternary condition must be a boolean")
		}
		if cond.Bool {
			return evalNode(x.Then, vars)
		}
		return evalNode(x.Else, vars)
	case *Call:
		return evalCall(x, vars)
	}
	return Value{}, newErr("INTERNAL", 0, "unknown AST node")
}

func evalBinary(x *Binary, vars map[string]Value) (Value, *ExprError) {
	left, err := evalNode(x.Left, vars)
	if err != nil {
		return Value{}, err
	}

	// Logical operators short-circuit.
	switch x.Op {
	case tAndAnd:
		if left.Kind != KBool {
			return Value{}, newErr(codeType, x.Pos, "'&&' requires boolean operands")
		}
		if !left.Bool {
			return vBool(false), nil
		}
		right, err := evalNode(x.Right, vars)
		if err != nil {
			return Value{}, err
		}
		if right.Kind != KBool {
			return Value{}, newErr(codeType, x.Pos, "'&&' requires boolean operands")
		}
		return vBool(right.Bool), nil
	case tOrOr:
		if left.Kind != KBool {
			return Value{}, newErr(codeType, x.Pos, "'||' requires boolean operands")
		}
		if left.Bool {
			return vBool(true), nil
		}
		right, err := evalNode(x.Right, vars)
		if err != nil {
			return Value{}, err
		}
		if right.Kind != KBool {
			return Value{}, newErr(codeType, x.Pos, "'||' requires boolean operands")
		}
		return vBool(right.Bool), nil
	}

	right, err := evalNode(x.Right, vars)
	if err != nil {
		return Value{}, err
	}

	switch x.Op {
	case tPlus, tMinus, tStar, tSlash, tPercent:
		if left.Kind != KInt || right.Kind != KInt {
			return Value{}, newErr(codeType, x.Pos, "arithmetic operator requires integer operands")
		}
		a, b := left.Int, right.Int
		switch x.Op {
		case tPlus:
			r, ov := addInt64(a, b)
			if ov {
				return Value{}, newErr(codeOverflow, x.Pos, "integer overflow in '+'")
			}
			return vInt(r), nil
		case tMinus:
			r, ov := subInt64(a, b)
			if ov {
				return Value{}, newErr(codeOverflow, x.Pos, "integer overflow in '-'")
			}
			return vInt(r), nil
		case tStar:
			r, ov := mulInt64(a, b)
			if ov {
				return Value{}, newErr(codeOverflow, x.Pos, "integer overflow in '*'")
			}
			return vInt(r), nil
		case tSlash:
			if b == 0 {
				return Value{}, newErr(codeDivisionByZero, x.Pos, "division by zero")
			}
			if a == math.MinInt64 && b == -1 {
				return Value{}, newErr(codeOverflow, x.Pos, "integer overflow in '/'")
			}
			return vInt(a / b), nil
		case tPercent:
			if b == 0 {
				return Value{}, newErr(codeDivisionByZero, x.Pos, "modulo by zero")
			}
			if a == math.MinInt64 && b == -1 {
				return Value{}, newErr(codeOverflow, x.Pos, "integer overflow in modulo")
			}
			return vInt(a % b), nil
		}
	case tEqEq:
		return vBool(valuesEqual(left, right)), nil
	case tNotEq:
		return vBool(!valuesEqual(left, right)), nil
	case tLt, tLtEq, tGt, tGtEq:
		switch {
		case left.Kind == KInt && right.Kind == KInt:
			a, b := left.Int, right.Int
			switch x.Op {
			case tLt:
				return vBool(a < b), nil
			case tLtEq:
				return vBool(a <= b), nil
			case tGt:
				return vBool(a > b), nil
			default:
				return vBool(a >= b), nil
			}
		case left.Kind == KStr && right.Kind == KStr:
			a, b := left.Str, right.Str
			switch x.Op {
			case tLt:
				return vBool(a < b), nil
			case tLtEq:
				return vBool(a <= b), nil
			case tGt:
				return vBool(a > b), nil
			default:
				return vBool(a >= b), nil
			}
		default:
			return Value{}, newErr(codeType, x.Pos, "comparison requires two integers or two strings")
		}
	}
	return Value{}, newErr("INTERNAL", x.Pos, "bad binary operator")
}

func evalCall(x *Call, vars map[string]Value) (Value, *ExprError) {
	arity, known := arities[x.Name]
	if !known {
		return Value{}, newErr(codeUnknownFunc, x.Pos, "unknown function %q", x.Name)
	}
	if len(x.Args) != arity {
		return Value{}, newErr(codeArity, x.Pos, "function %q expects %d argument(s), got %d", x.Name, arity, len(x.Args))
	}
	args := make([]Value, len(x.Args))
	for i, a := range x.Args {
		v, err := evalNode(a, vars)
		if err != nil {
			return Value{}, err
		}
		args[i] = v
	}
	return callFunction(x.Name, x.Pos, args)
}

func callFunction(name string, pos int, args []Value) (Value, *ExprError) {
	typeErr := func(msg string) (Value, *ExprError) {
		return Value{}, newErr(codeType, pos, "%s", msg)
	}

	switch name {
	case "len":
		switch args[0].Kind {
		case KStr:
			return vInt(int64(utf8.RuneCountInString(args[0].Str))), nil
		case KArr:
			return vInt(int64(len(args[0].Arr))), nil
		}
		return typeErr("len expects a string or array")

	case "sum":
		if args[0].Kind != KArr {
			return typeErr("sum expects an array of integers")
		}
		var total int64
		for _, e := range args[0].Arr {
			if e.Kind != KInt {
				return typeErr("sum expects an array of integers")
			}
			r, ov := addInt64(total, e.Int)
			if ov {
				return Value{}, newErr(codeOverflow, pos, "integer overflow in sum")
			}
			total = r
		}
		return vInt(total), nil

	case "min", "max":
		if args[0].Kind != KArr {
			return typeErr(name + " expects an array of integers")
		}
		arr := args[0].Arr
		if len(arr) == 0 {
			return Value{}, newErr(codeEmptyArray, pos, name+" of an empty array")
		}
		for _, e := range arr {
			if e.Kind != KInt {
				return typeErr(name + " expects an array of integers")
			}
		}
		result := arr[0].Int
		for _, e := range arr {
			if name == "min" {
				if e.Int < result {
					result = e.Int
				}
			} else {
				if e.Int > result {
					result = e.Int
				}
			}
		}
		return vInt(result), nil

	case "abs":
		if args[0].Kind != KInt {
			return typeErr("abs expects an integer")
		}
		if args[0].Int == math.MinInt64 {
			return Value{}, newErr(codeOverflow, pos, "integer overflow in abs")
		}
		if args[0].Int < 0 {
			return vInt(-args[0].Int), nil
		}
		return vInt(args[0].Int), nil

	case "neg":
		if args[0].Kind != KInt {
			return typeErr("neg expects an integer")
		}
		if args[0].Int == math.MinInt64 {
			return Value{}, newErr(codeOverflow, pos, "integer overflow in neg")
		}
		return vInt(-args[0].Int), nil

	case "upper":
		if args[0].Kind != KStr {
			return typeErr("upper expects a string")
		}
		s := args[0].Str
		out := make([]byte, len(s))
		for i := 0; i < len(s); i++ {
			c := s[i]
			if c >= 'a' && c <= 'z' {
				c = c - 'a' + 'A'
			}
			out[i] = c
		}
		return vStr(string(out)), nil

	case "lower":
		if args[0].Kind != KStr {
			return typeErr("lower expects a string")
		}
		s := args[0].Str
		out := make([]byte, len(s))
		for i := 0; i < len(s); i++ {
			c := s[i]
			if c >= 'A' && c <= 'Z' {
				c = c - 'A' + 'a'
			}
			out[i] = c
		}
		return vStr(string(out)), nil

	case "trim":
		if args[0].Kind != KStr {
			return typeErr("trim expects a string")
		}
		return vStr(strings.Trim(args[0].Str, " \t\n\r\f\v")), nil

	case "contains":
		if args[0].Kind != KStr || args[1].Kind != KStr {
			return typeErr("contains expects two strings")
		}
		return vBool(strings.Contains(args[0].Str, args[1].Str)), nil

	case "starts_with":
		if args[0].Kind != KStr || args[1].Kind != KStr {
			return typeErr("starts_with expects two strings")
		}
		return vBool(strings.HasPrefix(args[0].Str, args[1].Str)), nil

	case "ends_with":
		if args[0].Kind != KStr || args[1].Kind != KStr {
			return typeErr("ends_with expects two strings")
		}
		return vBool(strings.HasSuffix(args[0].Str, args[1].Str)), nil

	case "substr":
		if args[0].Kind != KStr || args[1].Kind != KInt || args[2].Kind != KInt {
			return typeErr("substr expects a string, an integer and an integer")
		}
		if args[2].Int < 0 {
			return typeErr("substr length must not be negative")
		}
		runes := []rune(args[0].Str)
		n := int64(len(runes))
		start := args[1].Int
		length := args[2].Int
		if start < 0 {
			start = n + start
		}
		if start < 0 {
			start = 0
		}
		if start > n {
			start = n
		}
		var end int64
		if length > n-start {
			end = n
		} else {
			end = start + length
		}
		return vStr(string(runes[start:end])), nil

	case "index_of":
		if args[0].Kind != KStr || args[1].Kind != KStr {
			return typeErr("index_of expects two strings")
		}
		s, sub := args[0].Str, args[1].Str
		idx := strings.Index(s, sub)
		if idx < 0 {
			return vInt(-1), nil
		}
		return vInt(int64(utf8.RuneCountInString(s[:idx]))), nil

	case "join":
		if args[0].Kind != KArr {
			return typeErr("join expects an array of strings and a string")
		}
		if args[1].Kind != KStr {
			return typeErr("join expects an array of strings and a string")
		}
		parts := make([]string, 0, len(args[0].Arr))
		for _, e := range args[0].Arr {
			if e.Kind != KStr {
				return typeErr("join expects an array of strings and a string")
			}
			parts = append(parts, e.Str)
		}
		return vStr(strings.Join(parts, args[1].Str)), nil

	case "split":
		if args[0].Kind != KStr || args[1].Kind != KStr {
			return typeErr("split expects two strings")
		}
		s, sep := args[0].Str, args[1].Str
		if sep == "" {
			var out []Value
			for _, r := range s {
				out = append(out, vStr(string(r)))
			}
			return vArr(out), nil
		}
		parts := strings.Split(s, sep)
		out := make([]Value, 0, len(parts))
		for _, p := range parts {
			out = append(out, vStr(p))
		}
		return vArr(out), nil

	case "type":
		return vStr(args[0].typeName()), nil

	case "to_string":
		if args[0].Kind == KStr {
			return vStr(args[0].Str), nil
		}
		if args[0].Kind == KInt {
			return vStr(strconv.FormatInt(args[0].Int, 10)), nil
		}
		return vStr(encodeValue(args[0])), nil
	}
	return Value{}, newErr("INTERNAL", pos, "unhandled function %q", name)
}
