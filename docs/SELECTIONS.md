# Selection Algebra

[← Back to README](../README.md) · See also: [Commands](COMMANDS.md)

### Selection Algebra

Recursive descent parser with boolean operators. Used by `:select`, `:count`, `:color`, and `/` search.

```vim
:select chain A and helix                " helix residues in chain A
:select resi 50-60 or name CA            " residue range or all Cα atoms
:select not water and not hydro          " heavy atoms, no water
:select backbone and chain B             " backbone of chain B
:select active = resi 100-120 and chain A
:select site = $sele                     " save mouse selection to named selection
:color red $active                       " use named selection with $
:show cartoon chain A                    " show cartoon only for chain A
/helix and chain A                       " search with n/N navigation
```

**Cross-object narrowing** — when `:set scope all` is in effect (the
default after multi-load), use `obj <name>` or the slash form to scope
the same selection to a specific object. PDB-style names that start
with a digit (`1ubq`, `7bz5`) are accepted as a single token:

```vim
:color red, obj 1ubq                  " just one of N loaded structures
:color blue, /relaxed_model_3/A//     " chain A of one specific model
:show cartoon, obj 2def and chain A   " combine with other predicates
```

**IMGT-canonical CDR / FR regions** (issue #36) — `imgt <region>` selects
the standard IMGT residue range for an antibody / TCR variable domain.
Assumes the chain is **already IMGT-numbered** (e.g. by an upstream
ANARCI pass — molterm doesn't compute the renumbering itself). Combine
with `chain X` to scope to a single chain. Region names and residue
ranges (per imgt.org):

| keyword | residue range |
|---|---|
| `fr1`            | 1-26    |
| `cdr1`           | 27-38   |
| `cdr1_anchored`  | 26-39   |
| `fr2`            | 39-55   |
| `cdr2`           | 56-65   |
| `cdr2_anchored`  | 55-66   |
| `fr3`            | 66-104  |
| `cdr3`           | 105-117 |
| `cdr3_anchored`  | 104-118 |
| `fr4`            | 118-128 |

The `_anchored` variants (issue #83) include the conserved framework
positions that bookend each CDR — Cys23/26-Cys104 for the V-domain
core disulfide, Trp/Phe118 for the FR4 anchor — so `imgt cdr3_anchored`
gives the canonical C-X-X-X-X-W/F bracket form used in many
structural papers.

**IMGT positions** (issue #84) — alongside the named regions, `imgt`
also accepts numeric forms for single positions, inclusive ranges, and
'+'-separated sets:

```vim
:select e108 = chain B and imgt 108              " single IMGT position
:select cdr3core = chain B and imgt 107-115      " range
:select glus  = chain B and imgt 106+108+115     " set
:select mixed = chain B and imgt 105-110+115     " range + set mixed
:zoom chain B and imgt 108                       " frame the load-bearing residue
```

```vim
:select cdr3a = chain A and imgt cdr3            " TCR α-chain CDR3 (105-117)
:select cdr3b = chain B and imgt cdr3_anchored   " TCR β with C/F anchors
:show ballstick chain A and imgt cdr3
:color magenta chain B and imgt cdr3
```

The same ranges work for antibody heavy/light chain CDRs since IMGT
numbering is unified across V-domain types. Unknown region names
return an empty selection rather than failing — keeps batch scripts
robust against typos in domain-specific keywords. **Position counts
are data-dependent**: a 9-aa CDR3-β with IMGT gaps at 109-112 returns
9 residues, but a chain renumbered with the gaps *filled in* will
return all 13 positions — the selector trusts the residue numbers it
sees, ANARCI must produce the canonical gap pattern upstream.

**Spatial proximity** — `within N of <expr>` selects atoms ≤ N Å from any
atom matching the inner expression; `exwithin` is the same minus the
inner set itself (useful for finding "neighbors not including self"):

```vim
:select within 4.5 of resn HEM           " every atom near a heme
:select exwithin 4.5 of chain A           " contacts of chain A on other chains
:select within 6 of $sele                 " expand mouse selection to neighbors
:show sticks within 5 of resi 100         " sidechains in the binding pocket
```

**Whole-residue / whole-chain expansion** — `same KW as <expr>` (where
`KW` is `residue`, `chain`, or `resname`/`resn`) promotes a per-atom
selection up to the enclosing residue, chain, or all residues of the
same type. Common with `within`, which returns individual atoms:

```vim
:select same residue as within 5 of resn HEM   " entire residues touching a heme
:select same chain as resn ATP                  " every chain that binds ATP
:select same resname as resn HIS                " all histidines in the structure
```

**Peptide-sequence search** — `pepseq <one-letter-codes>` (alias `seq`,
`sequence`) matches contiguous residue runs whose one-letter codes spell
the pattern. `.` and `?` are single-residue wildcards; matches never
cross chain breaks. Works with the rest of the algebra:

```vim
:select pepseq KVL                  " every Lys-Val-Leu run (β-globin motif)
:select pepseq H.L                  " His-anything-Leu (3-residue wildcard pattern)
:select chain B and pepseq KVL      " same motif but only in chain B
:show sticks pepseq GXG and chain A " glycine kinks
```

**Mouse selection workflow:**

1. `gs` (atom), `gS` (residue), or `gc` (chain) to enter select mode
2. Click to toggle atoms/residues/chains in `$sele` (status bar shows count)
3. `ESC` to exit select mode
4. `:select mysite = $sele` to save, `:color red $mysite`, `:show cartoon $mysite`
5. `:select clear` to reset

**Pick registers for measurement:**

1. Click atoms in inspect mode — each click stores pk1→pk2→pk3→pk4 (rotating)
2. `:measure` (distance pk1↔pk2), `:angle` (pk1-pk2-pk3), `:dihedral` (pk1-pk4)
3. Results shown as dashed lines + labels on viewport
4. `:overlay` to toggle visibility, `:overlay clear` to remove all

**Slash notation:** `/obj/chain/resi/name` — hierarchical selection (empty = wildcard)

```vim
//A/42/CA                       " chain A, residue 42, atom CA
//A+B/10-50                     " chains A and B, residues 10-50
/1abc//42                       " object 1abc, all chains, residue 42
//A                             " chain A, all residues
```

**Keywords:** `all`, `chain`, `resn`, `resi` (range), `name`, `element`, `helix`, `sheet`, `loop`, `backbone`/`bb`, `sidechain`/`sc`, `phosphate`, `sugar`, `base`, `hydro`, `water`, `het`/`ligand`, `protein`, `nucleic`, `dna`, `rna`, `polymer`, `obj`, `$name`

**Nucleic substructure** (issue #111) — `phosphate` (P, OP1/OP2, O5', O3'), `sugar` (C1'–C5', O4', O2'), and `base` (the heavy ring atoms) partition a nucleotide; each is gated on the residue being a standard nucleotide. `backbone`/`bb` and `sidechain`/`sc` are nucleic-aware: for a nucleotide, `backbone` = phosphate + sugar and `sidechain` = the base, so `chain I and backbone` traces a DNA/RNA strand the same way it does a protein.

```vim
:color orange phosphate            " highlight the phosphodiester backbone
:show sticks base and chain A      " bases of one strand
:select bb_trace = chain I and backbone
```

**Spatial / expansion:** `within N of <expr>`, `exwithin N of <expr>`, `same residue as <expr>`, `same chain as <expr>`, `same resname as <expr>` (alias `resn`); `byres <expr>` / `bychain <expr>` are sugar for `same residue|chain as <expr>` (issue #52)

**Sequence search:** `pepseq <one-letter-codes>` (aliases: `seq`, `sequence`); `.` / `?` = single-residue wildcard

**Operators:** `and`, `or`, `minus`, `xor`, `not`, `( )`, `+` (OR shorthand: `chain A+B`, `resi 10+20+30-40`)

**Set algebra examples (issue #52):**

```vim
:select paratope = chain H+L and within 5 of chain G
:select paratope_only = $paratope minus $epitope     " atoms in paratope not in epitope
:select interface   = $paratope xor $epitope         " atoms in exactly one side
:count byres $paratope                                " entire residues touching the antigen
:select bb_interface = backbone and bychain $paratope " whole chains, backbone only
```

`minus` and `xor` sit at the same precedence as `or` (left-associative); use parentheses when mixing with `and` to be explicit (`(chain A and resi 1-50) minus name CA`). `-` is *not* an operator — it stays reserved for residue ranges like `resi 1-10`.

---

