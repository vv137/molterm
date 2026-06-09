#!molterm scope=local
# residue_scan.mt — per-residue distance scan over a selection.
#
# Demonstrates `:foreach <var> in <selection>`, which iterates the distinct
# residues of a selection and binds, each pass:
#   ${r}        a `chain:resi` spec usable directly in pos(...)
#   ${r_chain}  chain id        ${r_resi}  residue number      ${r_resn}  resname
#
# This recipe walks every residue of ${SEL} and prints its Cα distance to a
# reference atom ${REF} — a quick way to rank a binding-site loop by proximity
# to a ligand atom, or to dump a per-residue profile for downstream parsing.
# Unlike phi_psi.mt (which needs the flanking residues passed explicitly), the
# loop needs no arithmetic on residue numbers — the selection drives iteration.
#
# Required env vars (set with :setenv before :run):
#   SEL   selection to walk    (e.g. "chain A and resi 10-20")
#   REF   reference atom spec  (e.g. "A:76:CA", chain:resi:name)
#
# Output: one `<chain>:<resi> <resn>  <dist> A` line per residue on stdout.
#
# Example (1ubq: distances of residues 1-6 to the C-terminal Cα):
#   :setenv SEL "chain A and resi 1-6" ; :setenv REF A:76:CA
#   :run @lib/residue_scan

:foreach r in ${SEL}
  let _d = distance(pos(${r}:CA), pos(${REF}))
  :echo ${r_chain}:${r_resi} ${r_resn}  ${_d:.2f} A
:end
