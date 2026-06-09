#!/usr/bin/env bash
# tests/test_script.sh — integration tests for the script runner's control flow
# (Phase 3+): :foreach numeric ranges, loop-variable scoping, :break/:continue/
# :return, and :let/:echo. Driven through the real binary in headless mode.
#
# Usage (via CTest):  bash test_script.sh <path-to-molterm-binary>
set -u
MOLTERM="${1:?usage: test_script.sh <molterm-binary>}"
fail=0

# Run a script (stdin) headless, capture stdout.
run() { printf '%s' "$1" | "$MOLTERM" --script - --no-tui 2>/dev/null; }

# assert_has <label> <output> <needle>
assert_has() {
    if printf '%s' "$2" | grep -qF -- "$3"; then echo "[PASS] $1"
    else echo "[FAIL] $1 (missing: $3)"; echo "--- output ---"; printf '%s\n' "$2"; fail=1; fi
}
# assert_absent <label> <output> <needle>
assert_absent() {
    if printf '%s' "$2" | grep -qF -- "$3"; then echo "[FAIL] $1 (unexpected: $3)"; fail=1
    else echo "[PASS] $1"; fi
}

out=$(run ':foreach i in 1..3
  :echo iter=${i}
:end
:echo after=[${i}]
')
assert_has   "foreach iterates"        "$out" "iter=1"
assert_has   "foreach iterates last"   "$out" "iter=3"
assert_has   "loop var scoped (erased)" "$out" "after=[]"

out=$(run ':foreach j in 1..9
  :if ${j} == 3
    :break
  :endif
  :echo j=${j}
:end
')
assert_has    "break: before"  "$out" "j=2"
assert_absent "break: stops"   "$out" "j=3"
assert_absent "break: stops 9" "$out" "j=9"

out=$(run ':foreach k in 1..4
  :if ${k} == 2
    :continue
  :endif
  :echo k=${k}
:end
')
assert_has    "continue: keeps 1" "$out" "k=1"
assert_absent "continue: skips 2" "$out" "k=2"
assert_has    "continue: keeps 3" "$out" "k=3"

out=$(run ':echo one
:return
:echo two
')
assert_has    "return: before" "$out" "one"
assert_absent "return: stops"  "$out" "two"

out=$(run ':let a = 2 + 3 * 4
:echo a=${a}
')
assert_has "let arithmetic" "$out" "a=14"

out=$(run ':let d = 3.5
:let v = [1, 2, 3]
:dump
')
assert_has "dump scalar json" "$out" '"d":{"kind":"scalar","value":3.5}'
assert_has "dump vec3 json"   "$out" '"v":{"kind":"vec3","value":[1,2,3]}'

echo
if [ "$fail" -eq 0 ]; then echo "ALL SCRIPT TESTS PASSED"; else echo "SCRIPT TESTS FAILED"; fi
exit "$fail"
