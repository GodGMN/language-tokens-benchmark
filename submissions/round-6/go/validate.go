package main

import (
	"encoding/json"
	"fmt"
)

// Error is the response error object.
type Error struct {
	Code    string  `json:"code"`
	Step    *string `json:"step"`
	Message string  `json:"message"`
}

func strPtr(s string) *string { return &s }

func errNoStep(code, format string, a ...any) *Error {
	return &Error{Code: code, Message: fmt.Sprintf(format, a...)}
}

func errAtStep(code, step, format string, a ...any) *Error {
	return &Error{Code: code, Step: strPtr(step), Message: fmt.Sprintf(format, a...)}
}

// Step is a fully validated workflow step.
type Step struct {
	ID          string
	Op          string
	Args        map[string]any
	Deps        []string
	When        any
	HasWhen     bool
	MaxAttempts int
	Index       int
}

// Workflow is a fully validated workflow.
type Workflow struct {
	Steps   []*Step
	ByID    map[string]*Step
	Closure map[string]map[string]bool // transitive dependencies, excluding self
}

var opNames = map[string]bool{
	// math
	"add": true, "subtract": true, "multiply": true, "divide": true, "mod": true,
	"sum": true, "min": true, "max": true, "range": true,
	// strings
	"concat": true, "split": true, "join": true, "uppercase": true, "lowercase": true,
	"trim": true, "replace": true, "length": true,
	// arrays
	"map": true, "filter": true, "reduce": true, "sort": true, "reverse": true,
	"unique": true, "slice": true, "flatten": true,
	// objects
	"get": true, "set": true, "keys": true, "values": true, "merge": true,
	"pick": true, "omit": true,
	// utilities
	"constant": true, "equals": true, "compare": true, "and": true, "or": true,
	"not": true, "type": true,
	// test
	"fail_until": true,
}

func validID(s string) bool {
	if len(s) == 0 {
		return false
	}
	c := s[0]
	if !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
		return false
	}
	for i := 1; i < len(s); i++ {
		c = s[i]
		if !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') || c == '_') {
			return false
		}
	}
	return true
}

func onlyKeys(m map[string]any, allowed ...string) bool {
	set := make(map[string]bool, len(allowed))
	for _, k := range allowed {
		set[k] = true
	}
	for k := range m {
		if !set[k] {
			return false
		}
	}
	return true
}

// validateConditionShape performs the structural checks on a `when` condition.
func validateConditionShape(c any) bool {
	m, ok := c.(map[string]any)
	if !ok {
		return false
	}
	opv, ok := m["op"]
	if !ok {
		return false
	}
	op, ok := opv.(string)
	if !ok {
		return false
	}
	switch op {
	case "all", "any":
		if !onlyKeys(m, "op", "conditions") {
			return false
		}
		arr, ok := m["conditions"].([]any)
		if !ok {
			return false
		}
		for _, sub := range arr {
			if !validateConditionShape(sub) {
				return false
			}
		}
		return true
	case "not":
		if !onlyKeys(m, "op", "condition") {
			return false
		}
		return validateConditionShape(m["condition"])
	case "eq", "ne", "gt", "gte", "lt", "lte":
		if !onlyKeys(m, "op", "left", "right") {
			return false
		}
		if _, ok := m["left"]; !ok {
			return false
		}
		if _, ok := m["right"]; !ok {
			return false
		}
		return true
	default:
		return false
	}
}

// validateWorkflow validates the whole document before any step runs.
func validateWorkflow(root any) (*Workflow, *Error) {
	m, ok := root.(map[string]any)
	if !ok {
		return nil, errNoStep("INVALID_WORKFLOW", "workflow document must be an object")
	}

	for k := range m {
		if k != "version" && k != "steps" {
			return nil, errNoStep("INVALID_WORKFLOW", "unknown top-level field %q", k)
		}
	}

	vv, ok := m["version"]
	if !ok {
		return nil, errNoStep("INVALID_WORKFLOW", "missing workflow version")
	}
	vn, ok := vv.(json.Number)
	if !ok || vn.String() != "1" {
		return nil, errNoStep("INVALID_WORKFLOW", "version must be the integer literal 1")
	}

	sv, ok := m["steps"]
	if !ok {
		return nil, errNoStep("INVALID_WORKFLOW", "missing steps array")
	}
	stepArr, ok := sv.([]any)
	if !ok {
		return nil, errNoStep("INVALID_WORKFLOW", "steps must be an array")
	}

	wf := &Workflow{ByID: make(map[string]*Step, len(stepArr))}
	allowedStep := map[string]bool{
		"id": true, "op": true, "args": true,
		"depends_on": true, "when": true, "retry": true,
	}

	for idx, raw := range stepArr {
		sm, ok := raw.(map[string]any)
		if !ok {
			return nil, errNoStep("INVALID_WORKFLOW", "step %d must be an object", idx)
		}

		idVal, hasID := sm["id"]
		idStr, idIsString := idVal.(string)
		invalidStep := func(format string, a ...any) *Error {
			if idIsString {
				return errAtStep("INVALID_WORKFLOW", idStr, format, a...)
			}
			return errNoStep("INVALID_WORKFLOW", format, a...)
		}

		for k := range sm {
			if !allowedStep[k] {
				return nil, invalidStep("unknown step field %q", k)
			}
		}
		if !hasID {
			return nil, invalidStep("step is missing id")
		}
		if !idIsString {
			return nil, invalidStep("step id must be a string")
		}
		if !validID(idStr) {
			return nil, invalidStep("invalid step id %q", idStr)
		}
		if _, dup := wf.ByID[idStr]; dup {
			return nil, invalidStep("duplicate step id %q", idStr)
		}

		opVal, hasOp := sm["op"]
		if !hasOp {
			return nil, invalidStep("step %q is missing op", idStr)
		}
		opStr, opIsString := opVal.(string)
		if !opIsString || !opNames[opStr] {
			return nil, invalidStep("unknown op for step %q", idStr)
		}

		args := map[string]any{}
		if av, ok := sm["args"]; ok {
			am, ok := av.(map[string]any)
			if !ok {
				return nil, invalidStep("args of step %q must be an object", idStr)
			}
			args = am
		}

		var deps []string
		if dv, ok := sm["depends_on"]; ok {
			arr, ok := dv.([]any)
			if !ok {
				return nil, invalidStep("depends_on of step %q must be an array", idStr)
			}
			for _, d := range arr {
				ds, ok := d.(string)
				if !ok {
					return nil, invalidStep("depends_on of step %q must contain strings", idStr)
				}
				deps = append(deps, ds)
			}
		}

		step := &Step{
			ID:          idStr,
			Op:          opStr,
			Args:        args,
			Deps:        deps,
			MaxAttempts: 1,
			Index:       idx,
		}

		if wv, ok := sm["when"]; ok {
			if !validateConditionShape(wv) {
				return nil, invalidStep("invalid when condition on step %q", idStr)
			}
			step.When = wv
			step.HasWhen = true
		}

		if rv, ok := sm["retry"]; ok {
			rm, ok := rv.(map[string]any)
			if !ok {
				return nil, invalidStep("retry of step %q must be an object", idStr)
			}
			for k := range rm {
				if k != "max_attempts" {
					return nil, invalidStep("unknown retry field %q", k)
				}
			}
			if mv, ok := rm["max_attempts"]; ok {
				n, ok := asI64(mv)
				if !ok || n < 1 || n > 5 {
					return nil, invalidStep("max_attempts of step %q must be an integer from 1 to 5", idStr)
				}
				step.MaxAttempts = int(n)
			}
		}

		wf.Steps = append(wf.Steps, step)
		wf.ByID[idStr] = step
	}

	// Every dependency must exist.
	for _, s := range wf.Steps {
		for _, d := range s.Deps {
			if _, ok := wf.ByID[d]; !ok {
				return nil, errAtStep("UNKNOWN_DEPENDENCY", s.ID, "step %q depends on unknown step %q", s.ID, d)
			}
		}
	}

	// Detect cycles before execution.
	color := make(map[string]int, len(wf.ByID))
	var visit func(string) bool
	visit = func(id string) bool {
		switch color[id] {
		case 1:
			return false
		case 2:
			return true
		}
		color[id] = 1
		for _, d := range wf.ByID[id].Deps {
			if !visit(d) {
				return false
			}
		}
		color[id] = 2
		return true
	}
	for _, s := range wf.Steps {
		if !visit(s.ID) {
			return nil, errNoStep("CYCLIC_DEPENDENCY", "workflow contains a dependency cycle")
		}
	}

	wf.Closure = computeClosure(wf)
	return wf, nil
}

func computeClosure(wf *Workflow) map[string]map[string]bool {
	memo := make(map[string]map[string]bool, len(wf.ByID))
	var visit func(string) map[string]bool
	visit = func(id string) map[string]bool {
		if c, ok := memo[id]; ok {
			return c
		}
		res := make(map[string]bool)
		memo[id] = res
		for _, d := range wf.ByID[id].Deps {
			res[d] = true
			for k := range visit(d) {
				res[k] = true
			}
		}
		return res
	}
	for id := range wf.ByID {
		visit(id)
	}
	return memo
}
