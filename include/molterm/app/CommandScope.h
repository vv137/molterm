#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "molterm/core/Selection.h"

namespace molterm {

class Application;
class MolObject;

// Whether commands like :color/:show/:hide apply to the current object
// only, or fan out across every object in the active tab.
enum class ScopeMode { Current, All };

struct ScopedTarget {
    std::shared_ptr<MolObject> obj;
    Selection sel;       // atoms in this object that match expr (or all atoms)
    bool wholeObject;    // true when expr was empty
};

// Iterate matching objects in the active tab and invoke fn per-object.
//
// Empty expr  → yield every in-scope object with sel = all atoms (wholeObject=true).
// Non-empty   → re-parse expr against each object; objects whose mask is
//               empty are silently skipped (the same expression naturally
//               narrows when it contains "obj <name>" or "/objname/...").
//
// Returns the number of objects fn was actually invoked on. fn returning
// false aborts iteration early (the count still reflects the objects
// already visited).
int forEachInScope(Application& app, const std::string& expr,
                   const std::function<bool(ScopedTarget&)>& fn);

// Variant taking an explicit ScopeMode — used by structure-mutating
// commands that pin to Current regardless of the global setting, and by
// the :! bang-prefix that flips scope for a single dispatch.
int forEachInScope(Application& app, ScopeMode mode, const std::string& expr,
                   const std::function<bool(ScopedTarget&)>& fn);

// Aggregated atom indices across all in-scope objects, for commands that
// want to compute a union (e.g. :zoom framing the bounding box across
// every aligned structure).
struct ScopedAtoms {
    std::vector<std::pair<std::shared_ptr<MolObject>, std::vector<int>>> perObject;
    int totalAtoms = 0;
};

ScopedAtoms collectInScope(Application& app, const std::string& expr);
ScopedAtoms collectInScope(Application& app, ScopeMode mode,
                           const std::string& expr);

}  // namespace molterm
