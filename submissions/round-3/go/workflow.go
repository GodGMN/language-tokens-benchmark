package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"regexp"
)

var idPattern = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*$`)

var knownOps = map[string]bool{
	"add": true, "subtract": true, "multiply": true, "divide": true, "mod": true,
	"sum": true, "min": true, "max": true, "range": true,
	"concat": true, "split": true, "join": true, "uppercase": true, "lowercase": true,
	"trim": true, "replace": true, "length": true,
	"map": true, "filter": true, "reduce": true, "sort": true, "reverse": true,
	"unique": true, "slice": true, "flatten": true,
	"get": true, "set": true, "keys": true, "values": true, "merge": true,
	"pick": true, "omit": true,
	"constant": true, "equals": true, "compare": true, "and": true, "or": true,
	"not": true, "type": true,
	"fail_until": true,
}

// Step is a validated workflow step.
type Step struct {
	ID          string
	Op          string
	Args        map[string]interface{}
	DependsOn   []string
	When        map[string]interface{}
	HasWhen     bool
	MaxAttempts int
}

// Workflow is a fully validated workflow.
type Workflow struct {
	Steps []*Step
	ByID  map[string]*Step
	// Deps holds the transitive dependency closure for every step.
	Deps map[string]map[string]bool
}

func stepIDOrNil(m map[string]interface{}) string {
	if v, ok := m["id"]; ok {
		if s, ok := v.(string); ok {
			return s
		}
	}
	return ""
}

// parseWorkflow performs all structural validation. It returns either a
// validated Workflow or a WErr (INVALID_WORKFLOW / UNKNOWN_DEPENDENCY /
// CYCLIC_DEPENDENCY).
func parseWorkflow(doc interface{}) (*Workflow, *WErr) {
	root, ok := doc.(map[string]interface{})
	if !ok {
		return nil, newErr("INVALID_WORKFLOW", "", "workflow document must be a JSON object")
	}

	allowedTop := map[string]bool{"version": true, "steps": true}
	for k := range root {
		if !allowedTop[k] {
			return nil, newErr("INVALID_WORKFLOW", "", fmt.Sprintf("unknown top-level field %q", k))
		}
	}

	v, ok := root["version"]
	if !ok {
		return nil, newErr("INVALID_WORKFLOW", "", "missing version")
	}
	vn, isInt := toIntLiteral(v)
	if !isInt || vn != 1 {
		return nil, newErr("INVALID_WORKFLOW", "", "version must be the integer literal 1")
	}

	sv, ok := root["steps"]
	if !ok {
		return nil, newErr("INVALID_WORKFLOW", "", "missing steps")
	}
	rawSteps, ok := sv.([]interface{})
	if !ok {
		return nil, newErr("INVALID_WORKFLOW", "", "steps must be an array")
	}

	wf := &Workflow{
		Steps: make([]*Step, 0, len(rawSteps)),
		ByID:  map[string]*Step{},
		Deps:  map[string]map[string]bool{},
	}

	allowedStep := map[string]bool{
		"id": true, "op": true, "args": true, "depends_on": true,
		"when": true, "retry": true,
	}

	for _, elem := range rawSteps {
		m, ok := elem.(map[string]interface{})
		if !ok {
			return nil, newErr("INVALID_WORKFLOW", "", "each step must be an object")
		}
		stepID := stepIDOrNil(m)

		idv, ok := m["id"]
		if !ok {
			return nil, newErr("INVALID_WORKFLOW", stepID, "step is missing id")
		}
		id, ok := idv.(string)
		if !ok {
			return nil, newErr("INVALID_WORKFLOW", stepID, "step id must be a string")
		}
		if !idPattern.MatchString(id) {
			return nil, newStepErr("INVALID_WORKFLOW", id, "invalid step id")
		}
		if _, dup := wf.ByID[id]; dup {
			return nil, newStepErr("INVALID_WORKFLOW", id, "duplicate step id")
		}

		for k := range m {
			if !allowedStep[k] {
				return nil, newStepErr("INVALID_WORKFLOW", id, fmt.Sprintf("unknown step field %q", k))
			}
		}

		opv, ok := m["op"]
		if !ok {
			return nil, newStepErr("INVALID_WORKFLOW", id, "step is missing op")
		}
		op, ok := opv.(string)
		if !ok || !knownOps[op] {
			return nil, newStepErr("INVALID_WORKFLOW", id, "unknown operation")
		}

		st := &Step{ID: id, Op: op, MaxAttempts: 1}

		if av, ok := m["args"]; ok {
			am, ok := av.(map[string]interface{})
			if !ok {
				return nil, newStepErr("INVALID_WORKFLOW", id, "args must be an object")
			}
			st.Args = am
		} else {
			st.Args = map[string]interface{}{}
		}

		if dv, ok := m["depends_on"]; ok {
			arr, ok := dv.([]interface{})
			if !ok {
				return nil, newStepErr("INVALID_WORKFLOW", id, "depends_on must be an array")
			}
			for _, e := range arr {
				ds, ok := e.(string)
				if !ok {
					return nil, newStepErr("INVALID_WORKFLOW", id, "depends_on entries must be strings")
				}
				st.DependsOn = append(st.DependsOn, ds)
			}
		}

		if wv, ok := m["when"]; ok {
			wm, ok := wv.(map[string]interface{})
			if !ok {
				return nil, newStepErr("INVALID_WORKFLOW", id, "when must be a condition object")
			}
			if e := validateConditionShape(wm, id); e != nil {
				return nil, e
			}
			st.When = wm
			st.HasWhen = true
		}

		if rv, ok := m["retry"]; ok {
			rm, ok := rv.(map[string]interface{})
			if !ok {
				return nil, newStepErr("INVALID_WORKFLOW", id, "retry must be an object")
			}
			for k := range rm {
				if k != "max_attempts" {
					return nil, newStepErr("INVALID_WORKFLOW", id, fmt.Sprintf("unknown retry field %q", k))
				}
			}
			if mv, ok := rm["max_attempts"]; ok {
				n, ok := toIntLiteral(mv)
				if !ok || n < 1 || n > 5 {
					return nil, newStepErr("INVALID_WORKFLOW", id, "max_attempts must be an integer in 1..5")
				}
				st.MaxAttempts = int(n)
			}
		}

		wf.Steps = append(wf.Steps, st)
		wf.ByID[id] = st
	}

	// Unknown dependencies.
	for _, st := range wf.Steps {
		for _, d := range st.DependsOn {
			if _, ok := wf.ByID[d]; !ok {
				return nil, newErr("UNKNOWN_DEPENDENCY", st.ID, fmt.Sprintf("unknown dependency %q", d))
			}
		}
	}

	// Cycle detection.
	state := map[string]int{} // 0 unvisited, 1 in stack, 2 done
	var visit func(string) bool
	visit = func(id string) bool {
		if state[id] == 1 {
			return true
		}
		if state[id] == 2 {
			return false
		}
		state[id] = 1
		for _, d := range wf.ByID[id].DependsOn {
			if visit(d) {
				return true
			}
		}
		state[id] = 2
		return false
	}
	for _, st := range wf.Steps {
		if visit(st.ID) {
			return nil, newErr("CYCLIC_DEPENDENCY", "", "dependency cycle detected")
		}
	}

	// Transitive dependency closures (safe now that the graph is acyclic).
	for _, st := range wf.Steps {
		seen := map[string]bool{}
		var walk func(string)
		walk = func(id string) {
			for _, d := range wf.ByID[id].DependsOn {
				if !seen[d] {
					seen[d] = true
					walk(d)
				}
			}
		}
		walk(st.ID)
		wf.Deps[st.ID] = seen
	}

	return wf, nil
}

func validateConditionShape(c map[string]interface{}, stepID string) *WErr {
	opv, ok := c["op"]
	if !ok {
		return newStepErr("INVALID_WORKFLOW", stepID, "condition is missing op")
	}
	op, ok := opv.(string)
	if !ok {
		return newStepErr("INVALID_WORKFLOW", stepID, "condition op must be a string")
	}

	checkKeys := func(allowed ...string) *WErr {
		set := map[string]bool{}
		for _, k := range allowed {
			set[k] = true
		}
		for k := range c {
			if !set[k] {
				return newStepErr("INVALID_WORKFLOW", stepID, fmt.Sprintf("unknown condition field %q", k))
			}
		}
		return nil
	}

	switch op {
	case "all", "any":
		if e := checkKeys("op", "conditions"); e != nil {
			return e
		}
		cv, ok := c["conditions"]
		if !ok {
			return newStepErr("INVALID_WORKFLOW", stepID, "compound condition is missing conditions")
		}
		arr, ok := cv.([]interface{})
		if !ok {
			return newStepErr("INVALID_WORKFLOW", stepID, "conditions must be an array")
		}
		for _, e := range arr {
			cm, ok := e.(map[string]interface{})
			if !ok {
				return newStepErr("INVALID_WORKFLOW", stepID, "each condition must be an object")
			}
			if err := validateConditionShape(cm, stepID); err != nil {
				return err
			}
		}
		return nil
	case "not":
		if e := checkKeys("op", "condition"); e != nil {
			return e
		}
		cv, ok := c["condition"]
		if !ok {
			return newStepErr("INVALID_WORKFLOW", stepID, "not condition is missing condition")
		}
		cm, ok := cv.(map[string]interface{})
		if !ok {
			return newStepErr("INVALID_WORKFLOW", stepID, "not condition must be an object")
		}
		return validateConditionShape(cm, stepID)
	case "eq", "ne", "gt", "gte", "lt", "lte":
		if e := checkKeys("op", "left", "right"); e != nil {
			return e
		}
		if _, ok := c["left"]; !ok {
			return newStepErr("INVALID_WORKFLOW", stepID, "condition is missing left")
		}
		if _, ok := c["right"]; !ok {
			return newStepErr("INVALID_WORKFLOW", stepID, "condition is missing right")
		}
		return nil
	default:
		return newStepErr("INVALID_WORKFLOW", stepID, fmt.Sprintf("unknown condition op %q", op))
	}
}

// decodeDocument parses exactly one JSON value, using json.Number so that
// integer-literal distinctions are preserved.
func decodeDocument(data []byte) (interface{}, error) {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.UseNumber()
	var v interface{}
	if err := dec.Decode(&v); err != nil {
		return nil, err
	}
	// Ensure there is no trailing content.
	var extra interface{}
	if err := dec.Decode(&extra); err != io.EOF {
		if err == nil {
			return nil, fmt.Errorf("unexpected trailing JSON content")
		}
		return nil, err
	}
	return v, nil
}
