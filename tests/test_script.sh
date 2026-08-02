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
# Same, but merge stderr — per-command failures stream there in headless mode.
run_err() { printf '%s' "$1" | "$MOLTERM" --script - --no-tui 2>&1; }

# A two-residue chain-A structure, so selection tests have something to match
# without depending on a downloaded fixture.
TESTPDB="$(mktemp -t molterm_test_XXXXXX).pdb"
NESTED="$(mktemp -t molterm_nested_XXXXXX).mt"
SECOND="$(mktemp -t molterm_second_XXXXXX).pdb"
trap 'rm -f "$TESTPDB" "$NESTED" "$SECOND"' EXIT
cat > "$TESTPDB" <<'PDB'
ATOM      1  N   ALA A   1       0.000   0.000   0.000  1.00 20.00           N
ATOM      2  CA  ALA A   1       1.458   0.000   0.000  1.00 20.00           C
ATOM      3  C   ALA A   1       2.009   1.420   0.000  1.00 20.00           C
ATOM      4  O   ALA A   1       1.251   2.390   0.000  1.00 20.00           O
ATOM      5  N   GLY A   2       3.332   1.550   0.000  1.00 20.00           N
ATOM      6  CA  GLY A   2       3.970   2.850   0.000  1.00 20.00           C
ATOM      7  C   GLY A   2       5.486   2.750   0.000  1.00 20.00           C
ATOM      8  O   GLY A   2       6.030   1.650   0.000  1.00 20.00           O
END
PDB

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

# ── Batch exit status (issue #117) ───────────────────────────────────────────
# A driver that keys off the return code has to be able to tell a clean batch
# render from one where commands failed. Before this, both exited 0 and the
# only evidence was a stdout line the driver never printed.

# rc <flags...> — run a script (stdin) headless, echo only its exit status.
rc() { printf '%s' "$1" | "$MOLTERM" --script - --no-tui "${@:2}" >/dev/null 2>&1; echo $?; }

# assert_eq <label> <actual> <expected>
assert_eq() {
    if [ "$2" = "$3" ]; then echo "[PASS] $1"
    else echo "[FAIL] $1 (got: $2, want: $3)"; fail=1; fi
}

assert_eq "exit: clean script exits 0" \
          "$(rc ':echo ok
')" 0
assert_eq "exit: failed command exits 1" \
          "$(rc ':color firebrick
')" 1
# The script must still run to completion — only the status changes.
out=$(run ':color firebrick
:echo AFTER_FAILURE
')
assert_has "exit: non-strict still runs on" "$out" "AFTER_FAILURE"
assert_eq "exit: failure anywhere counts, not just last" \
          "$(rc ':color firebrick
:echo later
')" 1
assert_eq "exit: --strict still exits 1" \
          "$(rc ':color firebrick
:echo later
' --strict)" 1

# ── Malformed selections fail the command (issue #121) ───────────────────────
# A selection the grammar can't fully consume used to narrow to whatever prefix
# parsed and report success, so `:color red chain A resi 5` coloured chain A and
# the run looked clean. It now fails, which the batch exit code then surfaces.
out=$(run_err ":load $TESTPDB
:color red chain A resi 5
:echo STILL_RUNNING
")
assert_has "bad selection: reported"       "$out" "Bad selection"
# The full quoted form — a bare "resi" would also match the echoed source line.
assert_has "bad selection: names token"    "$out" "unexpected 'resi'"
assert_has "bad selection: script goes on" "$out" "STILL_RUNNING"

out=$(run_err ":load $TESTPDB
:count (chain A
")
assert_has "bad selection: unclosed paren" "$out" "unclosed"

# A well-formed selection must not trip the check.
out=$(run_err ":load $TESTPDB
:count chain A and resi 1-2
:color red resi 1,2
")
assert_absent "good selection: no false error" "$out" "Bad selection"
assert_has    "good selection: comma list works" "$out" "8 atom(s) red"

# A bad selection is a failed command, so it reaches the exit status (#117).
assert_eq "bad selection: exits 1" \
          "$(rc ":load $TESTPDB
:color red chain A resi 5
")" 1

# Dispatch nests: `:run` executes its body through the same path, so a child's
# bad selection must be reported against the child, not swapped in as the
# parent's result. Guards the save/restore in CommandRegistry::execute.
printf ':echo INNER_START\n:color red chain A resi 5\n' > "$NESTED"
out=$(run_err ":load $TESTPDB
:run $NESTED
:echo OUTER_DONE
")
assert_has "nested: child error reported"   "$out" "Bad selection"
assert_has "nested: parent keeps its summary" "$out" "1 failed"
assert_has "nested: outer script continues" "$out" "OUTER_DONE"

# Fail closed: the command must stop BEFORE it mutates, not report afterwards.
# `:extract` is the sharp case — it used to carve a new object out of the
# truncated prefix and shrink the source, then announce the failure.
out=$(run_err ":load $TESTPDB
:extract chain A resi 5 as carved
:objects
:count chain A
")
assert_has "fail-closed: extract rejected" "$out" "Bad selection"
# Check the :objects listing, not the whole transcript — the echoed command
# line contains "carved" whether or not the object was created.
assert_absent "fail-closed: no object created" \
              "$(printf '%s' "$out" | grep '^Objects:')" "carved"
assert_has    "fail-closed: source untouched"  "$out" "8 atoms match: chain A"

# $sele must not be left holding the truncated prefix for later commands.
out=$(run_err ":load $TESTPDB
:select chain A resi 5
:count \$sele
")
assert_has "fail-closed: \$sele not poisoned" "$out" "0 atoms match"

# The :foreach header parses a selection outside command dispatch, so it needs
# its own check — it used to iterate the prefix silently at exit 0.
out=$(run_err ":load $TESTPDB
:foreach r in chain A resi 5
  :echo ITERATED
:end
:echo AFTER_LOOP
")
assert_has    "foreach: bad header reported"  "$out" "Bad selection"
assert_absent "foreach: body did not run"     "$out" "ITERATED"
assert_has    "foreach: script continues"     "$out" "AFTER_LOOP"
assert_eq "foreach: bad header exits 1" \
          "$(rc ":load $TESTPDB
:foreach r in chain A resi 5
  :echo x
:end
")" 1
# ── Unknown selectors (issue #124) ───────────────────────────────────────────
# A typo'd selector selected nothing, and under `not` that inverted to
# everything — `:color red not heliix` painted the whole structure at exit 0.
out=$(run_err ":load $TESTPDB
:color red not heliix
:color blue not helix
")
assert_has    "unknown selector: typo reported"   "$out" "unknown selector"
assert_absent "unknown selector: nothing painted" "$out" "8 atom(s) red"
assert_has    "unknown selector: real keyword works" "$out" "atom(s) blue"
assert_eq "unknown selector: exits 1" \
          "$(rc ":load $TESTPDB
:color red not heliix
")" 1

# A name that belongs to another loaded object is a cross-object reference, not
# a typo — it must stay silent so the caller's loop can reach that object. The
# assertion has to name the object as a BAREWORD: an expression like
# `chain B` contains no bareword and would pass even with the check deleted.
sed 's/ A   1 / B   1 /' "$TESTPDB" > "$SECOND"
FIRST_OBJ="$(basename "$TESTPDB" .pdb)"
SECOND_OBJ="$(basename "$SECOND" .pdb)"
out=$(run_err ":load $TESTPDB
:load $SECOND
:count $FIRST_OBJ
:count $SECOND_OBJ
:count definitelynotanobject
")
assert_absent "cross-object: known name is not a typo" "$out" "unknown selector '$FIRST_OBJ'"
assert_absent "cross-object: other object not a typo"  "$out" "unknown selector '$SECOND_OBJ'"
assert_has    "cross-object: both objects resolve"     "$out" "8 atoms match: $SECOND_OBJ"
# ...while a name nothing answers to is still caught.
assert_has "cross-object: real typo still caught" "$out" "unknown selector 'definitelynotanobject'"

# A well-formed header still iterates.
out=$(run_err ":load $TESTPDB
:foreach r in chain A and resi 1
  :echo GOOD_ITER=\${r}
:end
")
assert_has "foreach: good header still runs" "$out" "GOOD_ITER=A:1"

echo
if [ "$fail" -eq 0 ]; then echo "ALL SCRIPT TESTS PASSED"; else echo "SCRIPT TESTS FAILED"; fi
exit "$fail"
