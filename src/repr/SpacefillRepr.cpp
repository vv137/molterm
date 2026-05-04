#include "molterm/repr/SpacefillRepr.h"
#include "molterm/core/VdwTable.h"
#include <algorithm>
#include <cmath>

namespace molterm {

float SpacefillRepr::vdwRadius(const std::string& element) {
    return molterm::vdwRadius(element);
}

void SpacefillRepr::render(const MolObject& mol, const Camera& cam,
                           Canvas& canvas) {
    if (!mol.visible() || !mol.reprVisible(ReprType::Spacefill)) return;

    auto ctx = makeContext(mol, ReprType::Spacefill);
    const auto& atoms = ctx.atoms;
    int cw = canvas.subW(), ch = canvas.subH();

    struct ProjAtom { int idx; int sx, sy; float depth; int radius; int color; };
    std::vector<ProjAtom> projected;
    projected.reserve(atoms.size());

    for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
        if (!ctx.visible(i)) continue;
        const auto& a = atoms[i];
        float fsx, fsy, depth;
        cam.projectCached(a.x, a.y, a.z, fsx, fsy, depth);

        float vdw = vdwRadius(a.element);
        int r = static_cast<int>(vdw * scale_ * static_cast<float>(canvas.scaleX()) * cam.zoom() + 0.5f);
        if (!inFrustum(fsx, fsy, cw, ch, r)) continue;
        if (r < 1) r = 1;

        int color = ctx.colorFor(i);
        projected.push_back({i,
            static_cast<int>(std::round(fsx)),
            static_cast<int>(std::round(fsy)),
            depth, r, color});
    }

    // Sort back-to-front only when camera changed
    if (cam.isDirty() || sortDirty_ || sortedIndices_.size() != projected.size()) {
        sortedIndices_.resize(projected.size());
        for (int i = 0; i < static_cast<int>(projected.size()); ++i) sortedIndices_[i] = i;
        std::sort(sortedIndices_.begin(), sortedIndices_.end(),
            [&](int a, int b) { return projected[a].depth > projected[b].depth; });
        sortDirty_ = false;
    }

    for (int si : sortedIndices_) {
        if (si >= static_cast<int>(projected.size())) continue;
        const auto& pa = projected[si];
        canvas.setActiveAtomIndex(pa.idx);
        canvas.drawCircle(pa.sx, pa.sy, pa.depth, pa.radius, pa.color, true);
    }
    canvas.setActiveAtomIndex(-1);
}

} // namespace molterm
