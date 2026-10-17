package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
)

// Response is the single JSON document written to stdout.
type Response struct {
	Status         string         `json:"status"`
	Results        map[string]any `json:"results"`
	ExecutionOrder []string       `json:"execution_order"`
	Skipped        []string       `json:"skipped"`
	Error          *Error         `json:"error"`
}

func failedResponse(err *Error) Response {
	return Response{
		Status:         "failed",
		Results:        map[string]any{},
		ExecutionOrder: []string{},
		Skipped:        []string{},
		Error:          err,
	}
}

func decodeDocument(data []byte) (any, error) {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.UseNumber()
	var v any
	if err := dec.Decode(&v); err != nil {
		return nil, err
	}
	var extra any
	if err := dec.Decode(&extra); err != io.EOF {
		if err == nil {
			return nil, fmt.Errorf("trailing JSON value")
		}
		return nil, fmt.Errorf("trailing data: %v", err)
	}
	return v, nil
}

func runWorkflow() (Response, int) {
	if len(os.Args) != 3 || os.Args[1] != "run" {
		return failedResponse(errNoStep("INVALID_WORKFLOW", "usage: workflow run <workflow.json>")), 2
	}

	target := os.Args[2]
	var data []byte
	var err error
	if target == "-" {
		data, err = io.ReadAll(os.Stdin)
	} else {
		data, err = os.ReadFile(target)
	}
	if err != nil {
		return failedResponse(errNoStep("INVALID_WORKFLOW", "cannot read input: %v", err)), 2
	}

	root, err := decodeDocument(data)
	if err != nil {
		return failedResponse(errNoStep("INVALID_WORKFLOW", "invalid JSON: %v", err)), 2
	}

	wf, verr := validateWorkflow(root)
	if verr != nil {
		return failedResponse(verr), 2
	}

	results, order, skipped, rerr := Execute(wf)
	if results == nil {
		results = map[string]any{}
	}
	if order == nil {
		order = []string{}
	}
	if skipped == nil {
		skipped = []string{}
	}

	if rerr != nil {
		return Response{
			Status:         "failed",
			Results:        results,
			ExecutionOrder: order,
			Skipped:        skipped,
			Error:          rerr,
		}, apiExitCode(rerr)
	}

	return Response{
		Status:         "completed",
		Results:        results,
		ExecutionOrder: order,
		Skipped:        skipped,
		Error:          nil,
	}, 0
}

func apiExitCode(err *Error) int {
	if err.Code == "INTERNAL_ERROR" {
		return 3
	}
	if isStructuralCode(err.Code) {
		return 2
	}
	return 1
}

func runSafely() (resp Response, code int) {
	defer func() {
		if r := recover(); r != nil {
			resp = failedResponse(errNoStep("INTERNAL_ERROR", "internal error: %v", r))
			code = 3
		}
	}()
	return runWorkflow()
}

func main() {
	resp, code := runSafely()

	out, err := marshalCompact(resp)
	if err != nil {
		out = []byte(`{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INTERNAL_ERROR","step":null,"message":"response encoding failed"}}`)
		code = 3
	}

	os.Stdout.Write(out)
	os.Stdout.Write([]byte("\n"))
	os.Exit(code)
}
