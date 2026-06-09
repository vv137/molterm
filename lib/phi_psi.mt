#!molterm scope=local export=phi,psi
# phi_psi.mt — backbone φ/ψ torsions of one residue (Ramachandran)
#
# IUPAC-sign backbone dihedrals via dihedral():
#   φ = C(i-1) – N(i)  – Cα(i) – C(i)
#   ψ = N(i)   – Cα(i) – C(i)  – N(i+1)
# dihedral() returns the standard atan2 sign; φ/ψ negate it to match the IUPAC
# convention (α-helix ≈ (-63°, -43°); β-sheet ≈ (-120°, +130°)).  Needs the
# flanking residues, so pass i-1 / i / i+1 explicitly (string env vars can't do
# arithmetic).
#
# Required env vars (set with :setenv before :run):
#   CH    chain id                       (e.g. A)
#   PREV  previous residue number (i-1)  (e.g. 24)
#   RES   residue number (i)             (e.g. 25)
#   NEXT  next residue number (i+1)      (e.g. 26)
#
# Output registers:
#   $phi  φ in degrees [-180, 180]
#   $psi  ψ in degrees [-180, 180]
#
# Example call (1ubq residue 25, mid-helix → ≈ (-66°, -44°)):
#   :setenv CH A ; :setenv PREV 24 ; :setenv RES 25 ; :setenv NEXT 26
#   :run @lib/phi_psi
#   :label corner topleft = "φ/ψ = ${phi:.0f}°, ${psi:.0f}°"

let _phi_raw = dihedral(pos(${CH}:${PREV}:C), pos(${CH}:${RES}:N), pos(${CH}:${RES}:CA), pos(${CH}:${RES}:C))
let _psi_raw = dihedral(pos(${CH}:${RES}:N), pos(${CH}:${RES}:CA), pos(${CH}:${RES}:C), pos(${CH}:${NEXT}:N))
let phi = 0.0 - $_phi_raw
let psi = 0.0 - $_psi_raw
:echo phi/psi = ${phi:.1f}/${psi:.1f} deg
