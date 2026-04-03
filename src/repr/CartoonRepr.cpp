#include "molterm/repr/CartoonRepr.h"
#include <cmath>
#include <vector>

namespace molterm {

void CartoonRepr::render(const MolObject& mol, const Camera& cam,
                         Canvas& canvas) {
    if (!mol.visible() || !mol.reprVisible(ReprType::Cartoon)) return;

    int cw = canvas.subW(), ch = canvas.subH();
    float aspect = canvas.aspectYX();
    const auto& atoms = mol.atoms();
    auto scheme = mol.colorScheme();
    const std::vector<float>* rbw = (scheme == ColorScheme::Rainbow) ? &mol.rainbowFractions() : nullptr;
    auto rf = [&](int i) { return rbw ? (*rbw)[i] : -1.0f; };

    int scaleX = canvas.scaleX();

    // Collect Cα atoms with their SS type
    struct CaInfo {
        size_t atomIdx;
        int sx, sy;
        float depth;
        SSType ss;
        std::string chainId;
    };
    std::vector<CaInfo> cas;

    for (size_t i = 0; i < atoms.size(); ++i) {
        if (atoms[i].name != "CA") continue;

        CaInfo ca;
        ca.atomIdx = i;
        ca.chainId = atoms[i].chainId;
        ca.ss = atoms[i].ssType;

        float fsx, fsy;
        if (!cam.projectf(atoms[i].x, atoms[i].y, atoms[i].z,
                          cw, ch, fsx, fsy, ca.depth, aspect))
            continue;

        ca.sx = static_cast<int>(std::round(fsx));
        ca.sy = static_cast<int>(std::round(fsy));
        cas.push_back(ca);
    }

    // Draw segments between consecutive Cα in same chain
    for (size_t i = 1; i < cas.size(); ++i) {
        if (cas[i].chainId != cas[i-1].chainId) continue;

        // Use the SS type of the source residue to determine width
        SSType ss = cas[i-1].ss;
        float width;
        switch (ss) {
            case SSType::Helix: width = helixWidth_; break;
            case SSType::Sheet: width = sheetWidth_; break;
            case SSType::Loop:
            default:            width = loopWidth_;  break;
        }

        int r = static_cast<int>(width * static_cast<float>(scaleX) + 0.5f);
        if (r < 1) r = 1;

        int color = ColorMapper::colorForAtom(atoms[cas[i-1].atomIdx], scheme,
                                                mol.atomColor(static_cast<int>(cas[i-1].atomIdx)),
                                                rf(static_cast<int>(cas[i-1].atomIdx)));

        int x0 = cas[i-1].sx, y0 = cas[i-1].sy;
        int x1 = cas[i].sx, y1 = cas[i].sy;
        float d0 = cas[i-1].depth, d1 = cas[i].depth;

        if (r <= 1) {
            canvas.drawLine(x0, y0, d0, x1, y1, d1, color);
        } else {
            // Thick line via circle stamping
            Canvas::bresenham(x0, y0, d0, x1, y1, d1,
                [&](int x, int y, float depth) {
                    canvas.drawCircle(x, y, depth, r, color, true);
                });
        }
    }

    // Draw Cα markers on top — size matches their SS type
    for (const auto& ca : cas) {
        float width;
        switch (ca.ss) {
            case SSType::Helix: width = helixWidth_; break;
            case SSType::Sheet: width = sheetWidth_; break;
            case SSType::Loop:
            default:            width = loopWidth_;  break;
        }
        int r = static_cast<int>(width * static_cast<float>(scaleX) + 0.5f);
        if (r < 1) r = 1;

        int color = ColorMapper::colorForAtom(atoms[ca.atomIdx], scheme,
                                                mol.atomColor(static_cast<int>(ca.atomIdx)),
                                                rf(static_cast<int>(ca.atomIdx)));
        canvas.drawCircle(ca.sx, ca.sy, ca.depth, r, color, true);
    }
}

} // namespace molterm
