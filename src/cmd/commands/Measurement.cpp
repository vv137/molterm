// Measurement + analysis: :measure/:bond/:angle/:dihedral/:hbonds/:arrow/:contactmap/:dssp/:sasa/...


#include "molterm/app/Application.h"
#include "molterm/core/StringParse.h"
#include "molterm/cmd/CommandParser.h"
#include "molterm/cmd/CommandRegistry.h"
#include "molterm/cmd/CommandHelpers.h"
#include "molterm/cmd/Register.h"
#include "molterm/cmd/RegisterExpr.h"
#include "molterm/cmd/ExprContext.h"
#include "molterm/core/MolObject.h"
#include "molterm/core/Selection.h"
#include "molterm/app/CommandScope.h"
#include "molterm/core/Logger.h"
#include "molterm/core/SASA.h"
#include <memory>
#include "molterm/analysis/ContactMap.h"
#include "molterm/core/BondTable.h"

namespace molterm {

// Endpoint/bond resolution helpers, used only by the measurement and
// bond commands below (moved out of Application.cpp with their commands).
// Resolve a single endpoint token to an atom index — used by the
// distance/bond commands (:measure, :bond, :arrow). Accepts a pk1..pk4
// pick, a $named-selection (first atom), or a PDB serial number. Returns
// -1 when unresolved.
static int resolveEndpointToken(Application& app, const std::string& s) {
    if (s.size() >= 3 && s[0] == 'p' && s[1] == 'k' && s[2] >= '1' && s[2] <= '4')
        return app.pickReg(s[2] - '1');
    if (!s.empty() && s[0] == '$') {
        auto it = app.namedSelections().find(s.substr(1));
        if (it != app.namedSelections().end() && !it->second.empty())
            return it->second.indices()[0];
        return -1;
    }
    auto obj = app.tabs().currentTab().currentObject();
    if (!obj) return -1;
    const auto& atoms = obj->atoms();
    if (auto serial = parseInt(s)) {
        for (int i = 0; i < static_cast<int>(atoms.size()); ++i)
            if (atoms[i].serial == *serial) return i;
    }
    return -1;
}

// Split `s` into top-level selection endpoints — one per balanced
// parenthesized group, INCLUDING the parens and any object-qualifier prefix
// glued to the opening paren:
//   "(a (b) c) (d)"                     -> {"(a (b) c)", "(d)"}
//   "1crn/(resi 1) 1crn/(resi 2)"       -> {"1crn/(resi 1)", "1crn/(resi 2)"}
// Returns {} on unbalanced parens. Lets :measure/:bond take two selection
// expressions as positional args; keeping the `<obj>/` prefix is what makes
// the object qualifier survive on each endpoint (issue #101) — stripping it
// silently resolved both endpoints against the current object.
static std::vector<std::string> splitEndpointSelections(const std::string& s) {
    std::vector<std::string> groups;
    int depth = 0;
    size_t startPos = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '(') {
            if (depth == 0) {
                // Back up over a glued `<obj>/` prefix so the qualifier is
                // kept; stop at whitespace or an adjacent paren so we never
                // swallow the previous endpoint.
                startPos = i;
                while (startPos > 0 &&
                       !std::isspace(static_cast<unsigned char>(s[startPos - 1])) &&
                       s[startPos - 1] != ')' && s[startPos - 1] != '(')
                    --startPos;
            }
            ++depth;
        } else if (s[i] == ')') {
            if (depth == 0) return {};       // unbalanced ')'
            if (--depth == 0)
                groups.push_back(s.substr(startPos, i - startPos + 1));
        }
    }
    return depth == 0 ? groups : std::vector<std::string>{};
}

// Resolve a selection expression to a single atom, scope-aware (see
// resolveScoped). On success returns the owning object + atom index; on
// failure sets `err`. Empty / multi-atom selections are rejected — an
// endpoint must be unique.
static std::shared_ptr<MolObject> resolveSelectionToAtomScoped(
        Application& app, const std::string& expr, int& outIdx, std::string& err) {
    auto obj = app.tabs().currentTab().currentObject();
    if (!obj) { err = "No object selected"; return nullptr; }
    Selection sel = resolveScoped(app, expr, obj);
    if (sel.empty()) { err = "Empty selection: " + expr; return nullptr; }
    if (sel.size() != 1) {
        err = "Selection must resolve to exactly one atom (got " +
              std::to_string(sel.size()) + "): " + expr;
        return nullptr;
    }
    outIdx = sel.indices()[0];
    return obj;
}

// Resolve a pair of distance/bond endpoints from positional args. Accepts
//   (selA) (selB)  — two parenthesized selections, each one atom (issue #99)
//   tok1 tok2      — two serial/pick/$ endpoints (legacy)
// Both endpoints must live in the same object (a measurement/bond stores
// indices into one atom array). The `(selA) (selB)` form honors the
// `<obj>/(...)` qualifier and scoped `$name`, so it can target a non-current
// object — but a cross-object pair is rejected with a clear error rather
// than silently measuring unrelated atoms (issue #101). On success `obj` is
// the owning object. Returns false and sets `err` on any failure.
static bool resolveEndpointPair(Application& app,
                                const std::vector<std::string>& pos,
                                std::shared_ptr<MolObject>& obj,
                                int& i1, int& i2, std::string& err) {
    std::string joined;
    for (size_t k = 0; k < pos.size(); ++k) { if (k) joined += ' '; joined += pos[k]; }
    auto groups = splitEndpointSelections(joined);
    if (groups.size() == 2) {
        auto o1 = resolveSelectionToAtomScoped(app, groups[0], i1, err);
        if (!o1) return false;
        auto o2 = resolveSelectionToAtomScoped(app, groups[1], i2, err);
        if (!o2) return false;
        if (o1 != o2) {
            err = "Both endpoints must be in the same object (got " +
                  o1->name() + " and " + o2->name() + "); cross-object "
                  "endpoints aren't supported — qualify both with one object";
            return false;
        }
        obj = o1;
        return true;
    }
    if (!groups.empty()) {
        err = "Expected two parenthesized selections, e.g. (resi 2 and name SG) "
              "(resi 30 and name SG)";
        return false;
    }
    if (pos.size() >= 2) {
        obj = app.tabs().currentTab().currentObject();
        if (!obj) { err = "No object selected"; return false; }
        i1 = resolveEndpointToken(app, pos[0]);
        if (i1 < 0) { err = "Atom not found: " + pos[0]; return false; }
        i2 = resolveEndpointToken(app, pos[1]);
        if (i2 < 0) { err = "Atom not found: " + pos[1]; return false; }
        return true;
    }
    err = "Need two endpoints: serials, picks, or (selA) (selB)";
    return false;
}

// True if a bond between atom indices a and b already exists. Shared by
// :bond and :disulfide so the symmetric-pair check lives in one place.
static bool bondExists(const std::vector<BondData>& bonds, int a, int b) {
    for (const auto& bd : bonds)
        if ((bd.atom1 == a && bd.atom2 == b) || (bd.atom1 == b && bd.atom2 == a))
            return true;
    return false;
}

// Standard atomic weight (g/mol) by element symbol, for com() mass-weighting
// (issue #112). Covers the elements common in biomolecules and frequent
// ions/heteroatoms; anything unrecognized falls back to carbon's 12.011 so a
// stray element never zeroes out the weighted sum.
// Resolve the two endpoints for :bond / :unbond: empty args fall back to the
// pk1/pk2 picks, otherwise the shared serial/pick/(selection) grammar. `verb`
// names the command in the no-pick hint. Returns false and sets `err`.
static bool resolveBondEndpoints(Application& app, const std::vector<std::string>& pos,
                                 const char* verb, std::shared_ptr<MolObject>& obj,
                                 int& i1, int& i2, std::string& err) {
    if (pos.empty()) {
        obj = app.tabs().currentTab().currentObject();
        if (!obj) { err = "No object selected"; return false; }
        i1 = app.pickReg(0); i2 = app.pickReg(1);
        if (i1 >= 0 && i2 >= 0) return true;
        err = std::string("Click two atoms first (pk1, pk2), then :") + verb;
        return false;
    }
    return resolveEndpointPair(app, pos, obj, i1, i2, err);
}

void Application::registerMeasurementCommands(CommandRegistry& reg) {
    // Helper: resolve atom index from serial / pick / $named-selection.
    // Thin wrapper over the file-local resolveEndpointToken so :arrow,
    // :angle, :dihedral and :measure all share one resolution rule.
    auto resolveAtomIdx = [](Application& app, const std::string& s) -> int {
        return resolveEndpointToken(app, s);
    };

    // :measure [serial1 serial2] — distance (no args = pk1↔pk2)
    reg.registerCmd("measure", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto [pos, caption] = splitAtEqToken(cmd.args);

        std::shared_ptr<MolObject> obj;
        int i1, i2;
        bool minMode = false;   // closest-approach between multi-atom groups

        // Detect the two-parenthesized-selections form up front so multi-atom
        // groups switch to minimum-distance mode (issue #112): the closest
        // atom pair between the two groups. Single-atom groups behave exactly
        // as before. Serials/picks fall through to the legacy resolver.
        std::string joined;
        for (size_t k = 0; k < pos.size(); ++k) { if (k) joined += ' '; joined += pos[k]; }
        auto groups = pos.empty() ? std::vector<std::string>{}
                                  : splitEndpointSelections(joined);
        if (groups.size() == 2) {
            auto oA = app.tabs().currentTab().currentObject();
            if (!oA) return {false, "No object selected"};
            auto oB = oA;
            Selection selA = resolveScoped(app, groups[0], oA);
            Selection selB = resolveScoped(app, groups[1], oB);
            if (selA.empty()) return {false, "Empty selection: " + groups[0]};
            if (selB.empty()) return {false, "Empty selection: " + groups[1]};
            if (oA != oB)
                return {false, "Both endpoints must be in the same object (got " +
                               oA->name() + " and " + oB->name() + ")"};
            obj = oA;
            if (selA.size() == 1 && selB.size() == 1) {
                i1 = selA.indices()[0]; i2 = selB.indices()[0];
            } else {
                // Closest atom pair between the two groups (brute force —
                // groups are usually a residue or two).
                const auto& atoms = obj->atoms();
                float best = 0; bool have = false;
                for (int a : selA.indices()) {
                    for (int b : selB.indices()) {
                        if (a == b) continue;
                        float dx = atoms[a].x-atoms[b].x, dy = atoms[a].y-atoms[b].y,
                              dz = atoms[a].z-atoms[b].z;
                        float d2 = dx*dx + dy*dy + dz*dz;
                        if (!have || d2 < best) { best = d2; i1 = a; i2 = b; have = true; }
                    }
                }
                if (!have) return {false, "No atom pair to measure between the groups"};
                minMode = true;
            }
        } else if (pos.empty()) {
            obj = app.tabs().currentTab().currentObject();
            if (!obj) return {false, "No object selected"};
            i1 = app.pickRegs_[0]; i2 = app.pickRegs_[1];
            if (i1 < 0 || i2 < 0) return {false, "Click two atoms first (pk1, pk2), then :measure"};
        } else {
            std::string err;
            if (!resolveEndpointPair(app, pos, obj, i1, i2, err)) return {false, err};
        }
        const auto& atoms = obj->atoms();
        int n = static_cast<int>(atoms.size());
        if (i1 >= n || i2 >= n) return {false, "Invalid atom index"};

        float dist = geom::distance(atoms[i1].x, atoms[i1].y, atoms[i1].z,
                                    atoms[i2].x, atoms[i2].y, atoms[i2].z);

        char dbuf[16]; std::snprintf(dbuf, sizeof(dbuf), "%.2f", dist);
        std::string shortLabel = std::string(dbuf) + "A";
        std::string msg = std::string(minMode ? "Min distance " : "Distance ") +
            atoms[i1].label() + " — " + atoms[i2].label() + " = " + shortLabel;
        if (!caption.empty()) msg += "  \"" + caption + "\"";
        app.measurements().push_back({{i1, i2}, shortLabel, caption, obj->name()});
        MLOG_INFO("%s", msg.c_str());
        return {true, msg};
    }, ":measure [s1 s2 | pk1 pk2 | (selA) (selB)] [= \"caption\"]",
       "Measure a distance and persist a dashed line + value. Endpoints are "
       "PDB serials, the pk1/pk2 picks, or two parenthesized selections. When "
       "a selection resolves to more than one atom, :measure reports the "
       "closest approach (minimum heavy-atom distance) between the two groups.",
       {":measure", ":measure pk1 pk2",
        ":measure 12 47 = \"Glu-OE1 ↔ Lys-Nζ\"",
        ":measure (resi 2 and name SG) (resi 30 and name SG) = \"C28-C56 SS\"",
        ":measure (resi 72 and resn LYS) (resi 91 and resn GLU) = \"salt bridge\""},
       "Measurement");

    // :bond — draw an explicit bond between two atoms (issue #99). Adds a
    // real edge to the object's topology so it renders as a stick wherever
    // both endpoints are shown in a bond-drawing repr (wireframe/ballstick).
    // Endpoints follow the same grammar as :measure.
    reg.registerCmd("bond", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto [pos, caption] = splitAtEqToken(cmd.args);  // caption unused — bonds carry no label
        std::shared_ptr<MolObject> obj;
        int i1, i2;
        std::string err;
        if (!resolveBondEndpoints(app, pos, "bond", obj, i1, i2, err)) return {false, err};
        if (i1 == i2) return {false, "Cannot bond an atom to itself"};
        auto& bonds = obj->bonds();
        if (bondExists(bonds, i1, i2)) return {true, "Bond already exists"};
        bonds.push_back({std::min(i1, i2), std::max(i1, i2), 1});
        const auto& a = obj->atoms();
        float d = geom::distance(a[i1].x, a[i1].y, a[i1].z, a[i2].x, a[i2].y, a[i2].z);
        char buf[16]; std::snprintf(buf, sizeof(buf), "%.2f", d);
        return {true, std::string("Bond added (") + buf + " A)"};
    }, ":bond [s1 s2 | pk1 pk2 | (selA) (selB)]",
       "Draw a bond between two atoms (each endpoint must resolve to exactly "
       "one atom). Renders as a stick wherever both atoms are shown in a "
       "wireframe/ballstick repr.",
       {":bond pk1 pk2",
        ":bond (resi 2 and name SG) (resi 30 and name SG)"}, "Measurement");

    // :unbond — remove an existing bond between two atoms (issue #99). The
    // inverse of :bond/:disulfide; useful to drop a spurious auto-detected
    // edge. Same endpoint grammar.
    reg.registerCmd("unbond", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto [pos, caption] = splitAtEqToken(cmd.args);
        std::shared_ptr<MolObject> obj;
        int i1, i2;
        std::string err;
        if (!resolveBondEndpoints(app, pos, "unbond", obj, i1, i2, err)) return {false, err};
        auto& bonds = obj->bonds();
        auto before = bonds.size();
        bonds.erase(std::remove_if(bonds.begin(), bonds.end(), [&](const BondData& b) {
            return (b.atom1 == i1 && b.atom2 == i2) || (b.atom1 == i2 && b.atom2 == i1);
        }), bonds.end());
        if (bonds.size() == before) return {false, "No bond between those atoms"};
        return {true, "Bond removed"};
    }, ":unbond [s1 s2 | pk1 pk2 | (selA) (selB)]",
       "Remove a bond between two atoms (inverse of :bond / :disulfide).",
       {":unbond pk1 pk2",
        ":unbond (resi 2 and name SG) (resi 30 and name SG)"}, "Measurement");

    // :disulfide [selection] — auto-detect CYS SG–SG pairs and draw them as
    // bonds (issue #99). Disulfides are inter-residue, so loaders that only
    // apply intra-residue connectivity miss them; this fills the gap with a
    // one-liner that "just works" for figure scripts.
    reg.registerCmd("disulfide", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        const auto& atoms = obj->atoms();

        // Scope: whole structure by default, else the given selection.
        std::vector<int> scope;
        if (cmd.args.empty()) {
            scope.resize(atoms.size());
            for (int i = 0; i < static_cast<int>(atoms.size()); ++i) scope[i] = i;
        } else {
            std::string expr;
            for (size_t i = 0; i < cmd.args.size(); ++i) { if (i) expr += ' '; expr += cmd.args[i]; }
            Selection sel = app.parseSelection(expr, *obj);
            if (sel.empty()) return {false, "Empty selection: " + expr};
            scope = sel.indices();
        }

        // Collect candidate SG sulfur atoms in scope.
        std::vector<int> sgs;
        for (int idx : scope)
            if (atoms[idx].name == "SG") sgs.push_back(idx);
        if (sgs.size() < 2)
            return {false, "No SG–SG pairs in scope (need ≥2 cysteine SG atoms)"};

        auto& bonds = obj->bonds();

        // A disulfide S–S bond is ~2.05 Å; accept a tolerant 1.6–2.5 Å window.
        constexpr float kMin = 1.6f, kMax = 2.5f;
        int added = 0, already = 0;
        std::string detail;
        for (size_t i = 0; i < sgs.size(); ++i) {
            for (size_t j = i + 1; j < sgs.size(); ++j) {
                int a = sgs[i], b = sgs[j];
                float d = geom::distance(atoms[a].x, atoms[a].y, atoms[a].z,
                                         atoms[b].x, atoms[b].y, atoms[b].z);
                if (d < kMin || d > kMax) continue;
                if (bondExists(bonds, a, b)) { ++already; continue; }
                bonds.push_back({std::min(a, b), std::max(a, b), 1});
                ++added;
                char buf[16]; std::snprintf(buf, sizeof(buf), "%.2f", d);
                detail += (detail.empty() ? "  " : ", ");
                detail += atoms[a].chainId + std::to_string(atoms[a].resSeq) + "–" +
                          atoms[b].chainId + std::to_string(atoms[b].resSeq) +
                          " (" + buf + " A)";
            }
        }
        if (added == 0)
            return {already > 0
                        ? ExecResult{true, "All disulfides already bonded (" +
                                            std::to_string(already) + ")"}
                        : ExecResult{false, "No disulfide bonds found (SG–SG 1.6–2.5 A)"}};
        return {true, "Drew " + std::to_string(added) + " disulfide bond" +
                      (added == 1 ? "" : "s") + detail};
    }, ":disulfide [selection]",
       "Auto-detect cysteine SG–SG pairs (1.6–2.5 Å) and draw them as bonds. "
       "Defaults to the whole structure; pass a selection to limit scope.",
       {":disulfide", ":disulfide chain A"}, "Measurement");

    // :hbonds / :saltbridge — reuse ContactMap's interaction detection to
    // draw hydrogen bonds / salt bridges as persistent dashed contact lines
    // (issue #113), exactly like :measure: rendered live + into screenshots
    // and exported through `:export *.pml` as PyMOL distance objects.
    // Disulfides aside, this is the only way to annotate a non-covalent
    // interaction in 3D for a figure. Shared body, parameterized by type.
    auto drawInteractions = [](Application& app, const ParsedCommand& cmd,
                               InteractionType want, float cutoff,
                               const char* singular) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        const auto& atoms = obj->atoms();

        std::vector<int> scope;   // empty = whole structure
        if (!cmd.args.empty()) {
            std::string expr;
            for (size_t i = 0; i < cmd.args.size(); ++i) { if (i) expr += ' '; expr += cmd.args[i]; }
            Selection sel = app.parseSelection(expr, *obj);
            if (sel.empty()) return {false, "Empty selection: " + expr};
            scope = sel.indices();
        }

        // Intra-chain included (gap ≥ 2 skips peptide/phosphodiester
        // neighbors); priority dedup keeps one best contact per residue pair.
        auto contacts = ContactMap::detectInteractions(*obj, cutoff, false, 2, scope);

        auto& meas = app.measurements();
        // Pre-index this object's existing 2-atom measurements so the dedup
        // check is O(1) per contact, not O(measurements). Key packs the
        // ordered atom-index pair.
        auto pairKey = [](int a, int b) {
            return (static_cast<long long>(std::min(a, b)) << 32) |
                   static_cast<unsigned>(std::max(a, b));
        };
        // Index this object's existing 2-atom measurements by atom pair, so a
        // re-run *refreshes* a contact's distance in place (e.g. after :align
        // moved the coordinates) instead of reporting "already drawn" and
        // leaving the stale label — and never duplicates.
        std::unordered_map<long long, size_t> existing;
        for (size_t mi = 0; mi < meas.size(); ++mi)
            if (meas[mi].obj == obj->name() && meas[mi].atoms.size() == 2)
                existing[pairKey(meas[mi].atoms[0], meas[mi].atoms[1])] = mi;

        int added = 0, refreshed = 0;
        std::string detail;
        for (const auto& c : contacts) {
            if (c.type != want) continue;
            char dbuf[16]; std::snprintf(dbuf, sizeof(dbuf), "%.2f", c.distance);
            std::string lbl = std::string(dbuf) + "A";
            long long key = pairKey(c.atom1, c.atom2);
            auto it = existing.find(key);
            if (it != existing.end()) {
                meas[it->second].label = lbl;   // refresh distance, keep caption
                ++refreshed;
            } else {
                meas.push_back({{c.atom1, c.atom2}, lbl, "", obj->name()});
                existing[key] = meas.size() - 1;
                ++added;
            }
            if (added + refreshed <= 8) {
                const auto& a = atoms[c.atom1]; const auto& b = atoms[c.atom2];
                detail += (detail.empty() ? "  " : ", ");
                detail += a.chainId + std::to_string(a.resSeq) + "/" + a.name + "–" +
                          b.chainId + std::to_string(b.resSeq) + "/" + b.name +
                          " (" + dbuf + ")";
            }
        }
        if (added == 0 && refreshed == 0)
            return {false, std::string("No ") + singular + "s found in scope"};
        // An all-refresh re-run (e.g. after :align moved the coordinates) reads
        // as "Refreshed N", not "Drew 0 … (refreshed N)".
        std::string msg;
        if (added == 0) {
            msg = "Refreshed " + std::to_string(refreshed) + " " + singular +
                  (refreshed == 1 ? "" : "s");
        } else {
            msg = "Drew " + std::to_string(added) + " " + singular +
                  (added == 1 ? "" : "s");
            if (refreshed) msg += " (refreshed " + std::to_string(refreshed) + ")";
        }
        msg += detail;
        if (added + refreshed > 8) msg += ", …";
        return {true, msg};
    };

    reg.registerCmd("hbonds",
        [drawInteractions](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return drawInteractions(app, cmd, InteractionType::HBond, kHBondDistCutoff, "hydrogen bond");
        }, ":hbonds [selection]",
        "Auto-detect hydrogen bonds (N/O↔N/O ≤ 3.5 Å) and draw them as dashed "
        "contact lines with distance labels (rendered into screenshots, "
        "exported as PyMOL distance objects). Whole structure by default; pass "
        "a selection to limit scope.",
        {":hbonds", ":hbonds chain A"}, "Measurement");

    reg.registerCmd("saltbridge",
        [drawInteractions](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return drawInteractions(app, cmd, InteractionType::SaltBridge, kSaltBridgeDistCutoff, "salt bridge");
        }, ":saltbridge [selection]",
        "Auto-detect salt bridges (Asp/Glu carboxylate ↔ Lys/Arg/His ≤ 4.0 Å) and "
        "draw them as dashed contact lines with distance labels. Whole "
        "structure by default; pass a selection to limit scope.",
        {":saltbridge", ":saltbridge chain A+B"}, "Measurement");

    // :arrow — solid arrow with caption (issue #38). Distinct from :measure
    // (dashed + auto distance) because the visual semantics matter: a solid
    // arrow reads as "this is an axis vector", not "the author measured
    // here". Two endpoint forms:
    //   :arrow <serial1> <serial2> [= "label"]   # atom-to-atom, resolved once
    //   :arrow $regA $regB         [= "label"]   # vec3 / point register endpoints
    // Pull an optional `color <named|#RRGGBB|rgb(...)>` pair out of the
    // positional args of :arrow / :axis (issue #104). On an unknown spec it
    // returns the error string; otherwise `outColor` is set when present and
    // the two consumed tokens are erased from `pos`.
    auto extractColorOpt = [](std::vector<std::string>& pos,
                              std::optional<std::array<uint8_t, 3>>& outColor) -> std::string {
        for (size_t k = 0; k + 1 < pos.size(); ++k) {
            if (pos[k] == "color") {
                auto rgb = parseColorSpec(pos[k + 1]);
                if (!rgb) return "Unknown color: " + pos[k + 1];
                outColor = rgb;
                pos.erase(pos.begin() + k, pos.begin() + k + 2);
                return "";
            }
        }
        return "";
    };
    reg.registerCmd("arrow", [resolveAtomIdx, extractColorOpt](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto [pos, caption] = splitAtEqToken(cmd.args);
        ArrowOverlay arr;
        if (auto err = extractColorOpt(pos, arr.color); !err.empty()) return {false, err};
        if (pos.size() < 2) return {false, "Usage: :arrow <s1> <s2> [color <c>] [= \"label\"] | :arrow $regA $regB"};
        arr.caption = caption;
        // Resolve a single endpoint to world coords. `$reg` looks up a
        // Vec3 / Point register; bare token is treated as an atom serial
        // against the current object.
        auto resolveEndpoint = [&](const std::string& tok, std::array<float, 3>& out) -> std::string {
            if (!tok.empty() && tok[0] == '$') {
                auto it = app.registers().find(tok.substr(1));
                if (it == app.registers().end()) return "no such register: " + tok;
                if (it->second.kind != Register::Kind::Vec3)
                    return "register is not a vec3: " + tok;
                out[0] = static_cast<float>(it->second.vec[0]);
                out[1] = static_cast<float>(it->second.vec[1]);
                out[2] = static_cast<float>(it->second.vec[2]);
                return "";
            }
            int idx = resolveAtomIdx(app, tok);
            if (idx < 0) return "atom not found: " + tok;
            auto obj = app.tabs().currentTab().currentObject();
            if (!obj) return "no current object";
            const auto& a = obj->atoms()[idx];
            out = {a.x, a.y, a.z};
            return "";
        };
        if (auto err = resolveEndpoint(pos[0], arr.a); !err.empty()) return {false, err};
        if (auto err = resolveEndpoint(pos[1], arr.b); !err.empty()) return {false, err};
        app.arrows().push_back(std::move(arr));
        return {true, "Arrow added"};
    }, ":arrow <s1> <s2> [color <c>] [= \"label\"]",
       "Persistent solid arrow + caption between two atoms / vec3 registers (use $reg for non-atom endpoints; color = named/#RRGGBB)",
       {":arrow pk1 pk2 = \"V-V axis\"",
        ":arrow $p1 $p2 color teal = \"helix axis\""}, "Measurement");

    // :axis $reg [= "label"] — render the principal axis of a PCA register
    // as an arrow centered on its `center`, length proportional to the
    // square root of the eigenvalue (≈ 1σ along that axis). Provides the
    // natural composition over `:let G = pca(...); :axis $G`.
    reg.registerCmd("axis", [extractColorOpt](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto [pos, caption] = splitAtEqToken(cmd.args);
        std::optional<std::array<uint8_t, 3>> axisColor;
        if (auto err = extractColorOpt(pos, axisColor); !err.empty()) return {false, err};
        if (pos.empty() || pos[0].empty() || pos[0][0] != '$')
            return {false, "Usage: :axis $pcaRegister [color <c>] [= \"label\"]"};
        auto it = app.registers().find(pos[0].substr(1));
        if (it == app.registers().end()) return {false, "no such register: " + pos[0]};
        if (it->second.kind != Register::Kind::Pca)
            return {false, "register is not a pca-result: " + pos[0]};
        const auto& p = it->second.pca;
        // Length: ±1σ along the longest axis, where σ ≈ √eigval (covariance
        // eigenvalue is variance). 2× total span renders well at typical zoom;
        // use min length 1 Å so a perfectly-spherical input still draws.
        float halfLen = std::max(1.0f, static_cast<float>(std::sqrt(std::max(0.0, p.eigvals[0]))));
        ArrowOverlay arr;
        arr.caption = caption;
        arr.color = axisColor;
        for (int i = 0; i < 3; ++i) {
            float c = static_cast<float>(p.center[i]);
            float d = static_cast<float>(p.axis1[i]);
            arr.a[i] = c - halfLen * d;
            arr.b[i] = c + halfLen * d;
        }
        app.arrows().push_back(std::move(arr));
        return {true, "Axis added"};
    }, ":axis $pcaReg [color <c>] [= \"label\"]",
       "Draw the major axis (axis1) of a pca-result register as an arrow of length ±1σ centered on its centroid (color = named/#RRGGBB)",
       {":axis $G color slate = \"groove axis\""}, "Measurement");
    reg.registerCmd("angle", [resolveAtomIdx](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        const auto& atoms = obj->atoms();
        int n = static_cast<int>(atoms.size());
        auto [pos, caption] = splitAtEqToken(cmd.args);

        int i1, i2, i3;
        if (pos.empty()) {
            i1 = app.pickRegs_[0]; i2 = app.pickRegs_[1]; i3 = app.pickRegs_[2];
            if (i1 < 0 || i2 < 0 || i3 < 0) return {false, "Click three atoms first (pk1-pk3), then :angle"};
        } else if (pos.size() >= 3) {
            i1 = resolveAtomIdx(app, pos[0]);
            i2 = resolveAtomIdx(app, pos[1]);
            i3 = resolveAtomIdx(app, pos[2]);
            if (i1 < 0) return {false, "Atom not found: serial " + pos[0]};
            if (i2 < 0) return {false, "Atom not found: serial " + pos[1]};
            if (i3 < 0) return {false, "Atom not found: serial " + pos[2]};
        } else {
            return {false, "Usage: :angle [s1 s2 s3] [= \"caption\"]"};
        }
        if (i1 >= n || i2 >= n || i3 >= n) return {false, "Invalid atom index"};

        float deg = geom::angleDeg(atoms[i1].x, atoms[i1].y, atoms[i1].z,
                                   atoms[i2].x, atoms[i2].y, atoms[i2].z,
                                   atoms[i3].x, atoms[i3].y, atoms[i3].z);

        char buf[128];
        std::snprintf(buf, sizeof(buf), "%.1f", deg);
        std::string shortLabel = std::string(buf) + "°";
        std::string msg = "Angle " + atoms[i1].label() + " — " +
            atoms[i2].label() + " — " + atoms[i3].label() + " = " + buf + " deg";
        if (!caption.empty()) msg += "  \"" + caption + "\"";
        app.measurements().push_back({{i1, i2, i3}, shortLabel, caption, obj->name()});
        MLOG_INFO("%s", msg.c_str());
        return {true, msg};
    }, ":angle [serial1 serial2 serial3 | pk1 pk2 pk3] [= \"caption\"]",
       "Measure the angle at the middle atom; endpoints are atom serials "
       "(PDB serial, not selections) or the pk1/pk2/pk3 picks.",
       {":angle", ":angle pk1 pk2 pk3 = \"φ1\""}, "Measurement");

    // :dihedral [s1 s2 s3 s4] — dihedral (no args = pk1-pk4)
    reg.registerCmd("dihedral", [resolveAtomIdx](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        const auto& atoms = obj->atoms();
        int n = static_cast<int>(atoms.size());
        auto [pos, caption] = splitAtEqToken(cmd.args);

        int i1, i2, i3, i4;
        if (pos.empty()) {
            i1 = app.pickRegs_[0]; i2 = app.pickRegs_[1];
            i3 = app.pickRegs_[2]; i4 = app.pickRegs_[3];
            if (i1 < 0 || i2 < 0 || i3 < 0 || i4 < 0)
                return {false, "Click four atoms first (pk1-pk4), then :dihedral"};
        } else if (pos.size() >= 4) {
            i1 = resolveAtomIdx(app, pos[0]);
            i2 = resolveAtomIdx(app, pos[1]);
            i3 = resolveAtomIdx(app, pos[2]);
            i4 = resolveAtomIdx(app, pos[3]);
            if (i1 < 0) return {false, "Atom not found: serial " + pos[0]};
            if (i2 < 0) return {false, "Atom not found: serial " + pos[1]};
            if (i3 < 0) return {false, "Atom not found: serial " + pos[2]};
            if (i4 < 0) return {false, "Atom not found: serial " + pos[3]};
        } else {
            return {false, "Usage: :dihedral [s1 s2 s3 s4] [= \"caption\"]"};
        }
        if (i1 >= n || i2 >= n || i3 >= n || i4 >= n) return {false, "Invalid atom index"};

        float deg = geom::dihedralDeg(
            atoms[i1].x, atoms[i1].y, atoms[i1].z,
            atoms[i2].x, atoms[i2].y, atoms[i2].z,
            atoms[i3].x, atoms[i3].y, atoms[i3].z,
            atoms[i4].x, atoms[i4].y, atoms[i4].z);

        char buf[128];
        std::snprintf(buf, sizeof(buf), "%.1f", deg);
        std::string shortLabel = std::string(buf) + "°";
        std::string msg = "Dihedral " + atoms[i1].label() + " — " +
            atoms[i2].label() + " — " + atoms[i3].label() + " — " +
            atoms[i4].label() + " = " + buf + " deg";
        if (!caption.empty()) msg += "  \"" + caption + "\"";
        app.measurements().push_back({{i1, i2, i3, i4}, shortLabel, caption, obj->name()});
        MLOG_INFO("%s", msg.c_str());
        return {true, msg};
    }, ":dihedral [serial1..serial4 | pk1..pk4] [= \"caption\"]",
       "Measure a dihedral angle; endpoints are atom serials (PDB serial, "
       "not selections) or the pk1..pk4 picks.",
       {":dihedral", ":dihedral pk1 pk2 pk3 pk4 = \"χ1\""}, "Measurement");

    // :rmsd selA vs selB — report the RMSD of the optimal superposition of
    // two equal-count, in-correspondence selections WITHOUT moving anything
    // (issue #115). Complements :align/:super, which only report RMSD as a
    // side effect of actually superposing. Mirrors the rmsd() builtin's
    // current-object scope; the two selections must yield the same atom count
    // (e.g. matching residue ranges + atom name).
    reg.registerCmd("rmsd", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        // Split the args on the `vs` keyword.
        int vsIdx = -1;
        for (int i = 0; i < static_cast<int>(cmd.args.size()); ++i)
            if (cmd.args[i] == "vs") { vsIdx = i; break; }
        if (vsIdx < 1 || vsIdx >= static_cast<int>(cmd.args.size()) - 1)
            return {false, "Usage: :rmsd <selA> vs <selB>"};
        auto join = [&](int lo, int hi) {
            std::string s;
            for (int i = lo; i < hi; ++i) { if (!s.empty()) s += ' '; s += cmd.args[i]; }
            return s;
        };
        std::string exprA = join(0, vsIdx);
        std::string exprB = join(vsIdx + 1, static_cast<int>(cmd.args.size()));
        auto collect = [&](const std::string& expr,
                           std::vector<float>& xs, std::vector<float>& ys, std::vector<float>& zs) {
            Selection sel = app.parseSelection(expr, *obj);
            const auto& atoms = obj->atoms();
            for (int i : sel.indices()) {
                if (i < 0 || i >= static_cast<int>(atoms.size())) continue;
                xs.push_back(atoms[i].x); ys.push_back(atoms[i].y); zs.push_back(atoms[i].z);
            }
        };
        std::vector<float> axs, ays, azs, bxs, bys, bzs;
        collect(exprA, axs, ays, azs);
        collect(exprB, bxs, bys, bzs);
        int na = static_cast<int>(axs.size()), nb = static_cast<int>(bxs.size());
        if (na == 0 || nb == 0)
            return {false, "Empty selection (A=" + std::to_string(na) + ", B=" + std::to_string(nb) + ")"};
        if (na != nb)
            return {false, "Selections must have equal atom counts (" +
                           std::to_string(na) + " vs " + std::to_string(nb) +
                           ") — match residue ranges and atom names"};
        auto rr = geom::rmsdOf(axs, ays, azs, bxs, bys, bzs);
        if (!rr.valid) return {false, "RMSD: degenerate input"};
        char buf[64];
        std::snprintf(buf, sizeof(buf), "RMSD = %.3f A over %d atoms", rr.rmsd, rr.n);
        MLOG_INFO("%s", buf);
        return {true, buf};
    }, ":rmsd <selA> vs <selB>",
       "Report the RMSD (and N) of the optimal superposition of two equal-count, "
       "in-correspondence selections — without moving anything. Current object; "
       "the two sides must yield the same atom count.",
       {":rmsd chain A and name CA vs chain C and name CA",
        ":rmsd resi 1-50 and name CA vs resi 100-149 and name CA"}, "Analysis");

    // :contactmap [cutoff] — toggle contact map panel
    reg.registerCmd("contactmap", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        float cutoff = 8.0f;
        if (!cmd.args.empty()) {
            cutoff = parseFloat(cmd.args[0]).value_or(cutoff);
        }
        app.layout().toggleAnalysisPanel();
        app.tabs().currentTab().viewState().analysisPanelVisible = app.layout().analysisPanelVisible();
        if (app.layout().analysisPanelVisible()) {
            auto obj = app.tabs().currentTab().currentObject();
            if (obj) {
                app.contactMapPanel_.update(*obj, cutoff);
            }
            if (app.canvas()) app.canvas()->invalidate();
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Contact map visible (cutoff=%.1fA)", cutoff);
            return {true, buf};
        }
        if (app.canvas()) app.canvas()->invalidate();
        return {true, "Contact map hidden"};
    }, ":contactmap [cutoff]", "Toggle the residue contact map panel (default cutoff: 8 \xC3\x85)",
       {":contactmap", ":contactmap 6"}, "Analysis");

    // :cmap — alias for :contactmap
    reg.registerCmd("cmap", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        return app.cmdRegistry().execute(app, cmd.args.empty() ? "contactmap" :
            "contactmap " + cmd.args[0]);
    }, ":cmap [cutoff]", "Toggle contact map panel (alias for :contactmap)",
       {":cmap", ":cmap 6"}, "Analysis");

    reg.registerCmd("interface", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) {
            return {false, "Usage: :interface on|off|legend [cutoff]"};
        }
        // `:interface legend` opens an overlay showing the dash-color
        // legend and per-type contact statistics for the cached overlay.
        if (cmd.args[0] == "legend") {
            app.showInterfaceLegend();
            return {true, ""};
        }
        // First arg is on|off, second arg (optional) is cutoff in Å.
        // Numeric-only first arg keeps the original `:interface 4.5`
        // shorthand working: it implies "on" with that cutoff.
        bool wantOn;
        float cutoff = 4.5f;
        size_t cutoffArgIdx = 1;
        auto parsedBool = parseBool(cmd.args[0]);
        if (parsedBool) {
            wantOn = *parsedBool;
        } else {
            // Try as numeric cutoff → "on" with that cutoff.
            auto c = parseFloat(cmd.args[0]);
            if (!c) return {false, "Usage: :interface on|off|legend [cutoff]"};
            cutoff = *c; wantOn = true; cutoffArgIdx = 99;
        }
        if (cmd.args.size() > cutoffArgIdx) {
            auto c = parseFloat(cmd.args[cutoffArgIdx]);
            if (!c) return {false, "Cutoff must be a number"};
            cutoff = *c;
        }
        app.interface().active = wantOn;
        if (app.interface().active) {
            auto obj = app.tabs().currentTab().currentObject();
            if (!obj) {
                app.interface().active = false;
                return {false, "No object loaded"};
            }
            app.interface().cutoff = cutoff;
            if (!app.recomputeInterface()) {
                app.interface().active = false;
                char buf[80];
                std::snprintf(buf, sizeof(buf),
                              "No inter-chain contacts found (cutoff=%.1fA)",
                              cutoff);
                return {false, buf};
            }

            int nHB = 0, nSalt = 0, nHyd = 0, nOther = 0;
            for (const auto& c : app.interface().contacts) {
                switch (c.type) {
                    case InteractionType::HBond:       ++nHB;    break;
                    case InteractionType::SaltBridge:  ++nSalt;  break;
                    case InteractionType::Hydrophobic: ++nHyd;   break;
                    case InteractionType::Other:       ++nOther; break;
                }
            }
            auto tag = [&](InteractionType t) {
                return (app.interface().showMask & interactionBit(t))
                       ? "" : "*";
            };
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                "Interface: %zu residue pairs (cutoff=%.1fA) — "
                "salt %d%s, H-bond %d%s, hydrophobic %d%s, other %d%s"
                "%s",
                app.interface().contacts.size(), cutoff,
                nSalt,  tag(InteractionType::SaltBridge),
                nHB,    tag(InteractionType::HBond),
                nHyd,   tag(InteractionType::Hydrophobic),
                nOther, tag(InteractionType::Other),
                app.interface().showMask == kInterfaceShowAll
                    ? "" : "  [* hidden — :set interface_show all]");
            return {true, buf};
        }
        app.interface().contacts.clear();
        app.interface().atomMask.clear();
        app.interface().repr.clear();
        app.interface().fromZoom = false;
        return {true, "Interface overlay hidden"};
    }, ":interface on|off|legend [cutoff]",
       "Show/hide classified inter-chain contact overlay; 'legend' opens a color/stats overlay (default cutoff: 4.5 \xC3\x85)",
       {":interface on", ":interface off", ":interface on 5.0", ":interface legend"}, "Analysis");

    // :dssp — Re-run DSSP secondary-structure assignment on the current
    // state. Useful for trajectory frames where the loader's initial SS
    // (from headers, if any) doesn't reflect the current conformation.
    reg.registerCmd("dssp", [](Application& app, const ParsedCommand&) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object loaded"};
        // Drop cached SS for this state, force recompute, re-sync atoms.
        obj->invalidateSSCache();
        const auto& ss = obj->ssAtState(obj->activeState());
        if (ss.size() != obj->atoms().size()) {
            return {false, "DSSP: size mismatch (state " +
                           std::to_string(obj->activeState() + 1) + ")"};
        }
        auto& atoms = obj->atoms();
        int nH = 0, nE = 0;
        for (size_t i = 0; i < atoms.size(); ++i) {
            atoms[i].ssType = ss[i];
            if (ss[i] == SSType::Helix) ++nH;
            else if (ss[i] == SSType::Sheet) ++nE;
        }
        return {true, "DSSP recomputed: " + std::to_string(nH) +
                      " H, " + std::to_string(nE) + " E (state " +
                      std::to_string(obj->activeState() + 1) + ")"};
    }, ":dssp", "Recompute secondary structure (Kabsch-Sander) for the current state",
       {":dssp"}, "Analysis");

    // :sasa — Compute solvent accessible surface area (SASA) for the current
    // state using the PDB-REDO/dssp accessibility model, and report total /
    // per-chain absolute area (Å²) plus mean relative accessibility.
    reg.registerCmd("sasa", [](Application& app, const ParsedCommand&) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object loaded"};
        obj->invalidateSASACache();
        const auto& sasa = obj->sasaAtState(obj->activeState());
        const auto& atoms = obj->atoms();
        if (sasa.size() != atoms.size()) {
            return {false, "SASA: size mismatch (state " +
                           std::to_string(obj->activeState() + 1) + ")"};
        }

        double total = 0.0;
        std::vector<std::pair<std::string, double>> chains;   // ordered
        for (size_t i = 0; i < atoms.size(); ++i) {
            total += sasa[i];
            const std::string& c = atoms[i].chainId;
            auto it = std::find_if(chains.begin(), chains.end(),
                                   [&](const auto& p) { return p.first == c; });
            if (it == chains.end()) chains.push_back({c, sasa[i]});
            else it->second += sasa[i];
        }

        // Mean relative accessibility over standard amino-acid residues.
        const auto& rel = obj->sasaRelFractions();
        double relSum = 0.0;
        int relCount = 0;
        for (size_t i = 0; i < atoms.size();) {
            size_t j = i;
            while (j < atoms.size() &&
                   atoms[j].chainId == atoms[i].chainId &&
                   atoms[j].resSeq  == atoms[i].resSeq &&
                   atoms[j].insCode == atoms[i].insCode) ++j;
            if (!atoms[i].isHet && molterm::sasa::maxAsa(atoms[i].resName) > 0.0f &&
                rel.size() == atoms.size()) {
                relSum += rel[i];
                ++relCount;
            }
            i = j;
        }

        char buf[64];
        auto fmt = [&](const char* spec, double v) {
            std::snprintf(buf, sizeof(buf), spec, v);
            return std::string(buf);
        };

        std::string chainStr;
        for (const auto& [chainId, chainSasa] : chains) {
            if (!chainStr.empty()) chainStr += ", ";
            chainStr += (chainId.empty() ? "_" : chainId) + ": " + fmt("%.1f", chainSasa);
        }

        std::string msg = "SASA: " + fmt("%.1f", total) + " Å² total (" + chainStr + ")";
        if (relCount > 0) msg += "; mean rel " + fmt("%.2f", relSum / relCount);
        msg += " (state " + std::to_string(obj->activeState() + 1) + ")";
        return {true, msg};
    }, ":sasa", "Compute solvent accessible surface area (PDB-REDO/dssp) for the current state",
       {":sasa"}, "Analysis");
}

}  // namespace molterm
