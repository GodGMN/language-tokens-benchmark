package main

import (
	"fmt"
	"regexp"
)

var idPattern = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*$`)

var validOps = map[string]bool{
	// math
	"add": true, "subtract": true, "multiply": true, "divide": true, "mod": true,
	"sum": true, "min": true, "max": true, "range": true,
	// strings
	"concat": true, "split": true, "join": true, "uppercase": true,
	"lowercase": true, "trim": true, "replace": true, "length": true,
	// arrays
	"map": true, "filter": true, "reduce": true, "sort": true,
	"reverse": true, "unique": true, "slice": true, "flatten": true,
	// objects
	"get": true, "set": true, "keys": true, "values": true,
	"merge": true, "pick": true, "omit": true,
	// utilities
	"constant": true, "equals": true, "compare": true,
	"and": true, "or": true, "not": true, "type": true,
	// testing
	"fail_until": true,
}

var stepFields = map[string]bool{
	"id": true, "op": true, "args": true, "depends_on": true,
	"when": true, "retry": true,
}

// Step is a validated workflow step.
type Step struct {
	ID          string
	Op          string
	Args        Value
	Deps        []string
	When        *Value
	MaxAttempts int
}

// wfError is either a structural (exit 2) or runtime (exit 1) failure.
type wfError struct {
	Code string
	Step *string
	Msg  string
	Exit int
}

func docError(code, msg string, exit int) *wfError {
	return &wfError{Code: code, Step: nil, Msg: msg, Exit: exit}
}

func stepError(code, id, msg string, exit int) *wfError {
	s := id
	return &wfError{Code: code, Step: &s, Msg: msg, Exit: exit}
}

func validateDocument(doc Value) ([]Step, *wfError) {
	if doc.Kind != KObj {
		return nil, docError("INVALID_WORKFLOW", "workflow must be a JSON object", 2)
	}
	for k := range doc.O {
		if k != "version" && k != "steps" {
			return nil, docError("INVALID_WORKFLOW", "unknown top-level field: "+k, 2)
		}
	}
	ver, ok := doc.O["version"]
	if !ok {
		return nil, docError("INVALID_WORKFLOW", "missing version", 2)
	}
	if ver.Kind != KNum || !ver.N.IsInt || ver.N.I != 1 {
		return nil, docError("INVALID_WORKFLOW", "version must be the integer literal 1", 2)
	}
	stepsV, ok := doc.O["steps"]
	if !ok {
		return nil, docError("INVALID_WORKFLOW", "missing steps", 2)
	}
	if stepsV.Kind != KArr {
		return nil, docError("INVALID_WORKFLOW", "steps must be an array", 2)
	}

	steps := make([]Step, 0, len(stepsV.A))
	for _, sv := range stepsV.A {
		st, err := parseStep(sv)
		if err != nil {
			return nil, err
		}
		steps = append(steps, st)
	}

	// Unique ids.
	byID := make(map[string]bool, len(steps))
	for _, st := range steps {
		if byID[st.ID] {
			return nil, stepError("INVALID_WORKFLOW", st.ID, "duplicate step id: "+st.ID, 2)
		}
		byID[st.ID] = true
	}

	// All dependencies must exist.
	for _, st := range steps {
		for _, d := range st.Deps {
			if !byID[d] {
				return nil, stepError("UNKNOWN_DEPENDENCY", st.ID, "unknown dependency: "+d, 2)
			}
		}
	}

	// Cycles.
	deps := make(map[string][]string, len(steps))
	for _, st := range steps {
		deps[st.ID] = st.Deps
	}
	color := make(map[string]int, len(steps))
	var visit func(string) bool
	visit = func(id string) bool {
		switch color[id] {
		case 1:
			return true
		case 2:
			return false
		}
		color[id] = 1
		for _, d := range deps[id] {
			if visit(d) {
				return true
			}
		}
		color[id] = 2
		return false
	}
	for _, st := range steps {
		if visit(st.ID) {
			return nil, docError("CYCLIC_DEPENDENCY", "dependency cycle detected", 2)
		}
	}

	return steps, nil
}

func parseStep(v Value) (Step, *wfError) {
	var st Step
	if v.Kind != KObj {
		return st, docError("INVALID_WORKFLOW", "step must be an object", 2)
	}

	// Extract a usable id early so errors can name the step.
	idStr, idIsStr := "", false
	if idv, ok := v.O["id"]; ok && idv.Kind == KStr {
		idStr, idIsStr = idv.S, true
	}
	stepPtr := func() *string {
		if idIsStr {
			s := idStr
			return &s
		}
		return nil
	}

	for k := range v.O {
		if !stepFields[k] {
			return st, &wfError{Code: "INVALID_WORKFLOW", Step: stepPtr(), Msg: "unknown step field: " + k, Exit: 2}
		}
	}

	if !idIsStr {
		return st, docError("INVALID_WORKFLOW", "step id must be a string", 2)
	}
	if !idPattern.MatchString(idStr) {
		return st, stepError("INVALID_WORKFLOW", idStr, "invalid step id: "+idStr, 2)
	}
	st.ID = idStr

	opv, ok := v.O["op"]
	if !ok {
		return st, stepError("INVALID_WORKFLOW", idStr, "missing op", 2)
	}
	if opv.Kind != KStr {
		return st, stepError("INVALID_WORKFLOW", idStr, "op must be a string", 2)
	}
	if !validOps[opv.S] {
		return st, stepError("INVALID_WORKFLOW", idStr, "unknown op: "+opv.S, 2)
	}
	st.Op = opv.S

	st.Args = Obj(nil)
	if av, ok := v.O["args"]; ok {
		if av.Kind != KObj {
			return st, stepError("INVALID_WORKFLOW", idStr, "args must be an object", 2)
		}
		st.Args = av
	}

	st.Deps = []string{}
	if dv, ok := v.O["depends_on"]; ok {
		if dv.Kind != KArr {
			return st, stepError("INVALID_WORKFLOW", idStr, "depends_on must be an array", 2)
		}
		for _, d := range dv.A {
			if d.Kind != KStr {
				return st, stepError("INVALID_WORKFLOW", idStr, "depends_on entries must be strings", 2)
			}
			st.Deps = append(st.Deps, d.S)
		}
	}

	if wv, ok := v.O["when"]; ok {
		if msg := validateWhen(wv); msg != "" {
			return st, stepError("INVALID_WORKFLOW", idStr, msg, 2)
		}
		w := wv
		st.When = &w
	}

	st.MaxAttempts = 1
	if rv, ok := v.O["retry"]; ok {
		if rv.Kind != KObj {
			return st, stepError("INVALID_WORKFLOW", idStr, "retry must be an object", 2)
		}
		for k := range rv.O {
			if k != "max_attempts" {
				return st, stepError("INVALID_WORKFLOW", idStr, "unknown retry field: "+k, 2)
			}
		}
		if mv, ok := rv.O["max_attempts"]; ok {
			if mv.Kind != KNum || !mv.N.IsInt || mv.N.I < 1 || mv.N.I > 5 {
				return st, stepError("INVALID_WORKFLOW", idStr, "max_attempts must be an integer 1..5", 2)
			}
			st.MaxAttempts = int(mv.N.I)
		}
	}

	return st, nil
}

// validateWhen returns a non-empty message for a structurally invalid condition.
func validateWhen(v Value) string {
	if v.Kind != KObj {
		// Literals are accepted structurally; they are checked for boolean-ness
		// at runtime (INVALID_CONDITION).
		return ""
	}
	if isRefObject(v) {
		return ""
	}
	opv, ok := v.O["op"]
	if !ok {
		return "condition object must have an op"
	}
	if opv.Kind != KStr {
		return "condition op must be a string"
	}
	switch opv.S {
	case "eq", "ne", "gt", "gte", "lt", "lte":
		for k := range v.O {
			if k != "op" && k != "left" && k != "right" {
				return "unknown condition field: " + k
			}
		}
		if _, ok := v.O["left"]; !ok {
			return "condition missing left"
		}
		if _, ok := v.O["right"]; !ok {
			return "condition missing right"
		}
		return ""
	case "all", "any":
		for k := range v.O {
			if k != "op" && k != "conditions" {
				return "unknown condition field: " + k
			}
		}
		cv, ok := v.O["conditions"]
		if !ok || cv.Kind != KArr {
			return "conditions must be an array"
		}
		for _, c := range cv.A {
			if msg := validateWhen(c); msg != "" {
				return msg
			}
		}
		return ""
	case "not":
		for k := range v.O {
			if k != "op" && k != "condition" {
				return "unknown condition field: " + k
			}
		}
		cv, ok := v.O["condition"]
		if !ok {
			return "not condition missing condition"
		}
		return validateWhen(cv)
	default:
		return fmt.Sprintf("unknown condition op: %s", opv.S)
	}
}
