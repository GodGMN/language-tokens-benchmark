package main

import (
	"regexp"
)

// Err is a workflow error with an optional responsible step.
type Err struct {
	Code    string
	Step    *string
	Message string
}

// Step is a structurally validated workflow step.
type Step struct {
	ID          string
	Op          string
	Args        map[string]interface{}
	Deps        []string
	DepSet      map[string]bool // direct and transitive dependencies
	When        interface{}
	HasWhen     bool
	MaxAttempts int
	Index       int
}

// Document is a structurally validated workflow.
type Document struct {
	Steps []*Step
	ByID  map[string]*Step
}

var idPattern = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*$`)

var knownOps = map[string]bool{
	"add": true, "subtract": true, "multiply": true, "divide": true, "mod": true,
	"sum": true, "min": true, "max": true, "range": true,
	"concat": true, "split": true, "join": true, "uppercase": true,
	"lowercase": true, "trim": true, "replace": true, "length": true,
	"map": true, "filter": true, "reduce": true, "sort": true,
	"reverse": true, "unique": true, "slice": true, "flatten": true,
	"get": true, "set": true, "keys": true, "values": true, "merge": true,
	"pick": true, "omit": true,
	"constant": true, "equals": true, "compare": true,
	"and": true, "or": true, "not": true, "type": true,
	"fail_until": true,
}

func errStep(code string, sid *string, msg string) *Err {
	return &Err{Code: code, Step: sid, Message: msg}
}

func stepFieldAllowed(k string) bool {
	switch k {
	case "id", "op", "args", "depends_on", "when", "retry":
		return true
	}
	return false
}

func validateDocument(doc interface{}) (*Document, *Err) {
	obj, ok := doc.(map[string]interface{})
	if !ok {
		return nil, errStep("INVALID_WORKFLOW", nil, "workflow document must be an object")
	}
	for k := range obj {
		if k != "version" && k != "steps" {
			return nil, errStep("INVALID_WORKFLOW", nil, "unknown top-level field: "+k)
		}
	}

	ver, ok := obj["version"]
	if !ok {
		return nil, errStep("INVALID_WORKFLOW", nil, "missing version")
	}
	vi, ok := asI64(ver)
	if !ok || vi != 1 {
		return nil, errStep("INVALID_WORKFLOW", nil, "version must be the integer 1")
	}

	rawSteps, ok := obj["steps"]
	if !ok {
		return nil, errStep("INVALID_WORKFLOW", nil, "missing steps")
	}
	stepList, ok := rawSteps.([]interface{})
	if !ok {
		return nil, errStep("INVALID_WORKFLOW", nil, "steps must be an array")
	}

	d := &Document{ByID: map[string]*Step{}}
	seen := map[string]bool{}

	for idx, raw := range stepList {
		m, ok := raw.(map[string]interface{})
		if !ok {
			return nil, errStep("INVALID_WORKFLOW", nil, "each step must be an object")
		}

		var rawID *string
		if s, ok := m["id"].(string); ok {
			rawID = &s
		}

		for k := range m {
			if !stepFieldAllowed(k) {
				return nil, errStep("INVALID_WORKFLOW", rawID, "unknown step field: "+k)
			}
		}

		idv, ok := m["id"]
		if !ok {
			return nil, errStep("INVALID_WORKFLOW", nil, "step is missing id")
		}
		id, ok := idv.(string)
		if !ok {
			return nil, errStep("INVALID_WORKFLOW", nil, "step id must be a string")
		}
		if !idPattern.MatchString(id) {
			return nil, errStep("INVALID_WORKFLOW", &id, "invalid step id")
		}
		if seen[id] {
			return nil, errStep("INVALID_WORKFLOW", &id, "duplicate step id")
		}
		seen[id] = true

		opv, ok := m["op"]
		if !ok {
			return nil, errStep("INVALID_WORKFLOW", &id, "step is missing op")
		}
		op, ok := opv.(string)
		if !ok {
			return nil, errStep("INVALID_WORKFLOW", &id, "step op must be a string")
		}
		if !knownOps[op] {
			return nil, errStep("INVALID_WORKFLOW", &id, "unknown operation: "+op)
		}

		args := map[string]interface{}{}
		if av, present := m["args"]; present {
			am, ok := av.(map[string]interface{})
			if !ok {
				return nil, errStep("INVALID_WORKFLOW", &id, "args must be an object")
			}
			args = am
		}

		deps := []string{}
		if dv, present := m["depends_on"]; present {
			da, ok := dv.([]interface{})
			if !ok {
				return nil, errStep("INVALID_WORKFLOW", &id, "depends_on must be an array")
			}
			for _, el := range da {
				dep, ok := el.(string)
				if !ok {
					return nil, errStep("INVALID_WORKFLOW", &id, "depends_on entries must be strings")
				}
				deps = append(deps, dep)
			}
		}

		hasWhen := false
		var when interface{}
		if wv, present := m["when"]; present {
			hasWhen = true
			when = wv
			if err := validateCondition(wv, &id); err != nil {
				return nil, err
			}
		}

		maxAttempts := 1
		if rv, present := m["retry"]; present {
			rm, ok := rv.(map[string]interface{})
			if !ok {
				return nil, errStep("INVALID_WORKFLOW", &id, "retry must be an object")
			}
			for k := range rm {
				if k != "max_attempts" {
					return nil, errStep("INVALID_WORKFLOW", &id, "unknown retry field: "+k)
				}
			}
			mav, ok := rm["max_attempts"]
			if !ok {
				return nil, errStep("INVALID_WORKFLOW", &id, "retry is missing max_attempts")
			}
			mi, ok := asI64(mav)
			if !ok || mi < 1 || mi > 5 {
				return nil, errStep("INVALID_WORKFLOW", &id, "max_attempts must be an integer from 1 to 5")
			}
			maxAttempts = int(mi)
		}

		step := &Step{
			ID:          id,
			Op:          op,
			Args:        args,
			Deps:        deps,
			When:        when,
			HasWhen:     hasWhen,
			MaxAttempts: maxAttempts,
			Index:       idx,
		}
		d.Steps = append(d.Steps, step)
		d.ByID[id] = step
	}

	// All dependencies must exist.
	for _, s := range d.Steps {
		for _, dep := range s.Deps {
			if _, ok := d.ByID[dep]; !ok {
				return nil, errStep("UNKNOWN_DEPENDENCY", &s.ID, "unknown dependency: "+dep)
			}
		}
	}

	if hasCycle(d) {
		return nil, errStep("CYCLIC_DEPENDENCY", nil, "workflow has a dependency cycle")
	}

	computeDepSets(d)
	return d, nil
}

func isRefMap(m map[string]interface{}) bool {
	if len(m) != 1 {
		return false
	}
	v, ok := m["$ref"]
	if !ok {
		return false
	}
	_, ok = v.(string)
	return ok
}

// validateCondition checks the shape of a when condition. Semantic failures
// (bad references, non-boolean ref results) are runtime errors.
func validateCondition(v interface{}, sid *string) *Err {
	m, ok := v.(map[string]interface{})
	if !ok {
		return errStep("INVALID_WORKFLOW", sid, "condition must be an object")
	}
	if isRefMap(m) {
		return nil
	}
	opv, ok := m["op"]
	if !ok {
		return errStep("INVALID_WORKFLOW", sid, "condition is missing op")
	}
	op, ok := opv.(string)
	if !ok {
		return errStep("INVALID_WORKFLOW", sid, "condition op must be a string")
	}
	switch op {
	case "eq", "ne", "gt", "gte", "lt", "lte":
		if len(m) != 3 {
			return errStep("INVALID_WORKFLOW", sid, "comparison condition must have exactly op, left and right")
		}
		if _, ok := m["left"]; !ok {
			return errStep("INVALID_WORKFLOW", sid, "condition is missing left")
		}
		if _, ok := m["right"]; !ok {
			return errStep("INVALID_WORKFLOW", sid, "condition is missing right")
		}
	case "all", "any":
		if len(m) != 2 {
			return errStep("INVALID_WORKFLOW", sid, "compound condition must have exactly op and conditions")
		}
		cv, ok := m["conditions"]
		if !ok {
			return errStep("INVALID_WORKFLOW", sid, "condition is missing conditions")
		}
		arr, ok := cv.([]interface{})
		if !ok {
			return errStep("INVALID_WORKFLOW", sid, "conditions must be an array")
		}
		for _, sub := range arr {
			if err := validateCondition(sub, sid); err != nil {
				return err
			}
		}
	case "not":
		if len(m) != 2 {
			return errStep("INVALID_WORKFLOW", sid, "not condition must have exactly op and condition")
		}
		cv, ok := m["condition"]
		if !ok {
			return errStep("INVALID_WORKFLOW", sid, "not condition is missing condition")
		}
		if err := validateCondition(cv, sid); err != nil {
			return err
		}
	default:
		return errStep("INVALID_WORKFLOW", sid, "unknown condition op: "+op)
	}
	return nil
}

func hasCycle(d *Document) bool {
	const (
		white = 0
		gray  = 1
		black = 2
	)
	color := map[string]int{}
	var dfs func(id string) bool
	dfs = func(id string) bool {
		switch color[id] {
		case gray:
			return true
		case black:
			return false
		}
		color[id] = gray
		for _, dep := range d.ByID[id].Deps {
			if dfs(dep) {
				return true
			}
		}
		color[id] = black
		return false
	}
	for _, s := range d.Steps {
		if dfs(s.ID) {
			return true
		}
	}
	return false
}

func computeDepSets(d *Document) {
	memo := map[string]map[string]bool{}
	var visit func(id string) map[string]bool
	visit = func(id string) map[string]bool {
		if m, ok := memo[id]; ok {
			return m
		}
		m := map[string]bool{}
		memo[id] = m
		for _, dep := range d.ByID[id].Deps {
			m[dep] = true
			for k := range visit(dep) {
				m[k] = true
			}
		}
		memo[id] = m
		return m
	}
	for _, s := range d.Steps {
		s.DepSet = visit(s.ID)
	}
}
