#include "molterm/core/Geometry.h"

#include <algorithm>
#include <cmath>

namespace molterm::geom {

PcaResult pcaOf(const std::vector<float>& xs,
                const std::vector<float>& ys,
                const std::vector<float>& zs) {
    PcaResult out;
    const size_t n = std::min({xs.size(), ys.size(), zs.size()});
    if (n < 2) return out;

    // Centroid
    double cx = 0, cy = 0, cz = 0;
    for (size_t i = 0; i < n; ++i) { cx += xs[i]; cy += ys[i]; cz += zs[i]; }
    cx /= n; cy /= n; cz /= n;
    out.center = {cx, cy, cz};

    // Covariance accumulator (symmetric 3x3, row-major).
    double A[3][3] = {};
    for (size_t i = 0; i < n; ++i) {
        double dx = xs[i] - cx, dy = ys[i] - cy, dz = zs[i] - cz;
        A[0][0] += dx*dx; A[0][1] += dx*dy; A[0][2] += dx*dz;
        A[1][1] += dy*dy; A[1][2] += dy*dz;
        A[2][2] += dz*dz;
    }
    A[1][0] = A[0][1]; A[2][0] = A[0][2]; A[2][1] = A[1][2];

    // Jacobi eigendecomposition for symmetric 3x3. Iterates Givens
    // rotations until off-diagonal sum is ~0; bounded at 50 sweeps.
    double V[3][3] = {{1,0,0},{0,1,0},{0,0,1}};
    for (int sweep = 0; sweep < 50; ++sweep) {
        double off = std::abs(A[0][1]) + std::abs(A[0][2]) + std::abs(A[1][2]);
        if (off < 1e-12) break;
        for (int p = 0; p < 3; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                double apq = A[p][q];
                if (std::abs(apq) < 1e-15) continue;
                double theta = (A[q][q] - A[p][p]) / (2.0 * apq);
                double t = (theta >= 0)
                    ? 1.0 / (theta + std::sqrt(1.0 + theta*theta))
                    : 1.0 / (theta - std::sqrt(1.0 + theta*theta));
                double c = 1.0 / std::sqrt(1.0 + t*t);
                double s = t * c;
                double app_old = A[p][p], aqq_old = A[q][q];
                A[p][p] = app_old - t*apq;
                A[q][q] = aqq_old + t*apq;
                A[p][q] = A[q][p] = 0.0;
                for (int r = 0; r < 3; ++r) {
                    if (r != p && r != q) {
                        double arp = A[r][p], arq = A[r][q];
                        A[r][p] = A[p][r] = c*arp - s*arq;
                        A[r][q] = A[q][r] = s*arp + c*arq;
                    }
                }
                for (int r = 0; r < 3; ++r) {
                    double vrp = V[r][p], vrq = V[r][q];
                    V[r][p] = c*vrp - s*vrq;
                    V[r][q] = s*vrp + c*vrq;
                }
            }
        }
    }

    // Sort eigenvalues descending: order[0] = longest axis index, etc.
    int order[3] = {0, 1, 2};
    double eig[3] = {A[0][0], A[1][1], A[2][2]};
    if (eig[order[0]] < eig[order[1]]) std::swap(order[0], order[1]);
    if (eig[order[1]] < eig[order[2]]) std::swap(order[1], order[2]);
    if (eig[order[0]] < eig[order[1]]) std::swap(order[0], order[1]);

    out.axis1 = {V[0][order[0]], V[1][order[0]], V[2][order[0]]};
    out.axis2 = {V[0][order[1]], V[1][order[1]], V[2][order[1]]};
    out.axis3 = {V[0][order[2]], V[1][order[2]], V[2][order[2]]};
    out.eigvals = {eig[order[0]], eig[order[1]], eig[order[2]]};

    // Force right-handed PCA frame: axis3 == axis1 × axis2.
    double cr[3] = {
        out.axis1[1]*out.axis2[2] - out.axis1[2]*out.axis2[1],
        out.axis1[2]*out.axis2[0] - out.axis1[0]*out.axis2[2],
        out.axis1[0]*out.axis2[1] - out.axis1[1]*out.axis2[0]
    };
    if (cr[0]*out.axis3[0] + cr[1]*out.axis3[1] + cr[2]*out.axis3[2] < 0) {
        out.axis3[0] = -out.axis3[0];
        out.axis3[1] = -out.axis3[1];
        out.axis3[2] = -out.axis3[2];
    }
    out.valid = true;
    return out;
}

namespace {
// Fill axis2/axis3 with an arbitrary right-handed orthonormal complement
// of a unit axis1 — only axis1 is physically meaningful for helixAxisOf /
// superposeAxisOf, but a complete frame keeps `:axis` and `.axis2/3` sane.
void completeFrame(PcaResult& out) {
    const auto& a = out.axis1;
    // Seed perpendicular to the smallest |component| basis vector.
    std::array<double, 3> e = (std::abs(a[0]) <= std::abs(a[1]) &&
                               std::abs(a[0]) <= std::abs(a[2]))
        ? std::array<double, 3>{1, 0, 0}
        : (std::abs(a[1]) <= std::abs(a[2]) ? std::array<double, 3>{0, 1, 0}
                                            : std::array<double, 3>{0, 0, 1});
    std::array<double, 3> u = {a[1]*e[2] - a[2]*e[1],
                               a[2]*e[0] - a[0]*e[2],
                               a[0]*e[1] - a[1]*e[0]};
    double ul = std::sqrt(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    if (ul < 1e-12) { out.axis2 = {0,0,0}; out.axis3 = {0,0,0}; return; }
    out.axis2 = {u[0]/ul, u[1]/ul, u[2]/ul};
    out.axis3 = {a[1]*out.axis2[2] - a[2]*out.axis2[1],
                 a[2]*out.axis2[0] - a[0]*out.axis2[2],
                 a[0]*out.axis2[1] - a[1]*out.axis2[0]};
}
}  // namespace

PcaResult helixAxisOf(const std::vector<float>& xs,
                      const std::vector<float>& ys,
                      const std::vector<float>& zs) {
    PcaResult out;
    const size_t n = std::min({xs.size(), ys.size(), zs.size()});
    if (n < 3) return out;

    double cx = 0, cy = 0, cz = 0;
    for (size_t i = 0; i < n; ++i) { cx += xs[i]; cy += ys[i]; cz += zs[i]; }
    cx /= n; cy /= n; cz /= n;
    out.center = {cx, cy, cz};

    // Σ (v_i × v_{i+1}) over consecutive chord vectors — points along axis.
    double ax = 0, ay = 0, az = 0;
    for (size_t i = 0; i + 2 < n; ++i) {
        double v1x = xs[i+1]-xs[i],   v1y = ys[i+1]-ys[i],   v1z = zs[i+1]-zs[i];
        double v2x = xs[i+2]-xs[i+1], v2y = ys[i+2]-ys[i+1], v2z = zs[i+2]-zs[i+1];
        ax += v1y*v2z - v1z*v2y;
        ay += v1z*v2x - v1x*v2z;
        az += v1x*v2y - v1y*v2x;
    }
    double dx = ax, dy = ay, dz = az;
    double L = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (L < 1e-9) {
        // Near-linear trace: fall back to the first→last chord direction.
        dx = xs[n-1]-xs[0]; dy = ys[n-1]-ys[0]; dz = zs[n-1]-zs[0];
        L = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (L < 1e-9) return out;  // coincident points — degenerate
    }
    dx /= L; dy /= L; dz /= L;

    // Orient the axis first→last so two segments compare without a sign flip.
    double chx = xs[n-1]-xs[0], chy = ys[n-1]-ys[0], chz = zs[n-1]-zs[0];
    if (dx*chx + dy*chy + dz*chz < 0) { dx = -dx; dy = -dy; dz = -dz; }
    out.axis1 = {dx, dy, dz};

    double var = 0;
    for (size_t i = 0; i < n; ++i) {
        double proj = (xs[i]-cx)*dx + (ys[i]-cy)*dy + (zs[i]-cz)*dz;
        var += proj*proj;
    }
    out.eigvals = {var / n, 0, 0};
    completeFrame(out);
    out.valid = true;
    return out;
}

PcaResult superposeAxisOf(const std::vector<float>& ax, const std::vector<float>& ay,
                          const std::vector<float>& az, const std::vector<float>& bx,
                          const std::vector<float>& by, const std::vector<float>& bz) {
    PcaResult out;
    const size_t n = std::min({ax.size(), ay.size(), az.size(),
                               bx.size(), by.size(), bz.size()});
    if (n < 3) return out;

    double acx=0, acy=0, acz=0, bcx=0, bcy=0, bcz=0;
    for (size_t i = 0; i < n; ++i) {
        acx += ax[i]; acy += ay[i]; acz += az[i];
        bcx += bx[i]; bcy += by[i]; bcz += bz[i];
    }
    acx/=n; acy/=n; acz/=n; bcx/=n; bcy/=n; bcz/=n;

    // Cross-covariance S = Σ (a−ā)(b−b̄)ᵀ.
    double Sxx=0,Sxy=0,Sxz=0, Syx=0,Syy=0,Syz=0, Szx=0,Szy=0,Szz=0;
    for (size_t i = 0; i < n; ++i) {
        double axk=ax[i]-acx, ayk=ay[i]-acy, azk=az[i]-acz;
        double bxk=bx[i]-bcx, byk=by[i]-bcy, bzk=bz[i]-bcz;
        Sxx+=axk*bxk; Sxy+=axk*byk; Sxz+=axk*bzk;
        Syx+=ayk*bxk; Syy+=ayk*byk; Syz+=ayk*bzk;
        Szx+=azk*bxk; Szy+=azk*byk; Szz+=azk*bzk;
    }

    // Horn's symmetric 4×4 key matrix N; its top eigenvector is the
    // optimal rotation quaternion (scalar-first) mapping A onto B.
    double N[4][4];
    N[0][0]=Sxx+Syy+Szz; N[0][1]=Syz-Szy;     N[0][2]=Szx-Sxz;     N[0][3]=Sxy-Syx;
    N[1][1]=Sxx-Syy-Szz; N[1][2]=Sxy+Syx;     N[1][3]=Szx+Sxz;
    N[2][2]=-Sxx+Syy-Szz;N[2][3]=Syz+Szy;
    N[3][3]=-Sxx-Syy+Szz;
    N[1][0]=N[0][1]; N[2][0]=N[0][2]; N[3][0]=N[0][3];
    N[2][1]=N[1][2]; N[3][1]=N[1][3]; N[3][2]=N[2][3];

    // Shift by the Gershgorin bound so N+shift·I is positive semidefinite,
    // making the wanted (largest) eigenvalue strictly dominant for power
    // iteration regardless of how negative the others get.
    double shift = 0;
    for (int i = 0; i < 4; ++i) {
        double rowsum = 0;
        for (int j = 0; j < 4; ++j) rowsum += std::abs(N[i][j]);
        shift = std::max(shift, rowsum);
    }
    for (int i = 0; i < 4; ++i) N[i][i] += shift;

    double q[4] = {0.5, 0.5, 0.5, 0.5};
    for (int it = 0; it < 200; ++it) {
        double y[4] = {0,0,0,0};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) y[i] += N[i][j]*q[j];
        double ny = std::sqrt(y[0]*y[0]+y[1]*y[1]+y[2]*y[2]+y[3]*y[3]);
        if (ny < 1e-15) break;
        double maxd = 0;
        for (int i = 0; i < 4; ++i) { y[i] /= ny; maxd = std::max(maxd, std::abs(y[i]-q[i])); }
        for (int i = 0; i < 4; ++i) q[i] = y[i];
        if (maxd < 1e-12) break;
    }

    out.center = {0.5*(acx+bcx), 0.5*(acy+bcy), 0.5*(acz+bcz)};
    double vlen = std::sqrt(q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
    if (vlen < 1e-9) {
        // Pure-identity rotation: axis undefined, angle ≈ 0.
        out.axis1 = {0, 0, 1};
        out.eigvals = {0, 0, 0};
    } else {
        out.axis1 = {q[1]/vlen, q[2]/vlen, q[3]/vlen};
        double w = std::abs(q[0]); if (w > 1.0) w = 1.0;
        out.eigvals = {2.0 * std::acos(w) * (180.0 / 3.14159265358979323846), 0, 0};
    }
    completeFrame(out);
    out.valid = true;
    return out;
}

}  // namespace molterm::geom
