package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestSelfTestCases executes every selftest/*.json workflow through the same
// engine the CLI uses and compares the produced response with the expected
// document in selftest/expected/<name>.json.
func TestSelfTestCases(t *testing.T) {
	files, err := filepath.Glob("selftest/*.json")
	if err != nil {
		t.Fatal(err)
	}
	if len(files) == 0 {
		t.Fatal("no selftest cases found")
	}
	for _, file := range files {
		name := strings.TrimSuffix(filepath.Base(file), ".json")
		t.Run(name, func(t *testing.T) {
			data, err := os.ReadFile(file)
			if err != nil {
				t.Fatal(err)
			}
			resp, exit := runDocument(data)
			if resp == nil {
				t.Fatal("nil response")
			}

			expectedRaw, err := os.ReadFile(filepath.Join("selftest", "expected", name+".json"))
			if err != nil {
				t.Fatalf("missing expected output: %v", err)
			}
			expected, err := decodeDocument(expectedRaw)
			if err != nil {
				t.Fatalf("bad expected output: %v", err)
			}

			got, err := decodeDocument(mustMarshal(resp))
			if err != nil {
				t.Fatalf("bad response: %v", err)
			}
			if !structEqual(got, expected) {
				gotJSON, _ := compactJSON(got)
				expJSON, _ := compactJSON(expected)
				t.Fatalf("response mismatch\ngot:      %s\nexpected: %s", gotJSON, expJSON)
			}

			expExit := 0
			if obj, ok := expected.(map[string]interface{}); ok {
				if st, _ := obj["status"].(string); st != "completed" {
					code := ""
					if eo, ok := obj["error"].(map[string]interface{}); ok {
						code, _ = eo["code"].(string)
					}
					expExit = exitForCode(code)
				}
			}
			if exit != expExit {
				t.Fatalf("exit code = %d, want %d", exit, expExit)
			}
		})
	}
}

func mustMarshal(v interface{}) []byte {
	b, err := json.Marshal(v)
	if err != nil {
		panic(err)
	}
	return b
}
