package main

// evalConditionValue evaluates a condition (already fully reference-resolved).
// It returns the resulting value; the caller enforces that it is a boolean.
func evalConditionValue(v Value) (Value, *opErr) {
	if v.Kind != KObj {
		return v, nil
	}
	opv, ok := v.O["op"]
	if !ok {
		// A resolved object that does not look like a condition is treated as a
		// literal value (and will fail the boolean check at the call site).
		return v, nil
	}
	if opv.Kind != KStr {
		return Value{}, errCond("condition op must be a string")
	}
	switch opv.S {
	case "eq", "ne", "gt", "gte", "lt", "lte":
		left, lok := v.O["left"]
		right, rok := v.O["right"]
		if !lok || !rok {
			return Value{}, errCond("condition missing left or right")
		}
		switch opv.S {
		case "eq":
			return Bool(equalValues(left, right)), nil
		case "ne":
			return Bool(!equalValues(left, right)), nil
		case "gt":
			return Bool(compareValues(left, right) > 0), nil
		case "gte":
			return Bool(compareValues(left, right) >= 0), nil
		case "lt":
			return Bool(compareValues(left, right) < 0), nil
		case "lte":
			return Bool(compareValues(left, right) <= 0), nil
		}
	case "all":
		cv, ok := v.O["conditions"]
		if !ok || cv.Kind != KArr {
			return Value{}, errCond("all condition requires a conditions array")
		}
		for _, c := range cv.A {
			rv, err := evalConditionValue(c)
			if err != nil {
				return Value{}, err
			}
			if rv.Kind != KBool {
				return Value{}, errCond("nested condition did not evaluate to a boolean")
			}
			if !rv.B {
				return Bool(false), nil
			}
		}
		return Bool(true), nil
	case "any":
		cv, ok := v.O["conditions"]
		if !ok || cv.Kind != KArr {
			return Value{}, errCond("any condition requires a conditions array")
		}
		for _, c := range cv.A {
			rv, err := evalConditionValue(c)
			if err != nil {
				return Value{}, err
			}
			if rv.Kind != KBool {
				return Value{}, errCond("nested condition did not evaluate to a boolean")
			}
			if rv.B {
				return Bool(true), nil
			}
		}
		return Bool(false), nil
	case "not":
		cv, ok := v.O["condition"]
		if !ok {
			return Value{}, errCond("not condition requires a condition")
		}
		rv, err := evalConditionValue(cv)
		if err != nil {
			return Value{}, err
		}
		if rv.Kind != KBool {
			return Value{}, errCond("nested condition did not evaluate to a boolean")
		}
		return Bool(!rv.B), nil
	default:
		return Value{}, errCond("unknown condition op: " + opv.S)
	}
	return Value{}, errCond("invalid condition")
}
