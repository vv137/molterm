#include "molterm/repr/BackboneRepr.h"
#include <cmath>

namespace molterm {

void BackboneRepr::render(const MolObject& mol, const Camera& cam,
                          Canvas& canvas) {
    if (!mol.visible() || !mol.reprVisible(ReprType::Backbone)) return;

    auto ctx = makeContext(mol, ReprType::Backbone);
    const auto& atoms = ctx.atoms;
    int r = toSubPixels(thickness_, canvas.scaleX());

    // Collect Cα atoms grouped by chain
    int sw = canvas.subW(), sh = canvas.subH();
    int margin = r * 3;

    struct CaAtom {
        size_t idx;
        float sx, sy, depth;
        bool visible;
        std::string chainId;
    };
    std::vector<CaAtom> cas;
    for (size_t i = 0; i < atoms.size(); ++i) {
        const auto& a = atoms[i];
        if (a.name != "CA" && a.name != "P") continue;
        if (!ctx.visible(static_cast<int>(i))) continue;

        CaAtom ca;
        ca.idx = i;
        ca.chainId = a.chainId;
        cam.projectCached(a.x, a.y, a.z, ca.sx, ca.sy, ca.depth);
        ca.visible = (ca.sx >= -margin && ca.sx < sw + margin &&
                      ca.sy >= -margin && ca.sy < sh + margin);
        cas.push_back(ca);
    }

    for (size_t i = 1; i < cas.size(); ++i) {
        if (cas[i].chainId != cas[i-1].chainId) continue;
        if (!cas[i].visible && !cas[i-1].visible) continue;

        int x0 = static_cast<int>(std::round(cas[i-1].sx));
        int y0 = static_cast<int>(std::round(cas[i-1].sy));
        int x1 = static_cast<int>(std::round(cas[i].sx));
        int y1 = static_cast<int>(std::round(cas[i].sy));
        float d0 = cas[i-1].depth, d1 = cas[i].depth;

        int color = ctx.colorFor(static_cast<int>(cas[i-1].idx));

        if (r <= 1) {
            canvas.drawLine(x0, y0, d0, x1, y1, d1, color);
        } else {
            // Thick line: stamp filled circles along Bresenham path
            Canvas::bresenham(x0, y0, d0, x1, y1, d1,
                [&](int x, int y, float depth) {
                    canvas.drawCircle(x, y, depth, r, color, true);
                });
        }
    }

    for (const auto& ca : cas) {
        if (!ca.visible) continue;
        int sx = static_cast<int>(std::round(ca.sx));
        int sy = static_cast<int>(std::round(ca.sy));
        int color = ctx.colorFor(static_cast<int>(ca.idx));
        canvas.drawCircle(sx, sy, ca.depth, r, color, true);
    }
}

} // namespace molterm
