# molterm — analysis recipe library

Reusable `.mt` scripts that compute named structural metrics, intended for
`:run @lib/<name>` from figure scripts. Each recipe is a 10-30 line
composition of the register primitives (`:let` + `pos()` + `centroid()`/`com()`
+ `pca()` + `helix_axis()` + `superpose_axis()` + `rmsd()` +
`dot()`/`angle()`/`dihedral()`/`length()`/`distance()`/`midpoint()` +
`min`/`max`/…), so the formula itself *is* the documentation — fork the script
for variants, no rebuild required.

> For the scripting language itself — expression grammar, control flow
> (`:if`/`:foreach`/`:break`/`:continue`/`:return`), `:def` functions, scope,
> and `:dump` JSON output — see [`docs/SCRIPTING.md`](../docs/SCRIPTING.md).

## Lookup chain

`:run @lib/<name>` resolves `<name>` (with or without trailing `.mt`)
against the first match in:

1. `$MOLTERM_LIB_DIR/<name>.mt`            — per-invocation override
2. `~/.molterm/lib/<name>.mt`              — per-user library (writable)
3. `<install-prefix>/share/molterm/lib/<name>.mt`  — shipped recipes
4. `<exe-dir>/../lib/<name>.mt`            — build-tree layout
5. `<source-dir>/lib/<name>.mt`            — dev fallback

Forks live at `~/.molterm/lib/`; shipped baselines (delivered by
`scripts/update.sh` into `<prefix>/share/molterm/lib/`, or by `cmake
--install` to the same location) stay untouched. The updater never
writes to `~/.molterm/`.

## Shipped recipes

### `tcr_angles.mt` — TCR-pMHC crossing & incident angle

Singh 2020 / Pierce TCR3d definition: PCA on MHC α-helices for the
groove plane, V-V axis between TCR Cys23 Cα atoms, peptide axis between
P1 and PΩ Cα, both projected onto the groove plane for the *crossing*
angle; incident is `|angle(V-V, n̂) − 90°|`.

**Required env vars** (set with `:setenv` before `:run`):

| Var | Meaning | Example (1AO7) |
| --- | --- | --- |
| `TCR_A`      | TCRα chain id              | `D` |
| `TCR_B`      | TCRβ chain id              | `E` |
| `MHC`        | MHC heavy-chain id         | `A` |
| `PEP`        | peptide chain id           | `C` |
| `MHC_HELIX1` | α1 residue range           | `50-85` |
| `MHC_HELIX2` | α2 residue range           | `138-175` |
| `TCRA_CYS23` | Vα V-region Cys residue    | `22` (PDB numbering) |
| `TCRB_CYS23` | Vβ V-region Cys residue    | `23` |
| `PEP_FIRST`  | peptide P1 residue         | `1` |
| `PEP_LAST`   | peptide PΩ residue         | `9` |

> Cys23 is IMGT-canonical for V-region-Cys23 / Cys104 disulfide partners.
> PDB residue numbering differs per file — check `grep '^ATOM' …cif |
> awk '/CYS/ && $7=="<chain>" && $4=="CA"'` to find the actual position
> for an un-renumbered structure. Use 23 for ANARCI / IMGT-renumbered
> inputs.

**Output registers:**

| Reg | Value |
| --- | --- |
| `$crossing` | crossing angle in degrees (0..180) |
| `$incident` | signed offset from 90° — caller takes `abs($incident)` for the magnitude |
| `$v_axis` | vec3 V-V axis (TCRα Cys23 Cα → TCRβ Cys23 Cα) |
| `$p_axis` | vec3 peptide axis (P1 Cα → PΩ Cα) |
| `$groove_normal` | vec3 groove-plane normal n̂ (MHC α1+α2 PCA axis3) |

The three vec3 outputs let a figure script draw the geometry without
recomputing it — e.g. orient straight down the groove (no second PCA) and
annotate the two crossing axes in distinct colors:

```text
:run @lib/tcr_angles
:orient dir $groove_normal                     # top-down onto the groove
:arrow $va $vb color teal   = "V-V"            # endpoints via :let pos(...)
:arrow $pa $pb color orange = "peptide"
```

**Validated example — 1AO7 (A6 TCR / Tax / HLA-A2):**

```text
:load 1AO7.cif
:setenv TCR_A D ; :setenv TCR_B E
:setenv MHC A   ; :setenv PEP C
:setenv MHC_HELIX1 50-85
:setenv MHC_HELIX2 138-175
:setenv TCRA_CYS23 22  # PDB numbering for 1AO7 chain D
:setenv TCRB_CYS23 23
:setenv PEP_FIRST 1
:setenv PEP_LAST 9
:run @lib/tcr_angles
:registers           # → crossing ≈ 29.2°, incident ≈ 13.2°
```

Pierce TCR3d's published crossing angle for 1AO7 is ~28°; the molterm
output (29.2°) agrees to within 1.5° on this case. Discrepancies on
other structures usually trace to MHC helix-range choice or the V-Cys
residue number — both exposed as env vars precisely so the recipe can
be tuned per-PDB without forking.

### `kinase_state.mt` — protein-kinase active/inactive switches

Two orthogonal conformational switches that drive almost all kinase
drug-discovery structure work:

- **αC-in/out** — the conserved β3-Lys ↔ αC-Glu salt bridge, measured to
  the *nearer* of the two Glu Oε atoms (`min` over Oε1/Oε2, so the value
  is independent of which oxygen the file labels Oε1). Engaged (αC-in)
  ≈ 2.7–3.7 Å; αC-out breaks it (> ~4.5 Å).
- **DFG-in/out** — Modi & Dunbrack 2019 (PNAS) spatial definition, two
  distances from the DFG-Phe ring tip (Cζ): `D1 = |Cα(αC-Glu+4) − Cζ|`,
  `D2 = |Cα(β3-Lys) − Cζ|`. DFG-in: `D1 ≤ 11 Å and D2 ≥ 11 Å`; DFG-out:
  `D1 > 11 Å`; else intermediate.

| Var | Meaning | Example (1ATP) |
| --- | --- | --- |
| `KIN_CHAIN` | kinase chain id            | `E`   |
| `B3_LYS`    | β3 catalytic Lys resi      | `72`  |
| `AC_GLU`    | αC-helix Glu resi          | `91`  |
| `AC_GLU4`   | αC-Glu+4 resi (`AC_GLU+4`) | `95`  |
| `DFG_PHE`   | DFG-motif Phe resi         | `185` |

**Output registers:** `$ke_dist` (Lys-Nζ ↔ nearest Glu-Oε, Å), `$dfg_d1`,
`$dfg_d2` (Å).

**Validated — 1ATP (PKA catalytic subunit, active):**

```text
:load 1ATP.cif
:setenv KIN_CHAIN E
:setenv B3_LYS 72 ; :setenv AC_GLU 91 ; :setenv AC_GLU4 95
:setenv DFG_PHE 185
:run @lib/kinase_state
:registers     # → ke_dist ≈ 3.63 Å,  dfg_d1 ≈ 6.23 Å,  dfg_d2 ≈ 14.59 Å
```

`D1 ≪ 11 ≪ D2` and an engaged K–E bridge → active / αC-in / DFG-in, as
expected for 1ATP.

### `helix_kink.mt` — α-/TM-helix kink or bend angle

Splits one helix into two consecutive sub-segments, fits a helix axis to
each with `helix_axis()`, and reports the angle between them — proline
kinks, TM-helix hinges, GPCR helix bends. A straight helix reads small.

> **Why `helix_axis()`, not `pca()`.** `helix_axis()` fits the *helical*
> axis from consecutive-chord cross products, so it holds down to ~1 turn
> (~5–6 residues) per half — where a PCA long axis is still dominated by
> the short cloud's roundness. On a straight helix split into 7-residue
> halves, PCA scatters to ~22° while `helix_axis()` holds ~11°. The axis
> is oriented N→C, so no sign fix is needed. Leave a 1–2 residue gap at
> the kink so the bend isn't averaged into either fit.

| Var | Meaning | Example (2oar) |
| --- | --- | --- |
| `HELIX_CHAIN` | chain id holding the helix | `A`     |
| `SEG1`        | N-side residue range       | `16-28` |
| `SEG2`        | C-side residue range       | `31-43` |

**Output register:** `$kink` (bend angle in degrees; 0 = straight).

**Validated — 2oar (MscL) TM1, a long, near-straight transmembrane helix:**

```text
:load 2oar.cif
:setenv HELIX_CHAIN A ; :setenv SEG1 16-28 ; :setenv SEG2 31-43
:run @lib/helix_kink
:registers     # → kink ≈ 13.2°  (mild bend of a straight TM helix)
```

### `dna_bend.mt` — bend angle between two helical segments of a duplex

Fits a helix axis to two segments of one strand's phosphate backbone with
`helix_axis()` and reports the angle between them — the standard DNA-bend
definition (angle between flanking helical axes). Straight B-DNA ≈ 0–15°;
CAP/TBP/IHF kink their sites by 50–90°.

> **`helix_axis()` tracks bends PCA misses.** On CAP–DNA strand C split
> 16-24/25-33, a PCA long axis reads ~21° where `helix_axis()` reads ~57°
> for the same protein-induced bend. Each segment needs ≥ ~1 turn
> (~10–11 nt). A continuously wound duplex (nucleosomal DNA) is *not* a
> two-segment bend — there is no single angle; measure local windows.

| Var | Meaning | Example (1cgp) |
| --- | --- | --- |
| `DNA_CHAIN` | strand chain id            | `C`     |
| `HALF1`     | first segment residue range  | `16-24` |
| `HALF2`     | second segment residue range | `25-33` |

**Output register:** `$bend` (bend angle in degrees; 0 = straight).

**Validated — 1cgp (CAP–DNA), a strong protein-induced bend:**

```text
:fetch 1cgp
:setenv DNA_CHAIN C ; :setenv HALF1 16-24 ; :setenv HALF2 25-33
:run @lib/dna_bend
:registers     # → bend ≈ 56.5°  (CAP kink; full-site bend is ~80°)
```

### `ab_elbow.mt` — antibody Fab elbow angle

Fits a long axis (PCA) to the variable module (VL+VH) and the constant
module (CL+CH1) and reports the bend between them. The published Fab
elbow angle spans ~120°–225° (180° = straight); κ Fabs cluster high,
λ Fabs low.

> **Proxy, not the canonical fit.** The reference elbow angle is the angle
> between the *pseudo-2-fold (dyad) axes* relating VL↔VH and CL↔CH1, which
> needs a per-module symmetry-axis fit molterm does not yet expose. The
> PCA-axis proxy tracks elbow flexion and ranks conformers correctly, but
> absolute degrees may differ a few ° from the elbow-angle server — verify
> before quoting a number.

| Var | Meaning | Example (7FAB) |
| --- | --- | --- |
| `LIGHT_CHAIN` | light-chain id           | `L`         |
| `HEAVY_CHAIN` | heavy-chain id           | `H`         |
| `VL`          | VL residue range         | `1-107`     |
| `CL`          | CL residue range         | `108-204`   |
| `VH`          | VH residue range         | `1-113`     |
| `CH1`         | CH1 residue range        | `114-217`   |

**Output registers:** `$bend` (variable↔constant axis angle, 0 = straight),
`$elbow` (= 180 − bend, the conventional value).

**Validated — 7FAB (human Fab):**

```text
:load 7FAB.cif
:setenv LIGHT_CHAIN L ; :setenv HEAVY_CHAIN H
:setenv VL 1-107 ; :setenv CL 108-204 ; :setenv VH 1-113 ; :setenv CH1 114-217
:run @lib/ab_elbow
:registers     # → bend ≈ 37.9°,  elbow ≈ 142.1°  (within the Fab range)
```

### `domain_hinge.mt` — rigid-body rotation between two copies/states

Finds the screw axis of the optimal superposition of one selection onto
another with `superpose_axis()` and reports the rotation angle — the
magnitude of a domain motion (open↔closed, across two loaded objects) or
the rotation of a non-crystallographic / molecular symmetry operator. The
two selections must have **equal atom counts in correspondence** (i-th ↔
i-th): same residue range + atom name on identical/homologous chains.
Cross-object form `obj1/<sel> vs obj2/<sel>` compares two structures.

| Var | Meaning | Example (4hhb) |
| --- | --- | --- |
| `SEL_A` | first selection  | `chain A and name CA` |
| `SEL_B` | second selection | `chain C and name CA` |

**Output registers:** `$hinge_angle` (rotation in degrees), `$hinge_axis`
(vec3 screw axis), `$hinge_center` (vec3 anchor), `$hinge_rmsd` (post-fit
RMSD in Å — how rigid the two copies really are: ≈ 0 means a true rigid body
rotating about the hinge, large means the "domain" also deformed and the
single-axis model is only approximate).

**Validated — 4hhb α1 (A) vs α2 (C), related by the molecular 2-fold:**

```text
:load 4hhb.cif
:setenv SEL_A chain A and name CA
:setenv SEL_B chain C and name CA
:run @lib/domain_hinge
:registers     # → hinge_angle ≈ 179.9°  (the C2 dyad)
```

### `phi_psi.mt` — backbone φ/ψ torsions of one residue

Ramachandran φ/ψ of a residue via `dihedral()` (negated to IUPAC sign).
Pass the flanking residues i-1 / i / i+1 explicitly — string env vars
can't do arithmetic.

| Var | Meaning | Example (1ubq) |
| --- | --- | --- |
| `CH`   | chain id                      | `A`  |
| `PREV` | previous residue number (i-1) | `24` |
| `RES`  | residue number (i)            | `25` |
| `NEXT` | next residue number (i+1)     | `26` |

**Output registers:** `$phi`, `$psi` (degrees, [-180, 180]).

**Validated — 1ubq residue 25 (mid-helix):**

```text
:load 1ubq.cif
:setenv CH A ; :setenv PREV 24 ; :setenv RES 25 ; :setenv NEXT 26
:run @lib/phi_psi
:registers     # → phi ≈ -65.5°,  psi ≈ -44.4°  (α-helical)
```

### `residue_scan.mt` — per-residue distance scan over a selection

Walks every residue of `${SEL}` with `:foreach <var> in <selection>` and prints
each residue's Cα distance to a reference atom `${REF}` — a quick proximity
profile (e.g. rank a loop by distance to a ligand atom). Unlike `phi_psi.mt`,
the loop needs no residue-number arithmetic: the selection drives iteration and
binds `${r}` (a `chain:resi` spec), `${r_chain}`, `${r_resi}`, `${r_resn}`.

| Var | Meaning | Example (1UBQ) |
| --- | --- | --- |
| `SEL` | selection to walk    | `"chain A and resi 1-6"` |
| `REF` | reference atom spec  | `A:76:CA` |

Output: one `<chain>:<resi> <resn>  <dist> A` line per residue on stdout.

```text
:load 1ubq.cif
:setenv SEL "chain A and resi 1-6" ; :setenv REF A:76:CA
:run @lib/residue_scan     # → A:1 MET 37.06 A, A:2 GLN 34.67 A, …
```

## Contributing

New recipes welcome. The convention:

- 10-30 lines of `:let` / `:setenv`-driven composition. No new builtins.
- Required env vars listed in the file header, with default-friendly
  examples in the header comment.
- Output registers documented in the header.
- Start the file with `#!molterm scope=local export=<names>` and prefix
  every scratch register with `_` (`$_hlx1`, `$_groove`, …) — the local
  frame keeps caller state clean and the `_`-prefix is enforced as
  private at frame pop, so only the listed outputs propagate.
- A canonical PDB + expected output values in `lib/README.md` for
  validation.
