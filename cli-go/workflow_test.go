package main

import (
	"encoding/json"
	"reflect"
	"testing"
)

func runDoc(t *testing.T, src string) (*response, int) {
	t.Helper()
	doc, derr := decodeDocument([]byte(src))
	if derr != nil {
		return failureResponse(derr), 2
	}
	d, verr := validateDocument(doc)
	if verr != nil {
		return failureResponse(verr), 2
	}
	return execute(d)
}

func mustJSON(t *testing.T, v interface{}) interface{} {
	t.Helper()
	b, err := json.Marshal(v)
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	var out interface{}
	if err := json.Unmarshal(b, &out); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	return out
}

func checkResponse(t *testing.T, src, expected string, wantCode int) {
	t.Helper()
	got, code := runDoc(t, src)
	if code != wantCode {
		t.Fatalf("exit code = %d, want %d; response=%s", code, wantCode, dump(t, got))
	}
	want := normalizeMessages(parseJSON(t, expected))
	gotV := normalizeMessages(mustJSON(t, got))
	if !reflect.DeepEqual(gotV, want) {
		t.Fatalf("response mismatch\n got: %s\nwant: %s", dump(t, got), expected)
	}
}

func parseJSON(t *testing.T, src string) interface{} {
	t.Helper()
	var out interface{}
	if err := json.Unmarshal([]byte(src), &out); err != nil {
		t.Fatalf("parse expected JSON: %v", err)
	}
	return out
}

func normalizeMessages(v interface{}) interface{} {
	if m, ok := v.(map[string]interface{}); ok {
		if em, ok := m["error"].(map[string]interface{}); ok {
			em["message"] = ""
		}
	}
	return v
}

func dump(t *testing.T, v interface{}) string {
	t.Helper()
	b, _ := json.Marshal(v)
	return string(b)
}

// one-step helper: build a workflow around a single op/args.
func one(op string, args string, extra string) string {
	if args == "" {
		args = "{}"
	}
	s := `{"version":1,"steps":[{"id":"s","op":"` + op + `","args":` + args
	if extra != "" {
		s += "," + extra
	}
	s += `}]}`
	return s
}

func checkOK(t *testing.T, op, args, extra, results string) {
	t.Helper()
	checkResponse(t, one(op, args, extra),
		`{"status":"completed","results":{"s":`+results+`},"execution_order":["s"],"skipped":[],"error":null}`, 0)
}

func checkErr(t *testing.T, op, args, extra, code string, exit int) {
	t.Helper()
	checkResponse(t, one(op, args, extra),
		`{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"`+code+`","step":"s","message":"x"}}`, exit)
}

func TestSpecExamples(t *testing.T) {
	checkResponse(t, `{"version":1,"steps":[
		{"id":"n","op":"range","args":{"start":1,"end":5}},
		{"id":"evens","op":"filter","depends_on":["n"],"args":{"input":{"$ref":"n"},"predicate":{"type":"even"}}},
		{"id":"total","op":"sum","depends_on":["evens"],"args":{"input":{"$ref":"evens"}}}]}`,
		`{"status":"completed","results":{"n":[1,2,3,4,5],"evens":[2,4],"total":6},"execution_order":["n","evens","total"],"skipped":[],"error":null}`, 0)

	checkResponse(t, `{"version":1,"steps":[
		{"id":"x","op":"constant","args":{"value":3}},
		{"id":"y","op":"constant","args":{"value":99},"depends_on":["x"],
		 "when":{"op":"gt","left":{"$ref":"x"},"right":10}}]}`,
		`{"status":"completed","results":{"x":3,"y":null},"execution_order":["x","y"],"skipped":["y"],"error":null}`, 0)

	checkResponse(t, `{"version":1,"steps":[
		{"id":"flaky","op":"fail_until","args":{"failures":2,"value":"ok"},"retry":{"max_attempts":3}}]}`,
		`{"status":"completed","results":{"flaky":"ok"},"execution_order":["flaky"],"skipped":[],"error":null}`, 0)
}

func TestEmptyWorkflow(t *testing.T) {
	checkResponse(t, `{"version":1,"steps":[]}`,
		`{"status":"completed","results":{},"execution_order":[],"skipped":[],"error":null}`, 0)
}

func TestStructuralValidation(t *testing.T) {
	checkResponse(t, `{"version":1,"steps":[],"x":1}`,
		`{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INVALID_WORKFLOW","step":null,"message":"x"}}`, 2)
	checkResponse(t, `{"version":2,"steps":[]}`,
		`{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INVALID_WORKFLOW","step":null,"message":"x"}}`, 2)
	checkResponse(t, `{"steps":[]}`,
		`{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INVALID_WORKFLOW","step":null,"message":"x"}}`, 2)
	checkResponse(t, `{"version":1}`,
		`{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INVALID_WORKFLOW","step":null,"message":"x"}}`, 2)
	checkResponse(t, `{"version":1,"steps":[{"id":"a","op":"nope"}]}`,
		`{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INVALID_WORKFLOW","step":"a","message":"x"}}`, 2)
	checkResponse(t, `{"version":1,"steps":[{"id":"a","op":"constant","bad":1}]}`,
		`{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INVALID_WORKFLOW","step":"a","message":"x"}}`, 2)
	checkResponse(t, `{"version":1,"steps":[{"id":"a","op":"constant"},{"id":"a","op":"constant"}]}`,
		`{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INVALID_WORKFLOW","step":"a","message":"x"}}`, 2)
	checkResponse(t, `{"version":1,"steps":[{"id":"1a","op":"constant"}]}`,
		`{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INVALID_WORKFLOW","step":"1a","message":"x"}}`, 2)
	checkResponse(t, `{"version":1,"steps":[{"id":"a","op":"constant","retry":{"max_attempts":6}}]}`,
		`{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INVALID_WORKFLOW","step":"a","message":"x"}}`, 2)
	checkResponse(t, `{"version":1,"steps":[{"id":"a","op":"constant","depends_on":["b"]}]}`,
		`{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"UNKNOWN_DEPENDENCY","step":"a","message":"x"}}`, 2)
	checkResponse(t, `{"version":1,"steps":[
		{"id":"a","op":"constant","depends_on":["b"]},
		{"id":"b","op":"constant","depends_on":["a"]}]}`,
		`{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"CYCLIC_DEPENDENCY","step":null,"message":"x"}}`, 2)
	checkResponse(t, `{"version":1,"steps":[{"id":"a","op":"constant","depends_on":["a"]}]}`,
		`{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"CYCLIC_DEPENDENCY","step":null,"message":"x"}}`, 2)
}

func TestMath(t *testing.T) {
	checkOK(t, "add", `{"a":7,"b":2}`, "", `9`)
	checkOK(t, "subtract", `{"a":7,"b":2}`, "", `5`)
	checkOK(t, "multiply", `{"a":-7,"b":2}`, "", `-14`)
	checkOK(t, "divide", `{"a":7,"b":2}`, "", `3`)
	checkOK(t, "divide", `{"a":-7,"b":2}`, "", `-3`)
	checkOK(t, "divide", `{"a":7,"b":-2}`, "", `-3`)
	checkOK(t, "mod", `{"a":-7,"b":2}`, "", `-1`)
	checkOK(t, "mod", `{"a":7,"b":-2}`, "", `1`)
	checkOK(t, "sum", `{"input":[1,2,3]}`, "", `6`)
	checkOK(t, "sum", `{"input":[]}`, "", `0`)
	checkOK(t, "min", `{"input":[3,-1,2]}`, "", `-1`)
	checkOK(t, "max", `{"input":[3,-1,2]}`, "", `3`)
	checkOK(t, "range", `{"start":3,"end":3}`, "", `[3]`)
	checkOK(t, "range", `{"start":4,"end":3}`, "", `[]`)
	checkOK(t, "range", `{"start":-2,"end":1}`, "", `[-2,-1,0,1]`)
	checkOK(t, "add", `{"a":1.0,"b":2e1}`, "", `21`)

	checkErr(t, "divide", `{"a":1,"b":0}`, "", "DIVISION_BY_ZERO", 1)
	checkErr(t, "mod", `{"a":1,"b":0}`, "", "DIVISION_BY_ZERO", 1)
	checkErr(t, "add", `{"a":9223372036854775807,"b":1}`, "", "ARITHMETIC_OVERFLOW", 1)
	checkErr(t, "subtract", `{"a":-9223372036854775808,"b":1}`, "", "ARITHMETIC_OVERFLOW", 1)
	checkErr(t, "multiply", `{"a":9223372036854775807,"b":2}`, "", "ARITHMETIC_OVERFLOW", 1)
	checkErr(t, "divide", `{"a":-9223372036854775808,"b":-1}`, "", "ARITHMETIC_OVERFLOW", 1)
	checkErr(t, "sum", `{"input":[9223372036854775807,1]}`, "", "ARITHMETIC_OVERFLOW", 1)
	checkErr(t, "min", `{"input":[]}`, "", "EMPTY_ARRAY", 1)
	checkErr(t, "max", `{"input":[]}`, "", "EMPTY_ARRAY", 1)
	checkErr(t, "add", `{"a":1.5,"b":2}`, "", "INVALID_ARG", 1)
	checkErr(t, "add", `{"a":9223372036854775808,"b":1}`, "", "INVALID_ARG", 1)
	checkErr(t, "add", `{"a":"1","b":2}`, "", "INVALID_ARG", 1)
	checkErr(t, "add", `{"a":1,"b":2,"c":3}`, "", "INVALID_ARG", 1)
	checkErr(t, "sum", `{"input":[1,"x"]}`, "", "INVALID_ARG", 1)
}

func TestStrings(t *testing.T) {
	checkOK(t, "concat", `{"a":"foo","b":"bar"}`, "", `"foobar"`)
	checkOK(t, "split", `{"input":"a,b,,c","sep":","}`, "", `["a","b","","c"]`)
	checkOK(t, "split", `{"input":"hé","sep":""}`, "", `["h","é"]`)
	checkOK(t, "join", `{"input":["a","b"],"sep":"-"}`, "", `"a-b"`)
	checkOK(t, "join", `{"input":[],"sep":"-"}`, "", `""`)
	checkOK(t, "uppercase", `{"input":"aZé9"}`, "", `"AZé9"`)
	checkOK(t, "lowercase", `{"input":"AzÉ9"}`, "", `"azÉ9"`)
	checkOK(t, "trim", `{"input":" \t\nhi\r\f\u000b "}`, "", `"hi"`)
	checkOK(t, "replace", `{"input":"aaa","search":"aa","replace":"b"}`, "", `"ba"`)
	checkOK(t, "length", `{"input":"hé😀"}`, "", `3`)
	checkErr(t, "concat", `{"a":"x","b":1}`, "", "INVALID_ARG", 1)
	checkErr(t, "length", `{"input":["x"]}`, "", "INVALID_ARG", 1)
}

func TestArrays(t *testing.T) {
	checkOK(t, "map", `{"input":[1,2,3],"transform":{"type":"add","value":10}}`, "", `[11,12,13]`)
	checkOK(t, "map", `{"input":[1,-2],"transform":{"type":"abs"}}`, "", `[1,2]`)
	checkOK(t, "map", `{"input":[1,2],"transform":{"type":"negate"}}`, "", `[-1,-2]`)
	checkOK(t, "map", `{"input":[1,2],"transform":{"type":"multiply","value":3}}`, "", `[3,6]`)
	checkOK(t, "map", `{"input":[7,8],"transform":{"type":"op","op":"subtract","args":{"a":{"$self":true},"b":5}}}`, "", `[2,3]`)
	checkOK(t, "map", `{"input":[7,8],"transform":{"type":"op","op":"subtract","args":{"a":10,"b":{"$self":true}}}}`, "", `[3,2]`)
	checkOK(t, "map", `{"input":[1,2],"transform":{"type":"constant","value":"x"}}`, "", `["x","x"]`)
	checkOK(t, "map", `{"input":[1,"a",null],"transform":{"type":"type"}}`, "", `["number","string","null"]`)
	checkOK(t, "map", `{"input":[1,"a",true],"transform":{"type":"stringify"}}`, "", `["1","\"a\"","true"]`)
	checkOK(t, "filter", `{"input":[1,2,3,4],"predicate":{"type":"odd"}}`, "", `[1,3]`)
	checkOK(t, "filter", `{"input":[1,2,3,4],"predicate":{"type":"gte","value":3}}`, "", `[3,4]`)
	checkOK(t, "filter", `{"input":[1,2,3],"predicate":{"type":"eq","value":2}}`, "", `[2]`)
	checkOK(t, "filter", `{"input":[1,2,3],"predicate":{"type":"ne","value":2}}`, "", `[1,3]`)
	checkOK(t, "filter", `{"input":[0,"",null,false,[],{}],"predicate":{"type":"truthy"}}`, "", `[]`)
	checkOK(t, "filter", `{"input":[1,"a",null,true,false],"predicate":{"type":"type_is","value":"string"}}`, "", `["a"]`)
	checkOK(t, "filter", `{"input":["abc","xyz"],"predicate":{"type":"string_contains","value":"b"}}`, "", `["abc"]`)
	checkOK(t, "reduce", `{"input":[1,2,3],"op":"add"}`, "", `6`)
	checkOK(t, "reduce", `{"input":[1,2,3],"op":"subtract"}`, "", `-6`)
	checkOK(t, "reduce", `{"input":[2,3],"op":"multiply"}`, "", `6`)
	checkOK(t, "reduce", `{"input":[],"op":"multiply"}`, "", `1`)
	checkOK(t, "reduce", `{"input":[1,2],"op":"add","initial":10}`, "", `13`)
	checkOK(t, "sort", `{"input":[3,1,2]}`, "", `[1,2,3]`)
	checkOK(t, "sort", `{"input":["b","a"]}`, "", `["a","b"]`)
	checkOK(t, "sort", `{"input":[true,false]}`, "", `[false,true]`)
	checkOK(t, "sort", `{"input":[null,"a",1,true,[],{}]}`, "", `[1,"a",true,null,[],{}]`)
	checkOK(t, "sort", `{"input":[2,1,2,1]}`, "", `[1,1,2,2]`)
	checkOK(t, "reverse", `{"input":[1,2,3]}`, "", `[3,2,1]`)
	checkOK(t, "unique", `{"input":[1,2,1,"a","a",null,null]}`, "", `[1,2,"a",null]`)
	checkOK(t, "slice", `{"input":[0,1,2,3,4],"start":1,"end":3}`, "", `[1,2]`)
	checkOK(t, "slice", `{"input":[0,1,2,3,4],"start":-2}`, "", `[3,4]`)
	checkOK(t, "slice", `{"input":[0,1,2,3,4],"start":-100,"end":2}`, "", `[0,1]`)
	checkOK(t, "slice", `{"input":[0,1,2,3,4],"start":3,"end":1}`, "", `[]`)
	checkOK(t, "slice", `{"input":[0,1,2,3,4],"start":100}`, "", `[]`)
	checkOK(t, "flatten", `{"input":[1,[2,3],[],4,[[5]]]}`, "", `[1,2,3,4,[5]]`)
	checkErr(t, "map", `{"input":[1],"transform":{"type":"add","value":9223372036854775807}}`, "", "ARITHMETIC_OVERFLOW", 1)
	checkErr(t, "map", `{"input":[1],"transform":{"type":"divide","value":0}}`, "", "DIVISION_BY_ZERO", 1)
	checkErr(t, "map", `{"input":["a"],"transform":{"type":"add","value":1}}`, "", "INVALID_ARG", 1)
	checkErr(t, "map", `{"input":[1],"transform":{"type":"bogus"}}`, "", "INVALID_ARG", 1)
	checkErr(t, "filter", `{"input":["a"],"predicate":{"type":"even"}}`, "", "INVALID_ARG", 1)
	checkErr(t, "reduce", `{"input":[1],"op":"divide"}`, "", "INVALID_ARG", 1)
	checkErr(t, "slice", `{"input":[1],"start":1.5}`, "", "INVALID_ARG", 1)
}

func TestObjects(t *testing.T) {
	checkOK(t, "get", `{"input":{"a":1},"key":"a"}`, "", `1`)
	checkOK(t, "get", `{"input":{"a":1},"key":"b"}`, "", `null`)
	checkOK(t, "set", `{"input":{"a":1},"key":"b","value":2}`, "", `{"a":1,"b":2}`)
	checkOK(t, "set", `{"input":{"a":1},"key":"a","value":null}`, "", `{"a":null}`)
	checkOK(t, "keys", `{"input":{"b":1,"a":2}}`, "", `["a","b"]`)
	checkOK(t, "values", `{"input":{"b":1,"a":2}}`, "", `[2,1]`)
	checkOK(t, "merge", `{"a":{"x":1,"y":1},"b":{"y":2,"z":3}}`, "", `{"x":1,"y":2,"z":3}`)
	checkOK(t, "pick", `{"input":{"a":1,"b":2,"c":3},"keys":["c","a","z"]}`, "", `{"a":1,"c":3,"z":null}`)
	checkOK(t, "omit", `{"input":{"a":1,"b":2,"c":3},"keys":["b"]}`, "", `{"a":1,"c":3}`)
	checkErr(t, "get", `{"input":[1],"key":"a"}`, "", "INVALID_ARG", 1)
	checkErr(t, "pick", `{"input":{},"keys":[1]}`, "", "INVALID_ARG", 1)
}

func TestUtilities(t *testing.T) {
	checkOK(t, "constant", `{"value":{"b":[1,true,null],"a":"x"}}`, "", `{"a":"x","b":[1,true,null]}`)
	checkOK(t, "equals", `{"a":1,"b":1.0}`, "", `true`)
	checkOK(t, "equals", `{"a":{"x":[1,2]},"b":{"x":[1,2]}}`, "", `true`)
	checkOK(t, "equals", `{"a":1,"b":"1"}`, "", `false`)
	checkOK(t, "equals", `{"a":null,"b":null}`, "", `true`)
	checkOK(t, "compare", `{"a":2,"b":2,"op":"gte"}`, "", `true`)
	checkOK(t, "compare", `{"a":"a","b":"b","op":"lt"}`, "", `true`)
	checkOK(t, "compare", `{"a":1,"b":"a","op":"lt"}`, "", `true`)
	checkOK(t, "compare", `{"a":null,"b":[],"op":"lt"}`, "", `true`)
	checkOK(t, "and", `{"a":true,"b":true}`, "", `true`)
	checkOK(t, "and", `{"a":1,"b":"x"}`, "", `true`)
	checkOK(t, "and", `{"a":1,"b":0}`, "", `false`)
	checkOK(t, "or", `{"a":0,"b":""}`, "", `false`)
	checkOK(t, "or", `{"a":0,"b":"x"}`, "", `true`)
	checkOK(t, "not", `{"a":[]}`, "", `true`)
	checkOK(t, "not", `{"a":{}}`, "", `true`)
	checkOK(t, "not", `{"a":0.0}`, "", `true`)
	checkOK(t, "type", `{"value":null}`, "", `"null"`)
	checkOK(t, "type", `{"value":true}`, "", `"boolean"`)
	checkOK(t, "type", `{"value":1.5}`, "", `"number"`)
	checkOK(t, "type", `{"value":[]}`, "", `"array"`)
	checkOK(t, "type", `{"value":{}}`, "", `"object"`)
	checkErr(t, "compare", `{"a":1,"b":2,"op":"eq"}`, "", "INVALID_ARG", 1)
}

func TestReferences(t *testing.T) {
	checkResponse(t, `{"version":1,"steps":[
		{"id":"a","op":"constant","args":{"value":{"user":{"name":"ana"},"items":[10,20]}}},
		{"id":"n","op":"get","depends_on":["a"],"args":{"input":{"$ref":"a.user"},"key":"name"}},
		{"id":"i","op":"get","depends_on":["a"],"args":{"input":{"$ref":"a"},"key":"items"}},
		{"id":"v","op":"constant","depends_on":["a","i"],"args":{"value":{"$ref":"i.1"}}}]}`,
		`{"status":"completed","results":{"a":{"user":{"name":"ana"},"items":[10,20]},"n":"ana","i":[10,20],"v":20},"execution_order":["a","n","i","v"],"skipped":[],"error":null}`, 0)

	checkResponse(t, `{"version":1,"steps":[
		{"id":"a","op":"constant","args":{"value":0}},
		{"id":"b","op":"constant","args":{"value":{"$ref":"a"}}}]}`,
		`{"status":"failed","results":{"a":0},"execution_order":["a"],"skipped":[],"error":{"code":"INVALID_REFERENCE","step":"b","message":"x"}}`, 1)

	checkResponse(t, `{"version":1,"steps":[
		{"id":"a","op":"get","args":{"input":{"x":1},"key":"missing"}},
		{"id":"b","op":"constant","depends_on":["a"],"args":{"value":{"$ref":"a"}}}]}`,
		`{"status":"completed","results":{"a":null,"b":null},"execution_order":["a","b"],"skipped":[],"error":null}`, 0)

	checkResponse(t, `{"version":1,"steps":[
		{"id":"a","op":"constant","args":{"value":{"k":1}}},
		{"id":"b","op":"constant","depends_on":["a"],"args":{"value":{"$ref":"a.nope"}}}]}`,
		`{"status":"failed","results":{"a":{"k":1}},"execution_order":["a"],"skipped":[],"error":{"code":"INVALID_REFERENCE","step":"b","message":"x"}}`, 1)

	checkResponse(t, `{"version":1,"steps":[
		{"id":"a","op":"constant","args":{"value":[1]}},
		{"id":"b","op":"constant","depends_on":["a"],"args":{"value":{"$ref":"a.3"}}}]}`,
		`{"status":"failed","results":{"a":[1]},"execution_order":["a"],"skipped":[],"error":{"code":"INVALID_REFERENCE","step":"b","message":"x"}}`, 1)

	checkResponse(t, `{"version":1,"steps":[
		{"id":"a","op":"constant","args":{"value":1}},
		{"id":"b","op":"constant","args":{"value":{"$ref":"missing"}}}]}`,
		`{"status":"failed","results":{"a":1},"execution_order":["a"],"skipped":[],"error":{"code":"INVALID_REFERENCE","step":"b","message":"x"}}`, 1)

	checkResponse(t, `{"version":1,"steps":[
		{"id":"a","op":"constant","args":{"value":1},"when":{"op":"eq","left":1,"right":2}},
		{"id":"b","op":"constant","depends_on":["a"],"args":{"value":{"$ref":"a"}}}]}`,
		`{"status":"failed","results":{"a":null},"execution_order":["a"],"skipped":["a"],"error":{"code":"SKIPPED_REFERENCE","step":"b","message":"x"}}`, 1)

	// transitive dependency references are allowed
	checkResponse(t, `{"version":1,"steps":[
		{"id":"a","op":"constant","args":{"value":5}},
		{"id":"b","op":"constant","depends_on":["a"],"args":{"value":1}},
		{"id":"c","op":"constant","depends_on":["b"],"args":{"value":{"$ref":"a"}}}]}`,
		`{"status":"completed","results":{"a":5,"b":1,"c":5},"execution_order":["a","b","c"],"skipped":[],"error":null}`, 0)
}

func TestConditions(t *testing.T) {
	checkResponse(t, `{"version":1,"steps":[
		{"id":"a","op":"constant","args":{"value":7}},
		{"id":"b","op":"constant","depends_on":["a"],"when":{"op":"all","conditions":[
			{"op":"gt","left":{"$ref":"a"},"right":5},
			{"op":"lt","left":{"$ref":"a"},"right":10}]},"args":{"value":"yes"}}]}`,
		`{"status":"completed","results":{"a":7,"b":"yes"},"execution_order":["a","b"],"skipped":[],"error":null}`, 0)

	checkResponse(t, `{"version":1,"steps":[
		{"id":"a","op":"constant","args":{"value":7}},
		{"id":"b","op":"constant","depends_on":["a"],"when":{"op":"any","conditions":[
			{"op":"gt","left":{"$ref":"a"},"right":100},
			{"op":"eq","left":{"$ref":"a"},"right":7}]},"args":{"value":"yes"}}]}`,
		`{"status":"completed","results":{"a":7,"b":"yes"},"execution_order":["a","b"],"skipped":[],"error":null}`, 0)

	checkResponse(t, `{"version":1,"steps":[
		{"id":"a","op":"constant","args":{"value":7}},
		{"id":"b","op":"constant","depends_on":["a"],"when":{"op":"not","condition":{"op":"lt","left":{"$ref":"a"},"right":3}},"args":{"value":"yes"}}]}`,
		`{"status":"completed","results":{"a":7,"b":"yes"},"execution_order":["a","b"],"skipped":[],"error":null}`, 0)

	// structural string vs number ordering in conditions
	checkResponse(t, `{"version":1,"steps":[
		{"id":"a","op":"constant","args":{"value":1}},
		{"id":"b","op":"constant","depends_on":["a"],"when":{"op":"lt","left":{"$ref":"a"},"right":"x"},"args":{"value":"yes"}}]}`,
		`{"status":"completed","results":{"a":1,"b":"yes"},"execution_order":["a","b"],"skipped":[],"error":null}`, 0)

	// ref condition must be boolean
	checkResponse(t, `{"version":1,"steps":[
		{"id":"a","op":"constant","args":{"value":5}},
		{"id":"b","op":"constant","depends_on":["a"],"when":{"$ref":"a"},"args":{"value":"yes"}}]}`,
		`{"status":"failed","results":{"a":5},"execution_order":["a"],"skipped":[],"error":{"code":"INVALID_CONDITION","step":"b","message":"x"}}`, 1)

	checkResponse(t, `{"version":1,"steps":[
		{"id":"a","op":"constant","args":{"value":true}},
		{"id":"b","op":"constant","depends_on":["a"],"when":{"$ref":"a"},"args":{"value":"yes"}}]}`,
		`{"status":"completed","results":{"a":true,"b":"yes"},"execution_order":["a","b"],"skipped":[],"error":null}`, 0)

	checkResponse(t, `{"version":1,"steps":[{"id":"a","op":"constant","when":{"op":"bogus"}}]}`,
		`{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INVALID_WORKFLOW","step":"a","message":"x"}}`, 2)
}

func TestOrderAndRetries(t *testing.T) {
	// earliest in the steps array wins among ready steps
	checkResponse(t, `{"version":1,"steps":[
		{"id":"z","op":"constant","args":{"value":1}},
		{"id":"a","op":"constant","args":{"value":2}},
		{"id":"m","op":"constant","depends_on":["a"],"args":{"value":3}}]}`,
		`{"status":"completed","results":{"z":1,"a":2,"m":3},"execution_order":["z","a","m"],"skipped":[],"error":null}`, 0)

	// dependency declared after dependent still runs first
	checkResponse(t, `{"version":1,"steps":[
		{"id":"b","op":"constant","depends_on":["a"],"args":{"value":{"$ref":"a"}}},
		{"id":"a","op":"constant","args":{"value":4}}]}`,
		`{"status":"completed","results":{"b":4,"a":4},"execution_order":["a","b"],"skipped":[],"error":null}`, 0)

	// fail_until exhausted -> STEP_FAILED, and results/order keep previous steps
	checkResponse(t, `{"version":1,"steps":[
		{"id":"a","op":"constant","args":{"value":1}},
		{"id":"f","op":"fail_until","depends_on":["a"],"args":{"failures":5,"value":"x"},"retry":{"max_attempts":3}},
		{"id":"c","op":"constant","depends_on":["f"],"args":{"value":3}}]}`,
		`{"status":"failed","results":{"a":1},"execution_order":["a"],"skipped":[],"error":{"code":"STEP_FAILED","step":"f","message":"x"}}`, 1)

	// zero failures succeeds
	checkOK(t, "fail_until", `{"failures":0,"value":7}`, "", `7`)

	// non-transient errors are not retried even with max_attempts
	checkErr(t, "divide", `{"a":1,"b":0}`, `"retry":{"max_attempts":5}`, "DIVISION_BY_ZERO", 1)
}
