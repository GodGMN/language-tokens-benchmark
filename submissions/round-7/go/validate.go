package main

import (
	"encoding/json"
	"regexp"
)

var idPattern = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*$`)

type stepDef struct {
	ID          string
	Op          string
	Args        map[string]interface{}
	DependsOn   []string
	When        interface{} // nil when absent
	MaxAttempts int
	Index       int
	Closure     map[string]bool // transitive depends_on (excluding self)
}

func validateWorkflow(root interface{}) ([]*stepDef, *wfError) {
	obj, ok := root.(map[string]interface{})
	if !ok {
		return nil, errf(ErrInvalidWorkflow, "workflow document must be a JSON object")
	}
	for k := range obj {
		if k != "version" && k != "steps" {
			return nil, errf(ErrInvalidWorkflow, "unknown top-level field %q", k)
		}
	}

	ver, ok := obj["version"]
	if !ok {
		return nil, errf(ErrInvalidWorkflow, "missing required field \"version\"")
	}
	vn, ok := ver.(json.Number)
	if !ok {
		return nil, errf(ErrInvalidWorkflow, "\"version\" must be the integer literal 1")
	}
	vi, ok := numToI64(vn)
	if !ok || vi != 1 {
		return nil, errf(ErrInvalidWorkflow, "\"version\" must be the integer literal 1")
	}

	sv, ok := obj["steps"]
	if !ok {
		return nil, errf(ErrInvalidWorkflow, "missing required field \"steps\"")
	}
	stepArr, ok := sv.([]interface{})
	if !ok {
		return nil, errf(ErrInvalidWorkflow, "\"steps\" must be an array")
	}

	steps := make([]*stepDef, 0, len(stepArr))
	seen := map[string]bool{}
	for i, raw := range stepArr {
		sm, ok := raw.(map[string]interface{})
		if !ok {
			return nil, errf(ErrInvalidWorkflow, "step %d must be an object", i)
		}
		// Determine the id for error reporting: a string id is reported even
		// when it is invalid; anything else yields null.
		var idForErr *string
		if idv, ok := sm["id"]; ok {
			if s, ok := idv.(string); ok {
				idForErr = &s
			}
		}
		fail := func(msg string) *wfError {
			return &wfError{Code: ErrInvalidWorkflow, Msg: msg, stepID: idForErr}
		}

		for k := range sm {
			switch k {
			case "id", "op", "args", "depends_on", "when", "retry":
			default:
				return nil, fail("unknown step field " + k)
			}
		}

		idv, ok := sm["id"]
		if !ok {
			return nil, fail("step is missing \"id\"")
		}
		id, ok := idv.(string)
		if !ok {
			return nil, fail("step \"id\" must be a string")
		}
		if !idPattern.MatchString(id) {
			return nil, fail("step id " + id + " is not a valid identifier")
		}
		if seen[id] {
			return nil, fail("duplicate step id " + id)
		}
		seen[id] = true

		opv, ok := sm["op"]
		if !ok {
			return nil, fail("step " + id + " is missing \"op\"")
		}
		op, ok := opv.(string)
		if !ok {
			return nil, fail("step " + id + " \"op\" must be a string")
		}
		if _, ok := opTable[op]; !ok {
			return nil, fail("step " + id + " has unknown op " + op)
		}

		args := map[string]interface{}{}
		if av, ok := sm["args"]; ok {
			am, ok := av.(map[string]interface{})
			if !ok {
				return nil, fail("step " + id + " \"args\" must be an object")
			}
			args = am
		}

		deps := []string{}
		if dv, ok := sm["depends_on"]; ok {
			da, ok := dv.([]interface{})
			if !ok {
				return nil, fail("step " + id + " \"depends_on\" must be an array")
			}
			for _, d := range da {
				s, ok := d.(string)
				if !ok {
					return nil, fail("step " + id + " \"depends_on\" entries must be strings")
				}
				deps = append(deps, s)
			}
		}

		var when interface{}
		if wv, ok := sm["when"]; ok {
			if werr := validateCondition(wv); werr != nil {
				return nil, &wfError{Code: ErrInvalidWorkflow, Msg: "step " + id + ": " + werr.Msg, stepID: &id}
			}
			when = wv
		}

		maxAttempts := 1
		if rv, ok := sm["retry"]; ok {
			rm, ok := rv.(map[string]interface{})
			if !ok {
				return nil, fail("step " + id + " \"retry\" must be an object")
			}
			for k := range rm {
				if k != "max_attempts" {
					return nil, fail("step " + id + " retry has unknown field " + k)
				}
			}
			mv, ok := rm["max_attempts"]
			if !ok {
				return nil, fail("step " + id + " retry is missing \"max_attempts\"")
			}
			mn, ok := mv.(json.Number)
			if !ok {
				return nil, fail("step " + id + " retry.max_attempts must be an integer")
			}
			mi, ok := numToI64(mn)
			if !ok || mi < 1 || mi > 5 {
				return nil, fail("step " + id + " retry.max_attempts must be an integer between 1 and 5")
			}
			maxAttempts = int(mi)
		}

		steps = append(steps, &stepDef{
			ID:          id,
			Op:          op,
			Args:        args,
			DependsOn:   deps,
			When:        when,
			MaxAttempts: maxAttempts,
			Index:       i,
		})
	}

	// Dependency existence.
	byID := make(map[string]*stepDef, len(steps))
	for _, s := range steps {
		byID[s.ID] = s
	}
	for _, s := range steps {
		for _, d := range s.DependsOn {
			if _, ok := byID[d]; !ok {
				id := s.ID
				return nil, &wfError{Code: ErrUnknownDependency, Msg: "step " + s.ID + " depends on unknown step " + d, stepID: &id}
			}
		}
	}

	// Cycle detection (iterative DFS with colors).
	const (
		white = 0
		gray  = 1
		black = 2
	)
	color := map[string]int{}
	var visit func(id string) bool
	visit = func(id string) bool {
		color[id] = gray
		for _, d := range byID[id].DependsOn {
			switch color[d] {
			case gray:
				return true
			case white:
				if visit(d) {
					return true
				}
			}
		}
		color[id] = black
		return false
	}
	for _, s := range steps {
		if color[s.ID] == white {
			if visit(s.ID) {
				return nil, &wfError{Code: ErrCyclicDependency, Msg: "dependency cycle detected"}
			}
		}
	}

	// Transitive closures.
	for _, s := range steps {
		cl := map[string]bool{}
		var walk func(id string)
		walk = func(id string) {
			for _, d := range byID[id].DependsOn {
				if !cl[d] {
					cl[d] = true
					walk(d)
				}
			}
		}
		walk(s.ID)
		s.Closure = cl
	}

	return steps, nil
}

func validateCondition(v interface{}) *wfError {
	// A condition may also be a plain boolean value or a $ref to a boolean
	// result; those are checked for strict booleanness at evaluation time.
	if _, ok := v.(bool); ok {
		return nil
	}
	if _, ok := asRef(v); ok {
		return nil
	}
	m, ok := v.(map[string]interface{})
	if !ok {
		return errf(ErrInvalidWorkflow, "condition must be an object")
	}
	opv, ok := m["op"]
	if !ok {
		return errf(ErrInvalidWorkflow, "condition is missing \"op\"")
	}
	op, ok := opv.(string)
	if !ok {
		return errf(ErrInvalidWorkflow, "condition \"op\" must be a string")
	}
	switch op {
	case "eq", "ne", "gt", "gte", "lt", "lte":
		if _, ok := m["left"]; !ok {
			return errf(ErrInvalidWorkflow, "comparison condition %q is missing \"left\"", op)
		}
		if _, ok := m["right"]; !ok {
			return errf(ErrInvalidWorkflow, "comparison condition %q is missing \"right\"", op)
		}
	case "all", "any":
		cv, ok := m["conditions"]
		if !ok {
			return errf(ErrInvalidWorkflow, "condition %q is missing \"conditions\"", op)
		}
		arr, ok := cv.([]interface{})
		if !ok {
			return errf(ErrInvalidWorkflow, "condition %q \"conditions\" must be an array", op)
		}
		for _, c := range arr {
			if e := validateCondition(c); e != nil {
				return e
			}
		}
	case "not":
		cv, ok := m["condition"]
		if !ok {
			return errf(ErrInvalidWorkflow, "condition \"not\" is missing \"condition\"")
		}
		if e := validateCondition(cv); e != nil {
			return e
		}
	default:
		return errf(ErrInvalidWorkflow, "unknown condition op %q", op)
	}
	return nil
}
