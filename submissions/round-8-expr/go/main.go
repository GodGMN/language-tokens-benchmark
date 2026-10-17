package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"strconv"
)

func main() {
	os.Exit(run(os.Args[1:]))
}

func run(args []string) (code int) {
	defer func() {
		if r := recover(); r != nil {
			fmt.Fprintf(os.Stderr, "internal error: %v\n", r)
			os.Stdout.WriteString(`{"status":"error","code":"INTERNAL_ERROR","position":null,"message":"internal error"}` + "\n")
			code = 3
		}
	}()

	if len(args) != 2 || args[0] != "eval" {
		e := invalidRequest("usage: expr eval <request.json>")
		emitError(e)
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
		emitError(invalidRequest("cannot read request: %v", err))
		return 2
	}

	source, vars, reqErr := parseRequest(data)
	if reqErr != nil {
		emitError(reqErr)
		return 2
	}

	ast, perr := parseSource(source)
	if perr != nil {
		emitError(perr)
		return 1
	}

	val, eerr := evalNode(ast, vars)
	if eerr != nil {
		emitError(eerr)
		return 1
	}

	emitOK(val)
	return 0
}

// parseRequest validates the request envelope and converts vars to Values.
func parseRequest(data []byte) (string, map[string]Value, *ExprError) {
	dec := json.NewDecoder(bytes.NewReader(data))
	var raw json.RawMessage
	if err := dec.Decode(&raw); err != nil {
		return "", nil, invalidRequest("request is not valid JSON")
	}
	var extra json.RawMessage
	if err := dec.Decode(&extra); err != io.EOF {
		return "", nil, invalidRequest("trailing data after request")
	}

	trimmed := bytes.TrimSpace(raw)
	if len(trimmed) == 0 || trimmed[0] != '{' {
		return "", nil, invalidRequest("request must be a JSON object")
	}
	var obj map[string]json.RawMessage
	if err := json.Unmarshal(trimmed, &obj); err != nil || obj == nil {
		return "", nil, invalidRequest("request must be a JSON object")
	}

	for k := range obj {
		if k != "source" && k != "vars" {
			return "", nil, invalidRequest("unknown request field %q", k)
		}
	}

	srcRaw, ok := obj["source"]
	if !ok {
		return "", nil, invalidRequest(`missing "source"`)
	}
	srcJSON := bytes.TrimSpace(srcRaw)
	if len(srcJSON) == 0 || srcJSON[0] != '"' {
		return "", nil, invalidRequest(`"source" must be a string`)
	}
	var source string
	if err := json.Unmarshal(srcJSON, &source); err != nil {
		return "", nil, invalidRequest(`"source" must be a string`)
	}

	vars := map[string]Value{}
	if vRaw, ok := obj["vars"]; ok {
		vJSON := bytes.TrimSpace(vRaw)
		if len(vJSON) == 0 || vJSON[0] != '{' {
			return "", nil, invalidRequest(`"vars" must be an object`)
		}
		var vm map[string]json.RawMessage
		if err := json.Unmarshal(vJSON, &vm); err != nil || vm == nil {
			return "", nil, invalidRequest(`"vars" must be an object`)
		}
		for k, rawV := range vm {
			v, ok := rawToValue(rawV)
			if !ok {
				return "", nil, invalidRequest("unsupported value for variable %q", k)
			}
			vars[k] = v
		}
	}

	return source, vars, nil
}

// rawToValue converts a JSON value into a Value. Only null, booleans, i64
// integers, strings and arrays thereof are representable.
func rawToValue(raw json.RawMessage) (Value, bool) {
	dec := json.NewDecoder(bytes.NewReader(raw))
	dec.UseNumber()
	var x interface{}
	if err := dec.Decode(&x); err != nil {
		return Value{}, false
	}
	return fromJSONValue(x)
}

func fromJSONValue(x interface{}) (Value, bool) {
	switch t := x.(type) {
	case nil:
		return vNull(), true
	case bool:
		return vBool(t), true
	case string:
		return vStr(t), true
	case json.Number:
		i, err := strconv.ParseInt(t.String(), 10, 64)
		if err != nil {
			return Value{}, false
		}
		return vInt(i), true
	case []interface{}:
		arr := make([]Value, 0, len(t))
		for _, e := range t {
			v, ok := fromJSONValue(e)
			if !ok {
				return Value{}, false
			}
			arr = append(arr, v)
		}
		return vArr(arr), true
	default:
		return Value{}, false
	}
}
