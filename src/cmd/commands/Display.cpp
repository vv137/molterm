// Representation + coloring + visibility: :show/:hide/:enable/:disable/:color.


#include "molterm/app/Application.h"
#include "molterm/cmd/CommandParser.h"
#include "molterm/cmd/CommandRegistry.h"
#include "molterm/cmd/CommandHelpers.h"
#include "molterm/cmd/Register.h"
#include "molterm/cmd/RegisterExpr.h"
#include "molterm/cmd/ExprContext.h"
#include "molterm/core/MolObject.h"
#include "molterm/core/Selection.h"

namespace molterm {

void Application::registerDisplayCommands(CommandRegistry& reg) {
    // Helper: resolve repr name to ReprType. Returns false if unknown.
    auto resolveRepr = [](const std::string& name, ReprType& out) -> bool {
        if (name == "wireframe" || name == "wire" || name == "lines") { out = ReprType::Wireframe; return true; }
        if (name == "ballstick" || name == "sticks" || name == "bs")  { out = ReprType::BallStick; return true; }
        if (name == "spacefill" || name == "spheres" || name == "cpk") { out = ReprType::Spacefill; return true; }
        if (name == "cartoon" || name == "tube")     { out = ReprType::Cartoon; return true; }
        if (name == "ribbon")                        { out = ReprType::Ribbon; return true; }
        if (name == "backbone" || name == "trace" || name == "ca") { out = ReprType::Backbone; return true; }
        if (name == "surface" || name == "surf")     { out = ReprType::Surface; return true; }
        return false;
    };

    // :show <repr> [selection]
    reg.registerCmd("show", [resolveRepr](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :show <repr> [selection]"};
        ReprType rt;
        if (!resolveRepr(cmd.args[0], rt)) return {false, "Unknown representation: " + cmd.args[0]};

        std::string expr;
        for (size_t i = 1; i < cmd.args.size(); ++i) {
            if (i > 1) expr += " ";
            expr += cmd.args[i];
        }

        int totalAtoms = 0;
        int objs = forEachInScope(app, expr, [&](ScopedTarget& t) {
            if (t.wholeObject) {
                t.obj->showRepr(rt);
            } else {
                std::vector<int> idxs(t.sel.indices().begin(), t.sel.indices().end());
                t.obj->showReprForAtoms(rt, idxs);
                totalAtoms += static_cast<int>(idxs.size());
            }
            return true;
        });

        if (objs == 0)
            return {false, expr.empty() ? std::string("No object selected")
                                        : ("No atoms match: " + expr)};
        std::string msg = "Showing " + cmd.args[0];
        if (!expr.empty()) msg += " for " + std::to_string(totalAtoms) + " atom(s)";
        if (objs > 1) msg += " in " + std::to_string(objs) + " objects";
        return {true, msg};
    }, ":show <repr> [selection]", "Show representation (wireframe, ballstick, spacefill, cartoon, ribbon, backbone)",
       {":show cartoon", ":show ballstick chain A", ":show wire resn HEM"}, "Display");

    // :hide [repr|all] [selection]
    reg.registerCmd("hide", [resolveRepr](Application& app, const ParsedCommand& cmd) -> ExecResult {
        // ":hide" with no args, or ":hide all" → hide every repr on every
        // in-scope object.
        if (cmd.args.empty() || cmd.args[0] == kAllToken) {
            int objs = forEachInScope(app, "", [&](ScopedTarget& t) {
                t.obj->hideAllRepr();
                return true;
            });
            if (objs == 0) return {false, "No object selected"};
            std::string msg = "Hidden all representations";
            if (objs > 1) msg += " in " + std::to_string(objs) + " objects";
            return {true, msg};
        }

        ReprType rt;
        bool hasRepr = resolveRepr(cmd.args[0], rt);

        // Build the selection expression: when no repr is named the entire
        // arg list is the expression; otherwise everything after the repr.
        std::string expr;
        size_t exprStart = hasRepr ? 1 : 0;
        for (size_t i = exprStart; i < cmd.args.size(); ++i) {
            if (i > exprStart) expr += " ";
            expr += cmd.args[i];
        }

        int totalAtoms = 0;
        int objs = forEachInScope(app, expr, [&](ScopedTarget& t) {
            if (!hasRepr) {
                // ":hide chain A" (no repr) — hide every repr for matched atoms.
                std::vector<int> idxs(t.sel.indices().begin(), t.sel.indices().end());
                t.obj->hideAllReprForAtoms(idxs);
                totalAtoms += static_cast<int>(idxs.size());
            } else if (t.wholeObject) {
                t.obj->hideRepr(rt);
            } else {
                std::vector<int> idxs(t.sel.indices().begin(), t.sel.indices().end());
                t.obj->hideReprForAtoms(rt, idxs);
                totalAtoms += static_cast<int>(idxs.size());
            }
            return true;
        });

        if (objs == 0)
            return {false, expr.empty() ? std::string("No object selected")
                                        : ("No atoms match: " + expr)};
        std::string msg;
        if (!hasRepr) {
            msg = "Hidden all representations for " + std::to_string(totalAtoms) + " atom(s)";
        } else {
            msg = "Hidden " + cmd.args[0];
            if (!expr.empty()) msg += " for " + std::to_string(totalAtoms) + " atom(s)";
        }
        if (objs > 1) msg += " in " + std::to_string(objs) + " objects";
        return {true, msg};
    }, ":hide [repr|all] [selection]", "Hide representation, or all representations",
       {":hide all", ":hide cartoon", ":hide wire chain B"}, "Display");

    // :enable / :disable — toggle whole-object visibility (PyMOL convention).
    // Distinct from :show/:hide which operate on per-representation flags.
    auto resolveObjectArg = [](Application& app, const std::string& a)
        -> std::vector<std::shared_ptr<MolObject>> {
        auto& tab = app.tabs().currentTab();
        const auto& objs = tab.objects();
        std::vector<std::shared_ptr<MolObject>> out;
        if (a == kAllToken || a == kAllGlob) {
            for (const auto& o : objs) if (o) out.push_back(o);
            return out;
        }
        try {
            size_t parsed = 0;
            int n = std::stoi(a, &parsed);
            if (parsed == a.size() && n >= 1 &&
                n <= static_cast<int>(objs.size()) && objs[n - 1]) {
                out.push_back(objs[n - 1]);
                return out;
            }
        } catch (...) {}
        for (const auto& o : objs) {
            if (o && o->name() == a) { out.push_back(o); return out; }
        }
        return out;
    };
    auto setObjectVisibility = [resolveObjectArg](
        Application& app, const ParsedCommand& cmd, bool vis) -> ExecResult {
        std::vector<std::shared_ptr<MolObject>> targets;
        if (cmd.args.empty()) {
            auto cur = app.tabs().currentTab().currentObject();
            if (!cur) return {false, "No current object"};
            targets.push_back(cur);
        } else {
            targets = resolveObjectArg(app, cmd.args[0]);
            if (targets.empty()) {
                // Selection-style args trip users up (`:disable chain B`) —
                // :enable/:disable are whole-object toggles, not selection
                // ops. Point at :hide when the first arg starts a selection
                // keyword. Issue #94.
                if (Selection::isPrimaryKeyword(cmd.args[0])) {
                    return {false, "':" + std::string(vis ? "enable" : "disable") +
                                   "' takes an object name (or 'all' / index); "
                                   "for a selection, use ':hide <repr> " +
                                   cmd.args[0] + " ...'"};
                }
                return {false, "No such object: " + cmd.args[0]};
            }
        }
        int changed = 0;
        for (auto& obj : targets) {
            if (obj->visible() != vis) { obj->setVisible(vis); ++changed; }
        }
        using C = Layout::Component;
        app.layout().markDirty(C::ObjectPanel);
        app.layout().markDirty(C::Viewport);
        app.layout().markDirty(C::StatusBar);
        const char* verb = vis ? "Enabled" : "Disabled";
        if (targets.size() == 1) {
            return {true, std::string(verb) + " " + targets[0]->name()};
        }
        return {true, std::string(verb) + " " + std::to_string(changed) +
                      "/" + std::to_string(targets.size()) + " objects"};
    };

    reg.registerCmd("enable",
        [setObjectVisibility](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return setObjectVisibility(app, cmd, true);
        },
        ":enable [name|index|all]",
        "Make object visible (default: current object)",
        {":enable", ":enable 1ubq", ":enable 2", ":enable all"}, "Display");

    reg.registerCmd("disable",
        [setObjectVisibility](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return setObjectVisibility(app, cmd, false);
        },
        ":disable [name|index|all]",
        "Hide object entirely (default: current object)",
        {":disable", ":disable 1ubq", ":disable 2", ":disable all"}, "Display");

    // :color <scheme>
    reg.registerCmd("color", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty())
            return {false, "Usage: :color <scheme> or :color <name> <selection> | Colors: " + ColorMapper::availableColors()};

        const auto& first = cmd.args[0];

        // Selection expression = tokens after the scheme/color name (may be
        // empty → whole object). Shared by the element and named-color branches.
        std::string expr;
        for (size_t i = 1; i < cmd.args.size(); ++i) {
            if (i > 1) expr += " ";
            expr += cmd.args[i];
        }

        // Helper: apply a per-object scheme across the current scope.
        auto applyScheme = [&](ColorScheme scheme, const std::string& label) -> ExecResult {
            int objs = forEachInScope(app, "", [&](ScopedTarget& t) {
                t.obj->setColorScheme(scheme);
                t.obj->clearAtomColors();
                return true;
            });
            if (objs == 0) return {false, "No object selected"};
            std::string msg = label;
            if (objs > 1) msg += " (" + std::to_string(objs) + " objects)";
            return {true, msg};
        };

        // Scheme branches — :color chain (no further args) etc.
        if (first == "element" || first == "cpk" ||
            first == "heteroatom" || first == "hetero" || first == "het_color") {
            // Honor the optional selection (e.g. `color element ligand`) so the
            // CPK repaint is scoped, not applied to every heteroatom.
            int totalCount = 0;
            int objs = forEachInScope(app, expr, [&](ScopedTarget& t) {
                std::vector<int> idxs(t.sel.indices().begin(), t.sel.indices().end());
                totalCount += applyHeteroatomColors(*t.obj, idxs);
                return true;
            });
            if (objs == 0)
                return {false, expr.empty() ? std::string("No object selected")
                                            : ("No atoms match: " + expr)};
            std::string msg = "Colored " + std::to_string(totalCount) +
                              " heteroatoms by element";
            if (objs > 1) msg += " (" + std::to_string(objs) + " objects)";
            return {true, msg};
        }
        if (first == "chain" && cmd.args.size() == 1) {
            return applyScheme(ColorScheme::Chain, "Coloring by chain");
        }
        if (first == "ss" || first == "secondary") {
            return applyScheme(ColorScheme::SecondaryStructure, "Coloring by SS");
        }
        if (first == "bfactor" || first == "b") {
            return applyScheme(ColorScheme::BFactor, "Coloring by B-factor");
        }
        if (first == "plddt") {
            return applyScheme(ColorScheme::PLDDT, "Coloring by pLDDT (AlphaFold confidence)");
        }
        if (first == "rainbow") {
            return applyScheme(ColorScheme::Rainbow, "Coloring rainbow (N→C terminus)");
        }
        if (first == "restype" || first == "type") {
            return applyScheme(ColorScheme::ResType, "Coloring by residue type (nonpolar/polar/acidic/basic)");
        }
        if (first == "sasa" || first == "accessibility") {
            return applyScheme(ColorScheme::SASA, "Coloring by accessibility (buried→exposed)");
        }
        if (first == "clear" || first == "reset") {
            int objs = forEachInScope(app, "", [&](ScopedTarget& t) {
                t.obj->clearAtomColors();
                return true;
            });
            if (objs == 0) return {false, "No object selected"};
            std::string msg = "Cleared per-atom colors";
            if (objs > 1) msg += " (" + std::to_string(objs) + " objects)";
            return {true, msg};
        }

        // Otherwise: named color + optional selection expression.
        // :color red                → color all atoms red (every in-scope object)
        // :color red chain A        → color chain A red (every in-scope object)
        // :color "#cc4444" chain A  → hex / rgb() literals also accepted (issue #79)
        int colorPair = ColorMapper::colorByName(first);
        if (colorPair < 0) {
            if (auto rgb = parseHexColor(first)) {
                colorPair = packCustomRGB((*rgb)[0], (*rgb)[1], (*rgb)[2]);
            } else {
                return {false, "Unknown color/scheme: " + first + " | Available: " + ColorMapper::availableColors()};
            }
        }

        int totalAtoms = 0;
        int objs = forEachInScope(app, expr, [&](ScopedTarget& t) {
            std::vector<int> idxs(t.sel.indices().begin(), t.sel.indices().end());
            t.obj->setAtomColors(idxs, colorPair);
            totalAtoms += static_cast<int>(idxs.size());
            return true;
        });
        if (objs == 0)
            return {false, expr.empty() ? std::string("No object selected")
                                        : ("No atoms match: " + expr)};
        std::string msg;
        if (expr.empty()) msg = "Colored all atoms " + first;
        else              msg = "Colored " + std::to_string(totalAtoms) + " atom(s) " + first;
        if (objs > 1) msg += " in " + std::to_string(objs) + " objects";
        return {true, msg};
    }, ":color <scheme|name> [selection]",
       "Set coloring scheme (element, chain, ss, bfactor, plddt, rainbow, restype, sasa, heteroatom) or named color",
       {":color ss", ":color chain", ":color red chain A", ":color rainbow"}, "Coloring");

}

}  // namespace molterm
