package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
)

// Response is the mandatory output document.
type Response struct {
	Status         string           `json:"status"`
	Results        map[string]Value `json:"results"`
	ExecutionOrder []string         `json:"execution_order"`
	Skipped        []string         `json:"skipped"`
	Error          *ResponseError   `json:"error"`
}

type ResponseError struct {
	Code    string  `json:"code"`
	Step    *string `json:"step"`
	Message string  `json:"message"`
}

func main() {
	os.Exit(runMain(os.Args[1:]))
}

func runMain(args []string) (exitCode int) {
	defer func() {
		if r := recover(); r != nil {
			resp := Response{
				Status:         "failed",
				Results:        map[string]Value{},
				ExecutionOrder: []string{},
				Skipped:        []string{},
				Error:          &ResponseError{Code: "INTERNAL_ERROR", Step: nil, Message: fmt.Sprint(r)},
			}
			writeResponse(resp)
			exitCode = 3
		}
	}()

	resp, code := executeCLI(args)
	writeResponse(resp)
	return code
}

func executeCLI(args []string) (Response, int) {
	if len(args) != 2 || args[0] != "run" {
		return failureResponse("INVALID_WORKFLOW", nil, "usage: workflow run <workflow.json|->"), 2
	}

	var data []byte
	var err error
	if args[1] == "-" {
		data, err = io.ReadAll(os.Stdin)
	} else {
		data, err = os.ReadFile(args[1])
	}
	if err != nil {
		return failureResponse("INVALID_WORKFLOW", nil, "cannot read input: "+err.Error()), 2
	}

	doc, err := decodeDocument(data)
	if err != nil {
		return failureResponse("INVALID_WORKFLOW", nil, "invalid JSON: "+err.Error()), 2
	}

	steps, verr := validateDocument(doc)
	if verr != nil {
		return failureResponse(verr.Code, verr.Step, verr.Msg), verr.Exit
	}

	eng := newEngine(steps)
	if rerr := eng.run(); rerr != nil {
		return eng.response(rerr), rerr.Exit
	}
	return eng.response(nil), 0
}

func decodeDocument(data []byte) (Value, error) {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.UseNumber()
	var raw interface{}
	if err := dec.Decode(&raw); err != nil {
		return Value{}, err
	}
	var extra interface{}
	if err := dec.Decode(&extra); err != io.EOF {
		if err == nil {
			return Value{}, errors.New("multiple JSON documents")
		}
		return Value{}, err
	}
	return fromJSON(raw)
}

func failureResponse(code string, step *string, msg string) Response {
	return Response{
		Status:         "failed",
		Results:        map[string]Value{},
		ExecutionOrder: []string{},
		Skipped:        []string{},
		Error:          &ResponseError{Code: code, Step: step, Message: msg},
	}
}

func (e *engine) response(err *wfError) Response {
	results := make(map[string]Value, len(e.results))
	for k, v := range e.results {
		results[k] = v
	}
	order := make([]string, len(e.order))
	copy(order, e.order)
	skipped := make([]string, 0, len(e.skipped))
	for _, id := range e.order {
		if e.skipped[id] {
			skipped = append(skipped, id)
		}
	}
	resp := Response{
		Status:         "completed",
		Results:        results,
		ExecutionOrder: order,
		Skipped:        skipped,
		Error:          nil,
	}
	if err != nil {
		resp.Status = "failed"
		resp.Error = &ResponseError{Code: err.Code, Step: err.Step, Message: err.Msg}
	}
	return resp
}

func writeResponse(resp Response) {
	data, err := json.Marshal(resp)
	if err != nil {
		// Best-effort fallback; should be unreachable.
		os.Stdout.WriteString(`{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INTERNAL_ERROR","step":null,"message":"marshal failure"}}` + "\n")
		return
	}
	os.Stdout.Write(data)
	os.Stdout.WriteString("\n")
}
