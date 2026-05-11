# molterm — analysis recipe library

Reusable `.mt` scripts that compute named structural metrics, intended for
`:run @lib/<name>` from figure scripts. Each recipe is a 10-30 line
composition of the v0.31+ primitives (`:let` + `pos()` + `pca()` +
`dot()` + `angle()`), so the formula itself *is* the documentation —
fork the script for variants, no rebuild required.

## Lookup chain

`:run @lib/<name>` resolves `<name>` (with or without trailing `.mt`)
against the first match in:

1. `$MOLTERM_LIB_DIR/<name>.mt`            — per-invocation override
2. `~/.molterm/lib/<name>.mt`              — per-user library (writable)
3. `<install-prefix>/share/molterm/lib/<name>.mt`  — shipped recipes
4. `<exe-dir>/../lib/<name>.mt`            — build-tree layout
5. `<source-dir>/lib/<name>.mt`            — dev fallback

Forks live at `~/.molterm/lib/`; shipped baselines stay untouched.

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

## Contributing

New recipes welcome. The convention:

- 10-30 lines of `:let` / `:setenv`-driven composition. No new builtins.
- Required env vars listed in the file header, with default-friendly
  examples in the header comment.
- Output registers documented in the header.
- A canonical PDB + expected output values in `lib/README.md` for
  validation.
