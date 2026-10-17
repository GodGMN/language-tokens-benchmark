package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
)

// Error codes from SPEC.md.
const (
	codeInvalidRequest = "INVALID_REQUEST"
	codeInvalidPath    = "INVALID_PATH"
	codePathNotFound   = "PATH_NOT_FOUND"
	codeIndexError     = "INDEX_ERROR"
	codeUnknownOp      = "UNKNOWN_OP"
	codeTestFailed     = "TEST_FAILED"
	codeInvalidMove    = "INVALID_MOVE"
)

type patchError struct {
	code    string
	message string
}

func newPatchError(code, message string) *patchError {
	return &patchError{code: code, message: message}
}

type okResponse struct {
	Status   string `json:"status"`
	Document any    `json:"document"`
}

type errorResponse struct {
	Status  string `json:"status"`
	Code    string `json:"code"`
	Index   *int   `json:"index"`
	Message string `json:"message"`
}

type operation struct {
	kind  string
	path  string
	from  string
	value any
}

func main() {
	os.Exit(run(os.Args[1:]))
}

func run(args []string) int {
	if len(args) != 2 || args[0] != "apply" {
		return writeRequestError("usage: patch apply <request.json|->")
	}

	var data []byte
	var err error
	if args[1] == "-" {
		data, err = io.ReadAll(os.Stdin)
	} else {
		data, err = os.ReadFile(args[1])
	}
	if err != nil {
		return writeRequestError(fmt.Sprintf("cannot read request: %v", err))
	}

	doc, patch, perr := parseRequest(data)
	if perr != nil {
		// Envelope-level problems have no operation index.
		return writeError(perr.code, nil, perr.message, 2)
	}

	for i, rawOp := range patch {
		top, perr := parseOperation(rawOp)
		if perr != nil {
			// A malformed operation is INVALID_REQUEST (exit 2), while an
			// unknown op is a patch error (exit 1). Both are tied to an index.
			exitCode := 1
			if perr.code == codeInvalidRequest {
				exitCode = 2
			}
			idx := i
			return writeError(perr.code, &idx, perr.message, exitCode)
		}

		newDoc, perr := applyOperation(doc, top)
		if perr != nil {
			idx := i
			return writeError(perr.code, &idx, perr.message, 1)
		}
		doc = newDoc
	}

	enc := json.NewEncoder(os.Stdout)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(okResponse{Status: "ok", Document: doc}); err != nil {
		fmt.Fprintf(os.Stderr, "cannot write response: %v\n", err)
		return 3
	}
	return 0
}

func writeRequestError(message string) int {
	return writeError(codeInvalidRequest, nil, message, 2)
}

func writeError(code string, index *int, message string, exit int) int {
	enc := json.NewEncoder(os.Stdout)
	enc.SetEscapeHTML(false)
	resp := errorResponse{
		Status:  "error",
		Code:    code,
		Index:   index,
		Message: message,
	}
	if err := enc.Encode(resp); err != nil {
		fmt.Fprintf(os.Stderr, "cannot write response: %v\n", err)
		return 3
	}
	return exit
}

// decodeJSON decodes exactly one JSON value, preserving numbers as json.Number.
func decodeJSON(data []byte) (any, error) {
	dec := json.NewDecoder(bytes.NewReader(data))
	dec.UseNumber()
	var v any
	if err := dec.Decode(&v); err != nil {
		return nil, err
	}
	var extra any
	if err := dec.Decode(&extra); !errors.Is(err, io.EOF) {
		if err == nil {
			return nil, errors.New("unexpected trailing JSON value")
		}
		return nil, err
	}
	return v, nil
}

func parseRequest(data []byte) (any, []any, *patchError) {
	v, err := decodeJSON(data)
	if err != nil {
		return nil, nil, newPatchError(codeInvalidRequest, "invalid JSON: "+err.Error())
	}

	top, ok := v.(map[string]any)
	if !ok {
		return nil, nil, newPatchError(codeInvalidRequest, "request must be a JSON object")
	}

	for key := range top {
		if key != "document" && key != "patch" {
			return nil, nil, newPatchError(codeInvalidRequest, "unknown top-level field: "+key)
		}
	}

	doc, ok := top["document"]
	if !ok {
		return nil, nil, newPatchError(codeInvalidRequest, "missing required field: document")
	}
	patchValue, ok := top["patch"]
	if !ok {
		return nil, nil, newPatchError(codeInvalidRequest, "missing required field: patch")
	}
	patch, ok := patchValue.([]any)
	if !ok {
		return nil, nil, newPatchError(codeInvalidRequest, "patch must be an array")
	}

	return doc, patch, nil
}

func parseOperation(raw any) (*operation, *patchError) {
	m, ok := raw.(map[string]any)
	if !ok {
		return nil, newPatchError(codeInvalidRequest, "operation must be an object")
	}

	opValue, ok := m["op"]
	if !ok {
		return nil, newPatchError(codeInvalidRequest, "operation is missing op")
	}
	kind, ok := opValue.(string)
	if !ok {
		return nil, newPatchError(codeInvalidRequest, "operation op must be a string")
	}

	pathValue, ok := m["path"]
	if !ok {
		return nil, newPatchError(codeInvalidRequest, "operation is missing path")
	}
	path, ok := pathValue.(string)
	if !ok {
		return nil, newPatchError(codeInvalidRequest, "operation path must be a string")
	}

	op := &operation{kind: kind, path: path}

	switch kind {
	case "add", "replace", "test":
		if perr := checkFields(m, "op", "path", "value"); perr != nil {
			return nil, perr
		}
		value, ok := m["value"]
		if !ok {
			return nil, newPatchError(codeInvalidRequest, "operation is missing value")
		}
		op.value = value
	case "remove":
		if perr := checkFields(m, "op", "path"); perr != nil {
			return nil, perr
		}
	case "move", "copy":
		if perr := checkFields(m, "op", "path", "from"); perr != nil {
			return nil, perr
		}
		fromValue, ok := m["from"]
		if !ok {
			return nil, newPatchError(codeInvalidRequest, "operation is missing from")
		}
		from, ok := fromValue.(string)
		if !ok {
			return nil, newPatchError(codeInvalidRequest, "operation from must be a string")
		}
		op.from = from
	default:
		return nil, newPatchError(codeUnknownOp, "unknown op: "+kind)
	}

	return op, nil
}

func checkFields(m map[string]any, allowed ...string) *patchError {
	allowedSet := make(map[string]struct{}, len(allowed))
	for _, key := range allowed {
		allowedSet[key] = struct{}{}
	}
	for key := range m {
		if _, ok := allowedSet[key]; !ok {
			return newPatchError(codeInvalidRequest, "unexpected field for operation: "+key)
		}
	}
	return nil
}
