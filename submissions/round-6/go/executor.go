package main

import (
	"strconv"
)

// Error codes that are structural (exit 2) rather than runtime (exit 1).
func isStructuralCode(code string) bool {
	switch code {
	case "INVALID_WORKFLOW", "UNKNOWN_DEPENDENCY", "CYCLIC_DEPENDENCY":
		return true
	}
	return false
}

type Executor struct {
	wf           *Workflow
	results      map[string]any
	finished     map[string]bool
	skippedSet   map[string]bool
	order        []string
	skipped      []string
	failAttempts map[string]int
}

func Execute(wf *Workflow) (map[string]any, []string, []string, *Error) {
	ex := &Executor{
		wf:           wf,
		results:      make(map[string]any, len(wf.Steps)),
		finished:     make(map[string]bool, len(wf.Steps)),
		skippedSet:   make(map[string]bool),
		order:        []string{},
		skipped:      []string{},
		failAttempts: make(map[string]int),
	}

	for len(ex.finished) < len(wf.Steps) {
		next := -1
		for _, s := range wf.Steps {
			if ex.finished[s.ID] {
				continue
			}
			ready := true
			for _, d := range s.Deps {
				if !ex.finished[d] {
					ready = false
					break
				}
			}
			if ready {
				next = s.Index
				break
			}
		}
		if next < 0 {
			return ex.results, ex.order, ex.skipped,
				errNoStep("INTERNAL_ERROR", "no ready step; dependency graph is not executable")
		}

		s := wf.Steps[next]

		if s.HasWhen {
			ok, e := ex.evalCondition(s, s.When)
			if e != nil {
				return ex.results, ex.order, ex.skipped, e
			}
			if !ok {
				ex.results[s.ID] = nil
				ex.finished[s.ID] = true
				ex.skippedSet[s.ID] = true
				ex.order = append(ex.order, s.ID)
				ex.skipped = append(ex.skipped, s.ID)
				continue
			}
		}

		resolved, e := ex.resolveValue(s.Args, s)
		if e != nil {
			return ex.results, ex.order, ex.skipped, e
		}
		args, ok := resolved.(map[string]any)
		if !ok {
			return ex.results, ex.order, ex.skipped,
				errAtStep("INVALID_ARG", s.ID, "resolved args of step %q are not an object", s.ID)
		}

		var result any
		var opE *OpError
		for attempt := 1; attempt <= s.MaxAttempts; attempt++ {
			result, opE = ex.executeOp(s, args)
			if opE == nil {
				break
			}
			if opE.Code == "TRANSIENT_FAILURE" {
				if attempt < s.MaxAttempts {
					continue
				}
				// Retries exhausted: the final code is STEP_FAILED.
				return ex.results, ex.order, ex.skipped,
					errAtStep("STEP_FAILED", s.ID, "step %q failed after %d attempt(s): %s",
						s.ID, s.MaxAttempts, opE.Message)
			}
			return ex.results, ex.order, ex.skipped,
				errAtStep(opE.Code, s.ID, "%s", opE.Message)
		}
		if opE != nil {
			return ex.results, ex.order, ex.skipped,
				errAtStep(opE.Code, s.ID, "%s", opE.Message)
		}

		ex.results[s.ID] = result
		ex.finished[s.ID] = true
		ex.order = append(ex.order, s.ID)
	}

	return ex.results, ex.order, ex.skipped, nil
}

// resolveValue resolves $ref objects recursively inside arrays and objects.
func (ex *Executor) resolveValue(v any, step *Step) (any, *Error) {
	switch x := v.(type) {
	case map[string]any:
		if len(x) == 1 {
			if rv, ok := x["$ref"]; ok {
				if ref, ok := rv.(string); ok {
					return ex.resolveRef(ref, step)
				}
			}
		}
		out := make(map[string]any, len(x))
		for k, val := range x {
			r, e := ex.resolveValue(val, step)
			if e != nil {
				return nil, e
			}
			out[k] = r
		}
		return out, nil
	case []any:
		out := make([]any, len(x))
		for i, val := range x {
			r, e := ex.resolveValue(val, step)
			if e != nil {
				return nil, e
			}
			out[i] = r
		}
		return out, nil
	default:
		return v, nil
	}
}

func (ex *Executor) resolveRef(ref string, step *Step) (any, *Error) {
	parts := splitRef(ref)
	target := parts[0]

	if target == step.ID || !ex.wf.Closure[step.ID][target] {
		return nil, errAtStep("INVALID_REFERENCE", step.ID,
			"step %q may not reference step %q", step.ID, target)
	}
	if !ex.finished[target] {
		return nil, errAtStep("INVALID_REFERENCE", step.ID,
			"step %q references unfinished step %q", step.ID, target)
	}
	if ex.skippedSet[target] {
		return nil, errAtStep("SKIPPED_REFERENCE", step.ID,
			"step %q references skipped step %q", step.ID, target)
	}

	cur := ex.results[target]
	for _, seg := range parts[1:] {
		switch node := cur.(type) {
		case map[string]any:
			v, ok := node[seg]
			if !ok {
				return nil, errAtStep("INVALID_REFERENCE", step.ID,
					"reference %q: missing object key %q", ref, seg)
			}
			cur = v
		case []any:
			if !isDecimalIndex(seg) {
				return nil, errAtStep("INVALID_REFERENCE", step.ID,
					"reference %q: invalid array index %q", ref, seg)
			}
			idx, err := strconv.ParseInt(seg, 10, 64)
			if err != nil || idx < 0 || idx >= int64(len(node)) {
				return nil, errAtStep("INVALID_REFERENCE", step.ID,
					"reference %q: array index %q out of range", ref, seg)
			}
			cur = node[idx]
		default:
			return nil, errAtStep("INVALID_REFERENCE", step.ID,
				"reference %q: cannot descend into %s", ref, jsonTypeName(cur))
		}
	}
	return cur, nil
}

func splitRef(ref string) []string {
	parts := []string{}
	start := 0
	for i := 0; i <= len(ref); i++ {
		if i == len(ref) || ref[i] == '.' {
			parts = append(parts, ref[start:i])
			start = i + 1
		}
	}
	return parts
}

func isDecimalIndex(s string) bool {
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

func (ex *Executor) evalCondition(step *Step, cond any) (bool, *Error) {
	m, ok := cond.(map[string]any)
	if !ok {
		return false, errAtStep("INVALID_CONDITION", step.ID, "condition must be an object")
	}
	op, ok := m["op"].(string)
	if !ok {
		return false, errAtStep("INVALID_CONDITION", step.ID, "condition op must be a string")
	}

	switch op {
	case "all":
		arr, ok := m["conditions"].([]any)
		if !ok {
			return false, errAtStep("INVALID_CONDITION", step.ID, "all requires conditions array")
		}
		for _, sub := range arr {
			b, e := ex.evalCondition(step, sub)
			if e != nil {
				return false, e
			}
			if !b {
				return false, nil
			}
		}
		return true, nil
	case "any":
		arr, ok := m["conditions"].([]any)
		if !ok {
			return false, errAtStep("INVALID_CONDITION", step.ID, "any requires conditions array")
		}
		for _, sub := range arr {
			b, e := ex.evalCondition(step, sub)
			if e != nil {
				return false, e
			}
			if b {
				return true, nil
			}
		}
		return false, nil
	case "not":
		b, e := ex.evalCondition(step, m["condition"])
		if e != nil {
			return false, e
		}
		return !b, nil
	case "eq", "ne", "gt", "gte", "lt", "lte":
		left, e := ex.resolveValue(m["left"], step)
		if e != nil {
			return false, e
		}
		right, e := ex.resolveValue(m["right"], step)
		if e != nil {
			return false, e
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
		default:
			return compareValues(left, right) <= 0, nil
		}
	}
	return false, errAtStep("INVALID_CONDITION", step.ID, "unknown condition op %q", op)
}
