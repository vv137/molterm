// Scripting + registers: :let/:unlet/:registers/:expose/:run/:setenv/:echo/:save/:export/:resume.


#include "molterm/app/Application.h"
#include "molterm/cmd/CommandParser.h"
#include "molterm/cmd/CommandRegistry.h"
#include "molterm/cmd/CommandHelpers.h"
#include "molterm/cmd/Register.h"
#include "molterm/cmd/RegisterExpr.h"
#include "molterm/cmd/ExprContext.h"
#include "molterm/core/MolObject.h"
#include "molterm/core/Selection.h"
#include <fstream>
#include "molterm/io/SessionExporter.h"
#include "molterm/core/Logger.h"
#include "molterm/io/PdbWriter.h"
#include "molterm/io/SessionSaver.h"
#include "molterm/tui/Screen.h"

namespace molterm {

// JSON number / vec3 formatters for :dump. %.10g keeps enough precision to
// round-trip the stored doubles without trailing noise.
static std::string jnum(double v) {
    char b[40]; std::snprintf(b, sizeof(b), "%.10g", v); return b;
}
static std::string jvec(const std::array<double, 3>& v) {
    return "[" + jnum(v[0]) + "," + jnum(v[1]) + "," + jnum(v[2]) + "]";
}

void Application::registerScriptingCommands(CommandRegistry& reg) {
    // :let <name> = <expr> — typed registers (#32, #33, #35).
    //
    // The expression evaluator (RegisterExpr) supports scalars, vec3
    // literals (`[x,y,z]`), atom positions via `pos(<chain:resi:name>)`,
    // PCA results via `pca(<selection>)`, and the usual vector algebra
    // (+, -, *, /, dot, cross, length, normalize, midpoint, angle).
    // Field access on registers: `$g.axis1`, `$g.center`, `$v.length`,
    // etc. — see Register::getVec/getScalar.
    reg.registerCmd("let", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        // Split the raw argument tail on the first `=` (the expression grammar
        // has no `=` operator, so the first one is always the name/RHS
        // separator). Reading rawArgs preserves the original, comma/space-
        // significant RHS — `[1, 2, 3]` and `pca(chain A and resi 5)` reach the
        // evaluator verbatim, with no tokenize-then-rejoin guesswork and no
        // need for the builtins to undo a comma injection.
        auto eq = cmd.rawArgs.find('=');
        if (eq == std::string::npos)
            return {false, "Usage: :let <name> = <expr>"};
        std::string name = cmd.rawArgs.substr(0, eq);
        std::string rhs  = cmd.rawArgs.substr(eq + 1);
        trimWhitespace(name);
        trimWhitespace(rhs);
        if (!isBareName(name))
            return {false, "Register name must start with a letter or underscore "
                           "and contain no spaces"};

        RegisterExpr::Context ctx = makeExprContext(app);

        auto r = RegisterExpr::eval(rhs, ctx);
        if (!r.ok) return {false, ":let " + name + ": " + r.error};
        app.registers()[name] = r.value;
        return {true, formatRegister(name, r.value)};
    }, ":let <name> = <expr>",
       "Bind a typed register (scalar, vec3, or pca-result) for reuse later. "
       "Expression supports +,-,*,/, vec3 literals [x,y,z], $reg.field access, "
       "and builtins pos()/centroid()/com()/pca()/helix_axis()/superpose_axis()/"
       "rmsd()/dot()/cross()/distance()/length()/normalize()/midpoint()/angle()/"
       "dihedral().",
       {":let v_axis = pos(A:43:CA) - pos(B:23:CA)",
        ":let G = pca(chain A and helix)",
        ":let d = distance(centroid(chain A), centroid(chain B))",
        ":let theta = angle($v_axis, $G.axis1)"}, "Registers");

    // :unlet <name> | :unlet * — drop registers.
    reg.registerCmd("unlet", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :unlet <name> | :unlet *"};
        if (cmd.args[0] == "*" || cmd.args[0] == "all") {
            int n = static_cast<int>(app.registers().size());
            app.registers().clear();
            return {true, "Cleared " + std::to_string(n) + " registers"};
        }
        auto& tbl = app.registers();
        auto it = tbl.find(cmd.args[0]);
        if (it == tbl.end()) return {false, "No such register: " + cmd.args[0]};
        tbl.erase(it);
        return {true, "Unlet " + cmd.args[0]};
    }, ":unlet <name>|*", "Drop a named register (or all of them)",
       {":unlet v_axis", ":unlet *"}, "Registers");

    // :registers — list all registers with their values.
    reg.registerCmd("registers", [](Application& app, const ParsedCommand&) -> ExecResult {
        const auto& tbl = app.registers();
        if (tbl.empty()) return {true, "No registers"};
        std::string out;
        for (const auto& [name, r] : tbl) {
            if (!out.empty()) out += '\n';
            char buf[160];
            switch (r.kind) {
                case Register::Kind::Scalar:
                    std::snprintf(buf, sizeof(buf), "%s = %.6g", name.c_str(), r.scalar);
                    break;
                case Register::Kind::Vec3:
                    std::snprintf(buf, sizeof(buf), "%s = [%.4f, %.4f, %.4f]",
                                  name.c_str(), r.vec[0], r.vec[1], r.vec[2]);
                    break;
                case Register::Kind::Pca:
                    std::snprintf(buf, sizeof(buf),
                        "%s = pca: center=(%.3f, %.3f, %.3f) eigvals=[%.3f, %.3f, %.3f]",
                        name.c_str(),
                        r.pca.center[0], r.pca.center[1], r.pca.center[2],
                        r.pca.eigvals[0], r.pca.eigvals[1], r.pca.eigvals[2]);
                    break;
            }
            out += buf;
        }
        return {true, out};
    }, ":registers", "List all named registers and their typed values",
       {":registers"}, "Registers");

    // :dump [name...] — emit registers as a JSON object to stdout for an agent
    // / pipeline to parse (vs. :registers' human text). All registers, or just
    // the named ones. Keyed by register name; per-kind fields mirror the
    // session format so values round-trip.
    reg.registerCmd("dump", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        const auto& tbl = app.registers();
        std::vector<std::string> names = cmd.args;
        if (names.empty())
            for (const auto& [n, r] : tbl) { (void)r; names.push_back(n); }
        std::string out = "{";
        bool first = true;
        for (const auto& n : names) {
            auto it = tbl.find(n);
            if (it == tbl.end()) continue;
            const Register& r = it->second;
            out += (first ? "" : ",");
            first = false;
            out += "\"" + n + "\":{";
            switch (r.kind) {
                case Register::Kind::Scalar:
                    out += "\"kind\":\"scalar\",\"value\":" + jnum(r.scalar);
                    break;
                case Register::Kind::Vec3:
                    out += "\"kind\":\"vec3\",\"value\":" + jvec(r.vec);
                    break;
                case Register::Kind::Pca:
                    out += "\"kind\":\"pca\",\"center\":" + jvec(r.pca.center) +
                           ",\"axis1\":" + jvec(r.pca.axis1) +
                           ",\"axis2\":" + jvec(r.pca.axis2) +
                           ",\"axis3\":" + jvec(r.pca.axis3) +
                           ",\"eigvals\":" + jvec(r.pca.eigvals) +
                           ",\"angle\":" + jnum(r.pca.angle) +
                           ",\"rmsd\":" + jnum(r.pca.rmsd);
                    break;
            }
            out += "}";
        }
        out += "}";
        if (isHeadless()) {
            std::printf("%s\n", out.c_str());
            std::fflush(stdout);
            return {true, ""};
        }
        return {true, out};   // TUI: status line, not raw stdout
    }, ":dump [name...]",
       "Print registers as a JSON object to stdout (all, or only the named "
       "ones) for machine parsing — the structured counterpart to :registers.",
       {":dump", ":dump phi psi"}, "Registers");

    // :expose <name> [<name> ...] — mark registers for export from the
    // current script frame (issue #67). Each name will be copied into
    // the caller's register frame when the script exits. Names starting
    // with `_` are private and silently rejected. Outside a scope=local
    // script the command is a no-op (top-level frame has no caller).
    // Named :expose to avoid collision with the existing :export
    // commands for PML session export and PDB file export.
    reg.registerCmd("expose", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty())
            return {false, "Usage: :expose <name> [<name> ...]"};
        int ok = 0;
        std::vector<std::string> noFrame, privateNames;
        for (const auto& n : cmd.args) {
            switch (app.markExport(n)) {
                case Application::ExportResult::Ok:       ++ok; break;
                case Application::ExportResult::NoFrame:  noFrame.push_back(n); break;
                case Application::ExportResult::Private:  privateNames.push_back(n); break;
                case Application::ExportResult::Empty:    /* skip silently */ break;
            }
        }
        std::string msg = "Marked " + std::to_string(ok) + " export(s)";
        if (!privateNames.empty()) {
            msg += "; rejected " + std::to_string(privateNames.size()) +
                   " private name(s) (rename without leading `_`): ";
            for (size_t i = 0; i < privateNames.size(); ++i) {
                if (i) msg += ", ";
                msg += privateNames[i];
            }
        }
        if (!noFrame.empty()) {
            msg += "; ignored at top-level frame: " +
                   std::to_string(noFrame.size()) + " name(s) "
                   "(use inside a `#!molterm scope=local` script)";
        }
        return {true, msg};
    }, ":expose <name> [<name> ...]",
       "Mark registers for export from the current scope=local script "
       "frame to the caller. Names starting with `_` are auto-private. "
       "Also configurable via shebang `#!molterm scope=local export=name1,name2`.",
       {":expose crossing incident", ":expose tcr_angle"}, "Registers");

    // :run [--strict] [--fresh] <script.mt> [KEY=VALUE ...] — execute a command script
    reg.registerCmd("run", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :run [--strict] [--fresh] <script.mt> [KEY=VAL ...]"};
        bool strict = false, fresh = false;
        std::string path;
        std::unordered_map<std::string, std::string> callArgs;
        for (const auto& a : cmd.args) {
            if      (a == "--strict") strict = true;
            else if (a == "--fresh")  fresh  = true;
            else if (path.empty())    path   = a;
            else {
                // KEY=VALUE — script call args (issue #67). With at least
                // one such arg the script body runs in a fresh register
                // frame regardless of shebang, so caller state stays
                // protected even when the script author hasn't added
                // a `#!molterm scope=local` line.
                auto eq = a.find('=');
                if (eq == std::string::npos) {
                    return {false, "Trailing arg `" + a + "` must be KEY=VALUE "
                                   "(positional args not supported — name your params)"};
                }
                callArgs[a.substr(0, eq)] = a.substr(eq + 1);
            }
        }
        if (path.empty()) return {false, "Usage: :run [--strict] [--fresh] <script.mt> [KEY=VAL ...]"};
        std::string resolved = path;
        if (path.compare(0, kAtLibPrefix.size(), kAtLibPrefix) == 0) {
            std::string r = resolveAtLibPath(path);
            if (r.empty()) return {false,
                "Cannot resolve " + path +
                "  (tried $MOLTERM_LIB_DIR, ~/.molterm/lib/, "
                "<install>/share/molterm/lib/, <exe>/../lib/)"};
            resolved = r;
        }
        std::ifstream file(resolved);
        if (!file) return {false, "Cannot open: " + resolved};
        // --fresh: wipe overlay annotations (labels, measurements) before
        // executing the script, so a batch render driver
        // (`:run setup; :screenshot fig1; :run setup; :screenshot fig2`)
        // doesn't accumulate fig1's overlays into fig2 (issue #28).
        if (fresh) app.clearOverlayAnnotations();
        ScriptRunResult r = app.runScriptStream(file, strict, callArgs, path);
        // Surface every failure to MLOG_ERROR with file:line context
        // (issue #80) — TUI users can `tail -f ~/.molterm/molterm.log`
        // to see them as they fire; cmdline gets the brief summary.
        for (const auto& f : r.failureList) {
            MLOG_ERROR("%s:%d: `%s`: %s",
                       r.sourcePath.c_str(), f.lineNum,
                       f.srcLine.c_str(), f.reason.c_str());
        }
        if (strict && r.stopped) {
            MLOG_INFO("Ran %d commands from %s (stopped on error)", r.count, path.c_str());
            return {false, path + ":" + std::to_string(r.lastFailureLine()) +
                    ": `" + r.failLine + "`: " + r.firstFail};
        }
        MLOG_INFO("Ran %d commands from %s (%d failed)", r.count, path.c_str(), r.failures);
        if (r.failures > 0) {
            // Cite first failure with file:line so the user can jump
            // straight to it; remaining failures are in the log.
            std::string msg = "Ran " + std::to_string(r.count) + " commands from " + path +
                              " (" + std::to_string(r.failures) + " failed; first: " +
                              path + ":" + std::to_string(r.firstFailureLine()) +
                              " — " + r.firstFail + ")";
            if (r.failures > 1) msg += " — see molterm.log for full list";
            return {false, msg};
        }
        return {true, "Ran " + std::to_string(r.count) + " commands from " + path};
    }, ":run [--strict] [--fresh] <file>",
       "Execute a command script (# comments supported; --strict aborts on first error; "
       "--fresh clears overlay annotations before running so batch-render drivers "
       "don't leak labels/measurements between figures)",
       {":run render.mt", ":run --strict ci.mt", ":run --fresh fig2.mt",
        ":run @lib/tcr_angles"}, "Files");

    reg.registerCmd("setenv", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto& env = app.scriptEnv();
        if (cmd.args.empty()) {
            if (env.empty()) return {true, "(no script env vars set)"};
            std::string out;
            for (const auto& [k, v] : env) {
                if (!out.empty()) out += "\n";
                out += k + " = " + v;
            }
            return {true, out};
        }
        const std::string& name = cmd.args[0];
        if (!isValidEnvName(name))
            return {false, "Invalid name: " + name + " (use [A-Za-z_][A-Za-z0-9_]*)"};
        if (cmd.args.size() == 1) {
            env.erase(name);
            return {true, "Unset " + name};
        }
        std::string value = joinArgs(cmd.args, 1, cmd.args.size());
        env[name] = value;
        return {true, name + " = " + value};
    }, ":setenv [NAME [value]]",
       "Set / unset / list script env vars used by ${VAR} expansion in scripts and commands",
       {":setenv WS /store/casp17/H2324",
        ":load ${WS}/models/top1.cif",
        ":setenv WS"}, "Files");

    // :echo — emit args to stdout for LLM-agent-friendly automated
    // analysis. Args are pre-expanded by expandScriptVars() upstream,
    // so `${reg:.2f}` and `${ENV}` both work. Returns an empty status
    // message because the printf is the visible output — duplicating
    // it via the command-bar return path would print echo lines
    // twice in script mode (printf during + lastMsg at script end).
    reg.registerCmd("echo", [](Application&, const ParsedCommand& cmd) -> ExecResult {
        // Print the raw argument tail verbatim — preserving commas and spacing
        // the command tokenizer would otherwise collapse. Strip one layer of
        // surrounding quotes so `:echo "a, b"` and `:echo a, b` both print `a, b`.
        std::string text = cmd.rawArgs;
        if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') &&
            text.back() == text.front())
            text = text.substr(1, text.size() - 2);
        if (isHeadless()) {
            // Headless: write straight to stdout for pipelines / agents.
            std::printf("%s\n", text.c_str());
            std::fflush(stdout);
            return {true, ""};
        }
        // TUI: raw stdout would corrupt the ncurses screen, so surface the
        // text through the status line via the result message instead.
        return {true, text};
    }, ":echo <text>",
       "Print text (after ${var} / ${reg:fmt} expansion): to stdout when headless "
       "(machine-readable script output / agent telemetry), or the status line in "
       "TUI mode.",
       {":echo crossing = ${crossing:.2f}",
        ":echo workspace = ${WS}",
        ":echo \"label A\\tlabel B\""}, "Session");

    reg.registerCmd("save", [](Application& app, const ParsedCommand&) -> ExecResult {
        if (SessionSaver::saveSession(app))
            return {true, "Session saved to " + SessionSaver::sessionPath()};
        return {false, "Failed to save session"};
    }, ":save", "Save the current session to ~/.molterm/autosave.toml",
       {":save"}, "Session");

    // :export [<obj>] <path>
    // Extension-dispatched file export. `.pml` writes the WHOLE session
    // as a PyMOL script (all loaded objects + reprs + measurements);
    // `.pdb` / `.ent` writes a single MolObject (the named one or
    // current). `.cif` / `.mmcif` is reserved for a future gemmi-
    // backed writer.
    reg.registerCmd("export", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        constexpr const char* kUsage = "Usage: :export [<obj>] <path.pml|.pdb>";
        if (cmd.args.empty()) return {false, kUsage};
        auto endsWith = [](const std::string& s, const std::string& suffix) {
            return s.size() >= suffix.size() &&
                   s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        // The path is always the last arg (so `:export <path>` and
        // `:export <obj> <path>` both parse correctly).
        std::string path = cmd.args.back();
        // PML export — whole-session script; ignores any leading <obj>.
        if (endsWith(path, ".pml")) {
            auto& tab = app.tabs().currentTab();
            if (tab.objects().empty()) return {false, "No objects to export"};
            int vpW = app.layout().viewportWidth();
            int vpH = app.layout().viewportHeight();
            std::vector<SessionExporter::Measurement> exportMs;
            exportMs.reserve(app.measurements().size());
            for (const auto& m : app.measurements())
                exportMs.push_back({m.atoms, m.label, m.caption, m.obj});
            std::string result = SessionExporter::exportPML(
                path, tab, vpW, vpH, app.stereoMode(), app.stereoAngle(), exportMs);
            bool ok = result.find("Failed") == std::string::npos &&
                      result.find("Error") == std::string::npos;
            return {ok, result};
        }
        // PDB / .ent export — single object.
        if (endsWith(path, ".pdb") || endsWith(path, ".ent")) {
            auto& tab = app.tabs().currentTab();
            std::string objName;
            if (cmd.args.size() == 1) {
                auto cur = tab.currentObject();
                if (!cur) return {false, "No object selected"};
                objName = cur->name();
            } else {
                objName = cmd.args[0];
            }
            auto obj = app.store().get(objName);
            if (!obj) return {false, "Object not found: " + objName};
            if (!writePdb(*obj, path))
                return {false, "Failed to write " + path};
            return {true, "Wrote " + objName + " (" +
                          std::to_string(obj->atoms().size()) + " atoms) to " + path};
        }
        if (endsWith(path, ".cif") || endsWith(path, ".mmcif")) {
            return {false, "mmCIF export not implemented yet — write .pdb and "
                           "round-trip through gemmi (`gemmi convert in.pdb out.cif`) "
                           "for now."};
        }
        return {false, "Unsupported extension. Use .pml (whole session), "
                       ".pdb / .ent (one object), or wait for .cif."};
    }, ":export [<obj>] <path>",
       "Write to disk. .pml → whole session as PyMOL script; .pdb/.ent → one MolObject.",
       {":export figure.pml",
        ":export 1ubq.pdb",
        ":export carved tcr_alone.pdb",
        ":export 1ubq /tmp/backup.pdb"}, "Files");

    reg.registerCmd("resume", [](Application& app, const ParsedCommand&) -> ExecResult {
        std::string msg = SessionSaver::restoreSession(app);
        bool ok = msg.find("Failed") == std::string::npos &&
                  msg.find("Cannot") == std::string::npos &&
                  msg.find("not found") == std::string::npos;
        return {ok, msg};
    }, ":resume", "Restore the last session from ~/.molterm/autosave.toml",
       {":resume"}, "Session");

}

}  // namespace molterm
