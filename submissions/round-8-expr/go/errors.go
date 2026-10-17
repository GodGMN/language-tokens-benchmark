package main

import (
	"fmt"
	"os"
	"strings"
)

// Error codes from the spec.
const (
	codeInvalidRequest = "INVALID_REQUEST"
	codeParse          = "PARSE_ERROR"
	codeOverflow       = "OVERFLOW"
	codeDivisionByZero = "DIVISION_BY_ZERO"
	codeType           = "TYPE_ERROR"
	codeUnknownVar     = "UNKNOWN_VARIABLE"
	codeUnknownFunc    = "UNKNOWN_FUNCTION"
	codeArity          = "ARITY_ERROR"
	codeEmptyArray     = "EMPTY_ARRAY"
)

// ExprError is an error reported in the response envelope.
type ExprError struct {
	Code   string
	Pos    int
	HasPos bool
	Msg    string
}

func newErr(code string, pos int, format string, args ...interface{}) *ExprError {
	return &ExprError{Code: code, Pos: pos, HasPos: true, Msg: fmt.Sprintf(format, args...)}
}

func invalidRequest(format string, args ...interface{}) *ExprError {
	return &ExprError{Code: codeInvalidRequest, HasPos: false, Msg: fmt.Sprintf(format, args...)}
}

// exitCode returns the process exit code associated with the error.
func (e *ExprError) exitCode() int {
	if e.Code == codeInvalidRequest {
		return 2
	}
	return 1
}

// emitOK writes a success envelope to stdout.
func emitOK(v Value) {
	var b strings.Builder
	b.WriteString(`{"status":"ok","value":`)
	writeValue(&b, v)
	b.WriteString("}\n")
	os.Stdout.WriteString(b.String())
}

// emitError writes an error envelope to stdout.
func emitError(e *ExprError) {
	var b strings.Builder
	b.WriteString(`{"status":"error","code":`)
	writeJSONString(&b, e.Code)
	b.WriteString(`,"position":`)
	if e.HasPos {
		fmt.Fprintf(&b, "%d", e.Pos)
	} else {
		b.WriteString("null")
	}
	b.WriteString(`,"message":`)
	writeJSONString(&b, e.Msg)
	b.WriteString("}\n")
	os.Stdout.WriteString(b.String())
}
