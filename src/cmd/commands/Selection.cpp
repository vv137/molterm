// Selection: :select/:count/:cmp.


#include "molterm/app/Application.h"
#include "molterm/cmd/CommandParser.h"
#include "molterm/cmd/CommandRegistry.h"
#include "molterm/cmd/CommandHelpers.h"
#include "molterm/cmd/Register.h"
#include "molterm/cmd/RegisterExpr.h"
#include "molterm/cmd/ExprContext.h"
#include "molterm/core/MolObject.h"
#include "molterm/core/Selection.h"
#include "molterm/io/Aligner.h"

namespace molterm {

void Application::registerSelectionCommands(CommandRegistry& reg) {
    // :select <expression>
    reg.registerCmd("select", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :select <expr> | :select <name> = <expr> | :select clear"};
        // :select clear — wipe $sele AND pk1-pk4 (matches the `gx` hotkey
        // so the command and shortcut have identical semantics).
        if (cmd.args[0] == "clear") {
            auto [hadSele, hadPk] = app.clearVisualSelection();
            if (!hadSele && !hadPk) return {true, "Selection already empty"};
            return {true, "Cleared $sele (" + std::to_string(hadSele) +
                          " atoms) and pk1-pk4"};
        }
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};

        // Named form "name = expr" only when the text before the first '=' is
        // a single bare identifier; otherwise any '=' belongs to the
        // expression. Read rawArgs so the selection keeps its exact spacing and
        // commas (e.g. `resi 1,2,3`) instead of being space-rejoined from
        // tokens.
        std::string name;
        std::string expr = cmd.rawArgs;
        auto eq = cmd.rawArgs.find('=');
        if (eq != std::string::npos) {
            std::string before = cmd.rawArgs.substr(0, eq);
            trimWhitespace(before);
            if (isBareName(before)) {
                name = before;
                expr = cmd.rawArgs.substr(eq + 1);
            }
        }
        trimWhitespace(expr);

        if (expr.empty()) return {false, "Empty expression"};

        auto sel = app.parseSelection(expr, *obj);

        // `<other>/(<expr>)` qualifying a loaded-but-not-current object
        // parses to empty here (name mismatch in the parser). Fall back
        // to forEachInScope so the qualifier resolves against the named
        // object regardless of which one happens to be current. Issue #88.
        if (sel.empty()) {
            forEachInScope(app, ScopeMode::All, expr, [&](ScopedTarget& t) {
                if (t.obj == obj) return true;  // already tried current
                obj = t.obj;
                sel = std::move(t.sel);
                return false;  // stop on first match
            });
        }

        // Issue #91: a named Selection is per-mol-indices, so when the
        // expression also matches in other loaded objects we silently
        // narrow to the chosen object. Surface that — the user might
        // want to qualify with `<obj>/(...)` for each side. Skip in the
        // empty case (no match anywhere) and when the expression already
        // contains an object qualifier (qualifier author opted in to a
        // single-object scope).
        int otherObjMatches = 0;
        int otherObjCount = 0;
        if (!sel.empty() && expr.find('/') == std::string::npos) {
            forEachInScope(app, ScopeMode::All, expr, [&](ScopedTarget& t) {
                if (t.obj == obj) return true;
                otherObjMatches += static_cast<int>(t.sel.size());
                ++otherObjCount;
                return true;
            });
            // forEachInScope ends by saving the first matched object's
            // selection to $sele; restore the chosen object's so $sele
            // tracks the named-selection result the user just stored.
            app.namedSelections()[kSele] = sel;
        }
        auto multiNote = [&]() -> std::string {
            if (otherObjCount == 0) return "";
            return " [note: " + std::to_string(otherObjMatches) +
                   " more atom(s) in " + std::to_string(otherObjCount) +
                   " other object(s); use '<obj>/(" + expr + ")' to scope]";
        };

        if (!name.empty()) {
            // Store as named selection
            app.namedSelections()[name] = sel;
            if (sel.empty()) return {false, "Selection '" + name + "' is empty: " + expr};
            app.logSelectionInfo(name, expr, sel, *obj);
            return {true, "Selection '" + name + "' = " + std::to_string(sel.size()) +
                          " atoms" + multiNote()};
        }

        if (sel.empty()) return {false, "Selection empty: " + expr};
        app.logSelectionInfo("(anon)", expr, sel, *obj);
        return {true, "Selected " + std::to_string(sel.size()) + " atoms: " + expr +
                      multiNote()};
    }, ":select <expr>",
       "Select atoms (use 'name = expr' for a named selection; 'clear' to "
       "drop $sele). Fields: chain | resi | resn (alias resname) | name | "
       "element | within N of <sub> | byres / bychain. Object qualifier: "
       "<obj>/(...) or <obj>/* for whole-object.",
       {":select chain A", ":select s1 = resi 50-80",
        ":select s2 = resn TRP", ":select clear"}, "Selection");

    // :count <expression> — count atoms matching selection
    reg.registerCmd("count", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :count <expression>"};
        std::string expr = joinArgs(cmd.args, 0, cmd.args.size());
        auto scoped = collectInScope(app, expr);
        if (scoped.perObject.empty())
            return {false, "No object selected"};
        std::string msg = std::to_string(scoped.totalAtoms) + " atoms match: " + expr;
        if (scoped.perObject.size() > 1) {
            msg += " (";
            bool first = true;
            for (const auto& [obj, idxs] : scoped.perObject) {
                if (!first) msg += ", ";
                first = false;
                msg += obj->name() + ":" + std::to_string(idxs.size());
            }
            msg += ")";
        }
        return {true, msg};
    }, ":count <expr>", "Count atoms matching a selection expression",
       {":count chain A", ":count resn HEM", ":count $sele",
        ":count 1ubq/(chain A)"}, "Selection");

    // :cmp <expr-A> vs <expr-B> — Venn breakdown + verdict word (issue #53)
    // Useful for sanity-checking selection equivalence after a refactor
    // (`:cmp old_paratope vs new_paratope`) or confirming a `byres` /
    // `within` expansion didn't drag in extra residues. The trailing
    // verdict word (equal / A⊆B / B⊆A / disjoint / overlap) is grep-able
    // in scripts that don't yet have a real `:if`.
    reg.registerCmd("cmp", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        constexpr const char* kUsage = "Usage: :cmp <expr-A> vs <expr-B>";
        if (cmd.args.empty()) return {false, kUsage};

        // Split on `vs` — `=` / `==` / `,` would all be ambiguous
        // (= is :select name=expr; , can appear inside rgb(R,G,B); ==
        // suggests a boolean predicate, which this isn't).
        auto [aArgs, exprB] = splitAtToken(cmd.args, "vs");
        std::string exprA = joinArgs(aArgs, 0, aArgs.size());
        if (exprA.empty() || exprB.empty()) return {false, kUsage};

        // Pair each side's per-object selections by object identity and
        // compute the Venn breakdown within the same object — comparing
        // indices across objects would mix index spaces, so cross-object
        // pairs are treated as disjoint.
        std::unordered_map<MolObject*, Selection> aMap, bMap;
        forEachInScope(app, exprA, [&](ScopedTarget& t) {
            aMap.emplace(t.obj.get(), std::move(t.sel)); return true;
        });
        forEachInScope(app, exprB, [&](ScopedTarget& t) {
            bMap.emplace(t.obj.get(), std::move(t.sel)); return true;
        });
        if (aMap.empty() && bMap.empty()) return {false, "No object selected"};

        size_t aSize = 0, bSize = 0, both = 0;
        for (const auto& [obj, sel] : aMap) {
            aSize += sel.size();
            auto bit = bMap.find(obj);
            if (bit != bMap.end()) both += (sel & bit->second).size();
        }
        for (const auto& [obj, sel] : bMap) bSize += sel.size();
        size_t aMinusB = aSize - both;
        size_t bMinusA = bSize - both;

        const char* verdict;
        if (aSize == bSize && aMinusB == 0)            verdict = "equal";
        else if (aMinusB == 0)                          verdict = "A⊆B";
        else if (bMinusA == 0)                          verdict = "B⊆A";
        else if (both == 0)                             verdict = "disjoint";
        else                                            verdict = "overlap";

        std::string msg = "cmp:";
        msg += "  |A|="    + std::to_string(aSize);
        msg += "  |B|="    + std::to_string(bSize);
        msg += "  |A∩B|="  + std::to_string(both);
        msg += "  |A\\B|=" + std::to_string(aMinusB);
        msg += "  |B\\A|=" + std::to_string(bMinusA);
        msg += "  →  ";
        msg += verdict;
        return {true, msg};
    }, ":cmp <expr-A> vs <expr-B>",
       "Compare two selections: |A| |B| |A∩B| |A\\B| |B\\A| + verdict (equal/A⊆B/B⊆A/disjoint/overlap)",
       {":cmp chain A vs chain B",
        ":cmp $paratope vs byres within 5 of $antigen",
        ":cmp $old_sele vs $new_sele"}, "Selection");

    // :sele — list named selections
    reg.registerCmd(kSele, [](Application& app, const ParsedCommand&) -> ExecResult {
        auto& sels = app.namedSelections();
        if (sels.empty()) return {true, "No named selections"};
        std::string result = "Selections:";
        for (const auto& [name, sel] : sels) {
            result += " " + name + "(" + std::to_string(sel.size()) + ")";
        }
        return {true, result};
    }, ":sele", "List all named selections in the current session",
       {":sele"}, "Selection");

    // ── :align / :mmalign / :alignto ───────────────────────────────────────
    //
    // Syntax (canonical):
    //   :align    <mob> [sel] to <ref> [sel] [tm|mm]
    //   :alignto  <ref> [sel]                                — mob = current obj
    //   :alignto  <mob_sel> to <ref> [sel] [tm|mm]
    // Legacy (no "to"):  :align <mob> <ref> [shared_sel]
    //
    // Algorithm picker:
    //   - tm trailing token  → force TM-align (single-chain)
    //   - mm trailing token  → force MM-align (multi-chain complex)
    //   - otherwise          → MM if either side spans >1 chain after
    //                          selection, else TM. Tie-break = TM.
    //
    // :mmalign / :mmalignto are hidden back-compat aliases that hard-force
    // MM. :super = TM-only alias.

}

}  // namespace molterm
