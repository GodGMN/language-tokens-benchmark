package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
)

// Error codes (SPEC §7).
const (
	ErrInvalidWorkflow    = "INVALID_WORKFLOW"
	ErrUnknownDependency  = "UNKNOWN_DEPENDENCY"
	ErrCyclicDependency   = "CYCLIC_DEPENDENCY"
	ErrInvalidReference   = "INVALID_REFERENCE"
	ErrSkippedReference   = "SKIPPED_REFERENCE"
	ErrInvalidArg         = "INVALID_ARG"
	ErrInvalidCondition   = "INVALID_CONDITION"
	ErrArithmeticOverflow = "ARITHMETIC_OVERFLOW"
	ErrDivisionByZero     = "DIVISION_BY_ZERO"
	ErrEmptyArray         = "EMPTY_ARRAY"
	ErrTransientFailure   = "TRANSIENT_FAILURE"
	ErrStepFailed         = "STEP_FAILED"
)

type errorObject struct {
	Code    string  `json:"code"`
	Step    *string `json:"step"`
	Message string  `json:"message"`
}

type response struct {
	Status         string                 `json:"status"`
	Results        map[string]interface{} `json:"results"`
	ExecutionOrder []string               `json:"execution_order"`
	Skipped        []string               `json:"skipped"`
	Error          *errorObject           `json:"error"`
}

func newResponse() *response {
	return &response{
		Status:         "completed",
		Results:        map[string]interface{}{},
		ExecutionOrder: []string{},
		Skipped:        []string{},
	}
}

func (r *response) fail(code string, step *string, msg string) {
	r.Status = "failed"
	r.Error = &errorObject{Code: code, Step: step, Message: msg}
}

func writeResponse(r *response) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(r); err != nil {
		// Last-resort fallback (should never happen).
		fmt.Fprintln(os.Stdout, `{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INTERNAL_ERROR","step":null,"message":"encode failure"}}`)
		return
	}
	os.Stdout.Write(buf.Bytes())
}

func exitForCode(code string) int {
	switch code {
	case ErrInvalidWorkflow, ErrUnknownDependency, ErrCyclicDependency:
		return 2
	default:
		return 1
	}
}

func main() {
	exit := 0
	func() {
		defer func() {
			if rec := recover(); rec != nil {
				r := newResponse()
				r.fail("INTERNAL_ERROR", nil, fmt.Sprintf("internal error: %v", rec))
				writeResponse(r)
				exit = 3
			}
		}()
		exit = runCLI(os.Args[1:])
	}()
	os.Exit(exit)
}

func runCLI(args []string) int {
	if len(args) != 2 || args[0] != "run" {
		r := newResponse()
		r.fail(ErrInvalidWorkflow, nil, "usage: workflow run <workflow.json>")
		writeResponse(r)
		return 2
	}

	var data []byte
	var err error
	if args[1] == "-" {
		data, err = io.ReadAll(os.Stdin)
	} else {
		data, err = os.ReadFile(args[1])
	}
	if err != nil {
		r := newResponse()
		r.fail(ErrInvalidWorkflow, nil, "cannot read workflow document: "+err.Error())
		writeResponse(r)
		return 2
	}

	r, exit := runDocument(data)
	writeResponse(r)
	return exit
}

// runDocument validates and executes a raw JSON workflow document, returning
// the response and the process exit code.
func runDocument(data []byte) (*response, int) {
	doc, derr := decodeDocument(data)
	if derr != nil {
		r := newResponse()
		r.fail(ErrInvalidWorkflow, nil, derr.Error())
		return r, 2
	}

	steps, verr := validateWorkflow(doc)
	if verr != nil {
		r := newResponse()
		r.fail(verr.Code, verr.stepID, verr.Msg)
		return r, exitForCode(verr.Code)
	}

	res := execute(steps)
	r := newResponse()
	r.Results = res.results
	r.ExecutionOrder = res.order
	r.Skipped = res.skipped
	if res.err != nil {
		r.fail(res.err.Code, res.err.stepID, res.err.Msg)
		return r, exitForCode(res.err.Code)
	}
	return r, 0
}

func decodeDocument(data []byte) (interface{}, error) {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.UseNumber()
	var v interface{}
	if err := dec.Decode(&v); err != nil {
		return nil, fmt.Errorf("invalid JSON document: %v", err)
	}
	// Reject trailing content.
	var extra interface{}
	if err := dec.Decode(&extra); err != io.EOF {
		if err == nil {
			return nil, fmt.Errorf("invalid JSON document: trailing content")
		}
		return nil, fmt.Errorf("invalid JSON document: %v", err)
	}
	return v, nil
}
