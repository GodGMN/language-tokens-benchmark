package main

import (
	"strconv"
	"strings"
)

type errorInfo struct {
	Code    string  `json:"code"`
	Step    *string `json:"step"`
	Message string  `json:"message"`
}

type response struct {
	Status         string                 `json:"status"`
	Results        map[string]interface{} `json:"results"`
	ExecutionOrder []string               `json:"execution_order"`
	Skipped        []string               `json:"skipped"`
	Error          *errorInfo             `json:"error"`
}

func newResponse() *response {
	return &response{
		Status:         "completed",
		Results:        map[string]interface{}{},
		ExecutionOrder: []string{},
		Skipped:        []string{},
	}
}

func failureResponse(err *Err) *response {
	r := newResponse()
	r.Status = "failed"
	r.Error = &errorInfo{Code: err.Code, Step: err.Step, Message: err.Message}
	return r
}

type engine struct {
	doc        *Document
	results    map[string]interface{}
	skippedSet map[string]bool
	order      []string
	skipped    []string
	failCounts map[string]int64
}

func execute(d *Document) (*response, int) {
	e := &engine{
		doc:        d,
		results:    map[string]interface{}{},
		skippedSet: map[string]bool{},
		order:      []string{},
		skipped:    []string{},
		failCounts: map[string]int64{},
	}

	finished := map[string]bool{}
	for len(finished) < len(d.Steps) {
		s := e.nextReady(finished)
		if s == nil {
			// Structurally impossible after validation; report an internal error.
			return failureResponse(&Err{Code: "INTERNAL_ERROR", Message: "no executable step found"}), 3
		}

		if s.HasWhen {
			ok, err := e.evalCondition(s.When, s)
			if err != nil {
				err.Step = &s.ID
				return e.failed(err), 1
			}
			if !ok {
				e.results[s.ID] = nil
				e.skippedSet[s.ID] = true
				e.skipped = append(e.skipped, s.ID)
				e.order = append(e.order, s.ID)
				finished[s.ID] = true
				continue
			}
		}

		resolved, err := e.resolveValue(s.Args, s)
		if err != nil {
			err.Step = &s.ID
			return e.failed(err), 1
		}
		args, ok := resolved.(map[string]interface{})
		if !ok {
			err := &Err{Code: "INVALID_ARG", Step: &s.ID, Message: "args must resolve to an object"}
			return e.failed(err), 1
		}

		val, err := e.runWithRetry(s, args)
		if err != nil {
			err.Step = &s.ID
			return e.failed(err), 1
		}

		e.results[s.ID] = val
		e.order = append(e.order, s.ID)
		finished[s.ID] = true
	}

	return e.completed(), 0
}

func (e *engine) nextReady(finished map[string]bool) *Step {
	for _, s := range e.doc.Steps {
		if finished[s.ID] {
			continue
		}
		ready := true
		for _, dep := range s.Deps {
			if !finished[dep] {
				ready = false
				break
			}
		}
		if ready {
			return s
		}
	}
	return nil
}

func (e *engine) completed() *response {
	r := newResponse()
	r.Status = "completed"
	r.Results = e.copyResults()
	r.ExecutionOrder = append([]string{}, e.order...)
	r.Skipped = append([]string{}, e.skipped...)
	return r
}

func (e *engine) failed(err *Err) *response {
	r := newResponse()
	r.Status = "failed"
	r.Results = e.copyResults()
	r.ExecutionOrder = append([]string{}, e.order...)
	r.Skipped = append([]string{}, e.skipped...)
	r.Error = &errorInfo{Code: err.Code, Step: err.Step, Message: err.Message}
	return r
}

func (e *engine) copyResults() map[string]interface{} {
	out := make(map[string]interface{}, len(e.results))
	for k, v := range e.results {
		out[k] = v
	}
	return out
}

func (e *engine) runWithRetry(s *Step, args map[string]interface{}) (interface{}, *Err) {
	attempts := s.MaxAttempts
	if attempts < 1 {
		attempts = 1
	}
	for attempt := 1; attempt <= attempts; attempt++ {
		val, err := e.runOp(s, args)
		if err == nil {
			return val, nil
		}
		if err.Code == "TRANSIENT_FAILURE" {
			if attempt < attempts {
				continue
			}
			return nil, &Err{
				Code:    "STEP_FAILED",
				Step:    &s.ID,
				Message: "step failed after exhausting retries",
			}
		}
		return nil, err
	}
	return nil, &Err{Code: "STEP_FAILED", Step: &s.ID, Message: "step failed"}
}

func asRef(v interface{}) (string, bool) {
	m, ok := v.(map[string]interface{})
	if !ok || !isRefMap(m) {
		return "", false
	}
	return m["$ref"].(string), true
}

func (e *engine) resolveValue(v interface{}, s *Step) (interface{}, *Err) {
	if name, ok := asRef(v); ok {
		return e.resolveRef(name, s)
	}
	switch x := v.(type) {
	case []interface{}:
		out := make([]interface{}, len(x))
		for i, el := range x {
			rv, err := e.resolveValue(el, s)
			if err != nil {
				return nil, err
			}
			out[i] = rv
		}
		return out, nil
	case map[string]interface{}:
		out := make(map[string]interface{}, len(x))
		for k, el := range x {
			rv, err := e.resolveValue(el, s)
			if err != nil {
				return nil, err
			}
			out[k] = rv
		}
		return out, nil
	default:
		return v, nil
	}
}

func (e *engine) resolveRef(name string, s *Step) (interface{}, *Err) {
	parts := strings.Split(name, ".")
	sid := parts[0]

	if _, ok := e.doc.ByID[sid]; !ok {
		return nil, &Err{Code: "INVALID_REFERENCE", Message: "unknown step in reference: " + sid}
	}
	if !s.DepSet[sid] {
		return nil, &Err{Code: "INVALID_REFERENCE", Message: "reference target is not a dependency: " + sid}
	}
	if e.skippedSet[sid] {
		return nil, &Err{Code: "SKIPPED_REFERENCE", Message: "reference target was skipped: " + sid}
	}
	cur, ok := e.results[sid]
	if !ok {
		return nil, &Err{Code: "INVALID_REFERENCE", Message: "reference target has not finished: " + sid}
	}

	for _, seg := range parts[1:] {
		switch node := cur.(type) {
		case map[string]interface{}:
			nv, ok := node[seg]
			if !ok {
				return nil, &Err{Code: "INVALID_REFERENCE", Message: "missing object key in reference: " + seg}
			}
			cur = nv
		case []interface{}:
			idx, ok := parseIndex(seg)
			if !ok || idx < 0 || idx >= len(node) {
				return nil, &Err{Code: "INVALID_REFERENCE", Message: "invalid array index in reference: " + seg}
			}
			cur = node[idx]
		default:
			return nil, &Err{Code: "INVALID_REFERENCE", Message: "cannot descend into non-container in reference"}
		}
	}
	return cur, nil
}

func parseIndex(seg string) (int, bool) {
	if seg == "" {
		return 0, false
	}
	for i := 0; i < len(seg); i++ {
		if seg[i] < '0' || seg[i] > '9' {
			return 0, false
		}
	}
	n, err := strconv.Atoi(seg)
	if err != nil {
		return 0, false
	}
	return n, true
}

// evalCondition evaluates a when condition. Conditions evaluate to a strict
// boolean; a $ref condition whose value is not a boolean is INVALID_CONDITION.
func (e *engine) evalCondition(c interface{}, s *Step) (bool, *Err) {
	if name, ok := asRef(c); ok {
		v, err := e.resolveRef(name, s)
		if err != nil {
			return false, err
		}
		b, ok := v.(bool)
		if !ok {
			return false, &Err{Code: "INVALID_CONDITION", Message: "condition reference is not a boolean"}
		}
		return b, nil
	}

	m, ok := c.(map[string]interface{})
	if !ok {
		return false, &Err{Code: "INVALID_CONDITION", Message: "condition is not an object"}
	}
	op, _ := m["op"].(string)
	switch op {
	case "eq", "ne", "gt", "gte", "lt", "lte":
		left, err := e.resolveValue(m["left"], s)
		if err != nil {
			return false, err
		}
		right, err := e.resolveValue(m["right"], s)
		if err != nil {
			return false, err
		}
		switch op {
		case "eq":
			return valuesEqual(left, right), nil
		case "ne":
			return !valuesEqual(left, right), nil
		case "gt":
			return compareValues(left, right) > 0, nil
		case "gte":
			return compareValues(left, right) >= 0, nil
		case "lt":
			return compareValues(left, right) < 0, nil
		case "lte":
			return compareValues(left, right) <= 0, nil
		}
	case "all":
		arr, _ := m["conditions"].([]interface{})
		for _, sub := range arr {
			b, err := e.evalCondition(sub, s)
			if err != nil {
				return false, err
			}
			if !b {
				return false, nil
			}
		}
		return true, nil
	case "any":
		arr, _ := m["conditions"].([]interface{})
		for _, sub := range arr {
			b, err := e.evalCondition(sub, s)
			if err != nil {
				return false, err
			}
			if b {
				return true, nil
			}
		}
		return false, nil
	case "not":
		b, err := e.evalCondition(m["condition"], s)
		if err != nil {
			return false, err
		}
		return !b, nil
	}
	return false, &Err{Code: "INVALID_CONDITION", Message: "unknown condition op"}
}
