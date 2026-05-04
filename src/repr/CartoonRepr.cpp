#include "molterm/repr/CartoonRepr.h"
#include "molterm/repr/ReprUtil.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace molterm {

static bool isNucleotide(const std::string& rn) {
    return rn == "A" || rn == "G" || rn == "C" || rn == "U" ||
           rn == "DA" || rn == "DG" || rn == "DC" || rn == "DT" || rn == "DU" ||
           rn == "ADE" || rn == "GUA" || rn == "CYT" || rn == "URA" || rn == "THY";
}

// FNV-1a 64-bit. Cheap fingerprint for cache invalidation; we don't care
// about cryptographic strength, only that ~all real changes flip a bit.
static std::uint64_t fnv1a64(const void* data, std::size_t bytes) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t h = 1469598103934665603ULL;
    for (std::size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

bool CartoonRepr::CacheKey::operator==(const CacheKey& o) const {
    return mol == o.mol &&
           activeState == o.activeState &&
           atomCount == o.atomCount &&
           subdiv == o.subdiv &&
           coilSegments == o.coilSegments &&
           helixR == o.helixR && sheetR == o.sheetR && loopR == o.loopR &&
           nb == o.nb &&
           colorScheme == o.colorScheme &&
           atomColorsSize == o.atomColorsSize &&
           atomColorsHash == o.atomColorsHash &&
           perAtomRepr == o.perAtomRepr &&
           visMaskHash == o.visMaskHash;
}

// ─── Main render entry point ─────────────────────────────────────────────────

void CartoonRepr::render(const MolObject& mol, const Camera& cam,
                         Canvas& canvas) {
    if (!mol.visible() || !mol.reprVisible(ReprType::Cartoon)) return;
    auto ctx = makeContext(mol, ReprType::Cartoon);
    const auto& atoms = ctx.atoms;

    int subdiv = adjustLOD(subdivisions_, atoms.size());
    int coilSegments = (atoms.size() > 5000) ? 6 : 12;

    // Build cache fingerprint. The cartoon spline + parallel-transport
    // frame depend only on atoms/SS/coloring/repr params, not the camera —
    // so a spinning animation (camera-only changes) hits the cache.
    CacheKey key;
    key.mol = &mol;
    key.activeState = mol.activeState();
    key.atomCount = atoms.size();
    key.subdiv = subdiv;
    key.coilSegments = coilSegments;
    key.helixR = helixRadius_;
    key.sheetR = sheetRadius_;
    key.loopR = loopRadius_;
    key.nb = nucleicBackbone_;
    key.colorScheme = static_cast<int>(mol.colorScheme());
    {
        const auto& ac = mol.atomColors();
        key.atomColorsSize = ac.size();
        if (!ac.empty()) {
            key.atomColorsHash = fnv1a64(ac.data(), ac.size() * sizeof(int));
        }
    }
    key.perAtomRepr = mol.hasPerAtomRepr();
    if (key.perAtomRepr && !ctx.atomVis.empty()) {
        // std::vector<bool> is bit-packed and exposes no .data(); fold one
        // bit per atom into the FNV state. ~35k atoms ≈ 35k FNV rounds ≈
        // a few hundred µs, comfortably under the cache rebuild cost.
        std::uint64_t h = 1469598103934665603ULL;
        for (std::size_t i = 0; i < ctx.atomVis.size(); ++i) {
            h ^= ctx.atomVis[i] ? 1ULL : 0ULL;
            h *= 1099511628211ULL;
        }
        key.visMaskHash = h;
    }

    if (!cacheValid_ || key != cacheKey_) {
        rebuildCache(mol, ctx, subdiv, coilSegments);
        cacheKey_ = key;
        cacheValid_ = true;
    }

    // Per-frame: walk cached chain spans and rasterize. When the canvas
    // can rasterize triangles (pixel mode), accumulate them into a single
    // batch so the canvas can bin them into tiles and dispatch in
    // parallel; otherwise the chain falls back to circle-stamped lines.
    bool useTriangles = (canvas.scaleX() >= 8);
    std::vector<TriangleSpan> triBatch;
    if (useTriangles) triBatch.reserve(cachedSpine_.x.size() * 8);

    for (const auto& span : cachedChains_) {
        drawChainCached(span.start, span.end, coilSegments, cam, canvas,
                        useTriangles ? &triBatch : nullptr);
    }

    if (useTriangles && !triBatch.empty()) {
        canvas.drawTriangleBatch(triBatch.data(), triBatch.size());
    }

    // Nucleic acid base ladders (not currently cached — separate geometry path)
    renderNucleicBases(mol, ctx, cam, canvas);
}

// ─── Cache rebuild: camera-independent geometry ──────────────────────────────

void CartoonRepr::rebuildCache(const MolObject& mol, const RenderContext& ctx,
                               int subdiv, int coilSegments) const {
    (void)mol;
    (void)coilSegments;  // affects only ring construction, not cache geometry
    const auto& atoms = ctx.atoms;

    auto& C = cachedSpine_;
    C.x.clear(); C.y.clear(); C.z.clear();
    C.ss.clear(); C.color.clear(); C.arrowFrac.clear();
    C.tx.clear(); C.ty.clear(); C.tz.clear();
    C.nx.clear(); C.ny.clear(); C.nz.clear();
    C.bx.clear(); C.by.clear(); C.bz.clear();
    cachedChains_.clear();

    // ── Phase A: collect Cα/P per residue with C→O hint ──────────────────
    std::vector<CaAtom> cas;
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

        const std::string& resName = atoms[rs].resName;
        bool nucleic = isNucleotide(resName);
        bool wantC4 = (nucleicBackbone_ == NucleicBackbone::C4);

        auto isPrimaryBackbone = [&](const std::string& name) {
            if (!nucleic) return name == "CA";
            return wantC4 ? (name == "C4'" || name == "C4*")
                          : (name == "P");
        };
        auto isFallbackBackbone = [&](const std::string& name) {
            if (!nucleic) return false;
            return wantC4 ? (name == "P")
                          : (name == "C4'" || name == "C4*");
        };

        int caIdx = -1, fbIdx = -1, cIdx = -1, oIdx = -1;
        for (int j = rs; j < re; ++j) {
            if (caIdx < 0 && isPrimaryBackbone(atoms[j].name) &&
                ctx.visible(j)) {
                caIdx = j;
            }
            if (caIdx < 0 && fbIdx < 0 &&
                isFallbackBackbone(atoms[j].name) && ctx.visible(j)) {
                fbIdx = j;
            }
            if (atoms[j].name == "C" && cIdx < 0) cIdx = j;
            if (atoms[j].name == "O" && oIdx < 0) oIdx = j;
        }
        if (caIdx < 0) caIdx = fbIdx;
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

    // ── Phase B: per chain, build local spine + frame, append to cache ──
    std::size_t cStart = 0;
    while (cStart < cas.size()) {
        std::size_t cEnd = cStart + 1;
        while (cEnd < cas.size() && cas[cEnd].chain == cas[cStart].chain) ++cEnd;

        int cLen = static_cast<int>(cEnd - cStart);
        if (cLen < 2) { cStart = cEnd; continue; }

        // Step 1: Catmull-Rom spline (with hint info kept locally).
        std::vector<SplinePoint> spine;
        spine.reserve(cLen * subdiv + 1);

        for (int seg = 0; seg < cLen - 1; ++seg) {
            int i0 = static_cast<int>(cStart) + std::max(0, seg - 1);
            int i1 = static_cast<int>(cStart) + seg;
            int i2 = static_cast<int>(cStart) + std::min(seg + 1, cLen - 1);
            int i3 = static_cast<int>(cStart) + std::min(seg + 2, cLen - 1);

            int col1 = ctx.colorFor(cas[i1].idx);
            int col2 = ctx.colorFor(cas[i2].idx);
            bool isSheetEnd = (cas[i1].ss == SSType::Sheet &&
                               cas[i2].ss != SSType::Sheet);

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
        const auto& last = cas[cEnd - 1];
        spine.push_back({last.x, last.y, last.z, last.ss,
            ctx.colorFor(last.idx), -1.0f,
            last.hintX, last.hintY, last.hintZ, last.hasHint});

        int nPts = static_cast<int>(spine.size());
        if (nPts < 2) { cStart = cEnd; continue; }

        // Step 2: tangents
        std::vector<float> tx(nPts), ty(nPts), tz(nPts);
        for (int j = 0; j < nPts; ++j) {
            int prev = std::max(0, j - 1), next = std::min(nPts - 1, j + 1);
            tx[j] = spine[next].x - spine[prev].x;
            ty[j] = spine[next].y - spine[prev].y;
            tz[j] = spine[next].z - spine[prev].z;
            normalize3(tx[j], ty[j], tz[j]);
        }

        // Step 3: parallel-transport frame
        std::vector<float> nx(nPts), ny(nPts), nz(nPts);
        std::vector<float> bx(nPts), by(nPts), bz(nPts);
        if (std::abs(tx[0]) < 0.9f) {
            cross3(tx[0], ty[0], tz[0], 1, 0, 0, nx[0], ny[0], nz[0]);
        } else {
            cross3(tx[0], ty[0], tz[0], 0, 1, 0, nx[0], ny[0], nz[0]);
        }
        normalize3(nx[0], ny[0], nz[0]);
        cross3(tx[0], ty[0], tz[0], nx[0], ny[0], nz[0], bx[0], by[0], bz[0]);
        normalize3(bx[0], by[0], bz[0]);

        for (int j = 1; j < nPts; ++j) {
            float dot = nx[j-1]*tx[j] + ny[j-1]*ty[j] + nz[j-1]*tz[j];
            nx[j] = nx[j-1] - dot * tx[j];
            ny[j] = ny[j-1] - dot * ty[j];
            nz[j] = nz[j-1] - dot * tz[j];
            float l = len3(nx[j], ny[j], nz[j]);
            if (l < 1e-6f) { nx[j] = nx[j-1]; ny[j] = ny[j-1]; nz[j] = nz[j-1]; }
            else { nx[j] /= l; ny[j] /= l; nz[j] /= l; }
            cross3(tx[j], ty[j], tz[j], nx[j], ny[j], nz[j], bx[j], by[j], bz[j]);
            normalize3(bx[j], by[j], bz[j]);
        }

        // Step 3b: sheet hint blend (carbonyl C→O guides parallel strands)
        {
            int k = 0;
            while (k < nPts) {
                if (spine[k].ss != SSType::Sheet) { ++k; continue; }
                int rs2 = k;
                while (k < nPts && spine[k].ss == SSType::Sheet) ++k;
                int re2 = k;

                float prevPx = 0.0f, prevPy = 0.0f, prevPz = 0.0f;
                bool hasPrev = false;
                for (int j = rs2; j < re2; ++j) {
                    if (!spine[j].hasHint) continue;
                    float hxh = spine[j].hintX, hyh = spine[j].hintY, hzh = spine[j].hintZ;

                    float d = hxh * tx[j] + hyh * ty[j] + hzh * tz[j];
                    float px = hxh - d * tx[j];
                    float py = hyh - d * ty[j];
                    float pz = hzh - d * tz[j];
                    if (normalize3(px, py, pz) < 1e-6f) continue;

                    if (hasPrev && (px * prevPx + py * prevPy + pz * prevPz) < 0.0f) {
                        px = -px; py = -py; pz = -pz;
                    }
                    prevPx = px; prevPy = py; prevPz = pz;
                    hasPrev = true;

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

        // Append flattened arrays to cache and record the chain span.
        int spineStart = static_cast<int>(C.x.size());
        for (int j = 0; j < nPts; ++j) {
            C.x.push_back(spine[j].x);
            C.y.push_back(spine[j].y);
            C.z.push_back(spine[j].z);
            C.ss.push_back(spine[j].ss);
            C.color.push_back(spine[j].color);
            C.arrowFrac.push_back(spine[j].arrowFrac);
            C.tx.push_back(tx[j]); C.ty.push_back(ty[j]); C.tz.push_back(tz[j]);
            C.nx.push_back(nx[j]); C.ny.push_back(ny[j]); C.nz.push_back(nz[j]);
            C.bx.push_back(bx[j]); C.by.push_back(by[j]); C.bz.push_back(bz[j]);
        }
        int spineEnd = static_cast<int>(C.x.size());
        cachedChains_.push_back({cas[cStart].chain, spineStart, spineEnd});

        cStart = cEnd;
    }
}

// ─── Per-chain rendering from cache ──────────────────────────────────────────

void CartoonRepr::drawChainCached(int chainStart, int chainEnd, int coilSegments,
                                  const Camera& cam, Canvas& canvas,
                                  std::vector<TriangleSpan>* triBatch) const {
    int nPts = chainEnd - chainStart;
    if (nPts < 2) return;
    const auto& C = cachedSpine_;

    auto ssHalfW = [&](SSType ss) -> float {
        switch (ss) {
            case SSType::Helix: return helixRadius_;
            case SSType::Sheet: return sheetRadius_;
            default:            return loopRadius_;
        }
    };
    auto ssHalfH = [&](SSType ss) -> float {
        switch (ss) {
            case SSType::Helix: return 0.40f;
            case SSType::Sheet: return 0.20f;
            default:            return loopRadius_;
        }
    };

    if (triBatch) {
        struct Vert { float x, y, z; };
        auto makeRing = [&](int gi) -> std::vector<Vert> {
            float hw = ssHalfW(C.ss[gi]);
            float hh = ssHalfH(C.ss[gi]);
            bool isCoil = (C.ss[gi] == SSType::Loop);

            if (C.arrowFrac[gi] >= 0.0f) {
                constexpr float kArrowTipScale = 2.20f / 1.50f;
                float af = C.arrowFrac[gi];
                hw *= 1.0f + (kArrowTipScale - 1.0f) * af;
            }

            std::vector<Vert> ring;
            if (isCoil) {
                for (int s = 0; s < coilSegments; ++s) {
                    float angle = 2.0f * 3.14159265f * s / coilSegments;
                    float c = std::cos(angle), si = std::sin(angle);
                    ring.push_back({
                        C.x[gi] + hw * (c * C.nx[gi] + si * C.bx[gi]),
                        C.y[gi] + hw * (c * C.ny[gi] + si * C.by[gi]),
                        C.z[gi] + hw * (c * C.nz[gi] + si * C.bz[gi])
                    });
                }
            } else {
                ring.push_back({C.x[gi] + hw*C.bx[gi] + hh*C.nx[gi],
                                C.y[gi] + hw*C.by[gi] + hh*C.ny[gi],
                                C.z[gi] + hw*C.bz[gi] + hh*C.nz[gi]});
                ring.push_back({C.x[gi] - hw*C.bx[gi] + hh*C.nx[gi],
                                C.y[gi] - hw*C.by[gi] + hh*C.ny[gi],
                                C.z[gi] - hw*C.bz[gi] + hh*C.nz[gi]});
                ring.push_back({C.x[gi] - hw*C.bx[gi] - hh*C.nx[gi],
                                C.y[gi] - hw*C.by[gi] - hh*C.ny[gi],
                                C.z[gi] - hw*C.bz[gi] - hh*C.nz[gi]});
                ring.push_back({C.x[gi] + hw*C.bx[gi] - hh*C.nx[gi],
                                C.y[gi] + hw*C.by[gi] - hh*C.ny[gi],
                                C.z[gi] + hw*C.bz[gi] - hh*C.nz[gi]});
            }
            return ring;
        };

        auto resampleRing = [](const std::vector<Vert>& src, int target)
            -> std::vector<Vert> {
            int sn = static_cast<int>(src.size());
            std::vector<Vert> out;
            if (sn == 0 || target <= 0 || sn == target) return src;
            out.reserve(target);
            for (int j = 0; j < target; ++j) {
                float frac = static_cast<float>(j) / static_cast<float>(target)
                             * static_cast<float>(sn);
                int idx = static_cast<int>(frac);
                float t = frac - static_cast<float>(idx);
                const Vert& a = src[idx % sn];
                const Vert& b = src[(idx + 1) % sn];
                out.push_back({a.x + t * (b.x - a.x),
                               a.y + t * (b.y - a.y),
                               a.z + t * (b.z - a.z)});
            }
            return out;
        };

        auto emitStrip = [&](const std::vector<Vert>& ringA,
                             const std::vector<Vert>& ringB, int color) {
            int rn = static_cast<int>(std::min(ringA.size(), ringB.size()));
            for (int j = 0; j < rn; ++j) {
                int j1 = (j + 1) % rn;
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

        auto emitCap = [&](const std::vector<Vert>& ring, float cx, float cy,
                           float cz, int color) {
            int rn = static_cast<int>(ring.size());
            if (rn < 3) return;
            float ccsx, ccsy, ccsd;
            cam.projectCached(cx, cy, cz, ccsx, ccsy, ccsd);
            for (int j = 0; j < rn; ++j) {
                int j1 = (j + 1) % rn;
                const auto& a = ring[j]; const auto& b = ring[j1];
                float asx, asy, ad, bsx, bsy, bd;
                cam.projectCached(a.x, a.y, a.z, asx, asy, ad);
                cam.projectCached(b.x, b.y, b.z, bsx, bsy, bd);
                triBatch->push_back({{ccsx, asx, bsx},
                                     {ccsy, asy, bsy},
                                     {ccsd, ad, bd}, color});
            }
        };

        auto prevRing = makeRing(chainStart);
        emitCap(prevRing, C.x[chainStart], C.y[chainStart], C.z[chainStart],
                C.color[chainStart]);

        for (int i = chainStart + 1; i < chainEnd; ++i) {
            auto ring = makeRing(i);
            int color = C.color[i];

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

        emitCap(prevRing, C.x[chainEnd-1], C.y[chainEnd-1], C.z[chainEnd-1],
                C.color[chainEnd-1]);
    } else {
        // Braille/block/ascii: thick circle-stamped spline lines
        float scaleF = static_cast<float>(canvas.scaleX());
        const auto& rot = cam.rotation();
        float camZx = rot[6], camZy = rot[7], camZz = rot[8];

        for (int i = chainStart + 1; i < chainEnd; ++i) {
            float sx0, sy0, d0, sx1, sy1, d1;
            cam.projectCached(C.x[i-1], C.y[i-1], C.z[i-1], sx0, sy0, d0);
            cam.projectCached(C.x[i],   C.y[i],   C.z[i],   sx1, sy1, d1);

            SSType ss = C.ss[i-1];
            float baseR;
            switch (ss) {
                case SSType::Helix: baseR = 1.2f; break;
                case SSType::Sheet: baseR = 1.8f; break;
                default:            baseR = 0.4f; break;
            }

            float af = C.arrowFrac[i-1];
            if (af >= 0.0f) {
                if (af < 0.4f)
                    baseR *= (1.0f + af * 1.5f);
                else
                    baseR *= std::max(0.2f, 2.0f * (1.0f - af));
            }

            float hw = baseR * scaleF * cam.zoom();
            int r = std::max(2, static_cast<int>(hw + 0.5f));
            int color = C.color[i-1];

            float dotVal = std::abs(C.tx[i-1]*camZx + C.ty[i-1]*camZy + C.tz[i-1]*camZz);
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
    // Fused purine ring atoms (used in pixel mode to span both rings
    // with a single slab).
    static const char* k9ring[] = {
        "N1","C2","N3","C4","C5","C6","N7","C8","N9"
    };

    bool useTriangles = (canvas.scaleX() >= 8);
    std::vector<TriangleSpan> triBatch;

    // Build a flat 8-vertex slab (1.0 Å half-width × 0.2 Å half-thickness)
    // running from C1' to the ring centroid. Width lies in the ring
    // plane perpendicular to the slab axis; thickness lies along the
    // ring normal. Pushes 12 triangles into `triBatch`.
    auto pushSlab = [&](int c1idx, const std::vector<int>& ringAtoms,
                        int color) {
        if (ringAtoms.size() < 3) return;

        float cx, cy, cz, nnx, nny, nnz;
        ringGeometry(ringAtoms, cx, cy, cz, nnx, nny, nnz);

        float c1x = atoms[c1idx].x, c1y = atoms[c1idx].y, c1z = atoms[c1idx].z;
        float lx = cx - c1x, ly = cy - c1y, lz = cz - c1z;
        if (normalize3(lx, ly, lz) < 1e-6f) return;

        // Width axis: perpendicular to slab axis and ring normal,
        // i.e. lies in the ring plane.
        float wxa, wya, wza;
        cross3(lx, ly, lz, nnx, nny, nnz, wxa, wya, wza);
        if (normalize3(wxa, wya, wza) < 1e-6f) {
            // Slab axis is parallel to ring normal — pick any
            // perpendicular as a fallback.
            cross3(lx, ly, lz, 0.0f, 1.0f, 0.0f, wxa, wya, wza);
            if (normalize3(wxa, wya, wza) < 1e-6f) {
                cross3(lx, ly, lz, 1.0f, 0.0f, 0.0f, wxa, wya, wza);
                normalize3(wxa, wya, wza);
            }
        }
        // Thickness axis: complete the right-handed frame.
        float txa, tya, tza;
        cross3(lx, ly, lz, wxa, wya, wza, txa, tya, tza);
        normalize3(txa, tya, tza);

        constexpr float HW = 1.0f;   // half-width  (Å)
        constexpr float HT = 0.2f;   // half-thickness (Å)

        auto corner = [&](float bx, float by, float bz, float ws, float ts,
                          float& ox, float& oy, float& oz) {
            ox = bx + ws * HW * wxa + ts * HT * txa;
            oy = by + ws * HW * wya + ts * HT * tya;
            oz = bz + ws * HW * wza + ts * HT * tza;
        };

        // 8 corners: 0..3 at C1' end, 4..7 at centroid end.
        // Layout: 0=TL(+w+t) 1=TR(-w+t) 2=BR(-w-t) 3=BL(+w-t)
        float v[8][3];
        corner(c1x, c1y, c1z, +1, +1, v[0][0], v[0][1], v[0][2]);
        corner(c1x, c1y, c1z, -1, +1, v[1][0], v[1][1], v[1][2]);
        corner(c1x, c1y, c1z, -1, -1, v[2][0], v[2][1], v[2][2]);
        corner(c1x, c1y, c1z, +1, -1, v[3][0], v[3][1], v[3][2]);
        corner(cx,  cy,  cz,  +1, +1, v[4][0], v[4][1], v[4][2]);
        corner(cx,  cy,  cz,  -1, +1, v[5][0], v[5][1], v[5][2]);
        corner(cx,  cy,  cz,  -1, -1, v[6][0], v[6][1], v[6][2]);
        corner(cx,  cy,  cz,  +1, -1, v[7][0], v[7][1], v[7][2]);

        // Project once per vertex; reuse for all 12 triangles.
        float p[8][3];
        for (int k = 0; k < 8; ++k) {
            cam.projectCached(v[k][0], v[k][1], v[k][2],
                              p[k][0], p[k][1], p[k][2]);
        }

        auto pushQuad = [&](int a, int b, int c, int d) {
            triBatch.push_back({{p[a][0], p[b][0], p[c][0]},
                                {p[a][1], p[b][1], p[c][1]},
                                {p[a][2], p[b][2], p[c][2]}, color});
            triBatch.push_back({{p[a][0], p[c][0], p[d][0]},
                                {p[a][1], p[c][1], p[d][1]},
                                {p[a][2], p[c][2], p[d][2]}, color});
        };

        pushQuad(0, 1, 2, 3);  // front (at C1')
        pushQuad(5, 4, 7, 6);  // back  (at centroid, reverse winding)
        pushQuad(0, 4, 5, 1);  // top
        pushQuad(3, 2, 6, 7);  // bottom
        pushQuad(0, 3, 7, 4);  // left
        pushQuad(1, 5, 6, 2);  // right
    };

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

        if (useTriangles) {
            // One slab per residue; for purines use all 9 atoms of the
            // fused bicyclic so the slab spans both rings as a single
            // tablet.
            std::vector<int> ringAtoms;
            const char* const* names = purine ? k9ring : k6ring;
            int count = purine ? 9 : 6;
            ringAtoms.reserve(count);
            for (int n = 0; n < count; ++n) {
                int idx = findAtom(resStart, resEnd, names[n]);
                if (idx >= 0) ringAtoms.push_back(idx);
            }
            pushSlab(c1idx, ringAtoms, color);
        } else {
            // Text-mode fallback: stick + ring outline (purines also
            // get the 5-ring outline so the bicyclic is visible).
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
                cam.projectCached(atoms[c1idx].x, atoms[c1idx].y, atoms[c1idx].z,
                                  sx0, sy0, d0);
                cam.projectCached(hcx, hcy, hcz, sx1, sy1, d1);
                canvas.drawLine(static_cast<int>(sx0), static_cast<int>(sy0), d0,
                                static_cast<int>(sx1), static_cast<int>(sy1), d1,
                                color);

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

    if (useTriangles && !triBatch.empty()) {
        canvas.drawTriangleBatch(triBatch.data(), triBatch.size());
    }
}

} // namespace molterm
