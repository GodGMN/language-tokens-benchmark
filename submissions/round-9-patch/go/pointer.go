package main

import (
	"strconv"
	"strings"
)

// parsePointer parses an RFC 6901 JSON Pointer into unescaped segments.
// The empty string denotes the whole document and yields no segments.
func parsePointer(path string) ([]string, *patchError) {
	if path == "" {
		return nil, nil
	}
	if path[0] != '/' {
		return nil, newPatchError(codeInvalidPath, "path must be empty or start with '/'")
	}

	parts := strings.Split(path[1:], "/")
	segments := make([]string, len(parts))
	for i, part := range parts {
		unescaped, ok := unescapeSegment(part)
		if !ok {
			return nil, newPatchError(codeInvalidPath, "invalid '~' escape in path segment: "+part)
		}
		segments[i] = unescaped
	}
	return segments, nil
}

// unescapeSegment applies the RFC 6901 unescaping order: ~1 -> /, then ~0 -> ~.
// A '~' followed by anything other than '0' or '1' is invalid.
func unescapeSegment(segment string) (string, bool) {
	if !strings.ContainsRune(segment, '~') {
		return segment, true
	}

	var b strings.Builder
	b.Grow(len(segment))
	for i := 0; i < len(segment); i++ {
		if segment[i] != '~' {
			b.WriteByte(segment[i])
			continue
		}
		if i+1 >= len(segment) {
			return "", false
		}
		switch segment[i+1] {
		case '0':
			b.WriteByte('~')
			i++
		case '1':
			b.WriteByte('/')
			i++
		default:
			return "", false
		}
	}
	return b.String(), true
}

// parseArrayIndex accepts only "0" or [1-9][0-9]*. It rejects signs,
// leading zeroes, empty tokens and values that do not fit in an int.
func parseArrayIndex(token string) (int, bool) {
	if token == "" {
		return 0, false
	}
	if token == "0" {
		return 0, true
	}
	if token[0] < '1' || token[0] > '9' {
		return 0, false
	}
	for i := 1; i < len(token); i++ {
		if token[i] < '0' || token[i] > '9' {
			return 0, false
		}
	}
	n, err := strconv.ParseUint(token, 10, 64)
	if err != nil {
		return 0, false
	}
	maxInt := uint64(^uint(0) >> 1)
	if n > maxInt {
		return 0, false
	}
	return int(n), true
}

func sameTokens(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// isStrictDescendant reports whether child is a strict descendant of parent.
func isStrictDescendant(parent, child []string) bool {
	if len(child) <= len(parent) {
		return false
	}
	for i := range parent {
		if parent[i] != child[i] {
			return false
		}
	}
	return true
}
