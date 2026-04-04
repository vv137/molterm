#include "molterm/repr/SpacefillRepr.h"
#include <cmath>

namespace molterm {

float SpacefillRepr::vdwRadius(const std::string& element) {
    // Van der Waals radii in Å (Bondi)
    if (element == "H")  return 1.20f;
    if (element == "C")  return 1.70f;
    if (element == "N")  return 1.55f;
    if (element == "O")  return 1.52f;
    if (element == "S")  return 1.80f;
    if (element == "P")  return 1.80f;
    if (element == "F")  return 1.47f;
    if (element == "Cl") return 1.75f;
    if (element == "Br") return 1.85f;
    if (element == "I")  return 1.98f;
    if (element == "Se") return 1.90f;
    if (element == "Fe") return 2.00f;
    if (element == "Zn") return 1.39f;
    return 1.70f;  // default
}

void SpacefillRepr::render(const MolObject& mol, const Camera& cam,
                           Canvas& canvas) {
    if (!mol.visible() || !mol.reprVisible(ReprType::Spacefill)) return;

    int cw = canvas.subW(), ch = canvas.subH();
    const auto& atoms = mol.atoms();
    auto scheme = mol.colorScheme();
    const std::vector<float>* rbw = (scheme == ColorScheme::Rainbow) ? &mol.rainbowFractions() : nullptr;
    auto rf = [&](int i) { return rbw ? (*rbw)[i] : -1.0f; };

    // Project and sort by depth (back to front for proper occlusion with filled circles)
    struct ProjAtom { int idx; int sx, sy; float depth; int radius; int color; };
    std::vector<ProjAtom> projected;
    projected.reserve(atoms.size());


    for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
        const auto& a = atoms[i];
        float fsx, fsy, depth;
        cam.projectCached(a.x, a.y, a.z, fsx, fsy, depth);
        if (fsx < -100 || fsx > cw + 100 || fsy < -100 || fsy > ch + 100)
            continue;

        // Radius in sub-pixels: VDW radius * scale * camera zoom * canvas scale
        float vdw = vdwRadius(a.element);
        int r = static_cast<int>(vdw * scale_ * static_cast<float>(canvas.scaleX()) * cam.zoom() + 0.5f);
        if (r < 1) r = 1;

        int color = ColorMapper::colorForAtom(a, scheme, mol.atomColor(i), rf(i));
        projected.push_back({i,
            static_cast<int>(std::round(fsx)),
            static_cast<int>(std::round(fsy)),
            depth, r, color});
    }

    // Sort back-to-front (largest depth first = farthest first)
    std::sort(projected.begin(), projected.end(),
        [](const ProjAtom& a, const ProjAtom& b) { return a.depth > b.depth; });

    // Draw back to front
    for (const auto& pa : projected) {
        canvas.drawCircle(pa.sx, pa.sy, pa.depth, pa.radius, pa.color, true);
    }
}

} // namespace molterm
