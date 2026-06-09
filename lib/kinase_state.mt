#!molterm scope=local export=ke_dist,dfg_d1,dfg_d2
# kinase_state.mt — protein-kinase active/inactive conformation metrics
#
# Two orthogonal switches govern the catalytic state of a protein kinase:
#
#   1. αC-helix "in/out" — the conserved β3-Lys forms a salt bridge with the
#      αC-Glu only in the active "αC-in" state.  Metric: the Lys-Nζ ↔ Glu
#      carboxylate distance, taken to the *nearer* of the two Oε atoms
#      (min over Oε1/Oε2, the standard salt-bridge definition, so the result
#      is independent of which oxygen the file labels Oε1).  Engaged (αC-in)
#      ≈ 2.7–3.7 Å; αC-out breaks it (> ~4.5 Å).
#
#   2. DFG "in/out" — Modi & Dunbrack 2019 (PNAS) spatial definition, which is
#      expressible directly from three Cα/Cζ atoms:
#         D1 = | Cα(αC-Glu+4)  −  Cζ(DFG-Phe) |
#         D2 = | Cα(β3-Lys)    −  Cζ(DFG-Phe) |
#      DFG-in:  D1 ≤ 11 Å  and  D2 ≥ 11 Å
#      DFG-out: D1 >  11 Å  (Phe swung into the ATP pocket)
#      else:    DFG-inter (intermediate)
#
# Required env vars (set with :setenv before :run):
#   KIN_CHAIN   kinase chain id                         (e.g. E)
#   B3_LYS      β3 catalytic Lys residue num            (e.g. 72  for PKA)
#   AC_GLU      αC-helix Glu residue num                (e.g. 91  for PKA)
#   AC_GLU4     αC-Glu+4 residue num (= AC_GLU + 4)     (e.g. 95  for PKA)
#   DFG_PHE     DFG-motif Phe residue num               (e.g. 185 for PKA)
#
# Output registers:
#   $ke_dist    β3-Lys-Nζ ↔ nearest αC-Glu-Oε distance, Å (αC-in if ≲ 4 Å)
#   $dfg_d1     Cα(αC-Glu+4) ↔ Cζ(DFG-Phe) distance, Å
#   $dfg_d2     Cα(β3-Lys)   ↔ Cζ(DFG-Phe) distance, Å
#
# Example call (1ATP — PKA catalytic subunit, active / DFG-in):
#   :setenv KIN_CHAIN E
#   :setenv B3_LYS 72 ; :setenv AC_GLU 91 ; :setenv AC_GLU4 95
#   :setenv DFG_PHE 185
#   :run @lib/kinase_state
#   :label corner topleft  = "K-E = ${ke_dist:.1f} Å"
#   :label corner topright = "DFG D1/D2 = ${dfg_d1:.1f}/${dfg_d2:.1f} Å"

# αC-in/out: β3-Lys-Nζ to the nearer αC-Glu carboxylate oxygen (min Oε1/Oε2)
let _nz   = pos(${KIN_CHAIN}:${B3_LYS}:NZ)
let _l1   = distance($_nz, pos(${KIN_CHAIN}:${AC_GLU}:OE1))
let _l2   = distance($_nz, pos(${KIN_CHAIN}:${AC_GLU}:OE2))
let ke_dist = min($_l1, $_l2)

# DFG-in/out: two reference distances to the DFG-Phe ring tip (Cζ)
let _cz   = pos(${KIN_CHAIN}:${DFG_PHE}:CZ)
let dfg_d1 = distance(pos(${KIN_CHAIN}:${AC_GLU4}:CA), $_cz)
let dfg_d2 = distance(pos(${KIN_CHAIN}:${B3_LYS}:CA),  $_cz)
:echo K-E = ${ke_dist:.2f} A, DFG d1/d2 = ${dfg_d1:.2f}/${dfg_d2:.2f} A
