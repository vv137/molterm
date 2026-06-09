#include "molterm/cmd/Register.h"

#include <cmath>
#include <cstdio>

namespace molterm {

namespace {
inline double vlen(const std::array<double, 3>& v) {
    return std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
}
}  // namespace

std::optional<double> Register::getScalar(const std::string& field) const {
    // Empty field == primary value (Scalar's only meaningful access).
    // `length` is supported on Vec3 and on Pca eigvals so scripts can
    // write `${V.length:.2f}` without first projecting to Scalar.
    if (kind == Kind::Scalar && field.empty()) return scalar;
    if (kind == Kind::Vec3) {
        if (field == "x")      return vec[0];
        if (field == "y")      return vec[1];
        if (field == "z")      return vec[2];
        if (field == "length" || field == "len") return vlen(vec);
    }
    if (kind == Kind::Pca) {
        const bool superpose = pca.source == geom::PcaResult::Source::Superpose;
        // eig1..3 are genuine variances for pca()/helix_axis() only; angle/rmsd
        // are meaningful only for superpose_axis(). Reading the wrong field for
        // the producer returns nullopt (surfaced as an error) rather than a
        // misleading 0 — the overload is no longer silent.
        if (field == "eig1") { if (superpose) return std::nullopt; return pca.eigvals[0]; }
        if (field == "eig2") { if (superpose) return std::nullopt; return pca.eigvals[1]; }
        if (field == "eig3") { if (superpose) return std::nullopt; return pca.eigvals[2]; }
        if (field == "angle") { if (!superpose) return std::nullopt; return pca.angle; }
        if (field == "rmsd")  { if (!superpose) return std::nullopt; return pca.rmsd; }
    }
    return std::nullopt;
}

std::optional<std::array<double,3>> Register::getVec(const std::string& field) const {
    // Vec3: empty field returns the value itself; named axes (`.x`/`.y`/`.z`)
    // promote to Scalar, not Vec3, so they're handled by getScalar.
    if (kind == Kind::Vec3 && field.empty()) return vec;
    if (kind == Kind::Pca) {
        if (field == "axis1")  return pca.axis1;
        if (field == "axis2")  return pca.axis2;
        if (field == "axis3")  return pca.axis3;
        if (field == "center") return pca.center;
    }
    return std::nullopt;
}

std::string formatRegister(const std::string& name, const Register& r) {
    char buf[160];
    switch (r.kind) {
        case Register::Kind::Scalar:
            std::snprintf(buf, sizeof(buf), "%s = %.6g", name.c_str(), r.scalar);
            break;
        case Register::Kind::Vec3:
            std::snprintf(buf, sizeof(buf), "%s = [%.4f, %.4f, %.4f]",
                          name.c_str(), r.vec[0], r.vec[1], r.vec[2]);
            break;
        case Register::Kind::Pca:
            std::snprintf(buf, sizeof(buf),
                "%s = pca: center=(%.3f, %.3f, %.3f) eigvals=[%.3f, %.3f, %.3f]",
                name.c_str(),
                r.pca.center[0], r.pca.center[1], r.pca.center[2],
                r.pca.eigvals[0], r.pca.eigvals[1], r.pca.eigvals[2]);
            break;
    }
    return buf;
}

}  // namespace molterm
