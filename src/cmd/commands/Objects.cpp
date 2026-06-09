// Objects / misc: :help/:clear/:delete/:rm/:copy/:extract/:split/:rename/:info/:chains.


#include "molterm/app/Application.h"
#include "molterm/cmd/CommandParser.h"
#include "molterm/cmd/CommandRegistry.h"
#include "molterm/cmd/CommandHelpers.h"
#include "molterm/cmd/Register.h"
#include "molterm/cmd/RegisterExpr.h"
#include "molterm/cmd/ExprContext.h"
#include "molterm/core/MolObject.h"
#include "molterm/core/Selection.h"
#include <sstream>

namespace molterm {

void Application::registerObjectCommands(CommandRegistry& reg) {
    // :help [cmd] — overview overlay (no args) or per-command help (one arg).
    // Per-command help displays usage, description, and registered examples.
    reg.registerCmd("help", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) {
            app.showCommandIndex();
            return {true, ""};
        }
        std::string name = cmd.args[0];
        if (!name.empty() && name.front() == ':') name.erase(0, 1);
        const CommandInfo* info = app.cmdRegistry().lookup(name);
        if (!info) return {false, "Unknown command: :" + name};
        app.showCommandHelp(*info);
        return {true, ""};
    }, ":help [cmd]", "Show command index, or detailed help for one command",
       {":help", ":help fetch", ":help :align"}, "Help");

    // :clear — wipe the current tab (or every tab + the global store with 'all')
    reg.registerCmd("clear", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.size() > 1 ||
            (cmd.args.size() == 1 && cmd.args[0] != kAllToken)) {
            return {false, "Usage: :clear [all]"};
        }
        bool wipeEverything = !cmd.args.empty();

        if (wipeEverything) {
            int total = 0;
            for (size_t i = 0; i < app.tabs().count(); ++i) {
                total += static_cast<int>(app.tabs().tab(i).objects().size());
                app.tabs().tab(i).clear();
            }
            // Drop every entry in the global store too — :clear-with-tabs
            // is only useful as a hermetic reset, and leaving orphans in
            // the store would defeat that.
            for (const auto& n : app.store().names()) app.store().remove(n);
            // PixelCanvas diffs against prevRgb_; without invalidate the
            // last frame's pixels survive into the now-empty viewport.
            if (app.canvas()) app.canvas()->invalidate();
            if (total == 0) return {true, "Already empty"};
            return {true, "Cleared all objects (" + std::to_string(total) + " total)"};
        }

        auto& tab = app.tabs().currentTab();
        int n = static_cast<int>(tab.objects().size());
        if (n == 0) return {true, "Tab is already empty"};
        tab.clear();
        if (app.canvas()) app.canvas()->invalidate();
        return {true, "Cleared " + std::to_string(n) + " object(s)"};
    },
    ":clear [all]",
    "Wipe the current tab; ':clear all' empties every tab and the global object store",
    {":clear", ":clear all"},
    "Window");

    // Shared :delete / :rm handler. Removes from both the ObjectStore
    // and the active tab — per-tab consumers (count / show / color /
    // forEachInScope) iterate the tab's shared_ptr vector, so a store-
    // only remove would leave them dereferencing a freed entry.
    auto deleteObjectCmd = [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto& tab = app.tabs().currentTab();
        std::string name;
        int tabIdx = -1;
        if (cmd.args.empty()) {
            auto obj = tab.currentObject();
            if (!obj) return {false, "No object selected"};
            name = obj->name();
            tabIdx = tab.selectedObjectIdx();
        } else {
            name = cmd.args[0];
            auto obj = app.store().get(name);
            if (!obj) return {false, "Object not found: " + name};
            const auto& objs = tab.objects();
            for (int i = 0; i < static_cast<int>(objs.size()); ++i) {
                if (objs[i] && objs[i]->name() == name) { tabIdx = i; break; }
            }
        }
        app.store().remove(name);
        if (tabIdx >= 0) tab.removeObject(tabIdx);
        if (app.canvas()) app.canvas()->invalidate();
        return {true, "Deleted " + name};
    };
    reg.registerCmd("delete", deleteObjectCmd,
        ":delete [name]", "Delete an object (defaults to the currently selected one)",
        {":delete", ":delete 1bna"}, "Window");
    // :rm — vim/unix-style alias for :delete.
    reg.registerCmd("rm", deleteObjectCmd,
        ":rm [name]", "Remove an object (alias for :delete)",
        {":rm", ":rm 1bna"}, "Window");

    // :copy [<src-obj-or-sel>] [as <newname>]
    // Non-destructive clone. The first token is treated as an object
    // name iff it matches an object loaded in the store; otherwise the
    // whole pre-`as` chunk is parsed as a selection expression, and a
    // new MolObject is built from the matching atoms via subset().
    // Selection-form bonds are kept iff both endpoints survive, and
    // per-atom state (color, alpha, repr masks) carries over.
    reg.registerCmd("copy", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto& tab = app.tabs().currentTab();
        auto [srcArgs, newName] = splitAtToken(cmd.args, "as");
        // splitAtToken returns the whole arg list as `srcArgs` when the
        // token is absent; we still need the joined-string form for the
        // selection-grammar path that expects `chain A and resi 5-10`.
        std::string srcSpec = joinArgs(srcArgs, 0, srcArgs.size());

        // Whole-object path: srcSpec is empty (current) or names a
        // loaded object. Always-try-store-first so a literal name like
        // `1ubq` doesn't accidentally parse as a selection expression.
        if (srcSpec.empty() || app.store().get(srcSpec)) {
            std::string objName = srcSpec;
            if (objName.empty()) {
                auto cur = tab.currentObject();
                if (!cur) return {false, "No object selected"};
                objName = cur->name();
            }
            if (newName.empty()) newName = objName + "_copy";
            auto cloned = app.store().clone(objName, newName);
            if (!cloned) return {false, "Clone failed for " + objName};
            tab.addObject(cloned);
            return {true, "Copied " + objName + " -> " + cloned->name() +
                          " (" + std::to_string(cloned->atoms().size()) + " atoms)"};
        }

        // Selection path. Parse the spec against the current object
        // (consistent with :count's resolution — obj-qualified forms
        // like `1ubq/(chain A)` route through forEachInScope if we
        // ever extend, but :copy is single-object by design).
        auto src = tab.currentObject();
        if (!src) return {false, "No object selected"};
        auto sel = app.parseSelection(srcSpec, *src);
        if (sel.empty()) return {false, "No atoms match: " + srcSpec};
        if (newName.empty()) newName = src->name() + "_subset";
        auto sub = src->subset(sel.indices(), newName);
        auto added = app.store().add(std::move(sub));
        tab.addObject(added);
        return {true, "Copied selection -> " + added->name() +
                      " (" + std::to_string(added->atoms().size()) + " atoms from " +
                      src->name() + ")"};
    }, ":copy [<obj-or-sel>] [as <name>]",
       "Clone an object or a selection's atoms (non-destructive). Object form deep-copies the whole MolObject; selection form runs subset()+bond-remap. Auto-names <name>_copy (object) or <name>_subset (selection) when 'as' is omitted.",
       {":copy", ":copy 1ubq", ":copy as backup",
        ":copy chain A as just_A", ":copy byres within 5 of $hem as binding_site"}, "Window");

    // :extract <sel> [as <name>]
    // Destructive counterpart of `:copy <sel>` — creates the new object
    // exactly as :copy would, then removes those atoms from the source.
    // Useful for "carve TCR out of TCR-pMHC complex for independent
    // alignment" workflows where the user wants the carved piece to
    // stand alone and the source to no longer contain it.
    reg.registerCmd("extract", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty())
            return {false, "Usage: :extract <selection> [as <name>]"};
        auto& tab = app.tabs().currentTab();
        auto src = tab.currentObject();
        if (!src) return {false, "No object selected"};

        auto [exprArgs, newName] = splitAtToken(cmd.args, "as");
        std::string expr = joinArgs(exprArgs, 0, exprArgs.size());
        if (expr.empty()) return {false, "Usage: :extract <selection> [as <name>]"};
        auto sel = app.parseSelection(expr, *src);
        if (sel.empty()) return {false, "No atoms match: " + expr};
        if (sel.size() == src->atoms().size())
            return {false, "Refusing to extract every atom — the source "
                           "would be left empty. Use :rename if that's what you want."};
        if (newName.empty()) newName = src->name() + "_extract";

        auto sub = src->subset(sel.indices(), newName);
        size_t newAtoms = sub->atoms().size();
        auto added = app.store().add(std::move(sub));
        tab.addObject(added);
        src->removeAtoms(sel.indices());
        return {true, "Extracted " + std::to_string(newAtoms) + " atoms to " +
                      added->name() + "; " + src->name() + " now has " +
                      std::to_string(src->atoms().size()) + " atoms"};
    }, ":extract <selection> [as <name>]",
       "Cut atoms out of the current object into a new MolObject (destructive). Like :copy <sel> but the source loses those atoms.",
       {":extract chain A as tcr_a_alone",
        ":extract resi 50-60",
        ":extract byres within 5 of $hem as binding_site"}, "Window");

    // :split <obj> by chain
    // Non-destructive: for each chain in <obj>, build a new MolObject
    // containing just that chain's atoms. The source is left intact —
    // user can :rm it after if they want pure chain-objects.
    reg.registerCmd("split", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        constexpr const char* kUsage = "Usage: :split [<obj>] by chain";
        if (cmd.args.empty()) return {false, kUsage};
        auto& tab = app.tabs().currentTab();
        std::string objName, by;
        // Accept both `:split by chain` (uses current) and `:split 1abc by chain`.
        if (cmd.args[0] == "by") {
            auto cur = tab.currentObject();
            if (!cur) return {false, "No object selected"};
            objName = cur->name();
            by = cmd.args.size() > 1 ? cmd.args[1] : "";
        } else {
            objName = cmd.args[0];
            if (cmd.args.size() < 3 || cmd.args[1] != "by")
                return {false, kUsage};
            by = cmd.args[2];
        }
        if (by != "chain")
            return {false, "Only `by chain` is supported. Other split modes "
                           "(by model, by resname, ...) may land later."};
        auto src = app.store().get(objName);
        if (!src) return {false, "Object not found: " + objName};

        // Group atom indices by chainId in source order. std::map keeps
        // chain insertion order via lexicographic key, which is the
        // same order PDB chains usually appear in.
        std::map<std::string, std::vector<int>> byChain;
        for (int i = 0; i < static_cast<int>(src->atoms().size()); ++i) {
            byChain[src->atoms()[i].chainId].push_back(i);
        }
        if (byChain.empty()) return {false, "No atoms in " + objName};

        std::string msg = "Split " + objName + " into";
        int n = 0;
        for (const auto& [chainId, indices] : byChain) {
            std::string newName = objName + "_" + chainId;
            auto sub = src->subset(indices, newName);
            auto added = app.store().add(std::move(sub));
            tab.addObject(added);
            msg += " " + added->name() + "(" + std::to_string(indices.size()) + ")";
            ++n;
        }
        return {true, msg + "  [" + std::to_string(n) + " chains]"};
    }, ":split [<obj>] by chain",
       "Build one new MolObject per chain of <obj> (or current). Source unchanged — :rm it after if you want pure chain-objects.",
       {":split by chain", ":split 1abc by chain"}, "Window");

    // :rename
    reg.registerCmd("rename", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :rename <new_name> or :rename <old> <new>"};
        // Renaming to the current name is a no-op rather than an error, so
        // scripts that defensively assert a canonical name after :load (which
        // auto-names from the file stem) stay one-liners.
        if (cmd.args.size() < 2) {
            auto obj = app.tabs().currentTab().currentObject();
            if (!obj) return {false, "No object selected"};
            std::string oldName = obj->name();
            if (oldName == cmd.args[0]) return {true, "Already named " + oldName};
            if (app.store().rename(oldName, cmd.args[0]))
                return {true, "Renamed " + oldName + " -> " + cmd.args[0]};
            return {false, "Failed to rename"};
        }
        if (cmd.args[0] == cmd.args[1]) return {true, "Already named " + cmd.args[0]};
        if (app.store().rename(cmd.args[0], cmd.args[1]))
            return {true, "Renamed " + cmd.args[0] + " -> " + cmd.args[1]};
        return {false, "Failed to rename"};
    }, ":rename [old] <new>", "Rename an object (single arg renames the current object; no-op if already that name)",
       {":rename ref", ":rename 1bna ref"}, "Window");

    // :info
    reg.registerCmd("info", [](Application& app, const ParsedCommand&) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        return {true, obj->name() + ": " +
               std::to_string(obj->atoms().size()) + " atoms, " +
               std::to_string(obj->bonds().size()) + " bonds"};
    }, ":info", "Show atom/bond counts and metadata for the current object",
       {":info"}, "Session");

    // :chains — per-chain summary table for the current object (issue #103).
    // Surfaces the facts you need to drive a multi-chain recipe (which chain
    // is the peptide / MHC / TCR, its residue range, the conserved-Cys
    // positions) without dropping out to an external mmCIF parser. Prints in
    // --no-tui like :info, so figure scripts can read it back.
    reg.registerCmd("chains", [](Application& app, const ParsedCommand&) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        const auto& atoms = obj->atoms();
        if (atoms.empty()) return {false, obj->name() + ": no atoms"};

        static const std::set<std::string> kStdAA = {
            "ALA","ARG","ASN","ASP","CYS","GLN","GLU","GLY","HIS","ILE",
            "LEU","LYS","MET","PHE","PRO","SER","THR","TRP","TYR","VAL",
            "MSE","SEC","PYL"};
        static const std::set<std::string> kNucleic = {
            "A","C","G","U","T","DA","DC","DG","DT","DU","I"};

        struct ChainInfo {
            std::string id;
            int natoms = 0, loRes = 0, hiRes = 0;
            bool haveRes = false;
            std::set<std::pair<int,char>> residues;            // (resSeq, insCode)
            std::vector<int> cys;                              // CYS resSeq, in order
            std::set<int> cysSeen;
            int nProtein = 0, nNucleic = 0, nWater = 0, nOther = 0;
        };
        std::vector<ChainInfo> chains;
        std::unordered_map<std::string,int> idx;
        for (const auto& a : atoms) {
            auto it = idx.find(a.chainId);
            ChainInfo* c;
            if (it == idx.end()) {
                idx[a.chainId] = static_cast<int>(chains.size());
                chains.push_back({}); c = &chains.back(); c->id = a.chainId;
            } else c = &chains[it->second];
            c->natoms++;
            c->residues.insert({a.resSeq, a.insCode});
            if (!c->haveRes) { c->loRes = c->hiRes = a.resSeq; c->haveRes = true; }
            else { c->loRes = std::min(c->loRes, a.resSeq); c->hiRes = std::max(c->hiRes, a.resSeq); }
            if (a.resName == "HOH" || a.resName == "WAT") c->nWater++;
            else if (kStdAA.count(a.resName))             c->nProtein++;
            else if (kNucleic.count(a.resName))           c->nNucleic++;
            else                                          c->nOther++;
            if (a.resName == "CYS" && !c->cysSeen.count(a.resSeq)) {
                c->cysSeen.insert(a.resSeq); c->cys.push_back(a.resSeq);
            }
        }

        std::ostringstream os;
        os << obj->name() << " — " << chains.size()
           << (chains.size() == 1 ? " chain\n" : " chains\n");
        os << "chain  kind     nres  resi-range   natoms  CYS\n";
        for (const auto& c : chains) {
            int mx = std::max(std::max(c.nProtein, c.nNucleic),
                              std::max(c.nWater, c.nOther));
            const char* kind = (c.nProtein == mx) ? "protein"
                             : (c.nNucleic == mx) ? "nucleic"
                             : (c.nWater   == mx) ? "water"   : "hetero";
            std::string range = std::to_string(c.loRes) + "-" + std::to_string(c.hiRes);
            std::string cysStr;
            for (size_t k = 0; k < c.cys.size(); ++k) {
                if (k) cysStr += ",";
                cysStr += std::to_string(c.cys[k]);
            }
            if (cysStr.empty()) cysStr = "-";
            char row[256];
            std::snprintf(row, sizeof(row), "%-5s  %-7s  %4d  %-11s  %6d  %s\n",
                          c.id.c_str(), kind, static_cast<int>(c.residues.size()),
                          range.c_str(), c.natoms, cysStr.c_str());
            os << row;
        }
        std::string out = os.str();
        if (!out.empty() && out.back() == '\n') out.pop_back();
        return {true, out};
    }, ":chains",
       "Per-chain summary of the current object: kind, residue count, "
       "residue-number range, atom count, and conserved-Cys positions "
       "(handy for picking chain ids / Cys residues to drive a recipe).",
       {":chains"}, "Session");

}

}  // namespace molterm
