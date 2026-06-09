// Structural alignment: :align/:alignto/:super/:mmalign/:mmalignto/:loadalign.


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
#include "molterm/app/PathPatterns.h"

namespace molterm {

void Application::registerAlignmentCommands(CommandRegistry& reg) {
    // Trailing modifiers for :align — recognised in any order at the end
    // of the argument list. chain=A,B  shortcuts to both sides; chain1=
    // and chain2= are per-side overrides (issue #81).
    struct AlignMods {
        AlignMode mode = AlignMode::Auto;
        bool automap = false;
        std::string chain1;  // mobile-side chain list, e.g. "A,B,C"
        std::string chain2;  // target-side chain list
    };
    // Build a selection expression "(chain A or chain B or …)" from a
    // comma-separated chain-id list. Empty input → empty expression so
    // callers can blindly compose with `and`.
    auto chainListToExpr = [](const std::string& csv) -> std::string {
        if (csv.empty()) return std::string();
        std::string out = "(";
        bool first = true;
        size_t i = 0;
        while (i < csv.size()) {
            size_t j = csv.find(',', i);
            if (j == std::string::npos) j = csv.size();
            std::string tok = csv.substr(i, j - i);
            // trim
            while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front()))) tok.erase(tok.begin());
            while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back())))  tok.pop_back();
            if (!tok.empty()) {
                if (!first) out += " or ";
                out += "chain " + tok;
                first = false;
            }
            i = j + 1;
        }
        out += ")";
        return out;
    };
    // Pop trailing modifier tokens (tm/mm/automap/chain=/chain1=/chain2=)
    // from `toks`. Order-insensitive; consumes a run of recognised tokens
    // off the back so the remaining vector holds only the object + selection
    // tokens.
    auto popModeToken = [](std::vector<std::string>& toks) -> AlignMods {
        AlignMods m;
        while (!toks.empty()) {
            const std::string& last = toks.back();
            if      (last == "tm")      { m.mode = AlignMode::ForceTM; toks.pop_back(); }
            else if (last == "mm")      { m.mode = AlignMode::ForceMM; toks.pop_back(); }
            else if (last == "automap") { m.automap = true;            toks.pop_back(); }
            else if (last.rfind("chain=", 0) == 0) {
                std::string v = last.substr(6);
                m.chain1 = v; m.chain2 = v;
                toks.pop_back();
            }
            else if (last.rfind("chain1=", 0) == 0) { m.chain1 = last.substr(7); toks.pop_back(); }
            else if (last.rfind("chain2=", 0) == 0) { m.chain2 = last.substr(7); toks.pop_back(); }
            else break;
        }
        return m;
    };

    auto chainCount = [](const MolObject& obj, const std::vector<int>& atoms) -> int {
        std::set<std::string> chains;
        if (atoms.empty()) {
            for (const auto& a : obj.atoms()) chains.insert(a.chainId);
        } else {
            const auto& all = obj.atoms();
            for (int idx : atoms) {
                if (idx >= 0 && idx < static_cast<int>(all.size()))
                    chains.insert(all[idx].chainId);
            }
        }
        return static_cast<int>(chains.size());
    };

    // Shared dispatcher. mobile/target are pre-resolved shared_ptrs (the
    // caller decides whether the mobile name comes from cmd.args[0] or
    // from currentObject()). mobileLabel is the name to print in the
    // result message.
    auto runAlign = [chainCount, chainListToExpr](Application& app,
                                  std::shared_ptr<MolObject> mobile,
                                  std::shared_ptr<MolObject> target,
                                  const std::string& mobileLabel,
                                  const std::string& targetLabel,
                                  std::string mobileExpr,
                                  std::string targetExpr,
                                  AlignMods mods) -> ExecResult {
        // chain=/chain1=/chain2= shorthand (issue #81): fold the chain
        // list into the selection expressions before atom resolution, so
        // the downstream temp-PDB writer sees only the requested chains.
        if (!mods.chain1.empty()) {
            std::string c = chainListToExpr(mods.chain1);
            mobileExpr = mobileExpr.empty() ? c : "(" + mobileExpr + ") and " + c;
        }
        if (!mods.chain2.empty()) {
            std::string c = chainListToExpr(mods.chain2);
            targetExpr = targetExpr.empty() ? c : "(" + targetExpr + ") and " + c;
        }
        AlignMode mode = mods.mode;
        if (mods.automap) {
            // USalign's `-mm 1` (which molterm passes whenever the alignment
            // is `complex`) already does optimal chain pairing + permutation
            // internally. With automap alone, no caller selection is allowed
            // — USalign sees the whole assembly. With `chain=` shorthand the
            // chain restriction *is* the selection, so we let those through
            // and forbid only the free-form per-side expressions.
            bool sharedSel = (mobileExpr == targetExpr) && !mobileExpr.empty();
            if ((mods.chain1.empty() && mods.chain2.empty()) &&
                (!mobileExpr.empty() || !targetExpr.empty()) && !sharedSel) {
                return {false,
                    "automap takes no per-side selections: use `chain=A,B,…` or drop the sel args"};
            }
            mode = AlignMode::ForceMM;
        }

        std::vector<int> mobileAtoms, targetAtoms;
        if (!mobileExpr.empty()) {
            auto mSel = app.parseSelection(mobileExpr, *mobile);
            mobileAtoms = std::vector<int>(mSel.indices().begin(), mSel.indices().end());
            if (mobileAtoms.empty())
                return {false, "Mobile selection empty: " + mobileExpr};
        }
        if (!targetExpr.empty()) {
            auto tSel = app.parseSelection(targetExpr, *target);
            targetAtoms = std::vector<int>(tSel.indices().begin(), tSel.indices().end());
            if (targetAtoms.empty())
                return {false, "Target selection empty: " + targetExpr};
        }

        bool complex;
        switch (mode) {
            case AlignMode::ForceTM: complex = false; break;
            case AlignMode::ForceMM: complex = true;  break;
            default:
                complex = chainCount(*mobile, mobileAtoms) > 1
                       || chainCount(*target, targetAtoms) > 1;
        }

        auto result = complex
            ? Aligner::alignComplex(*mobile, *target, mobileAtoms, targetAtoms)
            : Aligner::align(*mobile, *target, mobileAtoms, targetAtoms);
        app.logAlignPair(mobileLabel, targetLabel, complex, result);
        if (!result.success) return {false, "Align failed: " + result.message};

        Aligner::applyTransform(*mobile, result);
        std::string prefix = complex ? "MM-" : "TM-";
        return {true, prefix + "aligned " + mobileLabel + " → " +
                      targetLabel + " | " + result.message};
    };

    // Parse :align-style args into (mobileName, mobileExpr, targetName,
    // targetExpr, mode). Mobile is taken from cmd.args[0] when
    // mobileFromCurrent==false; otherwise the entire arg list represents
    // ref-side syntax (a target name + optional selection, with an
    // optional `to` to introduce mobile-side selection on the current
    // object).
    auto joinTokens = [](const std::vector<std::string>& toks,
                         size_t begin = 0,
                         size_t end = std::string::npos) -> std::string {
        if (end == std::string::npos) end = toks.size();
        std::string s;
        for (size_t i = begin; i < end; ++i) {
            if (!s.empty()) s += ' ';
            s += toks[i];
        }
        return s;
    };

    auto parseAlignArgs = [popModeToken, joinTokens](
        const ParsedCommand& cmd, bool mobileFromCurrent,
        std::string& mobileName, std::string& mobileExpr,
        std::string& targetName, std::string& targetExpr,
        AlignMods& mods, std::string& err) -> bool {

        std::vector<std::string> args = cmd.args;
        const char* usage = mobileFromCurrent
            ? "Usage: :alignto <target> [sel] | <mobile_sel> to <target> [target_sel] [tm|mm] [automap] [chain=A,B]"
            : "Usage: :align <obj> [sel] to <obj> [sel] [tm|mm] [automap] [chain=A,B|chain1=…|chain2=…]";

        int toIdx = -1;
        for (int i = 0; i < static_cast<int>(args.size()); ++i) {
            if (args[i] == "to") { toIdx = i; break; }
        }

        if (toIdx >= 0) {
            std::vector<std::string> left(args.begin(), args.begin() + toIdx);
            std::vector<std::string> right(args.begin() + toIdx + 1, args.end());
            mods = popModeToken(right);
            if (right.empty()) { err = "Missing target after 'to'"; return false; }
            targetName = right[0];
            targetExpr = joinTokens(right, 1);
            if (mobileFromCurrent) {
                mobileExpr = joinTokens(left);
            } else {
                if (left.empty()) { err = usage; return false; }
                mobileName = left[0];
                mobileExpr = joinTokens(left, 1);
            }
            return true;
        }

        // No 'to' separator.
        mods = popModeToken(args);
        if (mobileFromCurrent) {
            if (args.empty()) { err = usage; return false; }
            targetName = args[0];
            targetExpr = joinTokens(args, 1);
            return true;
        }
        // Legacy two-name form: :align <mob> <ref> [shared_sel] [tm|mm] [automap]
        if (args.size() < 2) { err = usage; return false; }
        mobileName = args[0];
        targetName = args[1];
        mobileExpr = joinTokens(args, 2);
        targetExpr = mobileExpr;
        return true;
    };

    // :align — auto-detect TM vs MM by chain count
    auto doAlignByName = [parseAlignArgs, runAlign]
        (Application& app, const ParsedCommand& cmd, AlignMode forced)
        -> ExecResult {
        std::string mobileName, mobileExpr, targetName, targetExpr, err;
        AlignMods mods;
        if (!parseAlignArgs(cmd, /*mobileFromCurrent=*/false,
                            mobileName, mobileExpr, targetName, targetExpr,
                            mods, err)) return {false, err};
        if (forced != AlignMode::Auto) mods.mode = forced;
        auto mobile = app.store().get(mobileName);
        auto target = app.store().get(targetName);
        if (!mobile) return {false, "Object not found: " + mobileName};
        if (!target) return {false, "Object not found: " + targetName};
        return runAlign(app, mobile, target, mobileName, targetName,
                        mobileExpr, targetExpr, mods);
    };

    // :alignto always broadcasts: every non-target object in the current
    // tab is superposed onto target. The `to` keyword is purely a separator
    // between mobile-side and target-side selections — not a mode switch —
    // so the command works regardless of which object is currently selected
    // (in particular, it doesn't degenerate to identity when the current
    // object happens to be the target).
    auto doAlignTo = [parseAlignArgs, runAlign]
        (Application& app, const ParsedCommand& cmd, AlignMode forced)
        -> ExecResult {
        std::string mobileName, mobileExpr, targetName, targetExpr, err;
        AlignMods mods;
        if (!parseAlignArgs(cmd, /*mobileFromCurrent=*/true,
                            mobileName, mobileExpr, targetName, targetExpr,
                            mods, err)) return {false, err};
        if (forced != AlignMode::Auto) mods.mode = forced;
        auto target = app.store().get(targetName);
        if (!target) return {false, "Object not found: " + targetName};

        const auto& objs = app.tabs().currentTab().objects();
        std::vector<std::shared_ptr<MolObject>> mobiles;
        for (const auto& o : objs) {
            if (o && o.get() != target.get()) mobiles.push_back(o);
        }
        if (mobiles.empty())
            return {false, "No other objects in current tab to align onto " + targetName};

        int okCount = 0;
        std::string combined;
        for (auto& m : mobiles) {
            auto r = runAlign(app, m, target, m->name(), targetName,
                              mobileExpr, targetExpr, mods);
            if (r.ok) ++okCount;
            if (!combined.empty()) combined += "; ";
            combined += r.msg;
        }
        return {okCount > 0, combined};
    };

    reg.registerCmd("align",
        [doAlignByName](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return doAlignByName(app, cmd, AlignMode::Auto);
        },
        ":align <obj> [sel] to <obj> [sel] [tm|mm] [automap] [chain=A,B|chain1=…|chain2=…]",
        "Superpose structures (auto-pick TM vs MM by chain count; trailing tm/mm forces; "
        "automap = sequence-based chain pairing for reordered deposits; "
        "chain= restricts both sides to listed chains, chain1=/chain2= per-side)",
        {":align mob to ref",
         ":align mob chain A to ref chain A",
         ":align mob to ref chain=C,D,E",
         ":align complex1 to complex2 mm",
         ":align top1 to ref_8yiv automap chain=C,D,E"},
        "Analysis");

    reg.registerCmd("alignto",
        [doAlignTo](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return doAlignTo(app, cmd, AlignMode::Auto);
        },
        ":alignto <target> [sel] | <mobile_sel> to <target> [target_sel] [tm|mm] [automap] [chain=A,B]",
        "Superpose every other object in the tab onto target (auto TM/MM; trailing tm/mm forces; automap pairs chains by sequence; chain=A,B restricts both sides)",
        {":alignto ref",
         ":alignto chain A to ref chain A",
         ":alignto chain A+B to model chain A+B",
         ":alignto ref chain=C,D,E",
         ":alignto ref mm",
         ":alignto ref_8yiv automap"},
        "Analysis");

    // :super — TM-only alias for :align (kept for back-compat with existing
    // scripts; auto-mode would surprise users who expect :super to never
    // run MM, so this stays explicitly TM-forced).
    reg.registerCmd("super",
        [doAlignByName](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return doAlignByName(app, cmd, AlignMode::ForceTM);
        },
        ":super <obj> [sel] to <obj> [sel]",
        "Superpose structures (alias for :align tm)",
        {":super mob to ref"},
        "Analysis");

    // Hidden back-compat aliases that hard-force MM mode.
    reg.registerCmd("mmalign",
        [doAlignByName](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return doAlignByName(app, cmd, AlignMode::ForceMM);
        },
        ":mmalign <obj> [sel] to <obj> [sel]",
        "Deprecated alias for ':align ... mm'",
        {":mmalign complex1 to complex2"},
        "Hidden");

    reg.registerCmd("mmalignto",
        [doAlignTo](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return doAlignTo(app, cmd, AlignMode::ForceMM);
        },
        ":mmalignto <target> [sel]",
        "Deprecated alias for ':alignto ... mm'",
        {":mmalignto complex2"},
        "Hidden");

    reg.registerCmd("loadalign",
        [runAlign, popModeToken, joinTokens](Application& app, const ParsedCommand& cmd) -> ExecResult {
            if (cmd.args.empty())
                return {false, "Usage: :loadalign <pattern> [sel] [tm|mm]"};

            constexpr const char* kUsage =
                "Usage: :loadalign <pattern> [sel] [tm|mm] [automap] [chain=A,B]";
            std::vector<std::string> args = cmd.args;
            AlignMods mods = popModeToken(args);
            if (args.empty()) return {false, kUsage};

            // Selection grammar has no terminator, so without invoking
            // the lexer we split heuristically: the first token that can
            // begin a primary selector marks the boundary between file
            // patterns and the (shared) selection expression. Selection
            // owns the keyword list so adding a keyword there can't
            // silently break this split.
            std::string sharedExpr;
            for (size_t i = 0; i < args.size(); ++i) {
                if (Selection::isPrimaryKeyword(args[i])) {
                    sharedExpr = joinTokens(args, i);
                    args.resize(i);
                    break;
                }
            }
            if (args.empty()) return {false, kUsage};

            std::vector<std::string> matches = expandPathPatterns(args);

            if (matches.empty())
                return {false, "No files matched pattern(s): " +
                               cmd.args[0] + (cmd.args.size() > 1 ? " ..." : "")};

            // Detect successful loads via tab-size delta — robust to
            // future changes in loadFile's status-string format.
            auto& tab = app.tabs().currentTab();
            std::vector<std::shared_ptr<MolObject>> loaded;
            std::vector<std::string> errors;
            for (const auto& path : matches) {
                size_t before = tab.objects().size();
                std::string msg = app.loadFile(path);
                if (tab.objects().size() > before) {
                    loaded.push_back(tab.objects().back());
                } else {
                    errors.push_back(path + ": " + msg);
                }
            }
            if (loaded.empty())
                return {false, "All loads failed; first error: " +
                               (errors.empty() ? std::string("(none)") : errors[0])};

            std::string summary = "Loaded " + std::to_string(loaded.size()) +
                                  " structure(s)";
            if (loaded.size() < 2) {
                if (!errors.empty()) summary += " (" + std::to_string(errors.size()) + " failed)";
                return {true, summary + ": " + loaded.front()->name()};
            }

            const std::string& targetName = loaded.front()->name();
            int aligned = 0, alignFailed = 0;
            std::string detail;
            for (size_t i = 1; i < loaded.size(); ++i) {
                auto r = runAlign(app, loaded[i], loaded.front(),
                                  loaded[i]->name(), targetName,
                                  sharedExpr, sharedExpr, mods);
                if (r.ok) { ++aligned; detail += "\n  " + r.msg; }
                else { ++alignFailed; detail += "\n  " + loaded[i]->name() + ": " + r.msg; }
            }
            summary += " from '" + cmd.args[0] + "'; aligned " +
                       std::to_string(aligned) + " to " + targetName;
            if (!sharedExpr.empty()) summary += " on '" + sharedExpr + "'";
            if (alignFailed) summary += " (" + std::to_string(alignFailed) + " failed)";
            return {true, summary + detail};
        },
        ":loadalign <pattern> [sel] [tm|mm]",
        "Load every file matching the glob/brace pattern; align models 2..N onto the first (optional shared selection, e.g. confident domain)",
        {":loadalign relaxed_model_*.pdb",
         ":loadalign relaxed_model_{1..5}.pdb",
         ":loadalign model_?.cif chain A+B",
         ":loadalign models/*.cif mm"},
        "Files");

}

}  // namespace molterm
