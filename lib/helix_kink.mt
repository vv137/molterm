#!molterm scope=local export=kink
# helix_kink.mt — α-helix (or TM-helix) kink / bend angle
#
# Splits one helix into two consecutive sub-segments, fits a principal axis to
# each (PCA), and reports the angle between them.  A straight helix → ~0–10°;
# a proline kink or a hinge in a transmembrane helix opens it up (often 20–40°).
# Leave a 1–2 residue gap around the kink point so the bend itself isn't
# averaged into either fitted axis.
#
# Geometric definition:
#   axis_a = pca(SEG1).axis1   (long axis of the N-side half)
#   axis_b = pca(SEG2).axis1   (long axis of the C-side half)
#   Both axes are sign-flipped to point N→C (positive projection on the
#   centroid-A→centroid-B vector) so the angle measures the true bend, not its
#   supplement — PCA eigenvector signs are otherwise arbitrary.
#   kink = angle(axis_a, axis_b)
#
# Required env vars (set with :setenv before :run):
#   HELIX_CHAIN   chain id holding the helix         (e.g. A)
#   SEG1          N-side residue range               (e.g. 10-20)
#   SEG2          C-side residue range               (e.g. 23-33)
#
# Output registers:
#   $kink         bend angle in degrees (0 = straight)
#
# Example call:
#   :setenv HELIX_CHAIN A
#   :setenv SEG1 10-20 ; :setenv SEG2 23-33
#   :run @lib/helix_kink
#   :label corner topleft = "kink = ${kink:.1f}°"

select _ha = chain ${HELIX_CHAIN} and resi ${SEG1} and name CA
select _hb = chain ${HELIX_CHAIN} and resi ${SEG2} and name CA
let _pa = pca($_ha)
let _pb = pca($_hb)
let _axa = $_pa.axis1
let _axb = $_pb.axis1
# common N→C reference: centroid of SEG1 → centroid of SEG2
let _g  = $_pb.center - $_pa.center
let _sa = dot($_axa, $_g)
let _sb = dot($_axb, $_g)
let _oa = ($_sa / abs($_sa)) * $_axa
let _ob = ($_sb / abs($_sb)) * $_axb
let kink = angle($_oa, $_ob)
