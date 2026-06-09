#!molterm scope=local export=kink
# helix_kink.mt — α-helix (or TM-helix) kink / bend angle
#
# Splits one helix into two consecutive sub-segments, fits a helix axis to each
# with helix_axis(), and reports the angle between them.  A straight helix →
# ~0–10°; a proline kink or a transmembrane-helix hinge opens it up (often
# 20–40°).  Leave a 1–2 residue gap around the kink so the bend itself isn't
# averaged into either fitted axis.
#
# helix_axis() fits the *helical* axis (consecutive-chord cross products), not
# the PCA long axis, so it stays reliable down to ~1 turn (~5–6 residues) per
# half — where a PCA axis is still dominated by the short cloud's roundness.
# On a straight helix split into 7-residue halves, PCA scatters to ~22° while
# helix_axis holds ~11°.  Each helix_axis is oriented N→C, so no sign fix is
# needed before taking the angle.
#
# Required env vars (set with :setenv before :run):
#   HELIX_CHAIN   chain id holding the helix         (e.g. A)
#   SEG1          N-side residue range               (e.g. 16-28)
#   SEG2          C-side residue range               (e.g. 31-43)
#
# Output registers:
#   $kink         bend angle in degrees (0 = straight)
#
# Example call (2oar — MscL TM1, a long near-straight transmembrane helix):
#   :setenv HELIX_CHAIN A ; :setenv SEG1 16-28 ; :setenv SEG2 31-43
#   :run @lib/helix_kink
#   :label corner topleft = "kink = ${kink:.1f}°"

select _ha = chain ${HELIX_CHAIN} and resi ${SEG1} and name CA
select _hb = chain ${HELIX_CHAIN} and resi ${SEG2} and name CA
let _pa = helix_axis($_ha)
let _pb = helix_axis($_hb)
let kink = angle($_pa.axis1, $_pb.axis1)
:echo kink = ${kink:.1f} deg
