package main

import (
	"fmt"
	"regexp"
	"strconv"
	"strings"
)

// ---- response shape ---------------------------------------------------------

type ErrorInfo struct {
	Code    string  `json:"code"`
	Step    *string `json:"step"`
	Message string  `json:"message"`
}

type Response struct {
	Status         string           `json:"status"`
	Results        map[string]Value `json:"results"`
	ExecutionOrder []string         `json:"execution_order"`
	Skipped        []string         `json:"skipped"`
	Error          *ErrorInfo       `json:"error"`
}

func completedResponse(results map[string]Value, order, skipped []string) *Response {
	if results == nil {
		results = map[string]Value{}
	}
	if order == nil {
		order = []string{}
	}
	if skipped == nil {
		skipped = []string{}
	}
	return &Response{
		Status:         "completed",
		Results:        results,
		ExecutionOrder: order,
		Skipped:        skipped,
		Error:          nil,
	}
}

func failedResponse(code, stepID, message string, results map[string]Value, order, skipped []string) *Response {
	if results == nil {
		results = map[string]Value{}
	}
	if order == nil {
		order = []string{}
	}
	if skipped == nil {
		skipped = []string{}
	}
	s := stepID
	return &Response{
		Status:         "failed",
		Results:        results,
		ExecutionOrder: order,
		Skipped:        skipped,
		Error:          &ErrorInfo{Code: code, Step: &s, Message: message},
	}
}

func failedResponseNoStep(code, message string) *Response {
	return &Response{
		Status:         "failed",
		Results:        map[string]Value{},
		ExecutionOrder: []string{},
		Skipped:        []string{},
		Error:          &ErrorInfo{Code: code, Step: nil, Message: message},
	}
}

// ---- errors -----------------------------------------------------------------

type WFError struct {
	Code    string
	Step    *string
	Message string
}

func wfErr(code, format string, args ...interface{}) *WFError {
	return &WFError{Code: code, Message: fmt.Sprintf(format, args...)}
}

func strPtr(s string) *string { return &s }

// ---- step model -------------------------------------------------------------

type Step struct {
	ID          string
	Op          string
	Args        map[string]Value
	Depends     []string
	HasWhen     bool
	When        Value
	MaxAttempts int
	Index       int
	DepSet      map[string]bool
}

var idPattern = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_]*$`)

var knownOps = map[string]bool{
	"add": true, "subtract": true, "multiply": true, "divide": true, "mod": true,
	"sum": true, "min": true, "max": true, "range": true,
	"concat": true, "split": true, "join": true, "uppercase": true,
	"lowercase": true, "trim": true, "replace": true, "length": true,
	"map": true, "filter": true, "reduce": true, "sort": true,
	"reverse": true, "unique": true, "slice": true, "flatten": true,
	"get": true, "set": true, "keys": true, "values": true,
	"merge": true, "pick": true, "omit": true,
	"constant": true, "equals": true, "compare": true,
	"and": true, "or": true, "not": true, "type": true,
	"fail_until": true,
}

// ---- structural validation --------------------------------------------------

func validateWorkflow(root Value) ([]*Step, *WFError) {
	doc, ok := asObject(root)
	if !ok {
		return nil, wfErr("INVALID_WORKFLOW", "workflow document must be a JSON object")
	}
	for k := range doc {
		if k != "version" && k != "steps" {
			return nil, wfErr("INVALID_WORKFLOW", "unknown top-level field %q", k)
		}
	}
	versionValue, ok := doc["version"]
	if !ok {
		return nil, wfErr("INVALID_WORKFLOW", "missing workflow version")
	}
	version, ok := asInt(versionValue)
	if !ok || version != 1 {
		return nil, wfErr("INVALID_WORKFLOW", "version must be the integer literal 1")
	}
	stepsValue, ok := doc["steps"]
	if !ok {
		return nil, wfErr("INVALID_WORKFLOW", "missing steps array")
	}
	stepsArray, ok := asArray(stepsValue)
	if !ok {
		return nil, wfErr("INVALID_WORKFLOW", "steps must be an array")
	}

	steps := make([]*Step, 0, len(stepsArray))
	ids := make(map[string]bool, len(stepsArray))

	for index, sv := range stepsArray {
		so, ok := asObject(sv)
		if !ok {
			return nil, wfErr("INVALID_WORKFLOW", "step %d must be an object", index)
		}

		// Determine the step id for error reporting. It is used whenever the
		// id field holds a string, even if it is not a valid id pattern.
		var stepIDForError *string
		if raw, present := so["id"]; present {
			if s, ok := asString(raw); ok {
				stepIDForError = strPtr(s)
			}
		}

		idRaw, present := so["id"]
		if !present {
			return nil, wfErr("INVALID_WORKFLOW", "step %d is missing id", index)
		}
		id, ok := asString(idRaw)
		if !ok {
			return nil, wfErr("INVALID_WORKFLOW", "step %d id must be a string", index)
		}
		if !idPattern.MatchString(id) {
			return nil, &WFError{Code: "INVALID_WORKFLOW", Step: stepIDForError,
				Message: fmt.Sprintf("invalid step id %q", id)}
		}
		if ids[id] {
			return nil, &WFError{Code: "INVALID_WORKFLOW", Step: stepIDForError,
				Message: fmt.Sprintf("duplicate step id %q", id)}
		}
		ids[id] = true

		for k := range so {
			switch k {
			case "id", "op", "args", "depends_on", "when", "retry":
			default:
				return nil, &WFError{Code: "INVALID_WORKFLOW", Step: stepIDForError,
					Message: fmt.Sprintf("unknown step field %q", k)}
			}
		}

		opRaw, present := so["op"]
		if !present {
			return nil, &WFError{Code: "INVALID_WORKFLOW", Step: stepIDForError,
				Message: "step is missing op"}
		}
		op, ok := asString(opRaw)
		if !ok {
			return nil, &WFError{Code: "INVALID_WORKFLOW", Step: stepIDForError,
				Message: "step op must be a string"}
		}
		if !knownOps[op] {
			return nil, &WFError{Code: "INVALID_WORKFLOW", Step: stepIDForError,
				Message: fmt.Sprintf("unknown operation %q", op)}
		}

		args := map[string]Value{}
		if argsRaw, present := so["args"]; present {
			m, ok := asObject(argsRaw)
			if !ok {
				return nil, &WFError{Code: "INVALID_WORKFLOW", Step: stepIDForError,
					Message: "args must be an object"}
			}
			args = m
		}

		deps := []string{}
		if depsRaw, present := so["depends_on"]; present {
			arr, ok := asArray(depsRaw)
			if !ok {
				return nil, &WFError{Code: "INVALID_WORKFLOW", Step: stepIDForError,
					Message: "depends_on must be an array"}
			}
			for _, dv := range arr {
				ds, ok := asString(dv)
				if !ok {
					return nil, &WFError{Code: "INVALID_WORKFLOW", Step: stepIDForError,
						Message: "depends_on entries must be strings"}
				}
				deps = append(deps, ds)
			}
		}

		maxAttempts := 1
		if retryRaw, present := so["retry"]; present {
			rm, ok := asObject(retryRaw)
			if !ok {
				return nil, &WFError{Code: "INVALID_WORKFLOW", Step: stepIDForError,
					Message: "retry must be an object"}
			}
			for k := range rm {
				if k != "max_attempts" {
					return nil, &WFError{Code: "INVALID_WORKFLOW", Step: stepIDForError,
						Message: fmt.Sprintf("unknown retry field %q", k)}
				}
			}
			if maRaw, present := rm["max_attempts"]; present {
				ma, ok := asInt(maRaw)
				if !ok {
					return nil, &WFError{Code: "INVALID_WORKFLOW", Step: stepIDForError,
						Message: "retry.max_attempts must be an integer"}
				}
				if ma < 1 || ma > 5 {
					return nil, &WFError{Code: "INVALID_WORKFLOW", Step: stepIDForError,
						Message: "retry.max_attempts must be between 1 and 5"}
				}
				maxAttempts = int(ma)
			}
		}

		var when Value
		hasWhen := false
		if whenRaw, present := so["when"]; present {
			hasWhen = true
			when = whenRaw
			if cerr := validateCondition(whenRaw); cerr != nil {
				cerr.Step = stepIDForError
				return nil, cerr
			}
		}

		steps = append(steps, &Step{
			ID:          id,
			Op:          op,
			Args:        args,
			Depends:     deps,
			HasWhen:     hasWhen,
			When:        when,
			MaxAttempts: maxAttempts,
			Index:       index,
		})
	}

	for _, st := range steps {
		for _, dep := range st.Depends {
			if !ids[dep] {
				return nil, &WFError{Code: "UNKNOWN_DEPENDENCY", Step: strPtr(st.ID),
					Message: fmt.Sprintf("step %q depends on unknown step %q", st.ID, dep)}
			}
		}
	}

	if hasCycle(steps) {
		return nil, wfErr("CYCLIC_DEPENDENCY", "dependency cycle detected")
	}

	computeDepSets(steps)
	return steps, nil
}

func validateCondition(v Value) *WFError {
	m, ok := asObject(v)
	if !ok {
		return wfErr("INVALID_WORKFLOW", "condition must be an object")
	}
	opRaw, present := m["op"]
	if !present {
		return wfErr("INVALID_WORKFLOW", "condition is missing op")
	}
	if _, ok := asString(opRaw); !ok {
		return wfErr("INVALID_WORKFLOW", "condition op must be a string")
	}
	// Deeper condition problems are runtime INVALID_CONDITION errors; only
	// the basic shape of the `when` field is validated structurally.
	return nil
}

func hasCycle(steps []*Step) bool {
	byID := make(map[string]*Step, len(steps))
	for _, st := range steps {
		byID[st.ID] = st
	}
	color := make(map[string]int, len(steps))
	var visit func(id string) bool
	visit = func(id string) bool {
		switch color[id] {
		case 1:
			return true
		case 2:
			return false
		}
		color[id] = 1
		if st, ok := byID[id]; ok {
			for _, dep := range st.Depends {
				if visit(dep) {
					return true
				}
			}
		}
		color[id] = 2
		return false
	}
	for _, st := range steps {
		if visit(st.ID) {
			return true
		}
	}
	return false
}

func computeDepSets(steps []*Step) {
	byID := make(map[string]*Step, len(steps))
	for _, st := range steps {
		byID[st.ID] = st
	}
	memo := make(map[string]map[string]bool, len(steps))
	var collect func(id string) map[string]bool
	collect = func(id string) map[string]bool {
		if m, ok := memo[id]; ok {
			return m
		}
		set := map[string]bool{}
		if st, ok := byID[id]; ok {
			for _, dep := range st.Depends {
				set[dep] = true
				for ancestor := range collect(dep) {
					set[ancestor] = true
				}
			}
		}
		memo[id] = set
		return set
	}
	for _, st := range steps {
		st.DepSet = collect(st.ID)
	}
}

// ---- references -------------------------------------------------------------

func asRefObject(v Value) (string, bool) {
	m, ok := asObject(v)
	if !ok || len(m) != 1 {
		return "", false
	}
	rv, ok := m["$ref"]
	if !ok {
		return "", false
	}
	s, ok := asString(rv)
	if !ok {
		return "", false
	}
	return s, true
}

func parseIndexSegment(seg string) (int, bool) {
	if seg == "" {
		return 0, false
	}
	for i := 0; i < len(seg); i++ {
		if seg[i] < '0' || seg[i] > '9' {
			return 0, false
		}
	}
	n, err := strconv.ParseUint(seg, 10, 31)
	if err != nil {
		return 0, false
	}
	return int(n), true
}

func resolveRef(path string, st *Step, results map[string]Value, skipped map[string]bool) (Value, *WFError) {
	parts := strings.Split(path, ".")
	target := parts[0]
	if !st.DepSet[target] {
		return nil, wfErr("INVALID_REFERENCE", "reference %q is not a declared dependency", path)
	}
	if skipped[target] {
		return nil, wfErr("SKIPPED_REFERENCE", "reference %q targets a skipped step", path)
	}
	cur, ok := results[target]
	if !ok {
		return nil, wfErr("INVALID_REFERENCE", "reference %q targets a step that has not finished", path)
	}
	for _, seg := range parts[1:] {
		switch node := cur.(type) {
		case map[string]Value:
			next, ok := node[seg]
			if !ok {
				return nil, wfErr("INVALID_REFERENCE", "missing key %q in reference %q", seg, path)
			}
			cur = next
		case []Value:
			idx, ok := parseIndexSegment(seg)
			if !ok {
				return nil, wfErr("INVALID_REFERENCE", "invalid array index %q in reference %q", seg, path)
			}
			if idx < 0 || idx >= len(node) {
				return nil, wfErr("INVALID_REFERENCE", "array index %q out of range in reference %q", seg, path)
			}
			cur = node[idx]
		default:
			return nil, wfErr("INVALID_REFERENCE", "cannot traverse %q in reference %q", seg, path)
		}
	}
	return cur, nil
}

func resolveAny(v Value, st *Step, results map[string]Value, skipped map[string]bool) (Value, *WFError) {
	if path, ok := asRefObject(v); ok {
		return resolveRef(path, st, results, skipped)
	}
	switch x := v.(type) {
	case []Value:
		out := make([]Value, len(x))
		for i := range x {
			r, err := resolveAny(x[i], st, results, skipped)
			if err != nil {
				return nil, err
			}
			out[i] = r
		}
		return out, nil
	case map[string]Value:
		out := make(map[string]Value, len(x))
		for k, vv := range x {
			r, err := resolveAny(vv, st, results, skipped)
			if err != nil {
				return nil, err
			}
			out[k] = r
		}
		return out, nil
	default:
		return v, nil
	}
}

// ---- conditions -------------------------------------------------------------

// unknownConditionKey returns the first key not in allowed, or "".
func unknownConditionKey(m map[string]Value, allowed ...string) string {
	ok := make(map[string]bool, len(allowed))
	for _, a := range allowed {
		ok[a] = true
	}
	for k := range m {
		if !ok[k] {
			return k
		}
	}
	return ""
}

func evalCondition(v Value, st *Step, results map[string]Value, skipped map[string]bool) (bool, *WFError) {
	m, ok := asObject(v)
	if !ok {
		return false, wfErr("INVALID_CONDITION", "condition must be an object")
	}
	opRaw, present := m["op"]
	if !present {
		return false, wfErr("INVALID_CONDITION", "condition is missing op")
	}
	op, ok := asString(opRaw)
	if !ok {
		return false, wfErr("INVALID_CONDITION", "condition op must be a string")
	}

	switch op {
	case "eq", "ne", "gt", "gte", "lt", "lte":
		if bad := unknownConditionKey(m, "op", "left", "right"); bad != "" {
			return false, wfErr("INVALID_CONDITION", "unknown condition field %q", bad)
		}
		leftRaw, ok := m["left"]
		if !ok {
			return false, wfErr("INVALID_CONDITION", "condition is missing left")
		}
		rightRaw, ok := m["right"]
		if !ok {
			return false, wfErr("INVALID_CONDITION", "condition is missing right")
		}
		left, err := resolveAny(leftRaw, st, results, skipped)
		if err != nil {
			return false, err
		}
		right, err := resolveAny(rightRaw, st, results, skipped)
		if err != nil {
			return false, err
		}
		switch op {
		case "eq":
			return equalValues(left, right), nil
		case "ne":
			return !equalValues(left, right), nil
		}
		cmp := compareValues(left, right)
		switch op {
		case "gt":
			return cmp > 0, nil
		case "gte":
			return cmp >= 0, nil
		case "lt":
			return cmp < 0, nil
		case "lte":
			return cmp <= 0, nil
		}
	case "all", "any":
		if bad := unknownConditionKey(m, "op", "conditions"); bad != "" {
			return false, wfErr("INVALID_CONDITION", "unknown condition field %q", bad)
		}
		craw, ok := m["conditions"]
		if !ok {
			return false, wfErr("INVALID_CONDITION", "condition %q is missing conditions", op)
		}
		arr, ok := asArray(craw)
		if !ok {
			return false, wfErr("INVALID_CONDITION", "condition %q conditions must be an array", op)
		}
		if op == "all" {
			for _, c := range arr {
				res, err := evalCondition(c, st, results, skipped)
				if err != nil {
					return false, err
				}
				if !res {
					return false, nil
				}
			}
			return true, nil
		}
		for _, c := range arr {
			res, err := evalCondition(c, st, results, skipped)
			if err != nil {
				return false, err
			}
			if res {
				return true, nil
			}
		}
		return false, nil
	case "not":
		if bad := unknownConditionKey(m, "op", "condition"); bad != "" {
			return false, wfErr("INVALID_CONDITION", "unknown condition field %q", bad)
		}
		craw, ok := m["condition"]
		if !ok {
			return false, wfErr("INVALID_CONDITION", "condition not is missing condition")
		}
		res, err := evalCondition(craw, st, results, skipped)
		if err != nil {
			return false, err
		}
		return !res, nil
	}
	return false, wfErr("INVALID_CONDITION", "unknown condition op %q", op)
}

// ---- execution --------------------------------------------------------------
func executeWorkflow(steps []*Step) *Response {
	results := make(map[string]Value, len(steps))
	order := make([]string, 0, len(steps))
	skippedOrder := make([]string, 0)
	processed := make(map[string]bool, len(steps))
	skipped := make(map[string]bool)
	remaining := len(steps)

	for remaining > 0 {
		var next *Step
		for _, st := range steps {
			if processed[st.ID] {
				continue
			}
			ready := true
			for _, dep := range st.Depends {
				if !processed[dep] {
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
			// Validation guarantees this cannot happen.
			return failedResponseNoStep("INVALID_WORKFLOW", "could not find an executable step")
		}

		if next.HasWhen {
			condition, cerr := evalCondition(next.When, next, results, skipped)
			if cerr != nil {
				return failedResponse(cerr.Code, next.ID, cerr.Message, results, order, skippedOrder)
			}
			if !condition {
				results[next.ID] = nil
				order = append(order, next.ID)
				skippedOrder = append(skippedOrder, next.ID)
				skipped[next.ID] = true
				processed[next.ID] = true
				remaining--
				continue
			}
		}

		resolvedArgs, rerr := resolveAny(next.Args, next, results, skipped)
		if rerr != nil {
			return failedResponse(rerr.Code, next.ID, rerr.Message, results, order, skippedOrder)
		}
		resolvedMap, ok := asObject(resolvedArgs)
		if !ok {
			// args was validated to be an object; resolveAny preserves that.
			return failedResponse("INVALID_ARG", next.ID, "args must be an object", results, order, skippedOrder)
		}

		var value Value
		var lastErr *OpError
		for attempt := 1; attempt <= next.MaxAttempts; attempt++ {
			value, lastErr = executeOp(next.Op, resolvedMap, attempt)
			if lastErr == nil {
				break
			}
			if lastErr.Code != "TRANSIENT_FAILURE" {
				break
			}
		}
		if lastErr != nil {
			code := lastErr.Code
			if code == "TRANSIENT_FAILURE" {
				code = "STEP_FAILED"
			}
			return failedResponse(code, next.ID, lastErr.Message, results, order, skippedOrder)
		}

		results[next.ID] = value
		order = append(order, next.ID)
		processed[next.ID] = true
		remaining--
	}

	return completedResponse(results, order, skippedOrder)
}

// exitCodeFor maps an error code to the CLI exit status.
func exitCodeFor(code string) int {
	switch code {
	case "INVALID_WORKFLOW", "UNKNOWN_DEPENDENCY", "CYCLIC_DEPENDENCY":
		return 2
	default:
		return 1
	}
}
