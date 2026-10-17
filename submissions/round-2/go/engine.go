package main

import (
	"strconv"
	"strings"
)

type engine struct {
	steps      []Step
	byID       map[string]*Step
	depClosure map[string]map[string]bool
	results    map[string]Value
	finished   map[string]bool
	skipped    map[string]bool
	order      []string
	failCount  map[string]int
}

func newEngine(steps []Step) *engine {
	e := &engine{
		steps:      steps,
		byID:       make(map[string]*Step, len(steps)),
		depClosure: make(map[string]map[string]bool, len(steps)),
		results:    make(map[string]Value, len(steps)),
		finished:   make(map[string]bool, len(steps)),
		skipped:    make(map[string]bool, len(steps)),
		order:      []string{},
		failCount:  make(map[string]int, len(steps)),
	}
	for i := range steps {
		e.byID[steps[i].ID] = &e.steps[i]
	}
	return e
}

// closure returns the transitive dependency set of id.
func (e *engine) closure(id string) map[string]bool {
	if c, ok := e.depClosure[id]; ok {
		return c
	}
	c := make(map[string]bool)
	st := e.byID[id]
	if st != nil {
		for _, d := range st.Deps {
			c[d] = true
			for k := range e.closure(d) {
				c[k] = true
			}
		}
	}
	e.depClosure[id] = c
	return c
}

// run executes the workflow. A returned error means the workflow failed.
func (e *engine) run() *wfError {
	for len(e.finished) < len(e.steps) {
		idx := -1
		for i := range e.steps {
			st := &e.steps[i]
			if e.finished[st.ID] {
				continue
			}
			ready := true
			for _, d := range st.Deps {
				if !e.finished[d] {
					ready = false
					break
				}
			}
			if ready {
				idx = i
				break
			}
		}
		if idx < 0 {
			// Should be impossible after structural validation.
			return docError("CYCLIC_DEPENDENCY", "dependency cycle detected", 2)
		}
		st := &e.steps[idx]

		if st.When != nil {
			wv, oerr := e.resolveValue(*st.When, st.ID)
			if oerr != nil {
				return stepError(oerr.code, st.ID, oerr.msg, 1)
			}
			res, oerr := evalConditionValue(wv)
			if oerr != nil {
				return stepError(oerr.code, st.ID, oerr.msg, 1)
			}
			if res.Kind != KBool {
				return stepError("INVALID_CONDITION", st.ID, "condition did not evaluate to a boolean", 1)
			}
			if !res.B {
				e.results[st.ID] = Null()
				e.finished[st.ID] = true
				e.skipped[st.ID] = true
				e.order = append(e.order, st.ID)
				continue
			}
		}

		args, oerr := e.resolveValue(st.Args, st.ID)
		if oerr != nil {
			return stepError(oerr.code, st.ID, oerr.msg, 1)
		}

		var val Value
		var last *opErr
		ok := false
		for attempt := 0; attempt < st.MaxAttempts; attempt++ {
			val, last = e.runOp(st, args)
			if last == nil {
				ok = true
				break
			}
			if last.code != "TRANSIENT_FAILURE" {
				break
			}
		}
		if !ok {
			if last == nil {
				last = errCode("STEP_FAILED", "step failed")
			}
			code := last.code
			if code == "TRANSIENT_FAILURE" {
				code = "STEP_FAILED"
			}
			return stepError(code, st.ID, last.msg, 1)
		}

		e.results[st.ID] = val
		e.finished[st.ID] = true
		e.order = append(e.order, st.ID)
	}
	return nil
}

func (e *engine) resolveValue(v Value, stepID string) (Value, *opErr) {
	switch v.Kind {
	case KObj:
		if isRefObject(v) {
			return e.resolveRef(v.O["$ref"].S, stepID)
		}
		out := make(map[string]Value, len(v.O))
		for k, val := range v.O {
			rv, err := e.resolveValue(val, stepID)
			if err != nil {
				return Value{}, err
			}
			out[k] = rv
		}
		return Obj(out), nil
	case KArr:
		out := make([]Value, len(v.A))
		for i, val := range v.A {
			rv, err := e.resolveValue(val, stepID)
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

func (e *engine) resolveRef(ref, stepID string) (Value, *opErr) {
	parts := strings.Split(ref, ".")
	target := parts[0]
	if target == "" {
		return Value{}, errCode("INVALID_REFERENCE", "empty reference target")
	}
	if target == stepID {
		return Value{}, errCode("INVALID_REFERENCE", "self reference: "+ref)
	}
	if _, ok := e.byID[target]; !ok {
		return Value{}, errCode("INVALID_REFERENCE", "unknown reference target: "+target)
	}
	if !e.closure(stepID)[target] {
		return Value{}, errCode("INVALID_REFERENCE", "reference to non-dependency: "+target)
	}
	if e.skipped[target] {
		return Value{}, errCode("SKIPPED_REFERENCE", "reference to skipped step: "+target)
	}
	val, ok := e.results[target]
	if !ok {
		return Value{}, errCode("INVALID_REFERENCE", "reference target has not finished: "+target)
	}
	for _, seg := range parts[1:] {
		switch val.Kind {
		case KObj:
			nv, ok := val.O[seg]
			if !ok {
				return Value{}, errCode("INVALID_REFERENCE", "missing object key in reference: "+seg)
			}
			val = nv
		case KArr:
			if !isDigits(seg) {
				return Value{}, errCode("INVALID_REFERENCE", "invalid array index in reference: "+seg)
			}
			idx, err := strconv.ParseInt(seg, 10, 64)
			if err != nil || idx < 0 || idx >= int64(len(val.A)) {
				return Value{}, errCode("INVALID_REFERENCE", "array index out of range: "+seg)
			}
			val = val.A[idx]
		default:
			return Value{}, errCode("INVALID_REFERENCE", "cannot traverse into non-container in reference")
		}
	}
	return val, nil
}

func isDigits(s string) bool {
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
