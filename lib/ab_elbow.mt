#!molterm scope=local export=elbow,bend
# ab_elbow.mt — antibody Fab elbow angle (variable ↔ constant module bend)
#
# A Fab flexes about the "elbow" between its variable module (VL+VH) and its
# constant module (CL+CH1).  This recipe fits a long axis to each module (PCA)
# and reports the bend between them.  The published Fab elbow angle (Stanfield
# 2006; the elbow-angle web server) spans ~120°–225°, where 180° is a straight,
# fully-extended Fab.
#
# Geometric definition:
#   axis_v = pca(Fv).axis1   over VL+VH
#   axis_c = pca(C1).axis1   over CL+CH1
#   Both are sign-flipped to point variable→constant; PCA signs are arbitrary.
#   bend  = angle(axis_v, axis_c)     (0° = collinear / straight)
#   elbow = 180 − bend                (the conventional 180°-is-straight value)
#
# CAVEAT — this is a principal-axis proxy.  The canonical elbow angle is the
# angle between the *pseudo-2-fold (dyad) axes* relating VL↔VH and CL↔CH1, which
# needs a per-module symmetry-axis fit molterm does not yet expose (see the
# tracking issue).  The PCA proxy tracks elbow flexion and is fine for ranking
# conformers, but absolute degrees may differ a few ° from the server value —
# validate against it before quoting a number.
#
# Required env vars (set with :setenv before :run):
#   LIGHT_CHAIN   light-chain id                       (e.g. L)
#   HEAVY_CHAIN   heavy-chain id                       (e.g. H)
#   VL            VL (variable light) residue range    (e.g. 1-107)
#   CL            CL (constant light) residue range    (e.g. 108-214)
#   VH            VH (variable heavy) residue range    (e.g. 1-113)
#   CH1           CH1 (constant heavy) residue range   (e.g. 114-220)
#
# Output registers:
#   $bend         variable↔constant axis angle, ° (0 = straight)
#   $elbow        conventional elbow angle = 180 − bend, °
#
# Example call:
#   :setenv LIGHT_CHAIN L ; :setenv HEAVY_CHAIN H
#   :setenv VL 1-107 ; :setenv CL 108-214
#   :setenv VH 1-113 ; :setenv CH1 114-220
#   :run @lib/ab_elbow
#   :label corner topleft = "elbow = ${elbow:.1f}°"

select _fv = (chain ${LIGHT_CHAIN} and resi ${VL}) or (chain ${HEAVY_CHAIN} and resi ${VH})
select _c1 = (chain ${LIGHT_CHAIN} and resi ${CL}) or (chain ${HEAVY_CHAIN} and resi ${CH1})
let _pv = pca($_fv)
let _pc = pca($_c1)
let _axv = $_pv.axis1
let _axc = $_pc.axis1
# common variable→constant reference: Fv centroid → constant centroid
let _g  = $_pc.center - $_pv.center
let _sv = dot($_axv, $_g)
let _sc = dot($_axc, $_g)
let _ov = ($_sv / abs($_sv)) * $_axv
let _oc = ($_sc / abs($_sc)) * $_axc
let bend  = angle($_ov, $_oc)
let elbow = 180.0 - $bend
