package main

// WErr is a workflow error with the code that must be reported in the
// response. Step is nil when the error is a property of the whole document.
type WErr struct {
	Code    string
	Step    *string
	Message string
}

func newErr(code, step, message string) *WErr {
	var sp *string
	if step != "" {
		s := step
		sp = &s
	}
	return &WErr{Code: code, Step: sp, Message: message}
}

// newStepErr always records the step id, even when it is the empty string.
// This is used for validation errors where the document did contain a string
// id (which may itself be invalid).
func newStepErr(code, step, message string) *WErr {
	s := step
	return &WErr{Code: code, Step: &s, Message: message}
}
