#include "molterm/repr/CartoonRepr.h"
#include "molterm/repr/ReprUtil.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace molterm {

static bool isNucleotide(const std::string& rn) {
    return rn == "A" || rn == "G" || rn == "C" || rn == "U" ||
           rn == "DA" || rn == "DG" || rn == "DC" || rn == "DT" || rn == "DU" ||
           rn == "ADE" || rn == "GUA" || rn == "CYT" || rn == "URA" || rn == "THY";
}

// ─── Main render entry point ─────────────────────────────────────────────────

void CartoonRepr::render(const MolObject& mol, const Camera& cam,
                         Canvas& canvas) {
    if (!mol.visible() || !mol.reprVisible(ReprType::Cartoon)) return;
    auto ctx = makeContext(mol, ReprType::Cartoon);
    const auto& atoms = ctx.atoms;

    int subdiv = adjustLOD(subdivisions_, atoms.size());
    int coilSegments = (atoms.size() > 5000) ? 4 : 8;

    // Collect Cα/P atoms with residue-scoped C→O frame hints (proteins).
    // Walk each residue once; pick the first visible CA (or P), then look
    // up backbone C and O within the same residue to compute the carbonyl
    // direction. Hint is used later to orient β-sheet ribbons.
    std::vector<CaAtom> cas;
    {
        int n = static_cast<int>(atoms.size());
        int i = 0;
        while (i < n) {
            int rs = i;
            while (i < n &&
                   atoms[i].chainId == atoms[rs].chainId &&
                   atoms[i].resSeq == atoms[rs].resSeq &&
                   atoms[i].insCode == atoms[rs].insCode) {
                ++i;
            }
            int re = i;

            int caIdx = -1, cIdx = -1, oIdx = -1;
            for (int j = rs; j < re; ++j) {
                if ((atoms[j].name == "CA" || atoms[j].name == "P") &&
                    caIdx < 0 && ctx.visible(j)) {
                    caIdx = j;
                }
                if (atoms[j].name == "C" && cIdx < 0) cIdx = j;
                if (atoms[j].name == "O" && oIdx < 0) oIdx = j;
            }
            if (caIdx < 0) continue;

            float hx = 0.0f, hy = 0.0f, hz = 0.0f;
            bool hasHint = false;
            if (cIdx >= 0 && oIdx >= 0) {
                hx = atoms[oIdx].x - atoms[cIdx].x;
                hy = atoms[oIdx].y - atoms[cIdx].y;
                hz = atoms[oIdx].z - atoms[cIdx].z;
                if (normalize3(hx, hy, hz) > 1e-6f) hasHint = true;
            }

            cas.push_back({caIdx, atoms[caIdx].x, atoms[caIdx].y, atoms[caIdx].z,
                           atoms[caIdx].ssType, atoms[caIdx].chainId,
                           hx, hy, hz, hasHint});
        }
    }

    // Process each chain. When the canvas can rasterize triangles
    // (pixel mode), accumulate them into a single batch so the canvas
    // can bin them into tiles and dispatch in parallel; otherwise the
    // chain falls back to the line/circle path which writes to the
    // canvas immediately.
    bool useTriangles = (canvas.scaleX() >= 8);
    std::vector<TriangleSpan> triBatch;
    if (useTriangles) triBatch.reserve(cas.size() * subdiv * 8);

    size_t cStart = 0;
    while (cStart < cas.size()) {
        size_t cEnd = cStart + 1;
        while (cEnd < cas.size() && cas[cEnd].chain == cas[cStart].chain) ++cEnd;
        renderChain(cas, cStart, cEnd, subdiv, coilSegments, ctx, cam, canvas,
                    useTriangles ? &triBatch : nullptr);
        cStart = cEnd;
    }

    if (useTriangles && !triBatch.empty()) {
        canvas.drawTriangleBatch(triBatch.data(), triBatch.size());
    }

    // Nucleic acid base ladders
    renderNucleicBases(mol, ctx, cam, canvas);
}

// ─── Per-chain spline + rendering ────────────────────────────────────────────

void CartoonRepr::renderChain(const std::vector<CaAtom>& cas, size_t start,
                              size_t end, int subdiv, int coilSegments,
                              const RenderContext& ctx,
                              const Camera& cam, Canvas& canvas,
                              std::vector<TriangleSpan>* triBatch) const {
    int cLen = static_cast<int>(end - start);
    if (cLen < 2) return;

    // Step 1: Generate spline points
    std::vector<SplinePoint> spine;
    spine.reserve(cLen * subdiv + 1);

    for (int seg = 0; seg < cLen - 1; ++seg) {
        int i0 = static_cast<int>(start) + std::max(0, seg - 1);
        int i1 = static_cast<int>(start) + seg;
        int i2 = static_cast<int>(start) + std::min(seg + 1, cLen - 1);
        int i3 = static_cast<int>(start) + std::min(seg + 2, cLen - 1);

        int col1 = ctx.colorFor(cas[i1].idx);
        int col2 = ctx.colorFor(cas[i2].idx);

        // Detect sheet→non-sheet transition for arrowhead
        bool isSheetEnd = (cas[i1].ss == SSType::Sheet && cas[i2].ss != SSType::Sheet);

        for (int s = 0; s < subdiv; ++s) {
            float t = static_cast<float>(s) / static_cast<float>(subdiv);
            float af = isSheetEnd ? t : -1.0f;
            const auto& near = (t < 0.5f) ? cas[i1] : cas[i2];
            spine.push_back({
                catmullRom(cas[i0].x, cas[i1].x, cas[i2].x, cas[i3].x, t),
                catmullRom(cas[i0].y, cas[i1].y, cas[i2].y, cas[i3].y, t),
                catmullRom(cas[i0].z, cas[i1].z, cas[i2].z, cas[i3].z, t),
                near.ss,
                (t < 0.5f) ? col1 : col2,
                af,
                near.hintX, near.hintY, near.hintZ, near.hasHint
            });
        }
    }
    // Last point
    auto& last = cas[end - 1];
    spine.push_back({last.x, last.y, last.z, last.ss,
        ctx.colorFor(last.idx), -1.0f,
        last.hintX, last.hintY, last.hintZ, last.hasHint});

    int nPts = static_cast<int>(spine.size());
    if (nPts < 2) return;

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

    // Step 3b: Sheet frame guide.
    // Within each contiguous β-sheet run, blend the carbonyl C→O hint
    // (projected perpendicular to the tangent, sign-aligned along the run)
    // into the binormal at 65 % weight. This makes parallel strands lie
    // on a consistent plane instead of twisting under parallel-transport.
    {
        int k = 0;
        while (k < nPts) {
            if (spine[k].ss != SSType::Sheet) { ++k; continue; }
            int rs = k;
            while (k < nPts && spine[k].ss == SSType::Sheet) ++k;
            int re = k;

            float prevPx = 0.0f, prevPy = 0.0f, prevPz = 0.0f;
            bool hasPrev = false;
            for (int j = rs; j < re; ++j) {
                if (!spine[j].hasHint) continue;
                float hx = spine[j].hintX, hy = spine[j].hintY, hz = spine[j].hintZ;

                // Project hint perpendicular to tangent.
                float d = hx * tx[j] + hy * ty[j] + hz * tz[j];
                float px = hx - d * tx[j];
                float py = hy - d * ty[j];
                float pz = hz - d * tz[j];
                if (normalize3(px, py, pz) < 1e-6f) continue;

                // Align sign with previous projected hint.
                if (hasPrev && (px * prevPx + py * prevPy + pz * prevPz) < 0.0f) {
                    px = -px; py = -py; pz = -pz;
                }
                prevPx = px; prevPy = py; prevPz = pz;
                hasPrev = true;

                // Blend 65 % hint + 35 % existing binormal, recompute normal.
                float bbx = 0.65f * px + 0.35f * bx[j];
                float bby = 0.65f * py + 0.35f * by[j];
                float bbz = 0.65f * pz + 0.35f * bz[j];
                if (normalize3(bbx, bby, bbz) < 1e-6f) continue;

                float newNx, newNy, newNz;
                cross3(bbx, bby, bbz, tx[j], ty[j], tz[j], newNx, newNy, newNz);
                if (normalize3(newNx, newNy, newNz) < 1e-6f) continue;

                bx[j] = bbx; by[j] = bby; bz[j] = bbz;
                nx[j] = newNx; ny[j] = newNy; nz[j] = newNz;
            }
        }
    }

    // Step 4: Generate cross-section rings and emit triangles
    bool useTriangles = (canvas.scaleX() >= 8);

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

        // Linearly interpolate `src` around its perimeter to produce a ring
        // with `target` vertices. Used at SS transitions where helix/sheet
        // (4 verts) meets coil (coilSegments verts) — without this, the
        // strip would only stitch the first min(N, M) corners and leave a
        // gash on one side.
        auto resampleRing = [](const std::vector<Vert>& src, int target)
            -> std::vector<Vert> {
            int n = static_cast<int>(src.size());
            std::vector<Vert> out;
            if (n == 0 || target <= 0 || n == target) return src;
            out.reserve(target);
            for (int i = 0; i < target; ++i) {
                float frac = static_cast<float>(i) / static_cast<float>(target)
                             * static_cast<float>(n);
                int idx = static_cast<int>(frac);
                float t = frac - static_cast<float>(idx);
                const Vert& a = src[idx % n];
                const Vert& b = src[(idx + 1) % n];
                out.push_back({a.x + t * (b.x - a.x),
                               a.y + t * (b.y - a.y),
                               a.z + t * (b.z - a.z)});
            }
            return out;
        };

        auto emitStrip = [&](const std::vector<Vert>& ringA,
                             const std::vector<Vert>& ringB, int color) {
            int n = static_cast<int>(std::min(ringA.size(), ringB.size()));
            for (int j = 0; j < n; ++j) {
                int j1 = (j + 1) % n;
                const auto& a = ringA[j]; const auto& b = ringA[j1];
                const auto& c = ringB[j1]; const auto& d = ringB[j];

                float as, ay, ad, bs, by2, bd, cs, cy, cd, ds, dy, dd;
                cam.projectCached(a.x, a.y, a.z, as, ay, ad);
                cam.projectCached(b.x, b.y, b.z, bs, by2, bd);
                cam.projectCached(c.x, c.y, c.z, cs, cy, cd);
                cam.projectCached(d.x, d.y, d.z, ds, dy, dd);

                triBatch->push_back({{as, bs, ds}, {ay, by2, dy},
                                     {ad, bd, dd}, color});
                triBatch->push_back({{bs, cs, ds}, {by2, cy, dy},
                                     {bd, cd, dd}, color});
            }
        };

        // Fan from the spline-point center to each edge of the ring; closes
        // the open end of a chain so the cartoon doesn't show a hollow tube
        // mouth at the N- and C-termini.
        auto emitCap = [&](const std::vector<Vert>& ring, float cx, float cy,
                           float cz, int color) {
            int n = static_cast<int>(ring.size());
            if (n < 3) return;
            float ccsx, ccsy, ccsd;
            cam.projectCached(cx, cy, cz, ccsx, ccsy, ccsd);
            for (int j = 0; j < n; ++j) {
                int j1 = (j + 1) % n;
                const auto& a = ring[j]; const auto& b = ring[j1];
                float asx, asy, ad, bsx, bsy, bd;
                cam.projectCached(a.x, a.y, a.z, asx, asy, ad);
                cam.projectCached(b.x, b.y, b.z, bsx, bsy, bd);
                triBatch->push_back({{ccsx, asx, bsx},
                                     {ccsy, asy, bsy},
                                     {ccsd, ad, bd}, color});
            }
        };

        auto prevRing = makeRing(0);
        emitCap(prevRing, spine[0].x, spine[0].y, spine[0].z, spine[0].color);

        for (int i = 1; i < nPts; ++i) {
            auto ring = makeRing(i);
            int color = spine[i].color;

            if (prevRing.size() == ring.size()) {
                emitStrip(prevRing, ring, color);
            } else {
                int target = static_cast<int>(
                    std::max(prevRing.size(), ring.size()));
                if (static_cast<int>(prevRing.size()) == target) {
                    emitStrip(prevRing, resampleRing(ring, target), color);
                } else {
                    emitStrip(resampleRing(prevRing, target), ring, color);
                }
            }
            prevRing = std::move(ring);
        }

        emitCap(prevRing, spine[nPts - 1].x, spine[nPts - 1].y,
                spine[nPts - 1].z, spine[nPts - 1].color);
    } else {
        // Braille/block/ascii: thick circle-stamped spline lines
        float scaleF = static_cast<float>(canvas.scaleX());
        const auto& rot = cam.rotation();
        float camZx = rot[6], camZy = rot[7], camZz = rot[8];

        for (int i = 1; i < nPts; ++i) {
            float sx0, sy0, d0, sx1, sy1, d1;
            cam.projectCached(spine[i-1].x, spine[i-1].y, spine[i-1].z, sx0, sy0, d0);
            cam.projectCached(spine[i].x, spine[i].y, spine[i].z, sx1, sy1, d1);

            SSType ss = spine[i-1].ss;
            float baseR;
            switch (ss) {
                case SSType::Helix: baseR = 1.2f; break;
                case SSType::Sheet: baseR = 1.8f; break;
                default:            baseR = 0.4f; break;
            }

            float af = spine[i-1].arrowFrac;
            if (af >= 0.0f) {
                if (af < 0.4f)
                    baseR *= (1.0f + af * 1.5f);
                else
                    baseR *= std::max(0.2f, 2.0f * (1.0f - af));
            }

            float hw = baseR * scaleF * cam.zoom();
            int r = std::max(2, static_cast<int>(hw + 0.5f));
            int color = spine[i-1].color;

            float dotVal = std::abs(tx[i-1]*camZx + ty[i-1]*camZy + tz[i-1]*camZz);
            float brightness = 1.0f - dotVal * 0.35f;
            int shadedR = std::max(2, static_cast<int>(r * brightness));

            Canvas::bresenham(static_cast<int>(sx0), static_cast<int>(sy0), d0,
                              static_cast<int>(sx1), static_cast<int>(sy1), d1,
                [&](int x, int y, float d) {
                    canvas.drawCircle(x, y, d, shadedR, color, true);
                });
        }
    }
}

// ─── Nucleic acid base ladders ───────────────────────────────────────────────

void CartoonRepr::renderNucleicBases(const MolObject& /* mol */,
                                     const RenderContext& ctx,
                                     const Camera& cam,
                                     Canvas& canvas) const {
    const auto& atoms = ctx.atoms;

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

    auto findAtom = [&](int start, int end, const char* name) -> int {
        for (int j = start; j < end; ++j)
            if (atoms[j].name == name) return j;
        return -1;
    };

    auto ringGeometry = [&](const std::vector<int>& indices,
                            float& cx, float& cy, float& cz,
                            float& nnx, float& nny, float& nnz) {
        cx = cy = cz = 0;
        for (int idx : indices) { cx += atoms[idx].x; cy += atoms[idx].y; cz += atoms[idx].z; }
        float n = static_cast<float>(indices.size());
        cx /= n; cy /= n; cz /= n;
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

    auto drawRegularPolygon = [&](float cx, float cy, float cz,
                                  float nnx, float nny, float nnz,
                                  float radius, int nSides, int color) {
        float ux, uy, uz;
        if (std::abs(nnx) < 0.9f) { cross3(nnx, nny, nnz, 1, 0, 0, ux, uy, uz); }
        else { cross3(nnx, nny, nnz, 0, 1, 0, ux, uy, uz); }
        normalize3(ux, uy, uz);
        float vx, vy, vz;
        cross3(nnx, nny, nnz, ux, uy, uz, vx, vy, vz);
        normalize3(vx, vy, vz);

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
            float csx, csy, csd;
            cam.projectCached(cx, cy, cz, csx, csy, csd);
            for (int s = 0; s < nSides; ++s) {
                int s1 = (s + 1) % nSides;
                canvas.drawTriangle(csx, csy, csd,
                                    vsx[s], vsy[s], vsz[s],
                                    vsx[s1], vsy[s1], vsz[s1], color);
            }
        } else {
            for (int s = 0; s < nSides; ++s) {
                int s1 = (s + 1) % nSides;
                canvas.drawLine(static_cast<int>(vsx[s]), static_cast<int>(vsy[s]), vsz[s],
                                static_cast<int>(vsx[s1]), static_cast<int>(vsy[s1]), vsz[s1], color);
            }
        }
    };

    static const char* k6ring[] = {"N1","C2","N3","C4","C5","C6"};
    static const char* k5ring[] = {"C4","C5","N7","C8","N9"};

    int ai = 0;
    while (ai < static_cast<int>(atoms.size())) {
        const auto& a0 = atoms[ai];
        if (!isNucleotide(a0.resName)) { ++ai; continue; }
        if (!ctx.visible(ai)) { ++ai; continue; }

        int resStart = ai;
        while (ai < static_cast<int>(atoms.size()) &&
               atoms[ai].chainId == a0.chainId && atoms[ai].resSeq == a0.resSeq)
            ++ai;
        int resEnd = ai;

        int c1idx = findAtom(resStart, resEnd, "C1'");
        if (c1idx < 0) c1idx = findAtom(resStart, resEnd, "C1*");
        if (c1idx < 0) continue;

        bool purine = isPurine(a0.resName);
        int color = baseColor(a0.resName);

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
