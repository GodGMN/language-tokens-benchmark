package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
)

func main() {
	os.Exit(runMain())
}

func runMain() (exit int) {
	defer func() {
		if r := recover(); r != nil {
			fmt.Fprintf(os.Stderr, "internal error: %v\n", r)
			resp := failedResponseNoStep("INTERNAL_ERROR", fmt.Sprint(r))
			if err := writeResponse(resp); err != nil {
				fmt.Fprintf(os.Stderr, "could not write response: %v\n", err)
			}
			exit = 3
		}
	}()

	args := os.Args[1:]
	if len(args) != 2 || args[0] != "run" {
		fmt.Fprintln(os.Stderr, "usage: workflow run <workflow.json|->")
		resp := failedResponseNoStep("INVALID_WORKFLOW", "usage: workflow run <workflow.json|->")
		_ = writeResponse(resp)
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
		fmt.Fprintf(os.Stderr, "could not read workflow: %v\n", err)
		resp := failedResponseNoStep("INVALID_WORKFLOW", fmt.Sprintf("could not read workflow: %v", err))
		_ = writeResponse(resp)
		return 2
	}

	root, err := decodeDocument(data)
	if err != nil {
		fmt.Fprintf(os.Stderr, "invalid JSON document: %v\n", err)
		resp := failedResponseNoStep("INVALID_WORKFLOW", fmt.Sprintf("invalid JSON document: %v", err))
		_ = writeResponse(resp)
		return 2
	}

	steps, verr := validateWorkflow(root)
	if verr != nil {
		fmt.Fprintf(os.Stderr, "validation failed: %s: %s\n", verr.Code, verr.Message)
		resp := &Response{
			Status:         "failed",
			Results:        map[string]Value{},
			ExecutionOrder: []string{},
			Skipped:        []string{},
			Error:          &ErrorInfo{Code: verr.Code, Step: verr.Step, Message: verr.Message},
		}
		_ = writeResponse(resp)
		return exitCodeFor(verr.Code)
	}

	resp := executeWorkflow(steps)
	if err := writeResponse(resp); err != nil {
		fmt.Fprintf(os.Stderr, "could not write response: %v\n", err)
		return 3
	}
	if resp.Status == "completed" {
		return 0
	}
	if resp.Error != nil {
		return exitCodeFor(resp.Error.Code)
	}
	return 3
}

func writeResponse(resp *Response) error {
	enc := json.NewEncoder(os.Stdout)
	enc.SetEscapeHTML(false)
	return enc.Encode(resp)
}
