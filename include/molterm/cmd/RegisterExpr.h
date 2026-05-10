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
//   pca(<selection-expr>)         -> Pca  — principal axes of the selection.
//   dot(v1, v2)                   -> Scalar
//   cross(v1, v2)                 -> Vec3
//   length(v)                     -> Scalar
//   normalize(v)                  -> Vec3
//   midpoint(p1, p2)              -> Vec3
//   angle(v1, v2)                 -> Scalar (degrees)
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
        // Register table from Application::registers().
        std::unordered_map<std::string, Register>* regs = nullptr;
        // Resolves a "chain:resi:name" atom spec to absolute (x,y,z).
        // Returns false when no atom matches. Implementation lives in
        // Application (knows the current object + selection grammar).
        std::function<bool(const std::string&, double*, double*, double*)> resolveAtomPos;
        // Resolves a selection expression to absolute atom XYZ arrays
        // for pca(...). Returns count of atoms gathered.
        std::function<int(const std::string&,
                          std::vector<float>*, std::vector<float>*, std::vector<float>*)>
            collectSelectionXYZ;
    };

    struct Result {
        bool ok = false;
        std::string error;
        Register value;
    };

    static Result eval(const std::string& rhs, const Context& ctx);
};

}  // namespace molterm
