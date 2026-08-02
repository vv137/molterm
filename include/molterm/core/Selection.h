#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "molterm/core/AtomData.h"

namespace molterm {

class MolObject;

// A selection is a resolved set of atom indices + the expression that produced it
class Selection {
public:
    Selection() = default;
    explicit Selection(std::vector<int> indices, std::string expr = "");

    const std::vector<int>& indices() const { return indices_; }
    const std::string& expression() const { return expr_; }
    size_t size() const { return indices_.size(); }
    bool empty() const { return indices_.empty(); }

    // Object scope — the `<obj>` in an `<obj>/(...)` qualifier. A scoped
    // selection re-resolves only against the object of that name; empty
    // means unscoped (resolves against whatever object it is parsed
    // against). The parser tags the result of `<obj>/(...)` and the
    // `/obj/.../` slash form, and honors it when re-expanding `$name` so a
    // named selection built from `<obj>/(...)` does not leak into other
    // loaded objects (issue #101).
    const std::string& objectScope() const { return objScope_; }
    Selection& setObjectScope(std::string obj) { objScope_ = std::move(obj); return *this; }

    // Mutation (for mouse picking)
    void addIndex(int idx);
    void addIndices(const std::vector<int>& idxs);
    void removeIndex(int idx);
    bool has(int idx) const;
    void clear();

    // Set operations
    Selection operator&(const Selection& other) const;  // AND (intersection)
    Selection operator|(const Selection& other) const;  // OR (union)
    Selection operator-(const Selection& other) const;  // MINUS (set difference, issue #52)
    Selection operator^(const Selection& other) const;  // XOR (symmetric difference, issue #52)
    Selection operator~() const;                        // NOT (complement, needs totalAtoms)
    Selection complement(int totalAtoms) const;

    // Select all
    static Selection all(int totalAtoms);

    // Resolver for named selections: @name → Selection
    using NameResolver = std::function<const Selection*(const std::string&)>;

    // Parse a PyMOL-like selection expression against a MolObject
    //
    // Value lists (chain / resn / resi / name / element / imgt) union their
    // entries and accept '+', ',' and plain whitespace interchangeably:
    // `chain A+B` == `chain A,B` == `chain A B`. Whitespace counts because
    // CommandParser splits argv on commas, so `:color red resi 2,3,6` reaches
    // the parser as "resi 2 3 6" — treating that as one value is the silent
    // truncation of issue #116. `resi 10-12` stays an inclusive range.
    //
    // Supported: chain X, resn XXX, resi N, resi N-M, name XX, element X,
    //            helix, sheet, loop, backbone, sidechain, hydro,
    //            vis | visible,            (atoms shown by any active repr)
    //            @name (named selection reference),
    //            and, or, not, minus, xor, ( ),     (issue #52)
    //   VMD-style spatial / propagation operators:
    //            within N of <selection>     — atoms within N Å of <selection>
    //            exwithin N of <selection>   — within minus the sub-selection
    //            same KW as <selection>      — atoms sharing KW with <selection>
    //                                          (KW ∈ residue, chain, resname/resn)
    //            byres <selection>           — alias for `same residue as`
    //            bychain <selection>         — alias for `same chain as`
    //
    // PyMOL equivalents (consult before serializing named selections to .pml):
    //   within N of S       →  "(all within N of (S))"
    //   exwithin N of S     →  "((all within N of (S)) and not (S))"
    //   same residue as S   →  "byres (S)"
    //   same chain as S     →  "bychain (S)"
    //   same resname as S   →  has no string-level PyMOL equivalent; must
    //                          enumerate the matching resnames at eval time.
    // SessionExporter currently emits hand-built PyMOL commands and does not
    // round-trip Selection::expression(). When that changes, add a translator
    // here rather than letting the molterm-internal form leak into PML.
    // `error`, when non-null, receives a diagnostic if the expression did not
    // parse cleanly — an unbalanced paren, or a tail the grammar could not
    // consume. Without it a malformed expression silently narrows to whatever
    // prefix did parse, which is how `resi 2,3,6` shipped wrong figures
    // (issues #116, #121). The returned Selection is still the prefix match,
    // so callers that don't care keep their old behavior; callers that pass
    // `error` should treat a non-empty result as a failed command.
    static Selection parse(const std::string& expr, const MolObject& mol,
                           NameResolver resolver = nullptr,
                           std::string* error = nullptr);

    // True if `word` (ASCII, case-insensitive) is a token that begins a
    // primary selector (chain, resi, helix, within, ...). Used by callers
    // that need to find the boundary between non-selection tokens and an
    // unbracketed selection expression on the same line — e.g. ":loadalign
    // model_*.cif chain A+B". Mirrors the keywords accepted by parse().
    static bool isPrimaryKeyword(std::string_view word);

    // Predicate-based selection factory
    static Selection fromPredicate(const MolObject& mol,
                                    const std::function<bool(int, const AtomData&)>& pred,
                                    const std::string& expr);

private:
    std::vector<int> indices_;
    std::string expr_;
    std::string objScope_;  // owning object name, or empty when unscoped
};

} // namespace molterm
