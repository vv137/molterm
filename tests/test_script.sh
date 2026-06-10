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

out=$(run ':setenv A 1 ; :setenv B 2 ; :echo ${A}-${B}
')
assert_has "semicolon chains commands on one line" "$out" "1-2"

out=$(run ':let d = 3.5
:let v = [1, 2, 3]
:dump
')
assert_has "dump scalar json" "$out" '"d":{"kind":"scalar","value":3.5}'
assert_has "dump vec3 json"   "$out" '"v":{"kind":"vec3","value":[1,2,3]}'

out=$(run ':def greet(who)
  :echo hi ${who}
  :let _x = 1
:enddef
greet World
:echo leak=[${_x}]
')
assert_has "def: param bound" "$out" "hi World"
assert_has "def: local scope"  "$out" "leak=[]"

out=$(run ':def early(x)
  :echo got ${x}
  :return
  :echo NEVER
:enddef
early 9
')
assert_has    "def: return before" "$out" "got 9"
assert_absent "def: return stops"  "$out" "NEVER"

# ── Regression: malformed input must not crash the session ───────────────────
# A bad numeric :set value used to throw std::stof out of the handler and abort
# the whole process; the command-dispatch backstop now turns it into a failed
# command so later lines still run.
out=$(run ':set fog abc
:echo SURVIVED_SET
')
assert_has "set: bad numeric value does not crash" "$out" "SURVIVED_SET"

# A ${reg:fmt} spec with a non-float conversion (%s/%n) used to be spliced into
# snprintf against a double and segfault; the spec is now validated and falls
# back to the default format.
out=$(run ':let x = 1.5
:echo got=${x:s}
:echo SURVIVED_FMT
')
assert_has "fmt: hostile format spec does not crash" "$out" "SURVIVED_FMT"

# A valid printf spec must still format normally.
out=$(run ':let y = 3.14159
:echo y=${y:.2f}
')
assert_has "fmt: valid spec still formats" "$out" "y=3.14"

echo
if [ "$fail" -eq 0 ]; then echo "ALL SCRIPT TESTS PASSED"; else echo "SCRIPT TESTS FAILED"; fi
exit "$fail"
