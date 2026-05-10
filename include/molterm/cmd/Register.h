#pragma once

#include <array>
#include <optional>
#include <string>

#include "molterm/core/Geometry.h"

namespace molterm {

// Typed value stored in a named register by `:let <name> = <expr>`.
//
// Issues #32, #33, #35 introduce script-level vector/PCA primitives;
// Register is the typed slot they read and write. Three kinds are
// shipped today (Scalar, Vec3, Pca); Mat3 / Point can land later
// without breaking the wire format because the kind discriminator is
// part of the struct.
//
//   Scalar : single double — literal numbers, dot-products, lengths
//   Vec3   : three doubles — atom positions, axes, sums/differences
//   Pca    : geom::PcaResult — output of pca(<selection>); access
//            sub-fields via `$g.axis1`, `$g.center`, `$g.eigvals`
//
// `expr` carries the original RHS string so `:get <name>` can echo
// the formula that produced the value (handy when chaining registers).
struct Register {
    enum class Kind { Scalar, Vec3, Pca };
    Kind kind = Kind::Scalar;
    double scalar = 0.0;
    std::array<double, 3> vec{};
    geom::PcaResult pca;
    std::string expr;

    // Field accessors used by `${name.field}` interpolation and by the
    // expression evaluator's member-access path. Returning std::optional
    // lets callers distinguish "field exists but is zero" from "no such
    // field" without exception machinery.
    std::optional<double>               getScalar(const std::string& field) const;
    std::optional<std::array<double,3>> getVec(const std::string& field) const;
};

// Pretty-printer for a "<name> = <value>" status line. Shared between
// `:let` echo and `:registers` listing so the format stays in lockstep.
std::string formatRegister(const std::string& name, const Register& r);

}  // namespace molterm
