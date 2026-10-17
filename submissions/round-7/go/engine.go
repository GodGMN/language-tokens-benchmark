package main

// execResult is the outcome of executing a validated workflow.
type execResult struct {
	results map[string]interface{}
	order   []string
	skipped []string
	err     *wfError // nil on success
}

func newExecResult() *execResult {
	return &execResult{
		results: map[string]interface{}{},
		order:   []string{},
		skipped: []string{},
	}
}

func (r *execResult) fail(e *wfError, stepID string) {
	id := stepID
	e.stepID = &id
	r.err = e
}

// execute runs the workflow following the scheduling rules of SPEC §7.
func execute(steps []*stepDef) *execResult {
	res := newExecResult()
	done := map[string]bool{}
	skippedSet := map[string]bool{}
	attempts := map[string]int{}

	remaining := make([]*stepDef, len(steps))
	copy(remaining, steps)

	for len(remaining) > 0 {
		// Pick the earliest step in document order whose dependencies are done.
		idx := -1
		for i, s := range remaining {
			ready := true
			for _, d := range s.DependsOn {
				if !done[d] {
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
			// Should be unreachable: cycles are rejected during validation.
			res.err = errf(ErrCyclicDependency, "dependency cycle detected during execution")
			return res
		}
		step := remaining[idx]
		remaining = append(remaining[:idx], remaining[idx+1:]...)

		ctx := &execCtx{
			stepID:   step.ID,
			results:  res.results,
			skipped:  skippedSet,
			closure:  step.Closure,
			attempts: attempts,
		}

		// 1. Condition.
		if step.When != nil {
			ok, err := evalCondition(step.When, ctx)
			if err != nil {
				res.fail(err, step.ID)
				return res
			}
			if !ok {
				res.results[step.ID] = nil
				res.order = append(res.order, step.ID)
				res.skipped = append(res.skipped, step.ID)
				skippedSet[step.ID] = true
				done[step.ID] = true
				continue
			}
		}

		// 2. Resolve args.
		rawArgs := Value(step.Args)
		resolved, err := resolveValue(rawArgs, ctx)
		if err != nil {
			res.fail(err, step.ID)
			return res
		}
		argsMap, ok := resolved.(map[string]interface{})
		if !ok {
			res.fail(errf(ErrInvalidArg, "step args must resolve to an object"), step.ID)
			return res
		}

		// 3. Apply with retries.
		var value Value
		var opErr *wfError
		for attempt := 1; attempt <= step.MaxAttempts; attempt++ {
			value, opErr = applyOp(step.Op, argsMap, ctx)
			if opErr == nil {
				break
			}
			if opErr.Code != ErrTransientFailure {
				break
			}
		}
		if opErr != nil {
			if opErr.Code == ErrTransientFailure {
				opErr = errf(ErrStepFailed, "step %q failed after %d attempt(s): %s", step.ID, step.MaxAttempts, opErr.Msg)
			}
			res.fail(opErr, step.ID)
			return res
		}

		res.results[step.ID] = value
		res.order = append(res.order, step.ID)
		done[step.ID] = true
	}

	return res
}
