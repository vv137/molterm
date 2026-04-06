#include "molterm/repr/CartoonRepr.h"
#include "molterm/render/ColorMapper.h"
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

    const auto& atoms = mol.atoms();
    auto scheme = mol.colorScheme();
    const std::vector<float>* rbw = (scheme == ColorScheme::Rainbow) ? &mol.rainbowFractions() : nullptr;
    auto rf = [&](int i) -> float { return rbw ? (*rbw)[i] : -1.0f; };

    int subdiv = subdivisions_;
    if (atoms.size() > 20000) subdiv = std::max(2, subdiv / 4);
    else if (atoms.size() > 5000) subdiv = std::max(3, subdiv / 2);

    int coilSegments = 8;
    if (atoms.size() > 5000) coilSegments = 4;

    auto atomVis = mol.atomVisMask(ReprType::Cartoon);

    // Collect Cα/P atoms
    struct CaAtom { int idx; float x, y, z; SSType ss; std::string chain; };
    std::vector<CaAtom> cas;
    for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
        if (atoms[i].name != "CA" && atoms[i].name != "P") continue;
        if (!atomVis.empty() && !atomVis[i]) continue;
        cas.push_back({i, atoms[i].x, atoms[i].y, atoms[i].z,
                       atoms[i].ssType, atoms[i].chainId});
    }

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

        bool useTriangles = (canvas.scaleX() >= 8);  // pixel canvas only

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
                default:            return loopRadius_;
            }
        };

        if (useTriangles) {
            struct Vert { float x, y, z; };
            auto makeRing = [&](int i) -> std::vector<Vert> {
                float hw = ssHalfW(spine[i].ss);
                float hh = ssHalfH(spine[i].ss);
                bool isCoil = (spine[i].ss == SSType::Loop);

                if (spine[i].arrowFrac >= 0.0f) {
                    float af = spine[i].arrowFrac;
                    if (af < 0.5f) hw *= (1.0f + af * 1.2f);
                    else { hw *= (2.0f * (1.0f - af) * 1.6f); if (hw < 0.05f) hw = 0.05f; }
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

                    float as, ay, ad, bs, by2, bd, cs, cy, cd, ds, dy, dd;
                    cam.projectCached(a.x, a.y, a.z, as, ay, ad);
                    cam.projectCached(b.x, b.y, b.z, bs, by2, bd);
                    cam.projectCached(c.x, c.y, c.z, cs, cy, cd);
                    cam.projectCached(d.x, d.y, d.z, ds, dy, dd);

                    canvas.drawTriangle(as, ay, ad, bs, by2, bd, ds, dy, dd, color);
                    canvas.drawTriangle(bs, by2, bd, cs, cy, cd, ds, dy, dd, color);
                }
                prevRing = std::move(ring);
            }
        } else {
            // Braille/block/ascii: thick circle-stamped spline lines
            // SS-dependent radius with sheet arrowheads + Lambert pseudo-shading
            float scaleF = static_cast<float>(canvas.scaleX());
            const auto& rot = cam.rotation();
            // Camera Z-axis in world space (view direction)
            float camZx = rot[6], camZy = rot[7], camZz = rot[8];

            for (int i = 1; i < nPts; ++i) {
                float sx0, sy0, d0, sx1, sy1, d1;
                cam.projectCached(spine[i-1].x, spine[i-1].y, spine[i-1].z, sx0, sy0, d0);
                cam.projectCached(spine[i].x, spine[i].y, spine[i].z, sx1, sy1, d1);

                // SS-dependent base radius (sheet wider, loop thinner)
                SSType ss = spine[i-1].ss;
                float baseR;
                switch (ss) {
                    case SSType::Helix: baseR = 1.2f; break;
                    case SSType::Sheet: baseR = 1.8f; break;  // wide flat
                    default:            baseR = 0.4f; break;  // thin coil
                }

                // Sheet arrowhead: widen then taper
                float af = spine[i-1].arrowFrac;
                if (af >= 0.0f) {
                    if (af < 0.4f)
                        baseR *= (1.0f + af * 1.5f);   // widen to ~1.6x
                    else
                        baseR *= std::max(0.2f, 2.0f * (1.0f - af));  // taper to point
                }

                float hw = baseR * scaleF * cam.zoom();
                // Minimum radius = 2 sub-pixels to avoid 1-pixel thin lines in braille
                int r = std::max(2, static_cast<int>(hw + 0.5f));
                int color = spine[i-1].color;

                // Lambert shading: vary radius gently (keep minimum thickness)
                float dotVal = std::abs(tx[i-1]*camZx + ty[i-1]*camZy + tz[i-1]*camZz);
                float brightness = 1.0f - dotVal * 0.35f;  // subtle: 65-100% range
                int shadedR = std::max(2, static_cast<int>(r * brightness));

                Canvas::bresenham(static_cast<int>(sx0), static_cast<int>(sy0), d0,
                                  static_cast<int>(sx1), static_cast<int>(sy1), d1,
                    [&](int x, int y, float d) {
                        canvas.drawCircle(x, y, d, shadedR, color, true);
                    });
            }
        }

        cStart = cEnd;
    }

    // ── Nucleic acid base ladders ──────────────────────────────────────────
    // Draw C1'→base stick + regular polygon ring shapes (hexagon/pentagon).

    auto isNucleotide = [](const std::string& rn) -> bool {
        return rn == "A" || rn == "G" || rn == "C" || rn == "U" ||
               rn == "DA" || rn == "DG" || rn == "DC" || rn == "DT" || rn == "DU" ||
               rn == "ADE" || rn == "GUA" || rn == "CYT" || rn == "URA" || rn == "THY";
    };
    auto isPurine = [](const std::string& rn) -> bool {
        return rn == "A" || rn == "G" || rn == "DA" || rn == "DG" || rn == "ADE" || rn == "GUA";
    };
    auto baseColor = [](const std::string& rn) -> int {
        if (rn == "A" || rn == "DA" || rn == "ADE") return kColorRed;
        if (rn == "G" || rn == "DG" || rn == "GUA") return kColorGreen;
        if (rn == "C" || rn == "DC" || rn == "CYT") return kColorBlue;
        if (rn == "U" || rn == "DU" || rn == "URA") return kColorYellow;
        if (rn == "DT" || rn == "THY") return kColorCyan;
        return kColorOther;
    };

    // Helper: find atom by name in residue range
    auto findAtom = [&](int start, int end, const char* name) -> int {
        for (int j = start; j < end; ++j)
            if (atoms[j].name == name) return j;
        return -1;
    };

    // Helper: compute ring center and normal from atom positions
    auto ringGeometry = [&](const std::vector<int>& indices,
                            float& cx, float& cy, float& cz,
                            float& nnx, float& nny, float& nnz) {
        cx = cy = cz = 0;
        for (int idx : indices) { cx += atoms[idx].x; cy += atoms[idx].y; cz += atoms[idx].z; }
        float n = static_cast<float>(indices.size());
        cx /= n; cy /= n; cz /= n;
        // Normal from first 3 atoms
        if (indices.size() >= 3) {
            float ax = atoms[indices[1]].x - atoms[indices[0]].x;
            float ay = atoms[indices[1]].y - atoms[indices[0]].y;
            float az = atoms[indices[1]].z - atoms[indices[0]].z;
            float bx2 = atoms[indices[2]].x - atoms[indices[0]].x;
            float by2 = atoms[indices[2]].y - atoms[indices[0]].y;
            float bz2 = atoms[indices[2]].z - atoms[indices[0]].z;
            cross3(ax, ay, az, bx2, by2, bz2, nnx, nny, nnz);
            normalize3(nnx, nny, nnz);
        } else { nnx = 0; nny = 0; nnz = 1; }
    };

    // Helper: draw regular polygon in 3D (n sides, center, normal, radius, in-plane ref)
    auto drawRegularPolygon = [&](float cx, float cy, float cz,
                                  float nnx, float nny, float nnz,
                                  float radius, int nSides, int color) {
        // Build in-plane axes
        float ux, uy, uz;
        if (std::abs(nnx) < 0.9f) { cross3(nnx, nny, nnz, 1, 0, 0, ux, uy, uz); }
        else { cross3(nnx, nny, nnz, 0, 1, 0, ux, uy, uz); }
        normalize3(ux, uy, uz);
        float vx, vy, vz;
        cross3(nnx, nny, nnz, ux, uy, uz, vx, vy, vz);
        normalize3(vx, vy, vz);

        // Generate vertices (max 9 sides = purine ring)
        constexpr float PI = 3.14159265f;
        constexpr int kMaxSides = 9;
        float vsx[kMaxSides], vsy[kMaxSides], vsz[kMaxSides];
        nSides = std::min(nSides, kMaxSides);
        for (int s = 0; s < nSides; ++s) {
            float angle = 2.0f * PI * s / nSides - PI / 2.0f;
            float ca = std::cos(angle), sa = std::sin(angle);
            float wx = cx + radius * (ca * ux + sa * vx);
            float wy = cy + radius * (ca * uy + sa * vy);
            float wz = cz + radius * (ca * uz + sa * vz);
            cam.projectCached(wx, wy, wz, vsx[s], vsy[s], vsz[s]);
        }

        if (canvas.scaleX() >= 8) {
            // Pixel: filled triangles
            float csx, csy, csd;
            cam.projectCached(cx, cy, cz, csx, csy, csd);
            for (int s = 0; s < nSides; ++s) {
                int s1 = (s + 1) % nSides;
                canvas.drawTriangle(csx, csy, csd,
                                    vsx[s], vsy[s], vsz[s],
                                    vsx[s1], vsy[s1], vsz[s1], color);
            }
        } else {
            // Braille/block: outline only
            for (int s = 0; s < nSides; ++s) {
                int s1 = (s + 1) % nSides;
                canvas.drawLine(static_cast<int>(vsx[s]), static_cast<int>(vsy[s]), vsz[s],
                                static_cast<int>(vsx[s1]), static_cast<int>(vsy[s1]), vsz[s1], color);
            }
        }
    };

    // Pyrimidine 6-ring atom names
    static const char* k6ring[] = {"N1","C2","N3","C4","C5","C6"};
    // Purine 5-ring atom names (fused ring)
    static const char* k5ring[] = {"C4","C5","N7","C8","N9"};

    int ai = 0;
    while (ai < static_cast<int>(atoms.size())) {
        const auto& a0 = atoms[ai];
        if (!isNucleotide(a0.resName)) { ++ai; continue; }
        if (!atomVis.empty() && !atomVis[ai]) { ++ai; continue; }

        int resStart = ai;
        while (ai < static_cast<int>(atoms.size()) &&
               atoms[ai].chainId == a0.chainId && atoms[ai].resSeq == a0.resSeq)
            ++ai;
        int resEnd = ai;

        // Find C1'
        int c1idx = findAtom(resStart, resEnd, "C1'");
        if (c1idx < 0) c1idx = findAtom(resStart, resEnd, "C1*");
        if (c1idx < 0) continue;

        bool purine = isPurine(a0.resName);
        int color = baseColor(a0.resName);

        // 6-membered ring (both purine and pyrimidine have it)
        int hex[6], hexN = 0;
        for (auto* name : k6ring) {
            int idx = findAtom(resStart, resEnd, name);
            if (idx >= 0 && hexN < 6) hex[hexN++] = idx;
        }

        if (hexN >= 3) {
            std::vector<int> hv(hex, hex + hexN);
            float hcx, hcy, hcz, hnx, hny, hnz;
            ringGeometry(hv, hcx, hcy, hcz, hnx, hny, hnz);

            float sx0, sy0, d0, sx1, sy1, d1;
            cam.projectCached(atoms[c1idx].x, atoms[c1idx].y, atoms[c1idx].z, sx0, sy0, d0);
            cam.projectCached(hcx, hcy, hcz, sx1, sy1, d1);
            canvas.drawLine(static_cast<int>(sx0), static_cast<int>(sy0), d0,
                            static_cast<int>(sx1), static_cast<int>(sy1), d1, color);

            drawRegularPolygon(hcx, hcy, hcz, hnx, hny, hnz, 1.2f, 6, color);
        }

        // 5-membered ring (purine only)
        if (purine) {
            int pent[5], pentN = 0;
            for (auto* name : k5ring) {
                int idx = findAtom(resStart, resEnd, name);
                if (idx >= 0 && pentN < 5) pent[pentN++] = idx;
            }
            if (pentN >= 3) {
                std::vector<int> pv(pent, pent + pentN);
                float pcx, pcy, pcz, pnx, pny, pnz;
                ringGeometry(pv, pcx, pcy, pcz, pnx, pny, pnz);
                drawRegularPolygon(pcx, pcy, pcz, pnx, pny, pnz, 1.0f, 5, color);
            }
        }
    }
}

} // namespace molterm
