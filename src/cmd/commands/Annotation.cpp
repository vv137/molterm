// Presets + annotations: :preset/:label/:unlabel/:overlay.


#include "molterm/app/Application.h"
#include "molterm/cmd/CommandParser.h"
#include "molterm/cmd/CommandRegistry.h"
#include "molterm/cmd/CommandHelpers.h"
#include "molterm/cmd/Register.h"
#include "molterm/cmd/RegisterExpr.h"
#include "molterm/cmd/ExprContext.h"
#include "molterm/core/MolObject.h"
#include "molterm/core/Selection.h"
#include "molterm/app/CommandScope.h"

namespace molterm {

void Application::registerAnnotationCommands(CommandRegistry& reg) {
    // :preset — apply smart default representation
    reg.registerCmd("preset", [](Application& app, const ParsedCommand&) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        obj->applySmartDefaults();
        return {true, "Applied default preset (cartoon + ballstick ligands)"};
    }, ":preset", "Apply smart default representations (cartoon for protein, ballstick for ligands)",
       {":preset"}, "Display");

    reg.registerCmd("label", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty())
            return {false, "Usage: :label <sel|corner CORNER|screen FX FY|world X Y Z> [= \"text\"] or :label clear"};
        if (cmd.args[0] == "clear") {
            app.clearAtomLabels();
            app.freeLabels().clear();
            return {true, "Labels cleared"};
        }
        // Free-position labels (issue #34): three anchor modes that don't
        // bind to an atom. All three use the `= "text"` convention to
        // separate anchor args from the label string.
        if (cmd.args[0] == "corner" || cmd.args[0] == "screen" || cmd.args[0] == "world") {
            auto [anchorArgs, text] = splitAtEqToken(cmd.args);
            // splitAtEqToken strictly shortens anchorArgs when `=` is
            // present, so the size delta is a cheap presence check
            // (same idiom as the atom-anchored branch below).
            bool sawEq = anchorArgs.size() != cmd.args.size();
            // `:label <anchor> = ""` (or empty text after =) acts as a
            // clear-this-anchor instruction (issue #65). Without `=`,
            // it's a usage error.
            if (text.empty()) {
                if (!sawEq)
                    return {false, "Free-position labels require '= \"text\"' suffix "
                                   "(or `= \"\"` to clear)"};
                FreeLabelAnchor anchor;
                std::optional<FreeLabelCorner> which;
                if (anchorArgs[0] == "corner") {
                    anchor = FreeLabelAnchor::Corner;
                    if (anchorArgs.size() >= 2) {
                        which = parseCornerName(anchorArgs[1]);
                        if (!which) return {false, "Bad corner: " + anchorArgs[1]};
                    }
                } else {
                    anchor = (anchorArgs[0] == "screen")
                        ? FreeLabelAnchor::Screen : FreeLabelAnchor::World;
                }
                size_t removed = clearFreeLabelsByAnchor(app.freeLabels(), anchor, which);
                return {true, "Cleared " + std::to_string(removed) +
                              " " + anchorArgs[0] + " label(s)"};
            }
            FreeLabel fl;
            fl.text = text;
            const std::string& mode = anchorArgs[0];
            if (mode == "corner") {
                if (anchorArgs.size() < 2)
                    return {false, "Usage: :label corner topleft|topright|bottomleft|bottomright = \"text\""};
                fl.anchor = FreeLabelAnchor::Corner;
                auto c = parseCornerName(anchorArgs[1]);
                if (!c) return {false, "Bad corner: " + anchorArgs[1]};
                fl.corner = *c;
            } else if (mode == "screen") {
                if (anchorArgs.size() < 3)
                    return {false, "Usage: :label screen <fx> <fy> = \"text\""};
                fl.anchor = FreeLabelAnchor::Screen;
                try { fl.fx = std::stof(anchorArgs[1]); fl.fy = std::stof(anchorArgs[2]); }
                catch (...) { return {false, "Bad screen coords"}; }
            } else {  // world
                if (anchorArgs.size() < 4)
                    return {false, "Usage: :label world <x> <y> <z> = \"text\""};
                fl.anchor = FreeLabelAnchor::World;
                try {
                    fl.wx = std::stof(anchorArgs[1]);
                    fl.wy = std::stof(anchorArgs[2]);
                    fl.wz = std::stof(anchorArgs[3]);
                } catch (...) { return {false, "Bad world coords"}; }
            }
            app.freeLabels().push_back(std::move(fl));
            return {true, "Added free-position label"};
        }
        // Atom-anchored labels (legacy default).
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        // '=' splits selection from override text — same convention as :select.
        auto [exprArgs, customText] = splitAtEqToken(cmd.args);
        std::string expr = joinArgs(exprArgs, 0, exprArgs.size());
        if (expr.empty()) return {false, "Empty selection"};
        bool hasCustom = (exprArgs.size() != cmd.args.size());
        if (hasCustom && customText.empty())
            return {false, "Empty label text after '='"};
        // Honor the `<obj>/(...)` qualifier and scoped `$name` (issue #101).
        // Labels are stored against the resolved object so they render on
        // its own atoms in a multi-object overlay.
        auto sel = resolveScoped(app, expr, obj);
        if (sel.empty()) return {false, "No atoms match: " + expr};
        const std::string* text = hasCustom ? &customText : nullptr;
        for (int idx : sel.indices())
            app.addAtomLabel(obj->name(), idx, text);
        if (hasCustom)
            return {true, "Labeled " + std::to_string(sel.size()) +
                          " atoms as \"" + customText + "\""};
        return {true, "Labeled " + std::to_string(sel.size()) + " atoms"};
    }, ":label <selection|clear> [= \"text\"]",
       "Show residue labels (default <resname><resseq>; '= text' overrides per-atom; 'clear' removes)",
       {":label resi 50-60", ":label chain E and resi 1 = \"P1\"", ":label clear"}, "Display");

    reg.registerCmd("unlabel", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) {
            // "Remove all labels" must cover free labels too — corner /
            // screen / world anchors live in a separate store but read
            // as "labels" to the user.
            int n = static_cast<int>(app.labelCount());
            int fn = static_cast<int>(app.freeLabels().size());
            app.clearAtomLabels();
            app.freeLabels().clear();
            if (fn == 0) return {true, "Cleared " + std::to_string(n) + " label(s)"};
            return {true, "Cleared " + std::to_string(n) + " atom + " +
                          std::to_string(fn) + " free label(s)"};
        }
        if (cmd.args[0] == "corner") {
            std::optional<FreeLabelCorner> which;
            if (cmd.args.size() >= 2) {
                which = parseCornerName(cmd.args[1]);
                if (!which) return {false, "Bad corner: " + cmd.args[1]};
            }
            size_t removed = clearFreeLabelsByAnchor(app.freeLabels(),
                                FreeLabelAnchor::Corner, which);
            return {true, "Removed " + std::to_string(removed) + " corner label(s)"};
        }
        if (cmd.args[0] == "screen" || cmd.args[0] == "world") {
            auto anchor = (cmd.args[0] == "screen")
                ? FreeLabelAnchor::Screen : FreeLabelAnchor::World;
            size_t removed = clearFreeLabelsByAnchor(app.freeLabels(),
                                anchor, std::nullopt);
            return {true, "Removed " + std::to_string(removed) +
                          " " + cmd.args[0] + " label(s)"};
        }
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        std::string expr = joinArgs(cmd.args, 0, cmd.args.size());
        // Same scope-aware resolution as :label so `:unlabel <obj>/(...)`
        // and scoped `$name` drop labels from the right object (issue #101).
        auto sel = resolveScoped(app, expr, obj);
        if (sel.empty()) return {false, "No atoms match: " + expr};
        std::set<int> drop(sel.indices().begin(), sel.indices().end());
        size_t removed = app.removeAtomLabels(obj->name(), drop);
        return {true, "Removed " + std::to_string(removed) + " label(s)"};
    }, ":unlabel [selection|corner [<which>]|screen|world]",
       "Remove labels (no arg: all atom + free; selection: matching atoms; corner/screen/world: that anchor kind)",
       {":unlabel", ":unlabel chain E and resi 1",
        ":unlabel corner topleft", ":unlabel corner", ":unlabel screen"}, "Display");

    // :overlay on|off | :overlay clear
    reg.registerCmd("overlay", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) {
            return {false, "Usage: :overlay on|off | :overlay clear"};
        }
        if (cmd.args[0] == "clear") {
            int mc = static_cast<int>(app.measurements().size());
            int lc = static_cast<int>(app.labelCount());
            app.clearOverlayAnnotations();
            return {true, "Cleared " + std::to_string(mc) + " measurements, " +
                   std::to_string(lc) + " labels"};
        }
        auto v = parseBool(cmd.args[0]);
        if (!v) return {false, "Usage: :overlay on|off | :overlay clear"};
        app.overlayVisible_ = *v;
        return {true, *v ? "Overlays visible" : "Overlays hidden"};
    }, ":overlay on|off | clear", "Toggle overlay visibility (labels, measurements, $sele) or clear them",
       {":overlay on", ":overlay off", ":overlay clear"}, "Display");

}

}  // namespace molterm
