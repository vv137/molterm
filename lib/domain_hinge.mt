#!molterm scope=local export=hinge_angle,hinge_axis,hinge_center,hinge_rmsd
# domain_hinge.mt — rigid-body rotation relating two copies/states of a domain
#
# Given the SAME atoms in two places — two conformations of a domain (open vs
# closed, across two loaded objects), or two copies related by symmetry — finds
# the screw axis of the optimal superposition with superpose_axis() and reports
# the rotation angle.  That angle is the magnitude of a domain motion or the
# rotation of a non-crystallographic symmetry operator; the axis is the hinge.
#
# superpose_axis(selA vs selB) needs the two selections to have EQUAL atom
# counts in correspondence (i-th ↔ i-th) — the same residue range + atom name
# on identical/homologous chains.  Cross-object form: obj1/<sel> vs obj2/<sel>
# compares two loaded structures (e.g. an open and a closed model).
#
# Required env vars (set with :setenv before :run):
#   SEL_A   first selection   (e.g. "chain A and name CA")
#   SEL_B   second selection  (e.g. "chain C and name CA")
#
# Output registers:
#   $hinge_angle    rotation angle in degrees (0 = identical pose)
#   $hinge_axis     vec3 rotation (screw) axis
#   $hinge_center   vec3 anchor point (midpoint of the two centroids)
#   $hinge_rmsd     post-fit RMSD in Å — how rigid the two copies really are
#                   (≈ 0 = a true rigid body rotating about the hinge; large =
#                   the "domain" also deformed, so the single-axis model is only
#                   approximate)
#
# Example call (4hhb — α1 vs α2, related by the molecular 2-fold):
#   :setenv SEL_A chain A and name CA
#   :setenv SEL_B chain C and name CA
#   :run @lib/domain_hinge
#   :label corner topleft = "rotation = ${hinge_angle:.1f}° (rmsd ${hinge_rmsd:.2f} Å)"

let _sp = superpose_axis(${SEL_A} vs ${SEL_B})
let hinge_angle  = $_sp.eig1
let hinge_axis   = $_sp.axis1
let hinge_center = $_sp.center
let hinge_rmsd   = $_sp.rmsd
