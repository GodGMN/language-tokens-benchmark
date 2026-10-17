package main

// applyOperation parses the operation's pointers and applies it to doc.
// The returned document is the new root (it may differ when the root itself is
// replaced). On error the caller discards doc, preserving atomicity.
func applyOperation(doc any, op *operation) (any, *patchError) {
	path, perr := parsePointer(op.path)
	if perr != nil {
		return nil, perr
	}

	switch op.kind {
	case "add":
		return addAt(doc, path, op.value)
	case "remove":
		if len(path) == 0 {
			return nil, newPatchError(codeInvalidPath, "cannot remove the document root")
		}
		newDoc, _, e := removeAt(doc, path)
		return newDoc, e
	case "replace":
		return replaceAt(doc, path, op.value)
	case "move":
		return applyMove(doc, path, op.from)
	case "copy":
		return applyCopy(doc, path, op.from)
	case "test":
		return applyTest(doc, path, op.value)
	default:
		// parseOperation already rejected unknown ops.
		return nil, newPatchError(codeUnknownOp, "unknown op: "+op.kind)
	}
}

func applyMove(doc any, path []string, fromPath string) (any, *patchError) {
	from, perr := parsePointer(fromPath)
	if perr != nil {
		return nil, perr
	}

	// from must exist first (SPEC order for move).
	if _, e := getAt(doc, from); e != nil {
		return nil, e
	}
	if sameTokens(from, path) {
		return nil, newPatchError(codeInvalidMove, "cannot move a value onto itself")
	}
	if isStrictDescendant(from, path) {
		return nil, newPatchError(codeInvalidMove, "cannot move a value into its own descendant")
	}

	newDoc, removed, e := removeAt(doc, from)
	if e != nil {
		return nil, e
	}
	return addAt(newDoc, path, removed)
}

func applyCopy(doc any, path []string, fromPath string) (any, *patchError) {
	from, perr := parsePointer(fromPath)
	if perr != nil {
		return nil, perr
	}
	value, e := getAt(doc, from)
	if e != nil {
		return nil, e
	}
	return addAt(doc, path, deepCopy(value))
}

func applyTest(doc any, path []string, expected any) (any, *patchError) {
	actual, e := getAt(doc, path)
	if e != nil {
		return nil, e
	}
	if !jsonEqual(actual, expected) {
		return nil, newPatchError(codeTestFailed, "test operation failed")
	}
	return doc, nil
}

// getAt resolves a parsed pointer to its value.
func getAt(node any, tokens []string) (any, *patchError) {
	cur := node
	for _, token := range tokens {
		switch n := cur.(type) {
		case map[string]any:
			next, ok := n[token]
			if !ok {
				return nil, newPatchError(codePathNotFound, "object member not found: "+token)
			}
			cur = next
		case []any:
			idx, ok := parseArrayIndex(token)
			if !ok || idx >= len(n) {
				return nil, newPatchError(codeIndexError, "array index out of range: "+token)
			}
			cur = n[idx]
		default:
			return nil, newPatchError(codePathNotFound, "cannot traverse a scalar value")
		}
	}
	return cur, nil
}

// addAt implements add semantics, including array insertion and the "-" token.
func addAt(node any, tokens []string, value any) (any, *patchError) {
	if len(tokens) == 0 {
		return value, nil
	}

	token := tokens[0]
	switch n := node.(type) {
	case map[string]any:
		if len(tokens) == 1 {
			n[token] = value
			return n, nil
		}
		child, ok := n[token]
		if !ok {
			return nil, newPatchError(codePathNotFound, "object member not found: "+token)
		}
		newChild, e := addAt(child, tokens[1:], value)
		if e != nil {
			return nil, e
		}
		n[token] = newChild
		return n, nil
	case []any:
		if len(tokens) == 1 {
			idx := len(n)
			if token != "-" {
				parsed, ok := parseArrayIndex(token)
				if !ok || parsed > len(n) {
					return nil, newPatchError(codeIndexError, "array add index out of range: "+token)
				}
				idx = parsed
			}
			n = append(n, nil)
			copy(n[idx+1:], n[idx:])
			n[idx] = value
			return n, nil
		}
		idx, ok := parseArrayIndex(token)
		if !ok || idx >= len(n) {
			return nil, newPatchError(codeIndexError, "array index out of range: "+token)
		}
		newChild, e := addAt(n[idx], tokens[1:], value)
		if e != nil {
			return nil, e
		}
		n[idx] = newChild
		return n, nil
	default:
		return nil, newPatchError(codePathNotFound, "cannot add below a scalar value")
	}
}

// removeAt removes the value at tokens and returns the new root and removed value.
func removeAt(node any, tokens []string) (any, any, *patchError) {
	if len(tokens) == 0 {
		return nil, nil, newPatchError(codeInvalidPath, "cannot remove the document root")
	}

	token := tokens[0]
	switch n := node.(type) {
	case map[string]any:
		if len(tokens) == 1 {
			removed, ok := n[token]
			if !ok {
				return nil, nil, newPatchError(codePathNotFound, "object member not found: "+token)
			}
			delete(n, token)
			return n, removed, nil
		}
		child, ok := n[token]
		if !ok {
			return nil, nil, newPatchError(codePathNotFound, "object member not found: "+token)
		}
		newChild, removed, e := removeAt(child, tokens[1:])
		if e != nil {
			return nil, nil, e
		}
		n[token] = newChild
		return n, removed, nil
	case []any:
		idx, ok := parseArrayIndex(token)
		if !ok || idx >= len(n) {
			return nil, nil, newPatchError(codeIndexError, "array index out of range: "+token)
		}
		if len(tokens) == 1 {
			removed := n[idx]
			n = append(n[:idx], n[idx+1:]...)
			return n, removed, nil
		}
		newChild, removed, e := removeAt(n[idx], tokens[1:])
		if e != nil {
			return nil, nil, e
		}
		n[idx] = newChild
		return n, removed, nil
	default:
		return nil, nil, newPatchError(codePathNotFound, "cannot remove below a scalar value")
	}
}

// replaceAt implements replace semantics: the target must already exist.
func replaceAt(node any, tokens []string, value any) (any, *patchError) {
	if len(tokens) == 0 {
		return value, nil
	}

	token := tokens[0]
	switch n := node.(type) {
	case map[string]any:
		if len(tokens) == 1 {
			if _, ok := n[token]; !ok {
				return nil, newPatchError(codePathNotFound, "object member not found: "+token)
			}
			n[token] = value
			return n, nil
		}
		child, ok := n[token]
		if !ok {
			return nil, newPatchError(codePathNotFound, "object member not found: "+token)
		}
		newChild, e := replaceAt(child, tokens[1:], value)
		if e != nil {
			return nil, e
		}
		n[token] = newChild
		return n, nil
	case []any:
		idx, ok := parseArrayIndex(token)
		if !ok || idx >= len(n) {
			return nil, newPatchError(codeIndexError, "array index out of range: "+token)
		}
		if len(tokens) == 1 {
			n[idx] = value
			return n, nil
		}
		newChild, e := replaceAt(n[idx], tokens[1:], value)
		if e != nil {
			return nil, e
		}
		n[idx] = newChild
		return n, nil
	default:
		return nil, newPatchError(codePathNotFound, "cannot replace below a scalar value")
	}
}
