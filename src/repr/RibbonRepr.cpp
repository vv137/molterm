#include "molterm/repr/RibbonRepr.h"
#include "molterm/render/ColorMapper.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace molterm {

float RibbonRepr::catmullRom(float p0, float p1, float p2, float p3, float t) {
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * p1) +
                   (-p0 + p2) * t +
                   (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

// Normalize a 3D vector in place; returns length
static float normalize3(float& x, float& y, float& z) {
    float len = std::sqrt(x*x + y*y + z*z);
    if (len > 1e-8f) { x /= len; y /= len; z /= len; }
    return len;
}

// Cross product: out = a × b
static void cross3(float ax, float ay, float az,
                   float bx, float by, float bz,
                   float& ox, float& oy, float& oz) {
    ox = ay*bz - az*by;
    oy = az*bx - ax*bz;
    oz = ax*by - ay*bx;
}

void RibbonRepr::render(const MolObject& mol, const Camera& cam,
                         Canvas& canvas) {
    if (!mol.visible() || !mol.reprVisible(ReprType::Ribbon)) return;

    const auto& atoms = mol.atoms();
    auto scheme = mol.colorScheme();
    const std::vector<float>* rbw = (scheme == ColorScheme::Rainbow) ? &mol.rainbowFractions() : nullptr;
    auto rf = [&](int i) -> float { return rbw ? (*rbw)[i] : -1.0f; };

    // LOD: reduce subdivisions for large structures
    int subdiv = subdivisions_;
    if (atoms.size() > 20000) subdiv = std::max(2, subdiv / 4);
    else if (atoms.size() > 5000) subdiv = std::max(3, subdiv / 2);

    // Collect Cα atoms with C and O atoms for guide vectors
    struct CaAtom {
        int atomIdx;
        float wx, wy, wz;      // Cα position
        float ox, oy, oz;       // carbonyl O position (for ribbon guide)
        bool hasO;
        SSType ss;
        std::string chainId;
    };
    std::vector<CaAtom> allCas;

    auto atomVis = mol.atomVisMask(ReprType::Ribbon);

    for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
        // Track both protein Cα and nucleic acid P atoms
        if (atoms[i].name != "CA" && atoms[i].name != "P") continue;
        if (!atomVis.empty() && !atomVis[i]) continue;

        CaAtom ca;
        ca.atomIdx = i;
        ca.wx = atoms[i].x; ca.wy = atoms[i].y; ca.wz = atoms[i].z;
        ca.ss = atoms[i].ssType;
        ca.chainId = atoms[i].chainId;
        ca.hasO = false;

        // Find the carbonyl O in the same residue (for ribbon orientation)
        for (int j = i + 1; j < static_cast<int>(atoms.size()) && j < i + 10; ++j) {
            if (atoms[j].resSeq != atoms[i].resSeq || atoms[j].chainId != atoms[i].chainId) break;
            if (atoms[j].name == "O") {
                ca.ox = atoms[j].x; ca.oy = atoms[j].y; ca.oz = atoms[j].z;
                ca.hasO = true;
                break;
            }
        }
        allCas.push_back(ca);
    }

    // Process each chain
    size_t chainStart = 0;
    while (chainStart < allCas.size()) {
        size_t chainEnd = chainStart + 1;
        while (chainEnd < allCas.size() && allCas[chainEnd].chainId == allCas[chainStart].chainId)
            ++chainEnd;

        int chainLen = static_cast<int>(chainEnd - chainStart);
        if (chainLen < 2) { chainStart = chainEnd; continue; }

        // Build spline points with ribbon edge positions in world space
        struct RibbonPoint {
            float cx, cy, cz;       // center position (world)
            float lx, ly, lz;       // left edge (world)
            float rx, ry, rz;       // right edge (world)
            float width;             // half-width in Å
            int color;
            SSType ss;
        };
        std::vector<RibbonPoint> ribbon;
        ribbon.reserve(chainLen * subdiv + 1);

        for (int seg = 0; seg < chainLen - 1; ++seg) {
            int i0 = static_cast<int>(chainStart) + std::max(0, seg - 1);
            int i1 = static_cast<int>(chainStart) + seg;
            int i2 = static_cast<int>(chainStart) + std::min(seg + 1, chainLen - 1);
            int i3 = static_cast<int>(chainStart) + std::min(seg + 2, chainLen - 1);

            const auto& ca0 = allCas[i0]; const auto& ca1 = allCas[i1];
            const auto& ca2 = allCas[i2]; const auto& ca3 = allCas[i3];

            int color1 = ColorMapper::colorForAtom(atoms[ca1.atomIdx], scheme,
                mol.atomColor(ca1.atomIdx), rf(ca1.atomIdx));
            int color2 = ColorMapper::colorForAtom(atoms[ca2.atomIdx], scheme,
                mol.atomColor(ca2.atomIdx), rf(ca2.atomIdx));

            float w1, w2;
            auto ssWidth = [&](SSType ss) -> float {
                switch (ss) {
                    case SSType::Helix: return helixWidth_ * 0.15f;  // Å
                    case SSType::Sheet: return sheetWidth_ * 0.15f;
                    default:            return loopWidth_ * 0.08f;
                }
            };
            w1 = ssWidth(ca1.ss);
            w2 = ssWidth(ca2.ss);

            // Sheet arrowhead: widen at end of sheet strand, taper to point
            bool isSheetEnd = (ca1.ss == SSType::Sheet && ca2.ss != SSType::Sheet);

            for (int s = 0; s < subdiv; ++s) {
                float t = static_cast<float>(s) / static_cast<float>(subdiv);

                float cx = catmullRom(ca0.wx, ca1.wx, ca2.wx, ca3.wx, t);
                float cy = catmullRom(ca0.wy, ca1.wy, ca2.wy, ca3.wy, t);
                float cz = catmullRom(ca0.wz, ca1.wz, ca2.wz, ca3.wz, t);

                // Tangent (derivative of Catmull-Rom)
                float dt = 0.01f;
                float tx = catmullRom(ca0.wx, ca1.wx, ca2.wx, ca3.wx, t + dt) - cx;
                float ty = catmullRom(ca0.wy, ca1.wy, ca2.wy, ca3.wy, t + dt) - cy;
                float tz = catmullRom(ca0.wz, ca1.wz, ca2.wz, ca3.wz, t + dt) - cz;
                normalize3(tx, ty, tz);

                // Guide vector: Cα→O direction (interpolated)
                float gx, gy, gz;
                const auto& src = (t < 0.5f) ? ca1 : ca2;
                if (src.hasO) {
                    gx = src.ox - src.wx; gy = src.oy - src.wy; gz = src.oz - src.wz;
                } else {
                    // Fallback: arbitrary perpendicular
                    gx = -ty; gy = tx; gz = 0;
                }

                // Binormal: cross(tangent, guide) → ribbon normal direction
                float bx, by, bz;
                cross3(tx, ty, tz, gx, gy, gz, bx, by, bz);
                normalize3(bx, by, bz);

                // Ribbon direction: cross(binormal, tangent)
                float nx, ny, nz;
                cross3(bx, by, bz, tx, ty, tz, nx, ny, nz);
                normalize3(nx, ny, nz);

                float w = w1 * (1.0f - t) + w2 * t;

                // Sheet arrowhead: at end of strand, widen then taper
                if (isSheetEnd) {
                    float arrow = (t < 0.5f) ? (1.0f + t * 1.0f) : (2.0f * (1.0f - t));
                    w = w1 * arrow;
                }

                RibbonPoint rp;
                rp.cx = cx; rp.cy = cy; rp.cz = cz;
                rp.lx = cx + nx * w; rp.ly = cy + ny * w; rp.lz = cz + nz * w;
                rp.rx = cx - nx * w; rp.ry = cy - ny * w; rp.rz = cz - nz * w;
                rp.width = w;
                rp.color = (t < 0.5f) ? color1 : color2;
                rp.ss = (t < 0.5f) ? ca1.ss : ca2.ss;
                ribbon.push_back(rp);
            }
        }

        // Add last point
        {
            const auto& last = allCas[chainEnd - 1];
            float w = (last.ss == SSType::Helix) ? helixWidth_ * 0.15f :
                      (last.ss == SSType::Sheet) ? sheetWidth_ * 0.15f : loopWidth_ * 0.08f;
            int color = ColorMapper::colorForAtom(atoms[last.atomIdx], scheme,
                mol.atomColor(last.atomIdx), rf(last.atomIdx));
            // Use previous point's direction for edge offset
            if (!ribbon.empty()) {
                auto& prev = ribbon.back();
                float nx = (prev.lx - prev.cx) / std::max(0.01f, prev.width);
                float ny = (prev.ly - prev.cy) / std::max(0.01f, prev.width);
                float nz = (prev.lz - prev.cz) / std::max(0.01f, prev.width);
                ribbon.push_back({last.wx, last.wy, last.wz,
                                  last.wx + nx * w, last.wy + ny * w, last.wz + nz * w,
                                  last.wx - nx * w, last.wy - ny * w, last.wz - nz * w,
                                  w, color, last.ss});
            }
        }

        // Render: project ribbon edges and fill between them
        for (size_t j = 1; j < ribbon.size(); ++j) {
            const auto& p0 = ribbon[j - 1];
            const auto& p1 = ribbon[j];

            // Project all 4 edge points
            float l0sx, l0sy, l0d, r0sx, r0sy, r0d;
            float l1sx, l1sy, l1d, r1sx, r1sy, r1d;
            float c0sx, c0sy, c0d, c1sx, c1sy, c1d;

            cam.projectCached(p0.lx, p0.ly, p0.lz, l0sx, l0sy, l0d);
            cam.projectCached(p0.rx, p0.ry, p0.rz, r0sx, r0sy, r0d);
            cam.projectCached(p1.lx, p1.ly, p1.lz, l1sx, l1sy, l1d);
            cam.projectCached(p1.rx, p1.ry, p1.rz, r1sx, r1sy, r1d);
            cam.projectCached(p0.cx, p0.cy, p0.cz, c0sx, c0sy, c0d);
            cam.projectCached(p1.cx, p1.cy, p1.cz, c1sx, c1sy, c1d);

            // Fill the quad (two triangles) by scanline between left and right edges
            // Simple approach: for each y-row, draw horizontal lines between the edges
            int color = p0.color;

            // Draw filled quad: left edge line + right edge line + fill
            // Using 3 lines: left edge, center, right edge
            canvas.drawLine(static_cast<int>(l0sx), static_cast<int>(l0sy), l0d,
                            static_cast<int>(l1sx), static_cast<int>(l1sy), l1d, color);
            canvas.drawLine(static_cast<int>(c0sx), static_cast<int>(c0sy), c0d,
                            static_cast<int>(c1sx), static_cast<int>(c1sy), c1d, color);
            canvas.drawLine(static_cast<int>(r0sx), static_cast<int>(r0sy), r0d,
                            static_cast<int>(r1sx), static_cast<int>(r1sy), r1d, color);

            // Fill cross-lines between edges for solid appearance
            int nFill = std::max(4, static_cast<int>(
                std::abs(l0sx - r0sx) + std::abs(l0sy - r0sy)));
            for (int f = 0; f <= nFill; ++f) {
                float ft = static_cast<float>(f) / static_cast<float>(std::max(1, nFill));
                float fx0 = l0sx + (r0sx - l0sx) * ft;
                float fy0 = l0sy + (r0sy - l0sy) * ft;
                float fd0 = l0d + (r0d - l0d) * ft;
                float fx1 = l1sx + (r1sx - l1sx) * ft;
                float fy1 = l1sy + (r1sy - l1sy) * ft;
                float fd1 = l1d + (r1d - l1d) * ft;
                canvas.drawLine(static_cast<int>(fx0), static_cast<int>(fy0), fd0,
                                static_cast<int>(fx1), static_cast<int>(fy1), fd1, color);
            }
        }

        chainStart = chainEnd;
    }
}

} // namespace molterm
