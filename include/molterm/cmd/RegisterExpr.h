#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include "molterm/cmd/Register.h"
#include "molterm/core/Selection.h"

namespace molterm {

class MolObject;

// Evaluator for the RHS of `:let <name> = <expr>` (issues #32, #33, #35).
//
// Grammar (recursive descent, operator precedence):
//
//   expr     := term (('+'|'-') term)*
//   term     := factor (('*'|'/') factor)*           // scalar*vec, vec*scalar, scalar*scalar
//   factor   := '-' factor | primary
//   primary  := number
//             | '[' expr ',' expr ',' expr ']'        // vec3 literal
//             | '$' ident ('.' ident)?                // register ref + field
//             | ident '(' arglist ')'                 // function call
//             | '(' expr ')'
//   ident    := [A-Za-z_][A-Za-z0-9_]*
//
// Built-in functions:
//
//   pos(<atom-spec>)              -> Vec3 — Cα-or-name-resolved position.
//   centroid(<selection-expr>)    -> Vec3 — geometric mean of the selection
//                                    (≥ 1 atom; no PCA, unlike pca().center).
//   com(<selection-expr>)         -> Vec3 — mass-weighted center of the
//                                    selection (atomic weight by element).
//   pca(<selection-expr>)         -> Pca  — principal axes of the selection.
//   helix_axis(<selection-expr>)  -> Pca  — helix axis of an ordered Cα/P
//                                    trace (robust on short/curved/single-
//                                    strand segments); axis1 = axis.
//   superpose_axis(selA vs selB)  -> Pca  — screw axis of the optimal A→B
//                                    rigid superposition (equal counts);
//                                    axis1 = axis, angle = rotation angle°,
//                                    rmsd = post-fit residual.
//   rmsd(selA vs selB)            -> Scalar — RMSD of the optimal A→B
//                                    superposition (equal counts), without
//                                    moving anything.
//   dot(v1, v2)                   -> Scalar
//   cross(v1, v2)                 -> Vec3
//   length(v)                     -> Scalar
//   distance(p1, p2) / dist(...)  -> Scalar — |p1 − p2|.
//   normalize(v)                  -> Vec3
//   midpoint(p1, p2)              -> Vec3
//   angle(v1, v2)                 -> Scalar (degrees)
//   dihedral(p1, p2, p3, p4)      -> Scalar (signed degrees, [-180,180])
//
// Atom-spec for pos(): "<chain>:<resi>:<atomname>" (e.g. "A:1:CA").
//
// `pos(...)` and `pca(...)` receive the raw text inside their balanced
// parens — they do NOT participate in the expression-level tokenizer
// for that argument. This avoids re-implementing selection / atom-spec
// parsing inside the expression layer.
class RegisterExpr {
public:
    struct Context {
        // Register table from Application::registers(). Read-only —
        // eval() never assigns into this map; assignment is the caller's
        // job (`:let` writes the final result).
        const std::unordered_map<std::string, Register>* regs = nullptr;
        // Resolves a "chain:resi:name" atom spec to absolute (x,y,z).
        // Returns false when no atom matches. Implementation lives in
        // Application (knows the current object + selection grammar).
        std::function<bool(const std::string&, double*, double*, double*)> resolveAtomPos;
        // Resolves a selection expression to absolute atom XYZ arrays
        // for pca(...). Returns count of atoms gathered.
        std::function<int(const std::string&,
                          std::vector<float>*, std::vector<float>*, std::vector<float>*)>
            collectSelectionXYZ;
        // Like collectSelectionXYZ but also fills a parallel per-atom mass
        // array (atomic weight by element) for com(). centroid() uses the
        // same path and ignores the masses. Returns count of atoms gathered.
        std::function<int(const std::string&,
                          std::vector<float>*, std::vector<float>*, std::vector<float>*,
                          std::vector<float>*)>
            collectSelectionXYZMass;
    };

    struct Result {
        bool ok = false;
        std::string error;
        Register value;
    };

    static Result eval(const std::string& rhs, const Context& ctx);
};

}  // namespace molterm
