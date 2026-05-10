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

}  // namespace molterm::geom
