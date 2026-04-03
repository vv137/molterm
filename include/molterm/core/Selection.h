#pragma once

#include <functional>
#include <string>
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

    // Mutation (for mouse picking)
    void addIndex(int idx);
    void addIndices(const std::vector<int>& idxs);
    void removeIndex(int idx);
    bool has(int idx) const;
    void clear();

    // Set operations
    Selection operator&(const Selection& other) const;  // AND (intersection)
    Selection operator|(const Selection& other) const;  // OR (union)
    Selection operator~() const;                        // NOT (complement, needs totalAtoms)
    Selection complement(int totalAtoms) const;

    // Select all
    static Selection all(int totalAtoms);

    // Resolver for named selections: @name → Selection
    using NameResolver = std::function<const Selection*(const std::string&)>;

    // Parse a PyMOL-like selection expression against a MolObject
    // Supported: chain X, resn XXX, resi N, resi N-M, name XX, element X,
    //            helix, sheet, loop, backbone, sidechain, hydro,
    //            @name (named selection reference),
    //            and, or, not, ( )
    static Selection parse(const std::string& expr, const MolObject& mol,
                           NameResolver resolver = nullptr);

    // Predicate-based selection factory
    static Selection fromPredicate(const MolObject& mol,
                                    const std::function<bool(int, const AtomData&)>& pred,
                                    const std::string& expr);

private:
    std::vector<int> indices_;
    std::string expr_;
};

} // namespace molterm
