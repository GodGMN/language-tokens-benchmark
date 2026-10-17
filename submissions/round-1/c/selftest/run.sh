#!/bin/sh
# Self-tests for the C workflow engine.
#
# Every case compares the complete JSON response and the process exit code.
# Run with:  sh selftest/run.sh   (or: make selftest)
set -u
cd "$(dirname "$0")/.." || exit 1

total=0
fails=0

report() {
    # name ok got_rc want_rc got_out want_out
    total=$((total + 1))
    if [ "$2" = "1" ]; then
        echo "ok   $1"
    else
        fails=$((fails + 1))
        echo "FAIL $1"
        echo "     exit: got $3 want $4"
        echo "     got:  $5"
        echo "     want: $6"
    fi
}

check_file() {
    name=$1
    file=$2
    want_rc=$3
    want_out=$4
    got_out=$(./workflow run "$file")
    got_rc=$?
    if [ "$got_rc" -eq "$want_rc" ] && [ "$got_out" = "$want_out" ]; then
        report "$name" 1 "$got_rc" "$want_rc" "$got_out" "$want_out"
    else
        report "$name" 0 "$got_rc" "$want_rc" "$got_out" "$want_out"
    fi
}

check_stdin() {
    name=$1
    input=$2
    want_rc=$3
    want_out=$4
    got_out=$(printf '%s' "$input" | ./workflow run -)
    got_rc=$?
    if [ "$got_rc" -eq "$want_rc" ] && [ "$got_out" = "$want_out" ]; then
        report "$name" 1 "$got_rc" "$want_rc" "$got_out" "$want_out"
    else
        report "$name" 0 "$got_rc" "$want_rc" "$got_out" "$want_out"
    fi
}

# ---- selftest/arrays.json ----
want=$(cat <<'WANT'
{"status":"completed","results":{"nums":[1,2,3,4,5,6],"tripled":[3,6,9,12,15,18],"odds":[1,3,5],"total":21,"product":1440,"mixed_sorted":[2,3,"a","b",false,true,null,[1]],"deduped":[1,2,"a",[1]],"tail":[4,5],"flattened":[1,2,3,[4]],"reversed":[5,3,1],"stringified":["1","2","3","4","5","6"],"types":["number","string","null","boolean","array","object"]},"execution_order":["nums","tripled","odds","total","product","mixed_sorted","deduped","tail","flattened","reversed","stringified","types"],"skipped":[],"error":null}
WANT
)
check_file 'arrays' "selftest/arrays.json" 0 "$want"

# ---- selftest/conditions_compound.json ----
want=$(cat <<'WANT'
{"status":"completed","results":{"x":5,"between":"ran","unlikely":null,"negated":"ran"},"execution_order":["x","between","unlikely","negated"],"skipped":["unlikely"],"error":null}
WANT
)
check_file 'conditions_compound' "selftest/conditions_compound.json" 0 "$want"

# ---- selftest/error_overflow.json ----
want=$(cat <<'WANT'
{"status":"failed","results":{"big":9223372036854775807},"execution_order":["big"],"skipped":[],"error":{"code":"ARITHMETIC_OVERFLOW","step":"over","message":"addition overflow"}}
WANT
)
check_file 'error_overflow' "selftest/error_overflow.json" 1 "$want"

# ---- selftest/ex_a.json ----
want=$(cat <<'WANT'
{"status":"completed","results":{"n":[1,2,3,4,5],"evens":[2,4],"total":6},"execution_order":["n","evens","total"],"skipped":[],"error":null}
WANT
)
check_file 'ex_a' "selftest/ex_a.json" 0 "$want"

# ---- selftest/ex_b.json ----
want=$(cat <<'WANT'
{"status":"completed","results":{"x":3,"y":null},"execution_order":["x","y"],"skipped":["y"],"error":null}
WANT
)
check_file 'ex_b' "selftest/ex_b.json" 0 "$want"

# ---- selftest/ex_c.json ----
want=$(cat <<'WANT'
{"status":"completed","results":{"flaky":"ok"},"execution_order":["flaky"],"skipped":[],"error":null}
WANT
)
check_file 'ex_c' "selftest/ex_c.json" 0 "$want"

# ---- selftest/math_pipeline.json ----
want=$(cat <<'WANT'
{"status":"completed","results":{"n":[1,2,3,4,5,6],"shifted":[5,6,7,8,9,10],"evens":[6,8,10],"total":24,"spread":23,"truncated":11,"remainder":3},"execution_order":["n","shifted","evens","total","spread","truncated","remainder"],"skipped":[],"error":null}
WANT
)
check_file 'math_pipeline' "selftest/math_pipeline.json" 0 "$want"

# ---- selftest/object_ops.json ----
want=$(cat <<'WANT'
{"status":"completed","results":{"base":{"user":{"name":"Ada","age":36},"tags":["x","y"]},"sorted_keys":["tags","user"],"sorted_values":[["x","y"],{"name":"Ada","age":36}],"with_email":{"user":{"name":"Ada","age":36},"tags":["x","y"],"email":"ada@example.com"},"merged":{"user":{"name":"Ada Lovelace","age":37},"tags":["x","y"],"email":"ada@example.com"},"user_name":"Ada Lovelace","missing":null,"picked":{"email":"ada@example.com","nope":null,"user":{"name":"Ada Lovelace","age":37}},"omitted":{"tags":["x","y"]}},"execution_order":["base","sorted_keys","sorted_values","with_email","merged","user_name","missing","picked","omitted"],"skipped":[],"error":null}
WANT
)
check_file 'object_ops' "selftest/object_ops.json" 0 "$want"

# ---- selftest/refs_nested.json ----
want=$(cat <<'WANT'
{"status":"completed","results":{"config":{"user":{"name":"bob","roles":["dev","ops"]},"items":[10,20,30]},"name":"bob","second_item":20,"first_role":"dev","items_sum":60,"role_count":3},"execution_order":["config","name","second_item","first_role","items_sum","role_count"],"skipped":[],"error":null}
WANT
)
check_file 'refs_nested' "selftest/refs_nested.json" 0 "$want"

# ---- selftest/retry.json ----
want=$(cat <<'WANT'
{"status":"completed","results":{"flaky":"ok","after":"ok!"},"execution_order":["flaky","after"],"skipped":[],"error":null}
WANT
)
check_file 'retry' "selftest/retry.json" 0 "$want"

# ---- selftest/skipped_step.json ----
want=$(cat <<'WANT'
{"status":"completed","results":{"x":3,"y":null,"z":null},"execution_order":["x","y","z"],"skipped":["y","z"],"error":null}
WANT
)
check_file 'skipped_step' "selftest/skipped_step.json" 0 "$want"

# ---- selftest/string_pipeline.json ----
want=$(cat <<'WANT'
{"status":"completed","results":{"raw":"  Hello, World  ","trimmed":"Hello, World","upper":"HELLO, WORLD","parts":["HELLO","WORLD"],"joined":"HELLO-WORLD","size":12,"swapped":"Hello, C","lowered":"hello, c","chars":["H","e","l","l","o",","," ","W","o","r","l","d"]},"execution_order":["raw","trimmed","upper","parts","joined","size","swapped","lowered","chars"],"skipped":[],"error":null}
WANT
)
check_file 'string_pipeline' "selftest/string_pipeline.json" 0 "$want"

# ---- selftest/unicode.json ----
want=$(cat <<'WANT'
{"status":"completed","results":{"text":"héllo wörld","size":11,"upper":"HéLLO WöRLD","words":["héllo","wörld"],"chars":["h","é","l","l","o"," ","w","ö","r","l","d"],"ascii":"héllo world","soundex":"hello wörld","tagged":"hello wörld"},"execution_order":["text","size","upper","words","chars","ascii","soundex","tagged"],"skipped":[],"error":null}
WANT
)
check_file 'unicode' "selftest/unicode.json" 0 "$want"

# ---- inline: retry_exhausted ----
want=$(cat <<'WANT'
{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"STEP_FAILED","step":"f","message":"transient failure on attempt 2"}}
WANT
)
check_stdin 'retry_exhausted' '{"version":1,"steps":[{"id":"f","op":"fail_until","args":{"failures":3,"value":"ok"},"retry":{"max_attempts":2}}]}' 1 "$want"

# ---- inline: skipped_reference ----
want=$(cat <<'WANT'
{"status":"failed","results":{"a":null},"execution_order":["a"],"skipped":["a"],"error":{"code":"SKIPPED_REFERENCE","step":"b","message":"reference 'a' targets skipped step 'a'"}}
WANT
)
check_stdin 'skipped_reference' '{"version":1,"steps":[{"id":"a","op":"constant","args":{"value":1},"when":{"op":"eq","left":1,"right":2}},{"id":"b","op":"constant","depends_on":["a"],"args":{"value":{"$ref":"a"}}}]}' 1 "$want"

# ---- inline: division_by_zero ----
want=$(cat <<'WANT'
{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"DIVISION_BY_ZERO","step":"x","message":"division by zero"}}
WANT
)
check_stdin 'division_by_zero' '{"version":1,"steps":[{"id":"x","op":"divide","args":{"a":1,"b":0}}]}' 1 "$want"

# ---- inline: invalid_arg ----
want=$(cat <<'WANT'
{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INVALID_ARG","step":"x","message":"unknown field 'c'"}}
WANT
)
check_stdin 'invalid_arg' '{"version":1,"steps":[{"id":"x","op":"add","args":{"a":1,"b":2,"c":3}}]}' 1 "$want"

# ---- inline: invalid_workflow ----
want=$(cat <<'WANT'
{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INVALID_WORKFLOW","step":"bad","message":"step 'bad' has a missing or unknown 'op'"}}
WANT
)
check_stdin 'invalid_workflow' '{"version":1,"steps":[{"id":"bad","op":"nope"}]}' 2 "$want"

# ---- inline: unknown_dependency ----
want=$(cat <<'WANT'
{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"UNKNOWN_DEPENDENCY","step":"a","message":"step 'a' depends on unknown step 'ghost'"}}
WANT
)
check_stdin 'unknown_dependency' '{"version":1,"steps":[{"id":"a","op":"constant","args":{"value":1},"depends_on":["ghost"]}]}' 2 "$want"

# ---- inline: cyclic_dependency ----
want=$(cat <<'WANT'
{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"CYCLIC_DEPENDENCY","step":null,"message":"workflow contains a dependency cycle"}}
WANT
)
check_stdin 'cyclic_dependency' '{"version":1,"steps":[{"id":"a","op":"constant","args":{"value":1},"depends_on":["b"]},{"id":"b","op":"constant","args":{"value":1},"depends_on":["a"]}]}' 2 "$want"

# ---- inline: empty_array ----
want=$(cat <<'WANT'
{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"EMPTY_ARRAY","step":"x","message":"max of empty array"}}
WANT
)
check_stdin 'empty_array' '{"version":1,"steps":[{"id":"x","op":"max","args":{"input":[]}}]}' 1 "$want"

# ---- inline: invalid_reference ----
want=$(cat <<'WANT'
{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INVALID_REFERENCE","step":"a","message":"reference 'ghost' targets an unknown step"}}
WANT
)
check_stdin 'invalid_reference' '{"version":1,"steps":[{"id":"a","op":"constant","args":{"value":{"$ref":"ghost"}}}]}' 1 "$want"

# ---- inline: invalid_json ----
want=$(cat <<'WANT'
{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INVALID_WORKFLOW","step":null,"message":"expected string"}}
WANT
)
check_stdin 'invalid_json' '{bad' 2 "$want"

# ---- inline: stdin_example_a ----
want=$(cat <<'WANT'
{"status":"completed","results":{"n":[1,2,3,4,5],"evens":[2,4],"total":6},"execution_order":["n","evens","total"],"skipped":[],"error":null}
WANT
)
check_stdin 'stdin_example_a' '{
  "version": 1,
  "steps": [
    { "id": "n", "op": "range", "args": { "start": 1, "end": 5 } },
    { "id": "evens", "op": "filter", "depends_on": ["n"],
      "args": { "input": {"$ref": "n"}, "predicate": {"type": "even"} } },
    { "id": "total", "op": "sum", "depends_on": ["evens"],
      "args": { "input": {"$ref": "evens"} } }
  ]
}
' 0 "$want"

if [ "$fails" -ne 0 ]; then
    echo "$fails of $total self-tests FAILED"
    exit 1
fi
echo "all $total self-tests passed"
exit 0
