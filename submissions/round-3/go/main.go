package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
)

// ErrorInfo is the error object of the response.
type ErrorInfo struct {
	Code    string  `json:"code"`
	Step    *string `json:"step"`
	Message string  `json:"message"`
}

// Response is the single JSON document written to stdout.
type Response struct {
	Status         string                 `json:"status"`
	Results        map[string]interface{} `json:"results"`
	ExecutionOrder []string               `json:"execution_order"`
	Skipped        []string               `json:"skipped"`
	Error          *ErrorInfo             `json:"error"`
}

func emptyResponse() Response {
	return Response{
		Status:         "completed",
		Results:        map[string]interface{}{},
		ExecutionOrder: []string{},
		Skipped:        []string{},
	}
}

func failureResponse(code string, step *string, message string) Response {
	return Response{
		Status:         "failed",
		Results:        map[string]interface{}{},
		ExecutionOrder: []string{},
		Skipped:        []string{},
		Error:          &ErrorInfo{Code: code, Step: step, Message: message},
	}
}

func writeResponse(r Response) {
	enc := json.NewEncoder(os.Stdout)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(r); err != nil {
		fmt.Fprintf(os.Stderr, "failed to write response: %v\n", err)
	}
}

func exitCodeFor(code string) int {
	switch code {
	case "INVALID_WORKFLOW", "UNKNOWN_DEPENDENCY", "CYCLIC_DEPENDENCY":
		return 2
	}
	return 1
}

// runMain implements the CLI. It returns the process exit code.
func runMain() int {
	args := os.Args[1:]
	if len(args) != 2 || args[0] != "run" {
		writeResponse(failureResponse("INVALID_WORKFLOW", nil, "usage: workflow run <workflow.json>"))
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
		writeResponse(failureResponse("INVALID_WORKFLOW", nil, fmt.Sprintf("cannot read input: %v", err)))
		return 2
	}

	doc, err := decodeDocument(data)
	if err != nil {
		writeResponse(failureResponse("INVALID_WORKFLOW", nil, fmt.Sprintf("invalid JSON: %v", err)))
		return 2
	}

	wf, werr := parseWorkflow(doc)
	if werr != nil {
		writeResponse(failureResponse(werr.Code, werr.Step, werr.Message))
		return exitCodeFor(werr.Code)
	}

	eng := newEngine(wf)
	if werr := eng.Execute(); werr != nil {
		resp := Response{
			Status:         "failed",
			Results:        eng.results,
			ExecutionOrder: eng.order,
			Skipped:        skippedList(eng),
			Error:          &ErrorInfo{Code: werr.Code, Step: werr.Step, Message: werr.Message},
		}
		writeResponse(resp)
		return exitCodeFor(werr.Code)
	}

	resp := Response{
		Status:         "completed",
		Results:        eng.results,
		ExecutionOrder: eng.order,
		Skipped:        skippedList(eng),
		Error:          nil,
	}
	writeResponse(resp)
	return 0
}

func skippedList(e *Engine) []string {
	out := []string{}
	for _, id := range e.order {
		if e.skipped[id] {
			out = append(out, id)
		}
	}
	return out
}

func main() {
	os.Exit(entryPoint())
}

func entryPoint() (code int) {
	code = 3
	defer func() {
		if r := recover(); r != nil {
			fmt.Fprintf(os.Stderr, "internal panic: %v\n", r)
			writeResponse(failureResponse("INTERNAL_ERROR", nil, fmt.Sprintf("%v", r)))
			code = 3
		}
	}()
	return runMain()
}
