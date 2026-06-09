# molterm Scripting Reference

molterm scripts are line-oriented sequences of `:commands` with a small layer of
variables, a typed expression language, control flow, and user-defined
functions. Scripts run interactively (typed at the `:` prompt), from a file via
`:run`, or headless via `molterm --script - --no-tui` (stdin) for pipelines and
the MCP server.

This document is the language reference. For the analysis-recipe catalog see
[lib/README.md](../lib/README.md).

---

## Running scripts

```
:run path/to/script.mt              # run a script file
:run @lib/<name>                    # run a shipped/​user recipe (see lib/README.md)
:run @lib/phi_psi KEY=VAL KEY2=VAL  # pass arguments as KEY=VALUE env vars
:run script.mt --strict             # abort on the first command error
:run script.mt --fresh              # clear overlays before running
```

A script may begin with a shebang declaring its frame behavior:

```
#!molterm scope=local export=phi,psi
```

- `scope=local` runs the script in an isolated frame (its `:let`/`:setenv`
  bindings don't touch the caller); `scope=inherit` (default) shares the
  caller's frame. Passing `KEY=VAL` args forces `scope=local`.
- `export=a,b` lists registers that flow back to the caller on exit. Names
  starting with `_` are private and never exported.

Comments start with `#` (respecting quotes, so `:color #cfcfcf` survives).

---

## Variables

Two namespaces, both expanded with `${...}`:

| Kind      | Set with           | Holds                        | Lookup order in `${name}` |
|-----------|--------------------|------------------------------|---------------------------|
| Registers | `:let name = expr` | typed values (scalar/vec3/pca)| 1st                      |
| Env vars  | `:setenv NAME val` | strings                      | 2nd (then OS environment) |

Interpolation forms:

```
${name}            # whole value (vec3 → "(x, y, z)")
${name.field}      # a sub-field (see Field access)
${name:.2f}        # printf format spec
${name.field:.3f}  # field + format
```

`:registers` lists registers as human text; `:dump` emits them as JSON (below).

---

## Expression language (`:let` right-hand side)

```
expr    := term (('+' | '-') term)*
term    := factor (('*' | '/') factor)*       # scalar*vec, vec*scalar, scalar*scalar
factor  := '-' factor | primary
primary := number | '[' expr ',' expr ',' expr ']'   # vec3 literal
         | '$' ident ('.' ident)? | ident '(' args ')' | '(' expr ')'
```

### Types

- **Scalar** — a double. `:let d = 1.5`, `:let t = angle($u, $v)`.
- **Vec3** — `[x, y, z]`. Atom positions, axes, differences.
- **Pca** — the result of `pca()` / `helix_axis()` / `superpose_axis()`.

Type rules: `vec±vec`, `scalar±scalar`, `scalar*vec`, `vec*scalar`, `vec/scalar`.
`vec*vec` is an error — use `dot()` / `cross()`.

### Builtins

| Function | Args → result | Notes |
|----------|---------------|-------|
| `pos(<atom-spec>)` | → Vec3 | `chain:resi:name` or `obj/chain:resi:name` |
| `centroid(<sel>)` | → Vec3 | geometric mean (≥1 atom) |
| `com(<sel>)` | → Vec3 | mass-weighted center |
| `pca(<sel>)` | → Pca | principal axes (≥2 atoms) |
| `helix_axis(<sel>)` | → Pca | axis of an ordered Cα/P trace (≥3) |
| `superpose_axis(A vs B)` | → Pca | screw axis of optimal A→B fit (equal counts) |
| `rmsd(A vs B)` | → Scalar | RMSD of optimal fit, without moving anything |
| `dot(u, v)` `cross(u, v)` | Vec3,Vec3 → Scalar/Vec3 | |
| `length(v)`/`len`, `distance(p,q)`/`dist` | → Scalar | |
| `normalize(v)`/`norm`, `midpoint(p,q)` | → Vec3 | |
| `angle(u, v)` | → Scalar | degrees, 0–180 |
| `dihedral(a, b, c, d)` | → Scalar | signed degrees, ±180 |
| scalar math | → Scalar | `abs sqrt exp log log10 log2 sin cos tan asin acos atan floor ceil round` (1 arg); `min max pow atan2` (2 args; `atan2` in degrees) |

Selections inside `pca(...)`/`centroid(...)` use the normal selection grammar;
commas are accepted as list separators (`pca(resi 1,2,3)` == `pca(resi 1 2 3)`).

### Field access

| Register kind | Fields |
|---------------|--------|
| Vec3 | `.x` `.y` `.z` → Scalar; `.length`/`.len` → Scalar |
| Pca from `pca()`/`helix_axis()` | `.axis1` `.axis2` `.axis3` `.center` → Vec3; `.eig1` `.eig2` `.eig3` → Scalar |
| Pca from `superpose_axis()` | `.axis1` (rotation axis) `.center` → Vec3; `.angle` (degrees) `.rmsd` (Å) → Scalar |

Reading the wrong field for the producer is an **error** (not a silent 0): e.g.
`.eig1` on a `superpose_axis()` result, or `.angle` on a `pca()` result.

---

## Control flow

### Conditionals

```
:if ${x} < 4.0
  ...
:elseif ${x} < 8.0
  ...
:else
  ...
:endif
```

Conditions are `LHS op RHS` with `== != < > <= >=`; both sides are full
expressions (so `$reg.field`, math, and builtins work).

### Loops

Numeric range:

```
:foreach i in 1..10
  :echo step ${i}
:end
```

Over a selection — iterates the **distinct residues**, binding four env vars
each pass:

```
:foreach r in chain A and resi 10-20
  # ${r}        → "A:14"  (a chain:resi spec, usable in pos(...))
  # ${r_chain}  → "A"     ${r_resi} → "14"     ${r_resn} → "GLY"
  :let _d = distance(pos(${r}:CA), pos(A:76:CA))
  :echo ${r} ${r_resn} ${_d:.2f}
:end
```

The loop variable (and the `_chain`/`_resi`/`_resn` bindings) are **scoped** —
they don't leak past `:end`.

### Loop / script control

- `:break` — exit the nearest enclosing `:foreach`.
- `:continue` — skip to the next iteration.
- `:return` — stop the current script (or `:def` function body).

Each is gated by the enclosing `:if`, so `:if cond / :break / :endif` works.

---

## Functions

```
:def pairdist(a, b)
  :let _d = distance(pos(${a}), pos(${b}))
  :echo ${a}-${b} = ${_d:.2f}
:enddef

pairdist A:1:CA A:10:CA      # call it; params bind as env vars
```

The body runs in a fresh **local frame**: its registers/env don't leak, and
params are seeded from the call arguments. `:return` exits early. Recursion is
capped at depth 64. (A user function name takes precedence over a built-in
command of the same name.)

---

## Output for pipelines / agents

- `:echo <text>` — print a line to stdout (after `${...}` expansion).
- `:registers` — human-readable register listing.
- `:dump [name...]` — registers as a **JSON object** on stdout, keyed by name.
  All registers, or only the named ones:

  ```
  :dump
  {"d":{"kind":"scalar","value":3.5},"sp":{"kind":"pca","center":[...],"angle":35,"rmsd":0.4,...}}
  ```

From the MCP server, the **`dump_registers`** tool runs a command sequence,
appends `:dump`, and returns the parsed register JSON as structured content —
use it instead of `run_script` + text parsing when you need typed values back.

---

## See also

- [lib/README.md](../lib/README.md) — the analysis-recipe catalog (`:run @lib/...`).
- `:help <command>` — usage, description, and examples for any command.
