#!molterm scope=local export=crossing,incident
# tcr_angles.mt — TCR-pMHC crossing & incident angles (Singh 2020 / Pierce TCR3d)
#
# Geometric definition (per Rudolph 2006 / Singh 2020):
#   1. groove plane = PCA on MHC α1 + α2 helices; axis3 = plane normal n̂
#   2. V-V axis     = pos(TCRα Cys23 Cα) − pos(TCRβ Cys23 Cα)
#   3. peptide axis = pos(PEP P1 Cα) − pos(PEP PΩ Cα)
#   4. crossing angle = angle( project(V-V, plane), project(peptide, plane) )
#   5. incident angle = |angle(V-V, n̂) − 90°|         (caller takes abs)
#
# Required env vars (set with :setenv before :run):
#   TCR_A           TCRα chain id           (e.g. D)
#   TCR_B           TCRβ chain id           (e.g. E)
#   MHC             MHC heavy-chain id      (e.g. A)
#   PEP             peptide chain id        (e.g. C)
#   MHC_HELIX1      α1 residue range        (e.g. 50-85)
#   MHC_HELIX2      α2 residue range        (e.g. 138-175)
#   TCRA_CYS23      Vα Cys23 residue num    (e.g. 23   for IMGT-numbered chains)
#   TCRB_CYS23      Vβ Cys23 residue num    (e.g. 23)
#   PEP_FIRST       peptide P1 residue num  (e.g. 1)
#   PEP_LAST        peptide PΩ residue num  (e.g. 9)
#
# Output registers:
#   $crossing       crossing angle in degrees (0..180)
#   $incident       incident-angle offset from 90° (signed; callers take abs)
#
# Example call (1AO7 — A6 TCR / Tax / HLA-A2):
#   :setenv TCR_A D
#   :setenv TCR_B E
#   :setenv MHC A
#   :setenv PEP C
#   :setenv MHC_HELIX1 50-85
#   :setenv MHC_HELIX2 138-175
#   :setenv TCRA_CYS23 23
#   :setenv TCRB_CYS23 23
#   :setenv PEP_FIRST 1
#   :setenv PEP_LAST 9
#   :run @lib/tcr_angles
#   :label corner topleft  = "crossing = ${crossing:.1f}°"
#   :label corner topright = "incident = ${incident:.1f}°"

select _hlx1 = chain ${MHC} and resi ${MHC_HELIX1}
select _hlx2 = chain ${MHC} and resi ${MHC_HELIX2}
let _groove = pca($_hlx1 or $_hlx2)
let _n      = $_groove.axis3
let _v_axis = pos(${TCR_A}:${TCRA_CYS23}:CA) - pos(${TCR_B}:${TCRB_CYS23}:CA)
let _p_axis = pos(${PEP}:${PEP_FIRST}:CA) - pos(${PEP}:${PEP_LAST}:CA)
let _v_proj = $_v_axis - dot($_v_axis, $_n) * $_n
let _p_proj = $_p_axis - dot($_p_axis, $_n) * $_n
let crossing = angle($_v_proj, $_p_proj)
let _v_to_n  = angle($_v_axis, $_n)
let incident = $_v_to_n - 90.0
