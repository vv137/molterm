#pragma once

#include "molterm/cmd/RegisterExpr.h"

namespace molterm {

class Application;

// Build a fully-wired expression-evaluation context (registers + atom and
// selection resolvers) so every site that evaluates a `:let`/`:if` RHS —
// pos(), pca(), helix_axis(), superpose_axis(), centroid(), com(), rmsd() —
// behaves identically. Shared by the script runner (Application.cpp) and the
// :let command (cmd/commands/Scripting.cpp). Captures the Application by
// pointer by value, so the returned context's lambdas stay valid after return.
RegisterExpr::Context makeExprContext(Application& app);

}  // namespace molterm
