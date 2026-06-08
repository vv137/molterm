// tests/test_geometry.cpp — synthetic-input checks for the geometry helpers
// added with the helix_axis() / superpose_axis() / dihedral() builtins
// (issues #108/#109/#110).
//
// Build:  c++ -std=c++17 -O2 -I include \
//             tests/test_geometry.cpp build/CMakeFiles/molterm.dir/src/core/Geometry.cpp.o \
//             -o build/test_geometry
// Run:    ./build/test_geometry
//
// Unlike the real-structure validation in lib/README.md, this drives the
// helpers with constructed inputs whose answers are known exactly: an ideal
// helix of known axis, a point set rotated by a known angle about a known
// axis, and dihedrals at 0°/180°/±90°.

#include "molterm/core/Geometry.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

using molterm::geom::PcaResult;

static int g_fail = 0;
static void check(bool ok, const char* what, double got, double want) {
    std::printf("[%s] %-42s got=%.4f want=%.4f\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok) ++g_fail;
}

static double dotv(const std::array<double,3>& a, const std::array<double,3>& b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

// Rotate v about unit axis k by angle a (Rodrigues).
static std::array<double,3> rotate(const std::array<double,3>& v,
                                   const std::array<double,3>& k, double a) {
    double c = std::cos(a), s = std::sin(a);
    double kv = k[0]*v[0] + k[1]*v[1] + k[2]*v[2];
    std::array<double,3> kxv = {k[1]*v[2]-k[2]*v[1], k[2]*v[0]-k[0]*v[2], k[0]*v[1]-k[1]*v[0]};
    return {v[0]*c + kxv[0]*s + k[0]*kv*(1-c),
            v[1]*c + kxv[1]*s + k[1]*kv*(1-c),
            v[2]*c + kxv[2]*s + k[2]*kv*(1-c)};
}

int main() {
    const double pi = 3.14159265358979323846;

    // ── helix_axis: ideal helix about +z, then about a tilted axis ──
    {
        std::vector<float> xs, ys, zs;
        for (int i = 0; i < 20; ++i) {
            double th = i * 100.0 * pi / 180.0;   // 100°/residue (α-helix)
            xs.push_back(2.3f * std::cos(th));
            ys.push_back(2.3f * std::sin(th));
            zs.push_back(1.5f * i);               // rise 1.5 Å/residue
        }
        PcaResult h = molterm::geom::helixAxisOf(xs, ys, zs);
        check(h.valid && dotv(h.axis1, {0,0,1}) > 0.999, "helix_axis: +z helix → +z axis",
              h.valid ? dotv(h.axis1, {0,0,1}) : 0.0, 1.0);
    }
    {
        std::array<double,3> k = {0.6, 0, 0.8};   // tilted axis
        std::vector<float> xs, ys, zs;
        for (int i = 0; i < 20; ++i) {
            double th = i * 100.0 * pi / 180.0;
            std::array<double,3> p = {2.3*std::cos(th), 2.3*std::sin(th), 1.5*i};
            // re-express the +z helix about k by rotating the whole frame:
            std::array<double,3> r = rotate(p, {0,1,0}, std::atan2(0.6, 0.8));
            xs.push_back((float)r[0]); ys.push_back((float)r[1]); zs.push_back((float)r[2]);
        }
        PcaResult h = molterm::geom::helixAxisOf(xs, ys, zs);
        double d = h.valid ? std::abs(dotv(h.axis1, k)) : 0.0;
        check(h.valid && d > 0.999, "helix_axis: tilted helix → tilted axis", d, 1.0);
    }

    // ── superpose_axis: known rotation about a known axis ──
    {
        std::array<double,3> k = {0.0, 0.0, 1.0};
        double ang = 35.0 * pi / 180.0;
        std::vector<float> ax, ay, az, bx, by, bz;
        // A grab-bag of non-collinear points.
        double pts[6][3] = {{1,0,0},{0,2,0},{0,0,3},{1,1,0},{-2,1,1},{0.5,-1,2}};
        for (auto& p : pts) {
            ax.push_back((float)p[0]); ay.push_back((float)p[1]); az.push_back((float)p[2]);
            std::array<double,3> r = rotate({p[0],p[1],p[2]}, k, ang);
            bx.push_back((float)r[0]); by.push_back((float)r[1]); bz.push_back((float)r[2]);
        }
        PcaResult s = molterm::geom::superposeAxisOf(ax, ay, az, bx, by, bz);
        double axisdot = s.valid ? std::abs(dotv(s.axis1, k)) : 0.0;
        check(s.valid && axisdot > 0.999, "superpose_axis: recovers rotation axis", axisdot, 1.0);
        check(s.valid && std::abs(s.angle - 35.0) < 0.5, "superpose_axis: recovers angle (35°)",
              s.valid ? s.angle : 0.0, 35.0);
    }
    {
        // Identical sets → ~0° rotation.
        std::vector<float> x = {1,0,0,1}, y = {0,2,0,1}, z = {0,0,3,2};
        PcaResult s = molterm::geom::superposeAxisOf(x, y, z, x, y, z);
        check(s.valid && s.angle < 0.5, "superpose_axis: identity → 0°", s.valid ? s.angle : -1, 0.0);
    }

    // ── rmsd: superposition-invariant residual (issue #115) ──
    {
        // Identical sets → 0.
        std::vector<float> x = {1,0,0,1}, y = {0,2,0,1}, z = {0,0,3,2};
        auto r = molterm::geom::rmsdOf(x, y, z, x, y, z);
        check(r.valid && r.n == 4 && r.rmsd < 1e-5, "rmsd: identical sets → 0",
              r.valid ? r.rmsd : -1, 0.0);
    }
    {
        // A rigid rotation+translation of A must superpose back onto A with
        // ~0 residual — RMSD ignores pose, exactly the point of the builtin.
        std::array<double,3> k = {0.3, -0.6, 0.74};  // arbitrary unit-ish axis
        double kl = std::sqrt(k[0]*k[0]+k[1]*k[1]+k[2]*k[2]);
        k = {k[0]/kl, k[1]/kl, k[2]/kl};
        double ang = 50.0 * pi / 180.0;
        std::vector<float> ax, ay, az, bx, by, bz;
        double pts[6][3] = {{1,0,0},{0,2,0},{0,0,3},{1,1,0},{-2,1,1},{0.5,-1,2}};
        for (auto& p : pts) {
            ax.push_back((float)p[0]); ay.push_back((float)p[1]); az.push_back((float)p[2]);
            std::array<double,3> r = rotate({p[0],p[1],p[2]}, k, ang);
            bx.push_back((float)(r[0]+5.0)); by.push_back((float)(r[1]-3.0)); bz.push_back((float)(r[2]+1.0));
        }
        auto r = molterm::geom::rmsdOf(ax, ay, az, bx, by, bz);
        check(r.valid && r.rmsd < 1e-3, "rmsd: rigid-moved copy → ~0", r.valid ? r.rmsd : -1, 0.0);

        // The superpose_axis().rmsd field must agree with rmsdOf().
        PcaResult s = molterm::geom::superposeAxisOf(ax, ay, az, bx, by, bz);
        check(s.valid && std::abs(s.rmsd - r.rmsd) < 1e-6, "rmsd: superpose_axis.rmsd matches",
              s.valid ? s.rmsd : -1, r.rmsd);
    }
    {
        // Shift one point of B by 2 Å along x: the best-fit residual is a
        // known, bounded value (< the raw 2 Å, since fitting redistributes it).
        std::vector<float> x = {0,1,0,0,2}, y = {0,0,1,0,0}, z = {0,0,0,1,0};
        std::vector<float> bx = x, by = y, bz = z;
        bx[0] += 2.0f;
        auto r = molterm::geom::rmsdOf(x, y, z, bx, by, bz);
        check(r.valid && r.rmsd > 0.1 && r.rmsd < 2.0, "rmsd: 2A displacement → bounded residual",
              r.valid ? r.rmsd : -1, 0.0);
    }
    {
        // Mismatched counts → invalid.
        std::vector<float> x = {0,1}, y = {0,0}, z = {0,0};
        std::vector<float> bx = {0}, by = {0}, bz = {0};
        auto r = molterm::geom::rmsdOf(x, y, z, bx, by, bz);
        check(!r.valid, "rmsd: mismatched counts → invalid", r.valid ? 1 : 0, 0.0);
    }

    // ── dihedral: 0° (cis), 180° (trans), ±90° ──
    {
        using molterm::geom::dihedralDeg;
        double cis  = dihedralDeg(0,1,0,  0,0,0,  1,0,0,  1,1,0);
        double tran = dihedralDeg(0,1,0,  0,0,0,  1,0,0,  1,-1,0);
        double p90  = dihedralDeg(0,1,0,  0,0,0,  1,0,0,  1,0,1);
        check(std::abs(cis) < 0.01,            "dihedral: planar cis → 0°", cis, 0.0);
        check(std::abs(std::abs(tran) - 180) < 0.01, "dihedral: planar trans → 180°", tran, 180.0);
        check(std::abs(std::abs(p90) - 90) < 0.01,   "dihedral: out-of-plane → ±90°", p90, 90.0);
    }

    std::printf("%s (%d failure%s)\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
