package main

import (
	"fmt"
	"strconv"
	"strings"
)

// Engine executes a validated workflow.
type Engine struct {
	wf       *Workflow
	results  map[string]interface{}
	finished map[string]bool
	skipped  map[string]bool
	order    []string
	counters map[string]int
}

func newEngine(wf *Workflow) *Engine {
	return &Engine{
		wf:       wf,
		results:  map[string]interface{}{},
		finished: map[string]bool{},
		skipped:  map[string]bool{},
		order:    []string{},
		counters: map[string]int{},
	}
}

// Execute runs all steps. It returns nil on success, or the fatal WErr.
func (e *Engine) Execute() *WErr {
	for len(e.finished) < len(e.wf.Steps) {
		var next *Step
		for _, st := range e.wf.Steps {
			if e.finished[st.ID] {
				continue
			}
			ready := true
			for _, d := range st.DependsOn {
				if !e.finished[d] {
					ready = false
					break
				}
			}
			if ready {
				next = st
				break
			}
		}
		if next == nil {
			// Should have been caught by structural validation.
			return newErr("CYCLIC_DEPENDENCY", "", "no runnable step (cycle)")
		}
		if err := e.executeStep(next); err != nil {
			return err
		}
	}
	return nil
}

func (e *Engine) executeStep(st *Step) *WErr {
	if st.HasWhen {
		ok, err := e.evalCondition(st, st.When)
		if err != nil {
			return err
		}
		if !ok {
			e.results[st.ID] = nil
			e.finished[st.ID] = true
			e.skipped[st.ID] = true
			e.order = append(e.order, st.ID)
			return nil
		}
	}

	for attempt := 1; attempt <= st.MaxAttempts; attempt++ {
		resolved, err := e.resolveArgs(st)
		if err != nil {
			return err
		}
		e.counters[st.ID]++
		res, opErr := runOp(st.Op, resolved, st.ID, e.counters[st.ID])
		if opErr != nil {
			if opErr.Code == "TRANSIENT_FAILURE" {
				if attempt < st.MaxAttempts {
					continue
				}
				return newErr("STEP_FAILED", st.ID, "step failed after retries")
			}
			return opErr
		}
		e.results[st.ID] = res
		e.finished[st.ID] = true
		e.order = append(e.order, st.ID)
		return nil
	}
	return newErr("STEP_FAILED", st.ID, "step failed after retries")
}

// ---------------------------------------------------------------------------
// References
// ---------------------------------------------------------------------------

func (e *Engine) resolveArgs(st *Step) (map[string]interface{}, *WErr) {
	out := make(map[string]interface{}, len(st.Args))
	for k, v := range st.Args {
		r, err := e.resolveValue(st, v)
		if err != nil {
			return nil, err
		}
		out[k] = r
	}
	return out, nil
}

func (e *Engine) resolveValue(st *Step, v interface{}) (interface{}, *WErr) {
	switch x := v.(type) {
	case []interface{}:
		out := make([]interface{}, len(x))
		for i, el := range x {
			r, err := e.resolveValue(st, el)
			if err != nil {
				return nil, err
			}
			out[i] = r
		}
		return out, nil
	case map[string]interface{}:
		if path, ok := asRef(x); ok {
			return e.resolveRef(st, path)
		}
		out := make(map[string]interface{}, len(x))
		for k, el := range x {
			r, err := e.resolveValue(st, el)
			if err != nil {
				return nil, err
			}
			out[k] = r
		}
		return out, nil
	}
	return v, nil
}

func asRef(m map[string]interface{}) (string, bool) {
	if len(m) != 1 {
		return "", false
	}
	rv, ok := m["$ref"]
	if !ok {
		return "", false
	}
	s, ok := rv.(string)
	if !ok {
		return "", false
	}
	return s, true
}

func allDigits(s string) bool {
	if s == "" {
		return false
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return false
		}
	}
	return true
}

func (e *Engine) resolveRef(st *Step, path string) (interface{}, *WErr) {
	segs := strings.Split(path, ".")
	target := segs[0]
	if target == "" {
		return nil, newErr("INVALID_REFERENCE", st.ID, "empty reference target")
	}
	if target == st.ID {
		return nil, newErr("INVALID_REFERENCE", st.ID, "self-reference")
	}
	if !e.wf.Deps[st.ID][target] {
		return nil, newErr("INVALID_REFERENCE", st.ID, fmt.Sprintf("reference to non-dependency %q", target))
	}
	if !e.finished[target] {
		return nil, newErr("INVALID_REFERENCE", st.ID, fmt.Sprintf("reference to unfinished step %q", target))
	}
	if e.skipped[target] {
		return nil, newErr("SKIPPED_REFERENCE", st.ID, fmt.Sprintf("reference to skipped step %q", target))
	}
	val, ok := e.results[target]
	if !ok {
		return nil, newErr("INVALID_REFERENCE", st.ID, fmt.Sprintf("reference to unknown result %q", target))
	}

	for _, seg := range segs[1:] {
		switch cur := val.(type) {
		case map[string]interface{}:
			nv, ok := cur[seg]
			if !ok {
				return nil, newErr("INVALID_REFERENCE", st.ID, fmt.Sprintf("missing key %q in reference %q", seg, path))
			}
			val = nv
		case []interface{}:
			if !allDigits(seg) {
				return nil, newErr("INVALID_REFERENCE", st.ID, fmt.Sprintf("invalid array index %q in reference %q", seg, path))
			}
			idx, perr := strconv.ParseUint(seg, 10, 64)
			if perr != nil || idx >= uint64(len(cur)) {
				return nil, newErr("INVALID_REFERENCE", st.ID, fmt.Sprintf("array index %q out of range in reference %q", seg, path))
			}
			val = cur[idx]
		default:
			return nil, newErr("INVALID_REFERENCE", st.ID, fmt.Sprintf("cannot descend into non-container in reference %q", path))
		}
	}
	return val, nil
}

// ---------------------------------------------------------------------------
// Conditions
// ---------------------------------------------------------------------------

func (e *Engine) evalCondition(st *Step, cond map[string]interface{}) (bool, *WErr) {
	opv, ok := cond["op"]
	if !ok {
		return false, newErr("INVALID_CONDITION", st.ID, "condition missing op")
	}
	op, ok := opv.(string)
	if !ok {
		return false, newErr("INVALID_CONDITION", st.ID, "condition op must be a string")
	}

	switch op {
	case "all", "any":
		cv, ok := cond["conditions"]
		if !ok {
			return false, newErr("INVALID_CONDITION", st.ID, "compound condition missing conditions")
		}
		arr, ok := cv.([]interface{})
		if !ok {
			return false, newErr("INVALID_CONDITION", st.ID, "conditions must be an array")
		}
		for _, el := range arr {
			cm, ok := el.(map[string]interface{})
			if !ok {
				return false, newErr("INVALID_CONDITION", st.ID, "each condition must be an object")
			}
			b, err := e.evalCondition(st, cm)
			if err != nil {
				return false, err
			}
			if op == "all" && !b {
				return false, nil
			}
			if op == "any" && b {
				return true, nil
			}
		}
		if op == "all" {
			return true, nil
		}
		return false, nil
	case "not":
		cv, ok := cond["condition"]
		if !ok {
			return false, newErr("INVALID_CONDITION", st.ID, "not condition missing condition")
		}
		cm, ok := cv.(map[string]interface{})
		if !ok {
			return false, newErr("INVALID_CONDITION", st.ID, "not condition must be an object")
		}
		b, err := e.evalCondition(st, cm)
		if err != nil {
			return false, err
		}
		return !b, nil
	case "eq", "ne", "gt", "gte", "lt", "lte":
		lv, ok := cond["left"]
		if !ok {
			return false, newErr("INVALID_CONDITION", st.ID, "condition missing left")
		}
		rv, ok := cond["right"]
		if !ok {
			return false, newErr("INVALID_CONDITION", st.ID, "condition missing right")
		}
		left, err := e.resolveValue(st, lv)
		if err != nil {
			return false, err
		}
		right, err := e.resolveValue(st, rv)
		if err != nil {
			return false, err
		}
		switch op {
		case "eq":
			return equalsValues(left, right), nil
		case "ne":
			return !equalsValues(left, right), nil
		}
		c := compareValues(left, right)
		switch op {
		case "gt":
			return c > 0, nil
		case "gte":
			return c >= 0, nil
		case "lt":
			return c < 0, nil
		default:
			return c <= 0, nil
		}
	}
	return false, newErr("INVALID_CONDITION", st.ID, fmt.Sprintf("unknown condition op %q", op))
}
