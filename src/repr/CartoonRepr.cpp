#include "molterm/repr/CartoonRepr.h"
#include "molterm/render/ColorMapper.h"
#include "molterm/render/PixelCanvas.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace molterm {

float CartoonRepr::catmullRom(float p0, float p1, float p2, float p3, float t) {
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * p1) +
                   (-p0 + p2) * t +
                   (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

static float len3(float x, float y, float z) { return std::sqrt(x*x + y*y + z*z); }
static void normalize3(float& x, float& y, float& z) {
    float l = len3(x, y, z);
    if (l > 1e-8f) { x /= l; y /= l; z /= l; }
}
static void cross3(float ax, float ay, float az, float bx, float by, float bz,
                   float& ox, float& oy, float& oz) {
    ox = ay*bz - az*by; oy = az*bx - ax*bz; oz = ax*by - ay*bx;
}

void CartoonRepr::render(const MolObject& mol, const Camera& cam,
                         Canvas& canvas) {
    if (!mol.visible() || !mol.reprVisible(ReprType::Cartoon)) return;

    int cw = canvas.subW(), ch = canvas.subH();
    float aspect = canvas.aspectYX();
    const auto& atoms = mol.atoms();
    auto scheme = mol.colorScheme();
    const std::vector<float>* rbw = (scheme == ColorScheme::Rainbow) ? &mol.rainbowFractions() : nullptr;
    auto rf = [&](int i) -> float { return rbw ? (*rbw)[i] : -1.0f; };

    // Check if we have a PixelCanvas for triangle rasterization
    auto* pixCanvas = dynamic_cast<PixelCanvas*>(&canvas);

    int subdiv = subdivisions_;
    if (atoms.size() > 20000) subdiv = std::max(2, subdiv / 4);
    else if (atoms.size() > 5000) subdiv = std::max(3, subdiv / 2);

    int coilSegments = 8;
    if (atoms.size() > 5000) coilSegments = 4;

    // Collect Cα/P atoms
    struct CaAtom { int idx; float x, y, z; SSType ss; std::string chain; };
    std::vector<CaAtom> cas;
    for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
        if (atoms[i].name != "CA" && atoms[i].name != "P") continue;
        cas.push_back({i, atoms[i].x, atoms[i].y, atoms[i].z,
                       atoms[i].ssType, atoms[i].chainId});
    }

    cam.prepareProjection(cw, ch, aspect);

    // Process each chain
    size_t cStart = 0;
    while (cStart < cas.size()) {
        size_t cEnd = cStart + 1;
        while (cEnd < cas.size() && cas[cEnd].chain == cas[cStart].chain) ++cEnd;
        int cLen = static_cast<int>(cEnd - cStart);
        if (cLen < 2) { cStart = cEnd; continue; }

        // Step 1: Generate spline points
        struct SplinePoint { float x, y, z; SSType ss; int color; float arrowFrac; };
        std::vector<SplinePoint> spine;
        spine.reserve(cLen * subdiv + 1);

        for (int seg = 0; seg < cLen - 1; ++seg) {
            int i0 = static_cast<int>(cStart) + std::max(0, seg - 1);
            int i1 = static_cast<int>(cStart) + seg;
            int i2 = static_cast<int>(cStart) + std::min(seg + 1, cLen - 1);
            int i3 = static_cast<int>(cStart) + std::min(seg + 2, cLen - 1);

            int col1 = ColorMapper::colorForAtom(atoms[cas[i1].idx], scheme,
                mol.atomColor(cas[i1].idx), rf(cas[i1].idx));
            int col2 = ColorMapper::colorForAtom(atoms[cas[i2].idx], scheme,
                mol.atomColor(cas[i2].idx), rf(cas[i2].idx));

            // Detect sheet→non-sheet transition for arrowhead
            bool isSheetEnd = (cas[i1].ss == SSType::Sheet && cas[i2].ss != SSType::Sheet);

            for (int s = 0; s < subdiv; ++s) {
                float t = static_cast<float>(s) / static_cast<float>(subdiv);
                float af = isSheetEnd ? t : -1.0f;  // arrowFrac: 0→1 along arrowhead segment
                spine.push_back({
                    catmullRom(cas[i0].x, cas[i1].x, cas[i2].x, cas[i3].x, t),
                    catmullRom(cas[i0].y, cas[i1].y, cas[i2].y, cas[i3].y, t),
                    catmullRom(cas[i0].z, cas[i1].z, cas[i2].z, cas[i3].z, t),
                    (t < 0.5f) ? cas[i1].ss : cas[i2].ss,
                    (t < 0.5f) ? col1 : col2,
                    af
                });
            }
        }
        // Last point
        auto& last = cas[cEnd - 1];
        spine.push_back({last.x, last.y, last.z, last.ss,
            ColorMapper::colorForAtom(atoms[last.idx], scheme,
                mol.atomColor(last.idx), rf(last.idx)), -1.0f});

        int nPts = static_cast<int>(spine.size());
        if (nPts < 2) { cStart = cEnd; continue; }

        // Step 2: Compute tangents
        std::vector<float> tx(nPts), ty(nPts), tz(nPts);
        for (int i = 0; i < nPts; ++i) {
            int prev = std::max(0, i - 1), next = std::min(nPts - 1, i + 1);
            tx[i] = spine[next].x - spine[prev].x;
            ty[i] = spine[next].y - spine[prev].y;
            tz[i] = spine[next].z - spine[prev].z;
            normalize3(tx[i], ty[i], tz[i]);
        }

        // Step 3: Parallel-transport frame
        std::vector<float> nx(nPts), ny(nPts), nz(nPts);
        std::vector<float> bx(nPts), by(nPts), bz(nPts);

        // Initial normal: perpendicular to first tangent
        if (std::abs(tx[0]) < 0.9f) {
            cross3(tx[0], ty[0], tz[0], 1, 0, 0, nx[0], ny[0], nz[0]);
        } else {
            cross3(tx[0], ty[0], tz[0], 0, 1, 0, nx[0], ny[0], nz[0]);
        }
        normalize3(nx[0], ny[0], nz[0]);
        cross3(tx[0], ty[0], tz[0], nx[0], ny[0], nz[0], bx[0], by[0], bz[0]);
        normalize3(bx[0], by[0], bz[0]);

        // Propagate frame
        for (int i = 1; i < nPts; ++i) {
            // Project previous normal onto plane perpendicular to current tangent
            float dot = nx[i-1]*tx[i] + ny[i-1]*ty[i] + nz[i-1]*tz[i];
            nx[i] = nx[i-1] - dot * tx[i];
            ny[i] = ny[i-1] - dot * ty[i];
            nz[i] = nz[i-1] - dot * tz[i];
            float l = len3(nx[i], ny[i], nz[i]);
            if (l < 1e-6f) { nx[i] = nx[i-1]; ny[i] = ny[i-1]; nz[i] = nz[i-1]; }
            else { nx[i] /= l; ny[i] /= l; nz[i] /= l; }
            cross3(tx[i], ty[i], tz[i], nx[i], ny[i], nz[i], bx[i], by[i], bz[i]);
            normalize3(bx[i], by[i], bz[i]);
        }

        // Step 4: Generate cross-section rings and emit triangles
        // For PixelCanvas: project vertices and rasterize triangles
        // For text canvases: fallback to thick lines (circle stamp)

        auto ssHalfW = [&](SSType ss) -> float {
            switch (ss) {
                case SSType::Helix: return helixRadius_;
                case SSType::Sheet: return sheetRadius_;
                default:            return loopRadius_;
            }
        };
        auto ssHalfH = [&](SSType ss) -> float {
            switch (ss) {
                case SSType::Helix: return 0.3f;
                case SSType::Sheet: return 0.15f;
                default:            return loopRadius_;  // circular for coil
            }
        };

        if (pixCanvas) {
            // Triangle-based rendering for PixelCanvas
            // Generate 4-vertex rings for helix/sheet, N-vertex for coil
            struct Vert { float x, y, z; };
            auto makeRing = [&](int i) -> std::vector<Vert> {
                float hw = ssHalfW(spine[i].ss);
                float hh = ssHalfH(spine[i].ss);
                bool isCoil = (spine[i].ss == SSType::Loop);

                // Sheet arrowhead: widen then taper to point
                if (spine[i].arrowFrac >= 0.0f) {
                    float af = spine[i].arrowFrac;
                    if (af < 0.5f) {
                        hw *= (1.0f + af * 1.2f);  // widen to 1.6×
                    } else {
                        hw *= (2.0f * (1.0f - af) * 1.6f);  // taper to 0
                        if (hw < 0.05f) hw = 0.05f;
                    }
                }

                std::vector<Vert> ring;
                if (isCoil) {
                    for (int s = 0; s < coilSegments; ++s) {
                        float angle = 2.0f * 3.14159265f * s / coilSegments;
                        float c = std::cos(angle), si = std::sin(angle);
                        ring.push_back({
                            spine[i].x + hw * (c * nx[i] + si * bx[i]),
                            spine[i].y + hw * (c * ny[i] + si * by[i]),
                            spine[i].z + hw * (c * nz[i] + si * bz[i])
                        });
                    }
                } else {
                    // 4-point rectangular cross-section
                    ring.push_back({spine[i].x + hw*bx[i] + hh*nx[i],
                                    spine[i].y + hw*by[i] + hh*ny[i],
                                    spine[i].z + hw*bz[i] + hh*nz[i]});
                    ring.push_back({spine[i].x - hw*bx[i] + hh*nx[i],
                                    spine[i].y - hw*by[i] + hh*ny[i],
                                    spine[i].z - hw*bz[i] + hh*nz[i]});
                    ring.push_back({spine[i].x - hw*bx[i] - hh*nx[i],
                                    spine[i].y - hw*by[i] - hh*ny[i],
                                    spine[i].z - hw*bz[i] - hh*nz[i]});
                    ring.push_back({spine[i].x + hw*bx[i] - hh*nx[i],
                                    spine[i].y + hw*by[i] - hh*ny[i],
                                    spine[i].z + hw*bz[i] - hh*nz[i]});
                }
                return ring;
            };

            auto prevRing = makeRing(0);
            for (int i = 1; i < nPts; ++i) {
                auto ring = makeRing(i);
                int color = spine[i].color;
                int n = static_cast<int>(std::min(prevRing.size(), ring.size()));

                for (int j = 0; j < n; ++j) {
                    int j1 = (j + 1) % n;
                    auto& a = prevRing[j]; auto& b = prevRing[j1];
                    auto& c = ring[j1];    auto& d = ring[j];

                    // Project all 4 verts
                    float as, ay, ad, bs, by2, bd, cs, cy, cd, ds, dy, dd;
                    cam.projectCached(a.x, a.y, a.z, as, ay, ad);
                    cam.projectCached(b.x, b.y, b.z, bs, by2, bd);
                    cam.projectCached(c.x, c.y, c.z, cs, cy, cd);
                    cam.projectCached(d.x, d.y, d.z, ds, dy, dd);

                    // Two triangles per quad
                    pixCanvas->drawTriangle(as, ay, ad, bs, by2, bd, ds, dy, dd, color);
                    pixCanvas->drawTriangle(bs, by2, bd, cs, cy, cd, ds, dy, dd, color);
                }
                prevRing = std::move(ring);
            }
        } else {
            // Fallback for text-based canvases: thick spline lines
            float scaleF = static_cast<float>(canvas.scaleX());
            for (int i = 1; i < nPts; ++i) {
                float sx0, sy0, d0, sx1, sy1, d1;
                cam.projectCached(spine[i-1].x, spine[i-1].y, spine[i-1].z, sx0, sy0, d0);
                cam.projectCached(spine[i].x, spine[i].y, spine[i].z, sx1, sy1, d1);

                float hw = ssHalfW(spine[i-1].ss) * scaleF * cam.zoom();
                int r = std::max(1, static_cast<int>(hw + 0.5f));
                int color = spine[i-1].color;

                Canvas::bresenham(static_cast<int>(sx0), static_cast<int>(sy0), d0,
                                  static_cast<int>(sx1), static_cast<int>(sy1), d1,
                    [&](int x, int y, float d) {
                        canvas.drawCircle(x, y, d, r, color, true);
                    });
            }
        }

        cStart = cEnd;
    }
}

} // namespace molterm
