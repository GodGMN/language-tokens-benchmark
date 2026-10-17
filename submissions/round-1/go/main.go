package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
)

func main() {
	os.Exit(run())
}

func run() (exitCode int) {
	defer func() {
		if r := recover(); r != nil {
			resp := failureResponse(&Err{
				Code:    "INTERNAL_ERROR",
				Message: fmt.Sprintf("internal error: %v", r),
			})
			_ = writeResponse(resp)
			exitCode = 3
		}
	}()

	if len(os.Args) != 3 || os.Args[1] != "run" {
		resp := failureResponse(&Err{
			Code:    "INVALID_WORKFLOW",
			Message: "usage: workflow run <workflow.json|->",
		})
		_ = writeResponse(resp)
		return 2
	}

	var data []byte
	var err error
	if os.Args[2] == "-" {
		data, err = io.ReadAll(os.Stdin)
	} else {
		data, err = os.ReadFile(os.Args[2])
	}
	if err != nil {
		resp := failureResponse(&Err{
			Code:    "INVALID_WORKFLOW",
			Message: "could not read workflow: " + err.Error(),
		})
		_ = writeResponse(resp)
		return 2
	}

	doc, decodeErr := decodeDocument(data)
	if decodeErr != nil {
		resp := failureResponse(decodeErr)
		_ = writeResponse(resp)
		return 2
	}

	validated, valErr := validateDocument(doc)
	if valErr != nil {
		resp := failureResponse(valErr)
		_ = writeResponse(resp)
		return 2
	}

	resp, code := execute(validated)
	if err := writeResponse(resp); err != nil {
		return 3
	}
	return code
}

func decodeDocument(data []byte) (interface{}, *Err) {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.UseNumber()
	var doc interface{}
	if err := dec.Decode(&doc); err != nil {
		return nil, &Err{Code: "INVALID_WORKFLOW", Message: "invalid JSON: " + err.Error()}
	}
	var extra interface{}
	if err := dec.Decode(&extra); err != io.EOF {
		return nil, &Err{Code: "INVALID_WORKFLOW", Message: "trailing data after JSON document"}
	}
	return doc, nil
}

func writeResponse(resp *response) error {
	enc := json.NewEncoder(os.Stdout)
	enc.SetEscapeHTML(false)
	return enc.Encode(resp)
}
