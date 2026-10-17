package main

import (
	"fmt"
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

type Step struct {
	ID          string
	Op          string
	Args        map[string]Value
	Deps        []string
	DepClosure  map[string]bool
	When        Value
	HasWhen     bool
	MaxAttempts int
	Index       int
}

type Workflow struct {
	Steps []*Step
	ByID  map[string]*Step
}

type WorkflowError struct {
	Code    string
	Step    *string
	Message string
}

func stepPtr(s string) *string {
	v := s
	return &v
}

func wfErr(code string, step *string, format string, a ...interface{}) *WorkflowError {
	return &WorkflowError{Code: code, Step: step, Message: fmt.Sprintf(format, a...)}
}

func isRefObject(m map[string]Value) bool {
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

// ---------------------------------------------------------------------------
// Structural validation
// ---------------------------------------------------------------------------

func validateWorkflow(root Value) (*Workflow, *WorkflowError) {
	obj, ok := root.(map[string]Value)
	if !ok {
		return nil, wfErr("INVALID_WORKFLOW", nil, "workflow document must be a JSON object")
	}
	for _, k := range sortedKeys(obj) {
		if k != "version" && k != "steps" {
			return nil, wfErr("INVALID_WORKFLOW", nil, "unknown top-level field %q", k)
		}
	}
	verVal, ok := obj["version"]
	if !ok {
		return nil, wfErr("INVALID_WORKFLOW", nil, "missing required field \"version\"")
	}
	verNum, ok := verVal.(Number)
	if !ok {
		return nil, wfErr("INVALID_WORKFLOW", nil, "version must be the integer 1")
	}
	verInt, ok := intLiteral(verNum)
	if !ok || verInt != 1 {
		return nil, wfErr("INVALID_WORKFLOW", nil, "version must be the integer literal 1")
	}
	stepsVal, ok := obj["steps"]
	if !ok {
		return nil, wfErr("INVALID_WORKFLOW", nil, "missing required field \"steps\"")
	}
	stepsArr, ok := stepsVal.([]Value)
	if !ok {
		return nil, wfErr("INVALID_WORKFLOW", nil, "steps must be an array")
	}

	wf := &Workflow{ByID: make(map[string]*Step)}
	for i, sv := range stepsArr {
		st, err := validateStep(sv, i)
		if err != nil {
			return nil, err
		}
		if _, exists := wf.ByID[st.ID]; exists {
			return nil, wfErr("INVALID_WORKFLOW", stepPtr(st.ID), "duplicate step id %q", st.ID)
		}
		wf.ByID[st.ID] = st
		wf.Steps = append(wf.Steps, st)
	}

	for _, st := range wf.Steps {
		for _, d := range st.Deps {
			if _, ok := wf.ByID[d]; !ok {
				return nil, wfErr("UNKNOWN_DEPENDENCY", stepPtr(st.ID), "step %q depends on unknown step %q", st.ID, d)
			}
		}
	}

	if hasCycle(wf) {
		return nil, wfErr("CYCLIC_DEPENDENCY", nil, "dependency cycle detected")
	}

	computeClosures(wf)
	return wf, nil
}

func validateStep(sv Value, idx int) (*Step, *WorkflowError) {
	obj, ok := sv.(map[string]Value)
	if !ok {
		return nil, wfErr("INVALID_WORKFLOW", nil, "step %d must be an object", idx)
	}

	var stepID *string
	if idv, ok := obj["id"]; ok {
		if s, ok := idv.(string); ok {
			stepID = stepPtr(s)
		}
	}

	// Unknown fields.
	for _, k := range sortedKeys(obj) {
		switch k {
		case "id", "op", "args", "depends_on", "when", "retry":
		default:
			return nil, wfErr("INVALID_WORKFLOW", stepID, "unknown step field %q", k)
		}
	}

	st := &Step{Index: idx, MaxAttempts: 1, Args: map[string]Value{}}

	// id
	idVal, ok := obj["id"]
	if !ok {
		return nil, wfErr("INVALID_WORKFLOW", stepID, "step %d is missing required field \"id\"", idx)
	}
	idStr, ok := idVal.(string)
	if !ok {
		return nil, wfErr("INVALID_WORKFLOW", stepID, "step id must be a string")
	}
	stepID = stepPtr(idStr)
	if !idPattern.MatchString(idStr) {
		return nil, wfErr("INVALID_WORKFLOW", stepID, "invalid step id %q", idStr)
	}
	st.ID = idStr

	// op
	opVal, ok := obj["op"]
	if !ok {
		return nil, wfErr("INVALID_WORKFLOW", stepID, "step %q is missing required field \"op\"", idStr)
	}
	opStr, ok := opVal.(string)
	if !ok {
		return nil, wfErr("INVALID_WORKFLOW", stepID, "step %q op must be a string", idStr)
	}
	if !knownOps[opStr] {
		return nil, wfErr("INVALID_WORKFLOW", stepID, "unknown operation %q", opStr)
	}
	st.Op = opStr

	// args
	if argsVal, ok := obj["args"]; ok {
		argsObj, ok := argsVal.(map[string]Value)
		if !ok {
			return nil, wfErr("INVALID_WORKFLOW", stepID, "step %q args must be an object", idStr)
		}
		st.Args = argsObj
	}

	// depends_on
	if depsVal, ok := obj["depends_on"]; ok {
		depsArr, ok := depsVal.([]Value)
		if !ok {
			return nil, wfErr("INVALID_WORKFLOW", stepID, "step %q depends_on must be an array", idStr)
		}
		for _, e := range depsArr {
			s, ok := e.(string)
			if !ok {
				return nil, wfErr("INVALID_WORKFLOW", stepID, "step %q depends_on entries must be strings", idStr)
			}
			st.Deps = append(st.Deps, s)
		}
	}

	// when
	if whenVal, ok := obj["when"]; ok {
		st.HasWhen = true
		st.When = whenVal
		if err := validateCondition(whenVal, stepID); err != nil {
			return nil, err
		}
	}

	// retry
	if retryVal, ok := obj["retry"]; ok {
		retryObj, ok := retryVal.(map[string]Value)
		if !ok {
			return nil, wfErr("INVALID_WORKFLOW", stepID, "step %q retry must be an object", idStr)
		}
		for _, k := range sortedKeys(retryObj) {
			if k != "max_attempts" {
				return nil, wfErr("INVALID_WORKFLOW", stepID, "unknown retry field %q", k)
			}
		}
		if mv, ok := retryObj["max_attempts"]; ok {
			mn, ok := mv.(Number)
			if !ok {
				return nil, wfErr("INVALID_WORKFLOW", stepID, "max_attempts must be an integer")
			}
			mi, ok := intLiteral(mn)
			if !ok || mi < 1 || mi > 5 {
				return nil, wfErr("INVALID_WORKFLOW", stepID, "max_attempts must be an integer between 1 and 5")
			}
			st.MaxAttempts = int(mi)
		}
	}

	return st, nil
}

// validateCondition validates the *shape* of a condition. Runtime value errors
// (non-boolean results, bad references) are handled during evaluation.
func validateCondition(v Value, stepID *string) *WorkflowError {
	switch c := v.(type) {
	case bool, nil, Number, string, []Value:
		// Allowed structurally; non-boolean values fail at runtime with
		// INVALID_CONDITION (booleans are valid results).
		return nil
	case map[string]Value:
		if isRefObject(c) {
			return nil
		}
		opVal, ok := c["op"]
		if !ok {
			return wfErr("INVALID_WORKFLOW", stepID, "condition is missing required field \"op\"")
		}
		opStr, ok := opVal.(string)
		if !ok {
			return wfErr("INVALID_WORKFLOW", stepID, "condition op must be a string")
		}
		switch opStr {
		case "eq", "ne", "gt", "gte", "lt", "lte":
			for _, k := range sortedKeys(c) {
				if k != "op" && k != "left" && k != "right" {
					return wfErr("INVALID_WORKFLOW", stepID, "unknown condition field %q", k)
				}
			}
			if _, ok := c["left"]; !ok {
				return wfErr("INVALID_WORKFLOW", stepID, "condition %q requires \"left\"", opStr)
			}
			if _, ok := c["right"]; !ok {
				return wfErr("INVALID_WORKFLOW", stepID, "condition %q requires \"right\"", opStr)
			}
		case "all", "any":
			for _, k := range sortedKeys(c) {
				if k != "op" && k != "conditions" {
					return wfErr("INVALID_WORKFLOW", stepID, "unknown condition field %q", k)
				}
			}
			cv, ok := c["conditions"]
			if !ok {
				return wfErr("INVALID_WORKFLOW", stepID, "condition %q requires \"conditions\"", opStr)
			}
			arr, ok := cv.([]Value)
			if !ok {
				return wfErr("INVALID_WORKFLOW", stepID, "condition %q conditions must be an array", opStr)
			}
			for _, sub := range arr {
				if err := validateCondition(sub, stepID); err != nil {
					return err
				}
			}
		case "not":
			for _, k := range sortedKeys(c) {
				if k != "op" && k != "condition" {
					return wfErr("INVALID_WORKFLOW", stepID, "unknown condition field %q", k)
				}
			}
			cv, ok := c["condition"]
			if !ok {
				return wfErr("INVALID_WORKFLOW", stepID, "condition \"not\" requires \"condition\"")
			}
			return validateCondition(cv, stepID)
		default:
			return wfErr("INVALID_WORKFLOW", stepID, "unknown condition operator %q", opStr)
		}
		return nil
	}
	return nil
}

func hasCycle(wf *Workflow) bool {
	color := make(map[string]int)
	var visit func(id string) bool
	visit = func(id string) bool {
		color[id] = 1
		for _, d := range wf.ByID[id].Deps {
			switch color[d] {
			case 1:
				return true
			case 0:
				if visit(d) {
					return true
				}
			}
		}
		color[id] = 2
		return false
	}
	for _, st := range wf.Steps {
		if color[st.ID] == 0 && visit(st.ID) {
			return true
		}
	}
	return false
}

func computeClosures(wf *Workflow) {
	memo := make(map[string]map[string]bool)
	var visit func(id string) map[string]bool
	visit = func(id string) map[string]bool {
		if m, ok := memo[id]; ok {
			return m
		}
		m := make(map[string]bool)
		for _, d := range wf.ByID[id].Deps {
			m[d] = true
			for k := range visit(d) {
				m[k] = true
			}
		}
		memo[id] = m
		return m
	}
	for _, st := range wf.Steps {
		st.DepClosure = visit(st.ID)
	}
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

type Engine struct {
	wf           *Workflow
	results      map[string]Value
	order        []string
	skippedOrder []string
	skippedSet   map[string]bool
	executed     map[string]bool
	failCounts   map[string]int
}

func runWorkflow(wf *Workflow) *Response {
	e := &Engine{
		wf:         wf,
		results:    make(map[string]Value),
		skippedSet: make(map[string]bool),
		executed:   make(map[string]bool),
		failCounts: make(map[string]int),
	}
	for count := 0; count < len(wf.Steps); count++ {
		st := e.nextReady()
		if st == nil {
			// Cycles were rejected during validation; reaching here is a bug.
			panic("no ready step although no cycle was detected")
		}
		if resp := e.runStep(st); resp != nil {
			return resp
		}
	}
	resp := newResponse()
	resp.Status = "completed"
	for k, v := range e.results {
		resp.Results[k] = v
	}
	resp.ExecutionOrder = append([]string{}, e.order...)
	resp.Skipped = append([]string{}, e.skippedOrder...)
	return resp
}

func (e *Engine) nextReady() *Step {
	for _, st := range e.wf.Steps {
		if e.executed[st.ID] {
			continue
		}
		ready := true
		for _, d := range st.Deps {
			if !e.executed[d] {
				ready = false
				break
			}
		}
		if ready {
			return st
		}
	}
	return nil
}

func (e *Engine) runStep(st *Step) *Response {
	if st.HasWhen {
		ok, err := e.evalCondition(st, st.When)
		if err != nil {
			return e.fail(st.ID, err.Code, err.Msg)
		}
		if !ok {
			e.executed[st.ID] = true
			e.results[st.ID] = nil
			e.skippedSet[st.ID] = true
			e.order = append(e.order, st.ID)
			e.skippedOrder = append(e.skippedOrder, st.ID)
			return nil
		}
	}

	args, err := e.resolveArgs(st, st.Args)
	if err != nil {
		return e.fail(st.ID, err.Code, err.Msg)
	}

	var last *OpError
	for attempt := 1; attempt <= st.MaxAttempts; attempt++ {
		res, oerr := e.executeOp(st, args)
		if oerr == nil {
			e.executed[st.ID] = true
			e.results[st.ID] = res
			e.order = append(e.order, st.ID)
			return nil
		}
		last = oerr
		if oerr.Code != "TRANSIENT_FAILURE" {
			break
		}
	}
	code := last.Code
	if code == "TRANSIENT_FAILURE" {
		code = "STEP_FAILED"
	}
	return e.fail(st.ID, code, last.Msg)
}

func (e *Engine) fail(stepID, code, msg string) *Response {
	resp := newResponse()
	resp.Status = "failed"
	for k, v := range e.results {
		resp.Results[k] = v
	}
	resp.ExecutionOrder = append([]string{}, e.order...)
	resp.Skipped = append([]string{}, e.skippedOrder...)
	sid := stepID
	resp.Error = &ErrorInfo{Code: code, Step: &sid, Message: msg}
	return resp
}

// ---------------------------------------------------------------------------
// References
// ---------------------------------------------------------------------------

func isAllDigits(s string) bool {
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

func (e *Engine) resolveArgs(st *Step, args map[string]Value) (map[string]Value, *OpError) {
	out := make(map[string]Value, len(args))
	for _, k := range sortedKeys(args) {
		rv, err := e.resolveValue(st, args[k])
		if err != nil {
			return nil, err
		}
		out[k] = rv
	}
	return out, nil
}

func (e *Engine) resolveValue(st *Step, v Value) (Value, *OpError) {
	switch x := v.(type) {
	case map[string]Value:
		if isRefObject(x) {
			return e.resolveRef(st, x["$ref"].(string))
		}
		out := make(map[string]Value, len(x))
		for _, k := range sortedKeys(x) {
			rv, err := e.resolveValue(st, x[k])
			if err != nil {
				return nil, err
			}
			out[k] = rv
		}
		return out, nil
	case []Value:
		out := make([]Value, len(x))
		for i := range x {
			rv, err := e.resolveValue(st, x[i])
			if err != nil {
				return nil, err
			}
			out[i] = rv
		}
		return out, nil
	default:
		return v, nil
	}
}

func (e *Engine) resolveRef(st *Step, ref string) (Value, *OpError) {
	parts := splitRef(ref)
	if len(parts) == 0 || parts[0] == "" {
		return nil, newOpError("INVALID_REFERENCE", "empty reference")
	}
	target := parts[0]
	if target == st.ID || !st.DepClosure[target] {
		return nil, newOpError("INVALID_REFERENCE", "step %q may not reference step %q", st.ID, target)
	}
	if e.skippedSet[target] {
		return nil, newOpError("SKIPPED_REFERENCE", "step %q references skipped step %q", st.ID, target)
	}
	if !e.executed[target] {
		return nil, newOpError("INVALID_REFERENCE", "step %q references step %q that has not run", st.ID, target)
	}
	cur := e.results[target]
	for _, seg := range parts[1:] {
		switch c := cur.(type) {
		case map[string]Value:
			nv, ok := c[seg]
			if !ok {
				return nil, newOpError("INVALID_REFERENCE", "missing key %q in reference %q", seg, ref)
			}
			cur = nv
		case []Value:
			if !isAllDigits(seg) {
				return nil, newOpError("INVALID_REFERENCE", "invalid array index %q in reference %q", seg, ref)
			}
			idx, err := strconvParseIndex(seg)
			if err != nil || idx < 0 || idx >= int64(len(c)) {
				return nil, newOpError("INVALID_REFERENCE", "array index out of range in reference %q", ref)
			}
			cur = c[idx]
		default:
			return nil, newOpError("INVALID_REFERENCE", "cannot descend into %s value in reference %q", typeName(cur), ref)
		}
	}
	return cur, nil
}

func splitRef(ref string) []string {
	parts := make([]string, 0, 4)
	start := 0
	for i := 0; i < len(ref); i++ {
		if ref[i] == '.' {
			parts = append(parts, ref[start:i])
			start = i + 1
		}
	}
	parts = append(parts, ref[start:])
	return parts
}

func strconvParseIndex(s string) (int64, error) {
	var v int64
	for i := 0; i < len(s); i++ {
		d := int64(s[i] - '0')
		if v > (1<<62)/10 {
			return 0, fmt.Errorf("index too large")
		}
		v = v*10 + d
	}
	return v, nil
}

// ---------------------------------------------------------------------------
// Conditions
// ---------------------------------------------------------------------------

func (e *Engine) evalCondition(st *Step, v Value) (bool, *OpError) {
	switch c := v.(type) {
	case bool:
		return c, nil
	case map[string]Value:
		if isRefObject(c) {
			rv, err := e.resolveRef(st, c["$ref"].(string))
			if err != nil {
				return false, err
			}
			b, ok := rv.(bool)
			if !ok {
				return false, newOpError("INVALID_CONDITION", "condition reference did not yield a boolean")
			}
			return b, nil
		}
		opVal, _ := c["op"].(string) // validated as a string
		switch opVal {
		case "eq", "ne", "gt", "gte", "lt", "lte":
			left, err := e.resolveValue(st, c["left"])
			if err != nil {
				return false, err
			}
			right, err := e.resolveValue(st, c["right"])
			if err != nil {
				return false, err
			}
			cmp := compareValues(left, right)
			switch opVal {
			case "eq":
				return cmp == 0, nil
			case "ne":
				return cmp != 0, nil
			case "gt":
				return cmp > 0, nil
			case "gte":
				return cmp >= 0, nil
			case "lt":
				return cmp < 0, nil
			case "lte":
				return cmp <= 0, nil
			}
		case "all":
			arr, _ := c["conditions"].([]Value) // validated
			for _, sub := range arr {
				b, err := e.evalCondition(st, sub)
				if err != nil {
					return false, err
				}
				if !b {
					return false, nil
				}
			}
			return true, nil
		case "any":
			arr, _ := c["conditions"].([]Value) // validated
			for _, sub := range arr {
				b, err := e.evalCondition(st, sub)
				if err != nil {
					return false, err
				}
				if b {
					return true, nil
				}
			}
			return false, nil
		case "not":
			b, err := e.evalCondition(st, c["condition"])
			if err != nil {
				return false, err
			}
			return !b, nil
		}
	}
	return false, newOpError("INVALID_CONDITION", "condition did not evaluate to a boolean")
}
