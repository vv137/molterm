#!molterm scope=local export=bend
# dna_bend.mt — global bend angle of a DNA (or RNA) duplex
#
# Fits a helical axis to the 5' half and the 3' half of one strand (PCA over
# the phosphate backbone of each half) and reports the angle between them.
# Straight B-DNA → ~0–15°; protein-induced bends are large — CAP, TBP, and IHF
# kink their sites by 50–90°, and the nucleosome wraps ~147 bp through a
# continuous bend.
#
# IMPORTANT — segment length: PCA only recovers the helix axis once each half
# spans enough backbone to be clearly elongated along it.  Use ≥ ~1.5 helical
# turns per half (≳ 12–15 nt).  On a short oligo (e.g. a 12-mer split 6/6) the
# per-half cloud is rounder than it is long and the axis — hence the bend — is
# unreliable.  For sub-turn precision use a dedicated base-pair-step axis fit.
#
# Geometric definition:
#   axis_5 = pca(HALF1).axis1     (helix axis of the 5' half)
#   axis_3 = pca(HALF2).axis1     (helix axis of the 3' half)
#   Both axes are sign-flipped to point 5'→3' (positive projection on the
#   centroid-5'→centroid-3' vector); PCA eigenvector signs are arbitrary.
#   bend = angle(axis_5, axis_3)
#
# Notes:
#   - Use one strand and split it into two contiguous halves; leave the central
#     1–2 bp out if you want the bend localized at the middle.
#
# Required env vars (set with :setenv before :run):
#   DNA_CHAIN   strand chain id                        (e.g. I)
#   HALF1       5' half residue range                  (e.g. -73--1)
#   HALF2       3' half residue range                  (e.g. 1-73)
#
# Output registers:
#   $bend       global bend angle in degrees (0 = straight)
#
# Example call (1KX5 — nucleosome core, ~147 bp wrapped ~1.65 turns):
#   :setenv DNA_CHAIN I
#   :setenv HALF1 -73--1 ; :setenv HALF2 1-73
#   :run @lib/dna_bend
#   :label corner topleft = "bend = ${bend:.1f}°"

select _h1 = chain ${DNA_CHAIN} and resi ${HALF1} and name P
select _h2 = chain ${DNA_CHAIN} and resi ${HALF2} and name P
let _p1 = pca($_h1)
let _p2 = pca($_h2)
let _a1 = $_p1.axis1
let _a2 = $_p2.axis1
# common 5'→3' reference: centroid of HALF1 → centroid of HALF2
let _g  = $_p2.center - $_p1.center
let _s1 = dot($_a1, $_g)
let _s2 = dot($_a2, $_g)
let _o1 = ($_s1 / abs($_s1)) * $_a1
let _o2 = ($_s2 / abs($_s2)) * $_a2
let bend = angle($_o1, $_o2)
