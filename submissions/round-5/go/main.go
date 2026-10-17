package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
)

type Response struct {
	Status         string           `json:"status"`
	Results        map[string]Value `json:"results"`
	ExecutionOrder []string         `json:"execution_order"`
	Skipped        []string         `json:"skipped"`
	Error          *ErrorInfo       `json:"error"`
}

type ErrorInfo struct {
	Code    string  `json:"code"`
	Step    *string `json:"step"`
	Message string  `json:"message"`
}

func newResponse() *Response {
	return &Response{
		Results:        make(map[string]Value),
		ExecutionOrder: []string{},
		Skipped:        []string{},
	}
}

func emit(resp *Response) {
	if resp.Results == nil {
		resp.Results = make(map[string]Value)
	}
	if resp.ExecutionOrder == nil {
		resp.ExecutionOrder = []string{}
	}
	if resp.Skipped == nil {
		resp.Skipped = []string{}
	}
	enc := json.NewEncoder(os.Stdout)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(resp); err != nil {
		fmt.Fprintf(os.Stderr, "failed to encode response: %v\n", err)
		// Best effort fallback so stdout still carries a JSON document.
		os.Stdout.WriteString(`{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INTERNAL_ERROR","step":null,"message":"response encoding failed"}}` + "\n")
	}
}

func validationResponse(wfe *WorkflowError) *Response {
	resp := newResponse()
	resp.Status = "failed"
	resp.Error = &ErrorInfo{Code: wfe.Code, Step: wfe.Step, Message: wfe.Message}
	return resp
}

func inputErrorResponse(message string) *Response {
	resp := newResponse()
	resp.Status = "failed"
	resp.Error = &ErrorInfo{Code: "INVALID_WORKFLOW", Step: nil, Message: message}
	return resp
}

func main() {
	os.Exit(safeMain())
}

func safeMain() (code int) {
	defer func() {
		if r := recover(); r != nil {
			fmt.Fprintf(os.Stderr, "internal error: %v\n", r)
			resp := newResponse()
			resp.Status = "failed"
			resp.Error = &ErrorInfo{
				Code:    "INTERNAL_ERROR",
				Step:    nil,
				Message: fmt.Sprintf("internal error: %v", r),
			}
			emit(resp)
			code = 3
		}
	}()
	return runCLI()
}

func runCLI() int {
	args := os.Args[1:]
	if len(args) != 2 || args[0] != "run" {
		emit(inputErrorResponse("usage: workflow run <workflow.json> | workflow run -"))
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
		emit(inputErrorResponse(fmt.Sprintf("cannot read workflow input: %v", err)))
		return 2
	}

	root, derr := decodeJSON(data)
	if derr != nil {
		emit(inputErrorResponse(fmt.Sprintf("invalid JSON: %v", derr)))
		return 2
	}

	wf, wfe := validateWorkflow(root)
	if wfe != nil {
		emit(validationResponse(wfe))
		return 2
	}

	resp := runWorkflow(wf)
	emit(resp)
	if resp.Status == "completed" {
		return 0
	}
	return 1
}
