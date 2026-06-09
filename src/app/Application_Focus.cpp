#include "molterm/app/Application.h"

#include "molterm/analysis/ContactMap.h"
#include "molterm/core/BondTable.h"
#include "molterm/core/Logger.h"
#include "molterm/core/MolObject.h"
#include "molterm/core/SASA.h"
#include "molterm/core/Selection.h"
#include "molterm/render/Camera.h"
#include "molterm/render/Canvas.h"
#include "molterm/render/ColorMapper.h"
#include "molterm/repr/InterfaceRepr.h"
#include "molterm/repr/ReprUtil.h"
#include "molterm/repr/WireframeRepr.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <vector>

// Picking/info, current-object hooks, focus mode, and camera view-fit —
// split out of Application.cpp (issue: god-class decomposition). Same
// Application class; these are its method definitions in their own TU.
namespace molterm {

int Application::findNearestAtom(int termX, int termY) const {
    int scaleX = canvas_ ? canvas_->scaleX() : 1;
    int scaleY = canvas_ ? canvas_->scaleY() : 1;
    int subX = termX * scaleX + scaleX / 2;
    int subY = termY * scaleY + scaleY / 2;

    int bestIdx = -1;
    float bestDist2 = std::numeric_limits<float>::max();
    float bestDepth = std::numeric_limits<float>::max();

    // Query 3x3 neighborhood in spatial hash
    int cx = subX / kPickCellSize, cy = subY / kPickCellSize;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int key = (cy + dy) * 10000 + (cx + dx);
            auto it = pickGrid_.find(key);
            if (it == pickGrid_.end()) continue;
            for (int ci : it->second) {
                const auto& pa = projCache_[ci];
                float ddx = static_cast<float>(pa.sx - subX);
                float ddy = static_cast<float>(pa.sy - subY);
                float dist2 = ddx * ddx + ddy * ddy;
                if (dist2 < bestDist2 - 0.5f ||
                    (dist2 < bestDist2 + 0.5f && pa.depth < bestDepth)) {
                    bestDist2 = dist2;
                    bestDepth = pa.depth;
                    bestIdx = pa.idx;
                }
            }
        }
    }

    float maxRange = static_cast<float>(10 * std::max(scaleX, scaleY));
    if (bestDist2 > maxRange * maxRange) return -1;
    return bestIdx;
}

std::string Application::residueInfoString(const AtomData& a) const {
    char buf[80];
    std::snprintf(buf, sizeof(buf),
        "%s %d%s (chain %s)",
        a.resName.c_str(), a.resSeq,
        (a.insCode == ' ' ? "" : std::string(1, a.insCode).c_str()),
        a.chainId.c_str());
    return std::string(buf);
}

std::string Application::atomInfoString(const MolObject& mol, int atomIdx) const {
    if (atomIdx < 0 || atomIdx >= static_cast<int>(mol.atoms().size()))
        return "";
    const auto& a = mol.atoms()[atomIdx];
    char buf[256];
    std::string insStr = (a.insCode != ' ') ? std::string(1, a.insCode) : "";
    snprintf(buf, sizeof(buf),
        "%s/%s %d%s/%s (%s) B=%.1f occ=%.2f [%.2f, %.2f, %.2f]",
        a.chainId.c_str(), a.resName.c_str(), a.resSeq,
        insStr.c_str(),
        a.name.c_str(), a.element.c_str(),
        a.bFactor, a.occupancy,
        a.x, a.y, a.z);
    return std::string(buf);
}

bool Application::recomputeInterface() {
    auto obj = tabMgr_.currentTab().currentObject();
    if (!obj) {
        interfaceContacts_.clear();
        interfaceAtomMask_.clear();
        interfaceRepr_.clear();
        return false;
    }

    contactMapPanel_.update(*obj);
    contactMapPanel_.contactMap().computeInterface(*obj, interfaceCutoff_);
    interfaceContacts_ = contactMapPanel_.contactMap().interfaceContacts();
    if (interfaceContacts_.empty()) {
        interfaceAtomMask_.clear();
        interfaceRepr_.clear();
        return false;
    }

    const auto& atoms = obj->atoms();
    interfaceAtomMask_.assign(atoms.size(), false);
    std::set<std::tuple<std::string,int,char>> interfaceResidues;
    for (const auto& c : interfaceContacts_) {
        if (c.atom1 >= 0 && c.atom1 < (int)atoms.size()) {
            const auto& a = atoms[c.atom1];
            interfaceResidues.emplace(a.chainId, a.resSeq, a.insCode);
        }
        if (c.atom2 >= 0 && c.atom2 < (int)atoms.size()) {
            const auto& a = atoms[c.atom2];
            interfaceResidues.emplace(a.chainId, a.resSeq, a.insCode);
        }
    }
    for (size_t i = 0; i < atoms.size(); ++i) {
        const auto& a = atoms[i];
        if (interfaceResidues.count({a.chainId, a.resSeq, a.insCode}))
            interfaceAtomMask_[i] = true;
    }
    interfaceRepr_.setData(interfaceAtomMask_, interfaceContacts_);
    interfaceRepr_.setDrawSidechains(interfaceSidechains_);
    interfaceRepr_.setInteractionThickness(interfaceThickness_);
    interfaceRepr_.setLineThickness(std::max(1, interfaceThickness_ - 1));
    interfaceRepr_.setShowMask(interfaceShowMask_);
    return true;
}

void Application::onCurrentObjectChanged() {
    // Stale-overlay guard: when the user switches objects, any state
    // built against the prior mol's atom indices (interface mask /
    // contacts) no longer corresponds to what's being rendered. Refresh
    // it so dashes + sidechain hairs match the new current object.
    if (interfaceOverlay_) {
        if (!recomputeInterface()) {
            // New object has no inter-chain contacts — turn the overlay
            // off rather than leave a dangling on-state with empty data.
            interfaceOverlay_ = false;
            interfaceFromZoom_ = false;
        }
    }
    if (canvas_) canvas_->invalidate();
}

Selection Application::parseSelection(const std::string& expr, const MolObject& mol) {
    auto resolver = [this](const std::string& name) -> const Selection* {
        auto it = namedSelections_.find(name);
        return (it != namedSelections_.end()) ? &it->second : nullptr;
    };
    auto sel = Selection::parse(expr, mol, resolver);
    // Auto-save latest result under kSele so `$sele` references it.
    namedSelections_[kSele] = sel;
    return sel;
}

// ── Focus Selection mode ────────────────────────────────────────────────────
//
// Mol*-style click-to-focus. enterFocus snapshots camera + per-repr
// visibility, snaps the camera to the subject centroid, hides
// non-neighborhood atoms for atom-direct reprs (Wireframe/BallStick/
// Spacefill), and forces ball-stick visible on the neighborhood so
// sidechains pop. exitFocus restores everything.

std::vector<int> Application::expandByFocusGranularity(const MolObject& mol,
                                                       int atomIdx) const {
    std::vector<int> out;
    const auto& atoms = mol.atoms();
    if (atomIdx < 0 || atomIdx >= (int)atoms.size()) return out;
    const auto& a = atoms[atomIdx];

    if (focusGranularity_ == FocusGranularity::Chain) {
        for (int i = 0; i < (int)atoms.size(); ++i) {
            if (atoms[i].chainId == a.chainId) out.push_back(i);
        }
        return out;
    }

    if (focusGranularity_ == FocusGranularity::Sidechain) {
        for (int i = 0; i < (int)atoms.size(); ++i) {
            const auto& b = atoms[i];
            if (b.chainId != a.chainId || b.resSeq != a.resSeq ||
                b.insCode != a.insCode) continue;
            const std::string& nm = b.name;
            if (nm == "N" || nm == "CA" || nm == "C" || nm == "O") continue;
            out.push_back(i);
        }
        if (!out.empty()) return out;
        // Empty sidechain (e.g. Gly) → fall through to Residue.
    }

    // Residue (default + Sidechain fallback)
    for (int i = 0; i < (int)atoms.size(); ++i) {
        const auto& b = atoms[i];
        if (b.chainId == a.chainId && b.resSeq == a.resSeq &&
            b.insCode == a.insCode) {
            out.push_back(i);
        }
    }
    return out;
}

// ── View-fit (issue #98) ────────────────────────────────────────────────
// Best sub-pixel dimensions to fit against when no real canvas is active
// (e.g. a --no-tui script before the first screenshot). The screenshot
// command re-fits to the actual output size, so this only needs to be a
// reasonable default for the live preview.
static void bestFitCanvasDims(const Canvas* c, int& W, int& H, float& ay) {
    if (c && c->subW() > 0 && c->subH() > 0) {
        W = c->subW(); H = c->subH(); ay = c->aspectYX();
    } else {
        W = 1440; H = 1080; ay = 1.0f;   // 4:3, square sub-pixels
    }
}

float Application::computeFitZoom(int W, int H, float aspectYX) const {
    const auto& cam = tabMgr_.currentTab().camera();
    if (!viewFit_.active || viewFit_.xs.empty() || W <= 0 || H <= 0)
        return cam.zoom();

    // Project each subject atom through the current rotation+center and
    // take the half-extents along screen X and Y (world Å).
    const auto& R = cam.rotation();
    const float cx = cam.centerX(), cy = cam.centerY(), cz = cam.centerZ();
    float ex = 0.0f, ey = 0.0f;
    for (size_t i = 0; i < viewFit_.xs.size(); ++i) {
        const float x = viewFit_.xs[i] - cx;
        const float y = viewFit_.ys[i] - cy;
        const float z = viewFit_.zs[i] - cz;
        ex = std::max(ex, std::fabs(R[0]*x + R[1]*y + R[2]*z));
        ey = std::max(ey, std::fabs(R[3]*x + R[4]*y + R[5]*z));
    }
    ex = std::max(ex + viewFit_.pad, viewFit_.minExtent);
    ey = std::max(ey + viewFit_.pad, viewFit_.minExtent);

    // projectCached() maps Å→sub-pixels via Camera::scaleFromZoom on X and
    // scale/aspectYX on Y. Solve for the scale that keeps the projected
    // half-extent within fill*frame/2 in both dimensions, then invert through
    // Camera::zoomFromScale so the two formulas stay locked together.
    const float fW = viewFit_.fill * static_cast<float>(W) * 0.5f;
    const float fH = viewFit_.fill * static_cast<float>(H) * 0.5f * aspectYX;
    const float scale = std::min(fW / ex, fH / ey);
    return std::clamp(Camera::zoomFromScale(scale, W, H), 0.01f, 100.0f);
}

void Application::applyViewFit(int W, int H, float aspectYX) {
    if (!viewFit_.active) return;
    tabMgr_.currentTab().camera().setZoom(computeFitZoom(W, H, aspectYX));
}

void Application::setViewFit(std::vector<float> xs, std::vector<float> ys,
                             std::vector<float> zs, float fill, float pad,
                             float minExtent) {
    viewFit_.active = true;
    viewFit_.xs = std::move(xs);
    viewFit_.ys = std::move(ys);
    viewFit_.zs = std::move(zs);
    viewFit_.fill = fill;
    viewFit_.pad = pad;
    viewFit_.minExtent = minExtent;
    int W, H; float ay;
    bestFitCanvasDims(canvas_.get(), W, H, ay);
    applyViewFit(W, H, ay);
}

void Application::clearViewFit() {
    viewFit_.active = false;
    viewFit_.xs.clear();
    viewFit_.ys.clear();
    viewFit_.zs.clear();
}

void Application::enterFocus(MolObject& mol,
                             const std::vector<int>& subjectIndices,
                             const std::string& exprDesc) {
    if (subjectIndices.empty()) return;
    if (focusSnapshot_.active) exitFocus();   // refocus → exit then re-enter

    const auto& atoms = mol.atoms();

    // Snapshot camera state.
    auto& cam = tabMgr_.currentTab().camera();
    focusSnapshot_.active = true;
    focusSnapshot_.rot    = cam.rotation();
    focusSnapshot_.cx     = cam.centerX();
    focusSnapshot_.cy     = cam.centerY();
    focusSnapshot_.cz     = cam.centerZ();
    focusSnapshot_.panX   = cam.panXOffset();
    focusSnapshot_.panY   = cam.panYOffset();
    focusSnapshot_.zoom   = cam.zoom();

    // Snapshot per-repr visibility for the atom-direct reprs we touch.
    static const ReprType kTouchedReprs[] = {
        ReprType::Wireframe, ReprType::BallStick, ReprType::Spacefill,
    };
    focusSnapshot_.reprs.clear();
    for (ReprType r : kTouchedReprs) {
        FocusSavedRepr s;
        s.type        = r;
        s.objectLevel = mol.reprVisible(r);
        s.atomMask    = mol.atomVisMask(r);    // empty if all-visible
        focusSnapshot_.reprs.push_back(std::move(s));
    }
    // Spline reprs are hidden during focus (they obscure the close-up
    // sidechain/wireframe view); save their object-level state so we can
    // put them back on exit.
    focusSnapshot_.cartoonVisible  = mol.reprVisible(ReprType::Cartoon);
    focusSnapshot_.ribbonVisible   = mol.reprVisible(ReprType::Ribbon);
    focusSnapshot_.backboneVisible = mol.reprVisible(ReprType::Backbone);
    // Snapshot wireframe thickness so we can bump it during focus.
    if (auto* wf = dynamic_cast<WireframeRepr*>(getRepr(ReprType::Wireframe))) {
        focusSnapshot_.wireframeThickness = wf->thickness();
    }

    // Compute subject centroid (for camera snap) + enclosing radius
    // (for subject-size aware zoom — Mol*-style).
    float sx = 0, sy = 0, sz = 0;
    int n = 0;
    std::vector<float> fxs, fys, fzs;
    fxs.reserve(subjectIndices.size());
    fys.reserve(subjectIndices.size());
    fzs.reserve(subjectIndices.size());
    for (int idx : subjectIndices) {
        if (idx < 0 || idx >= (int)atoms.size()) continue;
        sx += atoms[idx].x; sy += atoms[idx].y; sz += atoms[idx].z;
        fxs.push_back(atoms[idx].x);
        fys.push_back(atoms[idx].y);
        fzs.push_back(atoms[idx].z);
        ++n;
    }
    if (n == 0) { focusSnapshot_.active = false; return; }
    sx /= n; sy /= n; sz /= n;

    // Snap the camera to the subject centroid, then fit its *projected*
    // extent to the frame (issue #98). The fit is recomputed per output
    // canvas (live + each :screenshot), so it fills the frame at any
    // resolution/aspect instead of a fixed reference viewport.
    cam.focusOn(sx, sy, sz, cam.zoom());
    setViewFit(fxs, fys, fzs, focusFillFraction_, focusExtraRadius_,
               focusMinRadius_);

    // Build subject mask first.
    focusAtomMask_.assign(atoms.size(), false);
    for (int idx : subjectIndices) {
        if (idx >= 0 && idx < (int)atoms.size()) focusAtomMask_[idx] = true;
    }

    // Spatial neighborhood: every atom within focus_radius of any
    // subject atom. This catches the close-pocket geometry — backbone
    // + sidechains touching the subject — but it's distance-based, so
    // a long sidechain reaching the pocket may have its CA outside.
    const float r2 = focusRadius_ * focusRadius_;
    focusNbhdMask_.assign(atoms.size(), false);
    for (size_t i = 0; i < atoms.size(); ++i) {
        const auto& ai = atoms[i];
        for (int j : subjectIndices) {
            if (j < 0 || j >= (int)atoms.size()) continue;
            const auto& aj = atoms[j];
            float dx = ai.x - aj.x, dy = ai.y - aj.y, dz = ai.z - aj.z;
            if (dx*dx + dy*dy + dz*dz <= r2) { focusNbhdMask_[i] = true; break; }
        }
    }

    // Ensure interface contacts are cached. We need them before building
    // nbhdIndices so the partner-residue expansion below can promote
    // whole interacting residues into the neighborhood.
    if (interfaceContacts_.empty()) {
        contactMapPanel_.update(mol);
        contactMapPanel_.contactMap().computeInterface(mol, 4.5f);
        interfaceContacts_ = contactMapPanel_.contactMap().interfaceContacts();
        focusComputedInterface_ = true;
    }

    // Promote any residue that has a contact reaching into the subject:
    // the user expects to see the whole interacting residue, not the
    // truncated portion that happens to fall inside focus_radius. Each
    // partner is identified by (chainId, resSeq, insCode), then every
    // atom sharing that key is added to the neighborhood mask.
    std::set<std::tuple<std::string, int, char>> partnerResidues;
    for (const auto& c : interfaceContacts_) {
        if (c.atom1 < 0 || c.atom2 < 0) continue;
        if (c.atom1 >= (int)atoms.size() || c.atom2 >= (int)atoms.size()) continue;
        bool s1 = focusAtomMask_[c.atom1];
        bool s2 = focusAtomMask_[c.atom2];
        if (s1 == s2) continue;     // both in subject, or neither — no partner edge
        int partner = s1 ? c.atom2 : c.atom1;
        const auto& a = atoms[partner];
        partnerResidues.emplace(a.chainId, a.resSeq, a.insCode);
    }
    // Fused pass: for each atom, promote it into the mask if its residue
    // matched a partner, then collect all in-mask atoms into nbhdIndices.
    std::vector<int> nbhdIndices;
    nbhdIndices.reserve(atoms.size());
    for (size_t i = 0; i < atoms.size(); ++i) {
        const auto& a = atoms[i];
        if (!focusNbhdMask_[i] && !partnerResidues.empty()
            && partnerResidues.count({a.chainId, a.resSeq, a.insCode})) {
            focusNbhdMask_[i] = true;
        }
        if (focusNbhdMask_[i]) nbhdIndices.push_back((int)i);
    }

    mol.hideRepr(ReprType::Cartoon);
    mol.hideRepr(ReprType::Ribbon);
    mol.hideRepr(ReprType::Backbone);

    // Bump wireframe thickness modestly so the local scaffold reads;
    // the zoom-scaling in WireframeRepr::render does the rest.
    if (auto* wf = dynamic_cast<WireframeRepr*>(getRepr(ReprType::Wireframe))) {
        wf->setThickness(std::max(0.14f, focusSnapshot_.wireframeThickness * 1.4f));
    }
    mol.showRepr(ReprType::Wireframe);
    mol.showReprForAtoms(ReprType::Wireframe, nbhdIndices);

    mol.showRepr(ReprType::BallStick);
    mol.showReprForAtoms(ReprType::BallStick, nbhdIndices);

    if (mol.reprVisible(ReprType::Spacefill)) {
        mol.showReprForAtoms(ReprType::Spacefill, nbhdIndices);
    }

    // Filter contacts to ones whose endpoints are both in the (now
    // residue-expanded) neighborhood, so the dashed lines render only
    // for what's visible in the pocket.
    std::vector<InterfaceContact> filtered;
    filtered.reserve(interfaceContacts_.size());
    for (const auto& c : interfaceContacts_) {
        if (c.atom1 < 0 || c.atom2 < 0) continue;
        if (c.atom1 >= (int)atoms.size() || c.atom2 >= (int)atoms.size()) continue;
        if (focusNbhdMask_[c.atom1] && focusNbhdMask_[c.atom2])
            filtered.push_back(c);
    }
    interfaceRepr_.setData(focusNbhdMask_, std::move(filtered));
    interfaceRepr_.setDrawSidechains(false);   // wireframe already covers it
    interfaceRepr_.setInteractionThickness(interfaceThickness_);
    interfaceRepr_.setShowMask(interfaceShowMask_);

    focusExpr_ = exprDesc.empty() ? std::string("focus") : exprDesc;
    char msg[160];
    std::snprintf(msg, sizeof(msg),
        "Focus: %d atoms (radius=%.1fA zoom=%.2f) — F or Esc to exit",
        (int)nbhdIndices.size(), focusRadius_,
        tabMgr_.currentTab().camera().zoom());
    cmdLine_.setMessage(msg);
    needsRedraw_ = true;
}

void Application::exitFocus() {
    if (!focusSnapshot_.active) return;
    clearViewFit();   // restoring the saved zoom — drop the fit intent
    auto obj = tabMgr_.currentTab().currentObject();
    if (!obj) {
        focusSnapshot_.active = false;
        focusAtomMask_.clear();
        focusNbhdMask_.clear();
        focusExpr_.clear();
        return;
    }

    // Restore camera.
    auto& cam = tabMgr_.currentTab().camera();
    cam.setRotation(focusSnapshot_.rot);
    cam.setCenter(focusSnapshot_.cx, focusSnapshot_.cy, focusSnapshot_.cz);
    cam.setPan(focusSnapshot_.panX, focusSnapshot_.panY);
    cam.setZoom(focusSnapshot_.zoom);

    // Restore per-repr visibility atom-for-atom.
    for (const auto& s : focusSnapshot_.reprs) {
        if (s.atomMask.empty()) {
            // Pre-focus state was "all visible" → clear any per-atom mask.
            // showRepr with no per-atom args resets the mask in current API.
            if (s.objectLevel) obj->showRepr(s.type);
            else               obj->hideRepr(s.type);
        } else {
            // Pre-focus state had a custom mask — re-apply it via
            // showReprForAtoms with the previously-visible indices.
            std::vector<int> idxs;
            idxs.reserve(s.atomMask.size());
            for (size_t i = 0; i < s.atomMask.size(); ++i)
                if (s.atomMask[i]) idxs.push_back((int)i);
            obj->showReprForAtoms(s.type, idxs);
            if (!s.objectLevel) obj->hideRepr(s.type);
        }
    }

    // Restore spline reprs.
    if (focusSnapshot_.cartoonVisible)  obj->showRepr(ReprType::Cartoon);
    else                                obj->hideRepr(ReprType::Cartoon);
    if (focusSnapshot_.ribbonVisible)   obj->showRepr(ReprType::Ribbon);
    else                                obj->hideRepr(ReprType::Ribbon);
    if (focusSnapshot_.backboneVisible) obj->showRepr(ReprType::Backbone);
    else                                obj->hideRepr(ReprType::Backbone);

    // Restore wireframe thickness.
    if (auto* wf = dynamic_cast<WireframeRepr*>(getRepr(ReprType::Wireframe))) {
        wf->setThickness(focusSnapshot_.wireframeThickness);
    }

    // Put the unfiltered interface contact list back if the global
    // overlay was on. If focus computed interactions on demand (no
    // pre-existing :interface), drop them entirely on exit.
    if (focusComputedInterface_) {
        interfaceContacts_.clear();
        interfaceAtomMask_.clear();
        interfaceRepr_.clear();
        focusComputedInterface_ = false;
    } else if (interfaceOverlay_ && !interfaceContacts_.empty()) {
        interfaceRepr_.setData(interfaceAtomMask_, interfaceContacts_);
        interfaceRepr_.setDrawSidechains(interfaceSidechains_);
        interfaceRepr_.setShowMask(interfaceShowMask_);
    } else {
        interfaceRepr_.clear();
    }

    focusSnapshot_.active = false;
    focusSnapshot_.reprs.clear();
    focusAtomMask_.clear();
    focusNbhdMask_.clear();
    focusExpr_.clear();
    cmdLine_.setMessage("Focus exited");
    needsRedraw_ = true;
}

} // namespace molterm
