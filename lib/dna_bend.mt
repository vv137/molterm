#!molterm scope=local export=bend
# dna_bend.mt — bend angle between two helical segments of a duplex
#
# Fits a helix axis to two segments of one strand's phosphate backbone with
# helix_axis() and reports the angle between them — the standard definition of
# a DNA bend (angle between the flanking helical axes).  Straight B-DNA →
# ~0–15°; protein-induced kinks are large (CAP, TBP, IHF bend their sites by
# 50–90°).
#
# helix_axis() fits the *helical* axis (consecutive-chord cross products), which
# tracks a bend that a PCA long axis badly under-reports on a curved arm: on
# CAP–DNA (1cgp) strand C split 16-24/25-33, PCA reads ~21° where helix_axis
# reads ~57° for the same protein-induced bend.  Each axis is oriented 5'→3',
# so no sign fix is needed before taking the angle.
#
# Notes:
#   - One strand, two contiguous segments; leave the central 1–2 bp out to
#     localize the bend at the middle.
#   - Each segment needs ≥ ~1 turn (~10–11 nt) for a stable axis.
#   - A continuously wound duplex (e.g. nucleosomal DNA) is not a two-segment
#     bend — there is no single angle; measure local windows instead.
#
# Required env vars (set with :setenv before :run):
#   DNA_CHAIN   strand chain id                        (e.g. C)
#   HALF1       first segment residue range            (e.g. 16-24)
#   HALF2       second segment residue range           (e.g. 25-33)
#
# Output registers:
#   $bend       bend angle in degrees (0 = straight)
#
# Example call (1cgp — CAP–DNA, a strong protein-induced bend):
#   :fetch 1cgp
#   :setenv DNA_CHAIN C ; :setenv HALF1 16-24 ; :setenv HALF2 25-33
#   :run @lib/dna_bend
#   :label corner topleft = "bend = ${bend:.1f}°"

select _h1 = chain ${DNA_CHAIN} and resi ${HALF1} and name P
select _h2 = chain ${DNA_CHAIN} and resi ${HALF2} and name P
let _p1 = helix_axis($_h1)
let _p2 = helix_axis($_h2)
let bend = angle($_p1.axis1, $_p2.axis1)
:echo bend = ${bend:.1f} deg
