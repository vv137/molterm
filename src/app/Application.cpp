#include "molterm/app/Application.h"

#include "molterm/app/CommandScope.h"
#include "molterm/app/PathPatterns.h"
#include "molterm/cmd/CommandHelpers.h"
#include "molterm/cmd/CommandParser.h"
#include "molterm/cmd/ExprContext.h"
#include "molterm/cmd/RegisterExpr.h"
#include "molterm/core/SASA.h"
#include "molterm/io/Aligner.h"
#include "molterm/io/CifLoader.h"
#include "molterm/io/PdbWriter.h"
#include "molterm/render/AsciiCanvas.h"
#include "molterm/render/BrailleCanvas.h"
#include "molterm/render/BlockCanvas.h"
#include "molterm/render/PixelCanvas.h"
#include "molterm/render/ProtocolPicker.h"
#include "molterm/render/ColorMapper.h"
#include "molterm/repr/WireframeRepr.h"
#include "molterm/repr/BallStickRepr.h"
#include "molterm/repr/BackboneRepr.h"
#include "molterm/repr/SpacefillRepr.h"
#include "molterm/repr/CartoonRepr.h"
#include "molterm/repr/RibbonRepr.h"
#include "molterm/repr/SurfaceRepr.h"
#include "molterm/repr/ReprUtil.h"
#include "molterm/io/SessionExporter.h"
#include "molterm/config/ConfigParser.h"
#include "molterm/core/BondTable.h"
#include "molterm/core/Geometry.h"
#include "molterm/core/Logger.h"
#include "molterm/core/Selection.h"
#include "molterm/io/SessionSaver.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <unordered_map>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <signal.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace molterm {

// Command-line argument meaning "every object/representation/tab in
// the current scope" — accepted by :hide, :clear, :enable, :disable,
// :set scope. The glob form is honored by commands whose grammar maps
// 1:1 onto a name token (e.g. :enable *).

// Read the current executable path. Empty on platforms without a
// supported syscall; callers must handle that gracefully.
static std::filesystem::path exeDir() {
    char buf[4096] = {};
#if defined(__APPLE__)
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) buf[0] = '\0';
#elif defined(__linux__)
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) buf[n] = '\0'; else buf[0] = '\0';
#endif
    if (!buf[0]) return {};
    return std::filesystem::path(buf).parent_path();
}

static std::string resolveUSalignPath() {
    namespace fs = std::filesystem;
    constexpr const char* kName = "USalign";
    std::error_code ec;
    auto dir = exeDir();
    if (!dir.empty()) {
        fs::path sibling = dir / kName;
        if (fs::exists(sibling, ec)) return sibling.string();
    }
#ifdef USALIGN_PATH
    if (fs::exists(USALIGN_PATH, ec)) return USALIGN_PATH;
#endif
    return kName;
}


// Resolve `@lib/<name>` (with or without trailing .mt) to a concrete
// file path for `:run` — issue #56. Lookup chain, first match wins:
//   1. $MOLTERM_LIB_DIR/<name>.mt        (per-invocation override)
//   2. ~/.molterm/lib/<name>.mt          (per-user library)
//   3. <exe>/../share/molterm/lib/<name>.mt   (install layout)
//   4. <exe>/../lib/<name>.mt           (build-tree layout — running from build/)
//   5. <source>/lib/<name>.mt           (dev fallback via MOLTERM_SOURCE_DIR)
// Returns empty string when no candidate exists, so the caller can
// surface a "Cannot resolve @lib/foo" message with the chain it tried.
std::string resolveAtLibPath(const std::string& spec) {
    namespace fs = std::filesystem;
    if (spec.size() <= kAtLibPrefix.size() ||
        spec.compare(0, kAtLibPrefix.size(), kAtLibPrefix) != 0) {
        return std::string();
    }
    std::string name = spec.substr(kAtLibPrefix.size());
    if (name.size() < 3 || name.substr(name.size() - 3) != ".mt") name += ".mt";

    std::vector<fs::path> candidates;
    if (const char* env = std::getenv("MOLTERM_LIB_DIR")) {
        candidates.emplace_back(fs::path(env) / name);
    }
    candidates.emplace_back(fs::path(ConfigParser::configDir()) / "lib" / name);
    auto dir = exeDir();
    if (!dir.empty()) {
        candidates.emplace_back(dir / ".." / "share" / "molterm" / "lib" / name);
        candidates.emplace_back(dir / ".." / "lib" / name);
    }
#ifdef MOLTERM_SOURCE_DIR
    candidates.emplace_back(fs::path(MOLTERM_SOURCE_DIR) / "lib" / name);
#endif

    std::error_code ec;
    for (const auto& p : candidates) {
        if (fs::exists(p, ec)) return fs::weakly_canonical(p, ec).string();
    }
    return std::string();
}

// Color the non-carbon atoms in `indices` by element (CPK). Carbons keep the
// active scheme color so e.g. `color element ligand` only repaints the ligand's
// heteroatoms instead of every N/O/S in the structure.
int applyHeteroatomColors(MolObject& obj, const std::vector<int>& indices) {
    const auto& atoms = obj.atoms();
    int count = 0;
    for (int i : indices) {
        if (i >= 0 && i < static_cast<int>(atoms.size()) && atoms[i].element != "C") {
            obj.setAtomColor(i, ColorMapper::colorForElement(atoms[i].element));
            ++count;
        }
    }
    return count;
}

// Whole-object overload (e.g. the `color by element` keybinding).
int applyHeteroatomColors(MolObject& obj) {
    std::vector<int> all(obj.atoms().size());
    std::iota(all.begin(), all.end(), 0);
    return applyHeteroatomColors(obj, all);
}

static const char* inspectLevelName(InspectLevel lvl) {
    switch (lvl) {
        case InspectLevel::Atom:    return "ATOM";
        case InspectLevel::Residue: return "RESIDUE";
        case InspectLevel::Chain:   return "CHAIN";
        case InspectLevel::Object:  return "OBJECT";
    }
    return "?";
}

static const char* pickModeName(PickMode pm) {
    switch (pm) {
        case PickMode::Inspect:       return "INSPECT";
        case PickMode::SelectAtom:    return "SEL:ATOM";
        case PickMode::SelectResidue: return "SEL:RES";
        case PickMode::SelectChain:   return "SEL:CHAIN";
        case PickMode::Focus:         return "FOCUS";
    }
    return "?";
}

// Parse a bool option value for :set commands.
// Accepts on/1/true/yes (true), off/0/false/no (false). Empty / unknown returns std::nullopt.
// Global resize flag
static volatile sig_atomic_t g_resized = 0;
static void resizeHandler(int) { g_resized = 1; }

// Clear pixel graphics artifacts and force full ncurses repaint
void clearScreenAndRepaint() {
    fprintf(stdout, "\033[2J");
    fflush(stdout);
    clearok(curscr, TRUE);
    wrefresh(curscr);
}

Application::Application() = default;
Application::~Application() {
    Logger::instance().close();
}

// Resolve ~/.molterm/history.txt — same convention as Logger.cpp:29
// (~/.molterm/molterm.log), so all per-user state lives in one place.
static std::string resolveHistoryPath() {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return std::string(home) + "/.molterm/history.txt";
}

void Application::init(int argc, char* argv[]) {
    Logger::instance().open();
    MLOG_INFO("MolTerm starting (argc=%d)", argc);

    // Restore command history from previous sessions before any commands
    // run (init-script or argv-driven loads will get pushed onto the same
    // history vector so they replay cleanly into the persisted file too).
    {
        std::string hp = resolveHistoryPath();
        if (!hp.empty()) cmdLine_.loadHistory(hp);
    }

    ColorMapper::initColors();

    Aligner::setUSalignPath(resolveUSalignPath());
    layout_.init(screen_.height(), screen_.width());

    keymapMgr_.loadDefaults();
    keymapMgr_.loadFromFile();  // Override with ~/.molterm/keymap.toml
    inputHandler_ = std::make_unique<InputHandler>(keymapMgr_.keymap());

    // Apply config from ~/.molterm/config.toml
    auto cfg = ConfigParser::loadConfig();

    // Default renderer from config (or Braille)
    RendererType rt = RendererType::Braille;
    if (cfg.defaultRenderer == "ascii")  rt = RendererType::Ascii;
    if (cfg.defaultRenderer == "block")  rt = RendererType::Block;
    if (cfg.defaultRenderer == "pixel" || cfg.defaultRenderer == "auto") {
        rt = RendererType::Pixel;
    } else if (cfg.defaultRenderer == "sixel") {
        forcedProtocol_ = GraphicsProtocol::Sixel; rt = RendererType::Pixel;
    } else if (cfg.defaultRenderer == "kitty") {
        forcedProtocol_ = GraphicsProtocol::Kitty; rt = RendererType::Pixel;
    } else if (cfg.defaultRenderer == "iterm2") {
        forcedProtocol_ = GraphicsProtocol::ITerm2; rt = RendererType::Pixel;
    }
    setRenderer(rt);
    autoCenter_ = cfg.autoCenter;
    initRepresentations();
    registerCommands();

    // Auto-load ~/.molterm/init.mt if present (after commands are registered, before file args).
    // Failures here are logged but never abort startup — even under --strict from the CLI,
    // which only applies to the user-supplied script.
    if (const char* home = std::getenv("HOME")) {
        std::string initPath = std::string(home) + "/.molterm/init.mt";
        std::ifstream initFile(initPath);
        if (initFile) {
            MLOG_INFO("Loading init script: %s", initPath.c_str());
            ScriptRunResult r = runScriptStream(initFile, /*strict=*/false, initPath);
            if (r.failures > 0) {
                MLOG_WARN("init.mt: %d of %d command(s) failed (first: %s)",
                          r.failures, r.count, r.firstFail.c_str());
                cmdLine_.setMessage("init.mt: " + std::to_string(r.failures) +
                                    " command(s) failed - see ~/.molterm/molterm.log");
            } else {
                MLOG_INFO("init.mt: ran %d command(s)", r.count);
            }
        }
    }

    // Set up SIGWINCH handler for terminal resize
    struct sigaction sa;
    sa.sa_handler = resizeHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGWINCH, &sa, nullptr);

    // Non-blocking input with timeout for responsive rendering
    screen_.setTimeout(50);

    // Enable mouse
    screen_.enableMouse();

    // Load files from command line args (skip flags)
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') continue;  // skip flags
        std::string msg = loadFile(argv[i]);
        if (!msg.empty()) {
            cmdLine_.setMessage(msg);
        }
    }
}

int Application::pushScriptFrame(const std::unordered_map<std::string, std::string>& seedEnv,
                                  const std::vector<std::string>& seedExports) {
    regStack_.emplace_back();          // empty register frame
    // Env inherits the parent's bindings, then args overlay them. Writes
    // inside the script land in the new top-of-stack map only.
    envStack_.push_back(envStack_.back());
    for (const auto& [k, v] : seedEnv) envStack_.back()[k] = v;
    exportStack_.push_back(seedExports);
    return static_cast<int>(regStack_.size());
}

int Application::popScriptFrame() {
    if (regStack_.size() < 2) return 0;     // never pop frame[0]
    auto frameRegs    = std::move(regStack_.back());
    auto frameExports = std::move(exportStack_.back());
    regStack_.pop_back();
    envStack_.pop_back();
    exportStack_.pop_back();
    int copied = 0;
    for (const auto& name : frameExports) {
        if (!name.empty() && name[0] == '_') continue;   // private — drop
        auto it = frameRegs.find(name);
        if (it == frameRegs.end()) continue;             // never set, skip
        regStack_.back()[name] = it->second;
        ++copied;
    }
    return copied;
}

Application::ExportResult Application::markExport(const std::string& name) {
    if (regStack_.size() < 2) return ExportResult::NoFrame;
    if (name.empty())                 return ExportResult::Empty;
    // `_`-prefix is enforced at popScriptFrame (single chokepoint, also
    // catches shebang `export=_foo` seedings). markExport surfaces it
    // here too so the user gets a tailored error message rather than
    // a silent "Marked 0 exports".
    if (name[0] == '_')               return ExportResult::Private;
    exportStack_.back().push_back(name);
    return ExportResult::Ok;
}

Application::ScriptRunResult Application::runScriptStream(std::istream& in, bool strict,
                                                            const std::string& sourcePath) {
    return runScriptStream(in, strict, {}, sourcePath);
}

Application::ScriptRunResult Application::runScriptStream(
    std::istream& in, bool strict,
    const std::unordered_map<std::string, std::string>& args,
    const std::string& sourcePath) {
    ScriptRunResult result;
    result.sourcePath = sourcePath.empty() ? std::string("<stdin>") : sourcePath;

    // Peek the first non-empty line for a `#!molterm` shebang. The
    // shebang is grammar-light:
    //   #!molterm scope=local export=name1,name2
    // Recognised keys: scope (local|inherit, default inherit), export
    // (comma list of names that flow back to the caller).
    enum class Scope { Inherit, Local };
    Scope scope = Scope::Inherit;
    std::vector<std::string> exports;
    std::string firstLine;
    bool consumedShebang = false;
    if (std::getline(in, firstLine)) {
        std::string t = firstLine;
        size_t s = t.find_first_not_of(" \t");
        if (s != std::string::npos &&
            t.compare(s, 9, "#!molterm") == 0) {
            consumedShebang = true;
            std::string rest = t.substr(s + 9);
            std::stringstream ss(rest);
            std::string tok;
            while (ss >> tok) {
                auto eq = tok.find('=');
                if (eq == std::string::npos) continue;
                std::string k = tok.substr(0, eq);
                std::string v = tok.substr(eq + 1);
                if (k == "scope") {
                    if      (v == "local")   scope = Scope::Local;
                    else if (v == "inherit") scope = Scope::Inherit;
                } else if (k == "export") {
                    std::string name;
                    for (char c : v) {
                        if (c == ',') { if (!name.empty()) exports.push_back(name); name.clear(); }
                        else if (!std::isspace(static_cast<unsigned char>(c))) name += c;
                    }
                    if (!name.empty()) exports.push_back(name);
                }
            }
        }
    }
    // Args alone imply scope=local even without a shebang — so `:run X
    // KEY=VAL` always isolates the script's writes from the caller.
    // RAII guard ensures every early-return / exception path through
    // the loop body still pops the frame exactly once.
    bool needLocal = (scope == Scope::Local) || !args.empty();
    ScriptFrameGuard frame(*this, needLocal, args, exports);

    // Buffer the script body (post-shebang) into a line vector so the
    // block-aware dispatcher can find matching :endif / :end without
    // re-reading the stream. srcLineOffset translates buffer index back
    // to the original 1-indexed file line number for failure reports
    // (issue #80) — if we consumed a shebang on line 1, buffer[0] is
    // file line 2.
    std::vector<std::string> lines;
    if (!firstLine.empty() && !consumedShebang) lines.push_back(std::move(firstLine));
    result.srcLineOffset = consumedShebang ? 2 : 1;
    std::string line;
    while (std::getline(in, line)) lines.push_back(std::move(line));
    dispatchScriptLines(lines, 0, lines.size(), result, strict);
    return result;                                  // RAII pops frame
}

namespace {
// Strip a trailing '#' comment, respecting single/double quotes so a '#'
// inside a quoted argument (e.g. a label string) is preserved. `#` only
// starts a comment when it's at the line start or preceded by whitespace
// AND followed by whitespace or end-of-line — so mid-token hashes like
// `:color #cfcfcf` (hex literal, issue #92) survive intact. Inline
// trailing comments (`:foo bar # note`) still strip as expected.
// Backslash escapes (`\#`) are NOT honoured.
void stripScriptComment(std::string& s) {
    bool inQuote = false;
    char qc = '\0';
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (inQuote) {
            if (c == qc) inQuote = false;
        } else if (c == '"' || c == '\'') {
            inQuote = true; qc = c;
        } else if (c == '#') {
            bool leftBoundary = (i == 0) ||
                std::isspace(static_cast<unsigned char>(s[i - 1]));
            bool rightBoundary = (i + 1 == s.size()) ||
                std::isspace(static_cast<unsigned char>(s[i + 1]));
            if (leftBoundary && rightBoundary) {
                s.resize(i);
                return;
            }
        }
    }
}

// True iff `line` (after leading whitespace + optional `:`) starts
// with the keyword `kw` followed by whitespace or end-of-line. On hit,
// fills `body` with the rest of the line (after the keyword).
bool isControlKeyword(const std::string& line, const char* kw, std::string& body) {
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos) return false;
    if (line[i] == ':') {
        ++i;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    }
    size_t klen = std::strlen(kw);
    if (line.compare(i, klen, kw) != 0) return false;
    size_t after = i + klen;
    if (after < line.size() && line[after] != ' ' && line[after] != '\t') return false;
    while (after < line.size() && (line[after] == ' ' || line[after] == '\t')) ++after;
    body = line.substr(after);
    // trim trailing whitespace
    size_t end = body.find_last_not_of(" \t\r");
    if (end == std::string::npos) body.clear();
    else                          body.resize(end + 1);
    return true;
}
}  // namespace

Application::ScriptFlow Application::dispatchScriptLines(
        const std::vector<std::string>& lines,
        size_t lo, size_t hi,
        ScriptRunResult& result, bool strict) {
    using Flow = ScriptFlow;
    // Centralise the "record a failure" path so every report (command
    // exec error, bad :if condition, malformed :foreach header, …)
    // contributes uniform context to result.failureList — issue #80.
    auto recordFailure = [&](size_t bufIdx, const std::string& srcLine,
                              const std::string& reason) {
        ++result.failures;
        int lineNum = static_cast<int>(bufIdx) + result.srcLineOffset;
        result.failureList.push_back({lineNum, srcLine, reason});
        if (result.firstFail.empty()) {
            result.firstFail = reason;
            result.failLine = srcLine;
        }
        // Stream errors to stderr in batch mode so they don't get buried
        // by stdout redirection or collapsed into the final lastMsg line
        // (which was previously routed to stdout for single-failure runs).
        // main.cpp suppresses its post-run failureList dump when headless.
        // Issue #90.
        if (isHeadless()) {
            std::fprintf(stderr, "%s\n",
                         result.formatFailure(result.failureList.back()).c_str());
        }
    };
    // Execute a single non-control line (the original runOne lambda).
    // bufIdx is the index of the source line in `lines`; srcLine is the
    // original (pre-`;`-split, pre-expand) text — both go into the
    // failure record so reports cite the *file* line, not the synthetic
    // semicolon-split fragment.
    auto runOne = [&](std::string cmd, size_t bufIdx, const std::string& srcLine) -> bool {
        size_t start = cmd.find_first_not_of(" \t");
        if (start == std::string::npos || cmd[start] == '#') return true;
        cmd.erase(0, start);
        if (!cmd.empty() && cmd[0] == ':') {
            cmd.erase(0, 1);
            size_t s2 = cmd.find_first_not_of(" \t");
            if (s2 == std::string::npos) return true;
            cmd.erase(0, s2);
        }
        size_t end = cmd.find_last_not_of(" \t");
        if (end == std::string::npos) return true;
        cmd.resize(end + 1);
        cmd = expandScriptVars(cmd);
        // User-defined function call? If the first token names a :def function,
        // run its body in a fresh local frame with params seeded from the call
        // args (as env vars) instead of dispatching to a built-in command.
        {
            size_t sp = cmd.find_first_of(" \t");
            std::string fname = cmd.substr(0, sp);
            auto fit = userScriptFns_.find(fname);
            if (fit != userScriptFns_.end()) {
                std::vector<std::string> callArgs;
                if (sp != std::string::npos) {
                    std::istringstream iss(cmd.substr(sp));
                    std::string tok;
                    while (iss >> tok) callArgs.push_back(tok);
                }
                if (++scriptFnDepth_ > 64) {
                    --scriptFnDepth_;
                    recordFailure(bufIdx, srcLine, "script function recursion too deep: " + fname);
                    if (strict) { result.stopped = true; return false; }
                    return true;
                }
                std::unordered_map<std::string, std::string> seed;
                for (size_t k = 0; k < fit->second.params.size(); ++k)
                    seed[fit->second.params[k]] = k < callArgs.size() ? callArgs[k] : "";
                const std::vector<std::string> fnBody = fit->second.body;  // stable copy
                ScriptFrameGuard guard(*this, true, seed, {});
                ScriptFlow ff = dispatchScriptLines(fnBody, 0, fnBody.size(), result, strict);
                --scriptFnDepth_;
                return ff != ScriptFlow::Stopped;
            }
        }
        ExecResult r = cmdRegistry_.execute(*this, cmd);
        ++result.count;
        if (!r.msg.empty()) result.lastMsg = r.msg;
        if (!r.ok) {
            recordFailure(bufIdx, srcLine.empty() ? cmd : srcLine, r.msg);
            if (strict) { result.stopped = true; return false; }
        } else if (!r.msg.empty() && isHeadless()) {
            // Stream success messages to stdout so multi-line outputs
            // like `:objects` and `:registers` reach the user instead
            // of being clobbered by the next command's result. TUI mode
            // surfaces them via cmdLine.setMessage. Issue #90.
            std::fwrite(r.msg.data(), 1, r.msg.size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        }
        return true;
    };
    // Find the next `;` outside of "..."/'...' quotes, so a literal
    // semicolon inside a quoted label/measure caption isn't treated as
    // a command separator (issue #87). stripScriptComment uses the same
    // quote-tracking rule for `#`.
    auto findUnquotedSemicolon = [](const std::string& s, size_t from) -> size_t {
        bool inQuote = false;
        char qc = '\0';
        for (size_t i = from; i < s.size(); ++i) {
            char c = s[i];
            if (inQuote) {
                if (c == qc) inQuote = false;
            } else if (c == '"' || c == '\'') {
                inQuote = true; qc = c;
            } else if (c == ';') {
                return i;
            }
        }
        return std::string::npos;
    };
    auto runLine = [&](std::string line, size_t bufIdx) -> bool {
        const std::string srcLine = line;
        stripScriptComment(line);
        size_t pos = 0;
        while (pos <= line.size()) {
            size_t next = findUnquotedSemicolon(line, pos);
            if (next == std::string::npos) next = line.size();
            if (!runOne(line.substr(pos, next - pos), bufIdx, srcLine)) return false;
            pos = next + 1;
        }
        return true;
    };

    // Find the matching close for a control block. Handles nesting:
    // each `openKw` line increments depth, each `closeKw` decrements.
    // On unmatched (depth never returns to 0) the caller logs an
    // "unmatched :foreach / :if" error and treats the rest of the
    // range as the body, so a missing :endif doesn't silently swallow
    // commands that were meant to run unconditionally.
    auto findMatchingClose = [&](size_t startIdx, const char* openKw,
                                  const char* closeKw, bool& matched) -> size_t {
        int depth = 1;
        std::string body;
        for (size_t i = startIdx + 1; i < hi; ++i) {
            if (isControlKeyword(lines[i], openKw,  body)) ++depth;
            else if (isControlKeyword(lines[i], closeKw, body)) {
                --depth;
                if (depth == 0) { matched = true; return i; }
            }
        }
        matched = false;
        return hi;
    };

    // Evaluate a comparison expression `LHS OP RHS` using the existing
    // register expression evaluator on each side (so `$reg.x`, math,
    // and pos() all work). Returns nullopt on parse / type failure.
    auto evalCondition = [&](const std::string& expr) -> std::optional<bool> {
        static const char* kOps[] = {"==", "!=", "<=", ">=", "<", ">"};
        for (const char* op : kOps) {
            size_t p = expr.find(op);
            if (p == std::string::npos) continue;
            std::string lhs = expr.substr(0, p);
            std::string rhs = expr.substr(p + std::strlen(op));
            trimWhitespace(lhs);
            trimWhitespace(rhs);
            // Same fully-wired context as :let, so selection-based builtins
            // (centroid/com/rmsd/pca/helix_axis/superpose_axis) work inside
            // conditionals too (previously only `regs` was wired here).
            RegisterExpr::Context ctx = makeExprContext(*this);
            auto lr = RegisterExpr::eval(lhs, ctx);
            auto rr = RegisterExpr::eval(rhs, ctx);
            if (!lr.ok || !rr.ok) return std::nullopt;
            if (lr.value.kind != Register::Kind::Scalar ||
                rr.value.kind != Register::Kind::Scalar) return std::nullopt;
            double l = lr.value.scalar, r = rr.value.scalar;
            if      (std::strcmp(op, "==") == 0) return l == r;
            else if (std::strcmp(op, "!=") == 0) return l != r;
            else if (std::strcmp(op, "<=") == 0) return l <= r;
            else if (std::strcmp(op, ">=") == 0) return l >= r;
            else if (std::strcmp(op, "<")  == 0) return l <  r;
            else                                  return l >  r;
        }
        return std::nullopt;
    };

    // Block stack — one entry per open :if. `active` is true while the
    // current branch should execute its body lines; `anyTaken` is true
    // once any branch (if / elseif / else) has been entered, so
    // subsequent elseif/else know to stay inactive.
    struct IfFrame { bool active; bool anyTaken; };
    std::vector<IfFrame> ifStack;
    auto anyInactive = [&]() {
        for (const auto& f : ifStack) if (!f.active) return true;
        return false;
    };

    std::string body;
    for (size_t i = lo; i < hi; ++i) {
        const std::string& srcLine = lines[i];

        if (isControlKeyword(srcLine, "if", body)) {
            if (anyInactive()) {
                // Still track so :endif pops correctly; subsequent
                // iterations skip the body automatically via
                // anyInactive() since active=false.
                ifStack.push_back({false, false});
                continue;
            }
            std::string expanded = expandScriptVars(body);
            auto cond = evalCondition(expanded);
            if (!cond) {
                recordFailure(i, srcLine, "bad :if condition: " + expanded);
                if (strict) { result.stopped = true; return Flow::Stopped; }
                ifStack.push_back({false, false});
            } else {
                ifStack.push_back({*cond, *cond});
            }
            continue;
        }
        if (isControlKeyword(srcLine, "elseif", body)) {
            if (ifStack.empty()) continue;
            if (ifStack.back().anyTaken) {
                ifStack.back().active = false;
            } else {
                std::string expanded = expandScriptVars(body);
                auto cond = evalCondition(expanded);
                bool c = cond.value_or(false);
                ifStack.back().active = c;
                if (c) ifStack.back().anyTaken = true;
            }
            continue;
        }
        if (isControlKeyword(srcLine, "else", body)) {
            if (ifStack.empty()) continue;
            ifStack.back().active = !ifStack.back().anyTaken;
            ifStack.back().anyTaken = true;
            continue;
        }
        if (isControlKeyword(srcLine, "endif", body)) {
            if (!ifStack.empty()) ifStack.pop_back();
            continue;
        }
        if (isControlKeyword(srcLine, "foreach", body)) {
            bool matched = false;
            size_t endIdx = findMatchingClose(i, "foreach", "end", matched);
            if (!matched) {
                recordFailure(i, srcLine, "unmatched :foreach (no :end found)");
                if (strict) { result.stopped = true; return Flow::Stopped; }
                i = endIdx;
                continue;
            }
            if (anyInactive()) { i = endIdx; continue; }
            // Parse `VAR in LO..HI`. expandScriptVars first so the
            // bounds can come from registers / env vars.
            std::string expanded = expandScriptVars(body);
            auto inPos = expanded.find(" in ");
            if (inPos == std::string::npos) {
                recordFailure(i, srcLine, "bad :foreach header (expected `VAR in LO..HI`): " + expanded);
                if (strict) { result.stopped = true; return Flow::Stopped; }
                i = endIdx;
                continue;
            }
            std::string var = expanded.substr(0, inPos);
            std::string rangeExpr = expanded.substr(inPos + 4);
            trimWhitespace(var);
            if (var.empty()) { i = endIdx; continue; }
            // `LO..HI` → numeric range. Otherwise the tail is a selection and
            // we iterate its residues, binding ${var} (a `chain:resi` spec
            // usable in pos()), ${var_chain}, ${var_resi}, ${var_resn}.
            auto dotPos = rangeExpr.find("..");
            if (dotPos != std::string::npos) {
                int loVal = 0, hiVal = 0;
                try {
                    loVal = std::stoi(rangeExpr.substr(0, dotPos));
                    hiVal = std::stoi(rangeExpr.substr(dotPos + 2));
                } catch (const std::exception&) {
                    recordFailure(i, srcLine, "bad :foreach numeric range: " + rangeExpr);
                    if (strict) { result.stopped = true; return Flow::Stopped; }
                    i = endIdx;
                    continue;
                }
                // Scope the loop variable: save any prior binding and restore
                // it after the loop so `$var` doesn't leak into the enclosing
                // scope.
                auto prior = registers().find(var);
                bool hadPrior = prior != registers().end();
                Register priorVal = hadPrior ? prior->second : Register{};
                Flow outFlow = Flow::Normal;
                for (int v = loVal; v <= hiVal; ++v) {
                    Register r; r.kind = Register::Kind::Scalar; r.scalar = v;
                    registers()[var] = r;
                    Flow f = dispatchScriptLines(lines, i + 1, endIdx, result, strict);
                    if (f == Flow::Continue) continue;
                    if (f == Flow::Break) break;
                    if (f == Flow::Stopped || f == Flow::Return) { outFlow = f; break; }
                }
                if (hadPrior) registers()[var] = priorVal;
                else          registers().erase(var);
                if (outFlow != Flow::Normal) return outFlow;
                i = endIdx;
                continue;
            }
            // Selection form: iterate distinct residues. rangeExpr is already
            // ${}-expanded, so `chain ${CH}` resolves before parsing.
            auto fobj = tabMgr_.currentTab().currentObject();
            if (!fobj) {
                recordFailure(i, srcLine, ":foreach over a selection needs a current object");
                if (strict) { result.stopped = true; return Flow::Stopped; }
                i = endIdx;
                continue;
            }
            Selection fsel = parseSelection(rangeExpr, *fobj);
            // Atoms of a residue are contiguous, so group consecutive picks by
            // (chain, resSeq, insCode) to get distinct residues in order.
            struct ResKey { std::string chain, resn; int resSeq; char ins; };
            std::vector<ResKey> residues;
            for (int idx : fsel.indices()) {
                if (idx < 0 || idx >= static_cast<int>(fobj->atoms().size())) continue;
                const auto& a = fobj->atoms()[idx];
                if (!residues.empty() && residues.back().chain == a.chainId &&
                    residues.back().resSeq == a.resSeq && residues.back().ins == a.insCode)
                    continue;
                residues.push_back({a.chainId, a.resName, a.resSeq, a.insCode});
            }
            // Save/restore the four bound env keys around the loop.
            const std::string keys[] = {var, var + "_chain", var + "_resi", var + "_resn"};
            std::unordered_map<std::string, std::optional<std::string>> saved;
            for (const auto& k : keys) {
                auto it = scriptEnv().find(k);
                saved[k] = it != scriptEnv().end()
                    ? std::optional<std::string>(it->second) : std::nullopt;
            }
            Flow outFlow = Flow::Normal;
            for (const auto& rk : residues) {
                std::string resi = std::to_string(rk.resSeq);
                scriptEnv()[var]            = rk.chain + ":" + resi;
                scriptEnv()[var + "_chain"] = rk.chain;
                scriptEnv()[var + "_resi"]  = resi;
                scriptEnv()[var + "_resn"]  = rk.resn;
                Flow f = dispatchScriptLines(lines, i + 1, endIdx, result, strict);
                if (f == Flow::Continue) continue;
                if (f == Flow::Break) break;
                if (f == Flow::Stopped || f == Flow::Return) { outFlow = f; break; }
            }
            for (const auto& [k, v] : saved) {
                if (v) scriptEnv()[k] = *v;
                else   scriptEnv().erase(k);
            }
            if (outFlow != Flow::Normal) return outFlow;
            i = endIdx;       // past the :end
            continue;
        }
        if (isControlKeyword(srcLine, "end", body)) {
            // Stray :end (foreach consumed its own); ignore.
            continue;
        }
        // `:def name(p1, p2) ... :enddef` — capture the body (without running
        // it) and register a callable function. The header is NOT expanded:
        // name and params are literal identifiers.
        if (isControlKeyword(srcLine, "def", body)) {
            bool matched = false;
            size_t endIdx = findMatchingClose(i, "def", "enddef", matched);
            if (!matched) {
                recordFailure(i, srcLine, "unmatched :def (no :enddef found)");
                if (strict) { result.stopped = true; return Flow::Stopped; }
                i = endIdx;
                continue;
            }
            if (anyInactive()) { i = endIdx; continue; }
            std::string fname;
            std::vector<std::string> params;
            auto lp = body.find('(');
            if (lp != std::string::npos) {
                fname = body.substr(0, lp);
                auto rp = body.find(')', lp);
                std::string inside = body.substr(lp + 1,
                    rp == std::string::npos ? std::string::npos : rp - lp - 1);
                size_t p = 0;
                while (p <= inside.size()) {
                    size_t comma = inside.find(',', p);
                    std::string tok = inside.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
                    trimWhitespace(tok);
                    if (!tok.empty()) params.push_back(tok);
                    if (comma == std::string::npos) break;
                    p = comma + 1;
                }
            } else {
                std::istringstream iss(body);
                iss >> fname;
                std::string p;
                while (iss >> p) params.push_back(p);
            }
            trimWhitespace(fname);
            if (!isBareName(fname)) {
                recordFailure(i, srcLine, "bad :def name: " + fname);
                if (strict) { result.stopped = true; return Flow::Stopped; }
                i = endIdx;
                continue;
            }
            UserScriptFn fn;
            fn.params = std::move(params);
            for (size_t k = i + 1; k < endIdx; ++k) fn.body.push_back(lines[k]);
            userScriptFns_[fname] = std::move(fn);
            i = endIdx;
            continue;
        }
        if (isControlKeyword(srcLine, "enddef", body)) {
            // Stray :enddef (def consumed its own); ignore.
            continue;
        }
        // Loop control — only act when no enclosing :if branch is inactive,
        // so `:if cond / :break / :endif` honors the condition. They unwind to
        // the nearest :foreach (break/continue) or the script frame (return).
        if (isControlKeyword(srcLine, "break", body)) {
            if (anyInactive()) continue;
            return Flow::Break;
        }
        if (isControlKeyword(srcLine, "continue", body)) {
            if (anyInactive()) continue;
            return Flow::Continue;
        }
        if (isControlKeyword(srcLine, "return", body)) {
            if (anyInactive()) continue;
            return Flow::Return;
        }

        // Non-control line. Skip if any enclosing :if branch is inactive.
        if (anyInactive()) continue;
        if (!runLine(srcLine, i)) return Flow::Stopped;
    }
    return Flow::Normal;
}

namespace {
// Unknown tokens render as literal text ("{wat}" stays "{wat}") so a typo
// in label_format never crashes a figure script — the user sees their typo.
std::string expandLabelTemplate(const std::string& fmt, const AtomData& a) {
    std::string out;
    out.reserve(fmt.size() + 8);
    for (size_t i = 0; i < fmt.size(); ) {
        if (fmt[i] != '{') { out += fmt[i++]; continue; }
        size_t close = fmt.find('}', i + 1);
        if (close == std::string::npos) { out += fmt.substr(i); break; }
        std::string tok = fmt.substr(i + 1, close - i - 1);
        if      (tok == "resname")        out += a.resName;
        else if (tok == "resseq" ||
                 tok == "seqid")          out += std::to_string(a.resSeq);
        else if (tok == "chain")          out += a.chainId;
        else if (tok == "name")           out += a.name;
        else if (tok == "element")        out += a.element;
        else if (tok == "restype")        out += SeqBar::toOneLetter(a.resName);
        else                              out += fmt.substr(i, close - i + 1);
        i = close + 1;
    }
    return out;
}


}  // namespace
// The label-corner / bg-mode helpers below have external linkage: they are
// shared with the annotation and settings command modules.

// Accept either the long form (topleft) or the two-letter short (tl)
// — keeps :label corner and :unlabel corner in lockstep so a new alias
// only has to be added in one place.
std::optional<Application::FreeLabelCorner> parseCornerName(const std::string& w) {
    using C = Application::FreeLabelCorner;
    if (w == "topleft"     || w == "tl") return C::TopLeft;
    if (w == "topright"    || w == "tr") return C::TopRight;
    if (w == "bottomleft"  || w == "bl") return C::BottomLeft;
    if (w == "bottomright" || w == "br") return C::BottomRight;
    return std::nullopt;
}

// Remove free labels of a given anchor kind (Corner / Screen / World).
// When `corner` is set, only that one corner is removed; otherwise every
// label of the kind goes. Returns the number of entries removed.
// Shared by `:unlabel corner|screen|world` and `:label <kind> = ""`
// (issue #65) so the per-anchor mutation rule lives in one place.
size_t clearFreeLabelsByAnchor(std::vector<Application::FreeLabel>& fls,
                                Application::FreeLabelAnchor anchor,
                                std::optional<Application::FreeLabelCorner> corner) {
    size_t before = fls.size();
    fls.erase(std::remove_if(fls.begin(), fls.end(),
              [&](const Application::FreeLabel& fl) {
                  if (fl.anchor != anchor) return false;
                  if (anchor == Application::FreeLabelAnchor::Corner && corner)
                      return fl.corner == *corner;
                  return true;
              }), fls.end());
    return before - fls.size();
}


// Short aliases for tab-completion only — not iterated by `:set` listing
// (would print the same value twice). Pair-wise alignment with kSetOptionsLong
// is not enforced; the alias->long mapping lives in the per-option
// if/else cascade inside the `:set` handler.
inline constexpr const char* kSetOptionsShort[] = {
    "bt", "wt", "br", "ps", "ot", "od",
    "lfs", "anf", "anlw", "scale", "sm",
    "ch", "csh", "cl", "csd", "csa", "chr", "cth", "ctr", "nb",
    "bsf", "sfs",
    "ic", "it", "iclass", "isc", "is",
    "sa", "v", "lf", "transp",
    "fr", "fe", "fmr", "fd", "fg",
};

const char* bgModeName(BgMode m) {
    switch (m) {
        case BgMode::Transparent: return "transparent";
        case BgMode::White:       return "white";
        case BgMode::Black:       return "black";
        case BgMode::Custom:      return "custom";
    }
    return "transparent";
}

void Application::logViewState(const ParsedCommand& cmd, int basisAtoms) const {
    if (!verbose_) return;
    auto& tab = tabMgr_.currentTab();
    const auto& cam = tab.camera();
    // For commands that fitted a bbox to a selection (:center / :zoom /
    // :orient / :focus), report the selection-basis atom count — that
    // is the number that actually went into center/zoom. Otherwise fall
    // back to the current object's total atom count. Previously this
    // always reported the latter, which made it look like the bbox was
    // computed from the whole object even when an obj/(sel) qualifier
    // narrowed it (issue #85 confusion source).
    int reported;
    const char* label;
    if (basisAtoms >= 0) {
        reported = basisAtoms;
        label = "basis_atoms";
    } else {
        auto obj = tab.currentObject();
        reported = obj ? static_cast<int>(obj->atoms().size()) : 0;
        label = "total_atoms";
    }
    // Width 7 = max(label_len)+1 (longest is "orient", 6) for column alignment.
    std::fprintf(stderr,
        "[view] %-7s -> center=(%.2f, %.2f, %.2f) zoom=%.3f pan=(%.2f, %.2f) %s=%d\n",
        cmd.name.c_str(),
        cam.centerX(), cam.centerY(), cam.centerZ(),
        cam.zoom(), cam.panXOffset(), cam.panYOffset(), label, reported);
}

void Application::logSelectionInfo(const std::string& name,
                                   const std::string& expr,
                                   const Selection& sel,
                                   const MolObject& obj) const {
    if (!verbose_) return;
    const auto& atoms = obj.atoms();
    std::set<std::string> chains;
    std::set<std::tuple<std::string, int, char>> residues;
    for (int idx : sel.indices()) {
        if (idx < 0 || idx >= static_cast<int>(atoms.size())) continue;
        const auto& a = atoms[idx];
        chains.insert(a.chainId);
        residues.emplace(a.chainId, a.resSeq, a.insCode);
    }
    std::string chainList;
    for (const auto& c : chains) {
        if (!chainList.empty()) chainList += ',';
        chainList += c.empty() ? "_" : c;
    }
    std::fprintf(stderr,
        "[sel] %s = %s  ->  %zu atoms / %zu residues",
        name.c_str(), expr.c_str(), sel.size(), residues.size());
    if (chains.size() > 1)
        std::fprintf(stderr, " across chains %s", chainList.c_str());
    std::fputc('\n', stderr);
}

void Application::logAlignPair(const std::string& mobile,
                               const std::string& target,
                               bool complex,
                               const AlignResult& r) const {
    if (!verbose_) return;
    // r.message is built by Aligner::parseOutput on success ("TM1=… TM2=…
    // RMSD=… Aligned=…") and carries the failure cause otherwise. Reuse
    // it so the wire format stays in lockstep with the user-facing
    // result string.
    std::fprintf(stderr, "[align] %s -> %s (%s) %s%s\n",
                 mobile.c_str(), target.c_str(),
                 complex ? "MM" : "TM",
                 r.success ? "" : "failed: ",
                 r.message.c_str());
}

int Application::countVisibleAtoms() const {
    int total = 0;
    const auto& tab = tabMgr_.currentTab();
    for (const auto& obj : tab.objects()) {
        if (!obj || !obj->visible()) continue;
        // If no repr is enabled at all, no atoms render — treat as 0 for
        // this object. Otherwise count atoms that would draw under any
        // active repr's mask (or all atoms when no per-atom mask is set).
        bool anyRepr = false;
        for (const auto& [type, _] : representations_) {
            if (obj->reprVisible(type)) { anyRepr = true; break; }
        }
        if (!anyRepr) continue;
        if (!obj->hasPerAtomRepr()) {
            total += static_cast<int>(obj->atoms().size());
            continue;
        }
        for (int i = 0; i < static_cast<int>(obj->atoms().size()); ++i) {
            for (const auto& [type, _] : representations_) {
                if (obj->reprVisibleForAtom(type, i)) { ++total; break; }
            }
        }
    }
    return total;
}

void Application::logRenderStats(int pixW, int pixH, int dpi,
                                 int visibleAtoms,
                                 double elapsedSec) const {
    if (!verbose_) return;
    std::fprintf(stderr, "[render] PNG %dx%d", pixW, pixH);
    if (dpi > 0) std::fprintf(stderr, " @ %d dpi", dpi);
    std::fprintf(stderr,
        " bg=%s outline=%s fog=%.2f elapsed=%.2fs visible_atoms=%d\n",
        bgModeName(bgMode_), outlineEnabled_ ? "on" : "off", fogStrength_,
        elapsedSec, visibleAtoms);
}

float Application::computeRenderScale(int canvasHeight, int dpi) const {
    switch (overlaySizeMode_) {
        case OverlaySizeMode::Pixels:
            return 1.0f;
        case OverlaySizeMode::Physical: {
            int eff = (dpi > 0) ? dpi : liveDpi_;
            if (eff <= 0 || liveDpi_ <= 0) return 1.0f;
            return static_cast<float>(eff) / static_cast<float>(liveDpi_);
        }
        case OverlaySizeMode::Relative: {
            if (canvasHeight <= 0 || referenceCanvasHeight_ <= 0) return 1.0f;
            return static_cast<float>(canvasHeight) /
                   static_cast<float>(referenceCanvasHeight_);
        }
    }
    return 1.0f;
}

void Application::applyBgMode(PixelCanvas& pc) const {
    switch (bgMode_) {
        case BgMode::Transparent: pc.setBackground(0,   0,   0,   true);  break;
        case BgMode::White:       pc.setBackground(255, 255, 255, false); break;
        case BgMode::Black:       pc.setBackground(0,   0,   0,   false); break;
        case BgMode::Custom:      pc.setBackground(bgCustomR_, bgCustomG_, bgCustomB_, false); break;
    }
}

std::string Application::expandScriptVars(const std::string& line) const {
    if (line.find_first_of("$\\") == std::string::npos) return line;
    std::string out;
    out.reserve(line.size());
    for (size_t i = 0; i < line.size(); ) {
        char c = line[i];
        if (c == '\\' && i + 1 < line.size() && line[i + 1] == '$') {
            out += '$';
            i += 2;
            continue;
        }
        if (c == '$' && i + 1 < line.size() && line[i + 1] == '{') {
            size_t close = line.find('}', i + 2);
            if (close != std::string::npos) {
                // Body grammar: <name>[.field][:fmt]
                //   name  : isValidEnvName (alnum + underscore)
                //   field : optional dotted accessor (axis1, center, x, length, …)
                //   fmt   : optional printf spec (e.g. .2f, 4.1f, .0e, d)
                // Lookup order:
                //   1. registers_ (typed; vec3 → "(x, y, z)" by default)
                //   2. scriptEnv_ (set by :setenv)
                //   3. process getenv()
                std::string body = line.substr(i + 2, close - i - 2);
                std::string name, field, fmt;
                size_t dot = body.find('.');
                size_t colon = body.find(':');
                size_t nameEnd = std::min(dot, colon);
                name = body.substr(0, nameEnd);
                if (dot != std::string::npos && dot < colon) {
                    field = body.substr(dot + 1,
                              (colon == std::string::npos ? body.size() : colon) - dot - 1);
                }
                if (colon != std::string::npos) fmt = body.substr(colon + 1);
                bool consumed = false;
                if (isValidEnvName(name)) {
                    const auto& regs = registers();
                    auto regIt = regs.find(name);
                    if (regIt != regs.end()) {
                        const Register& r = regIt->second;
                        // Try Scalar field first, then Vec3.
                        if (auto s = r.getScalar(field)) {
                            char buf[64];
                            std::string spec = fmt.empty() ? std::string("%g") : ("%" + fmt);
                            std::snprintf(buf, sizeof(buf), spec.c_str(), *s);
                            out += buf;
                            consumed = true;
                        } else if (auto v = r.getVec(field)) {
                            char buf[128];
                            std::string spec = fmt.empty()
                                ? std::string("(%.3f, %.3f, %.3f)")
                                : ("(%" + fmt + ", %" + fmt + ", %" + fmt + ")");
                            std::snprintf(buf, sizeof(buf), spec.c_str(),
                                          (*v)[0], (*v)[1], (*v)[2]);
                            out += buf;
                            consumed = true;
                        }
                        // Field doesn't match anything — fall through to env.
                    }
                    if (!consumed) {
                        const auto& env = scriptEnv();
                        auto it = env.find(name);
                        if (it != env.end()) {
                            out += it->second;
                            consumed = true;
                        } else if (const char* sysenv = std::getenv(name.c_str())) {
                            out += sysenv;
                            consumed = true;
                        }
                    }
                }
                if (consumed || isValidEnvName(name)) {
                    i = close + 1;
                    continue;
                }
            }
        }
        out += c;
        ++i;
    }
    return out;
}

std::string Application::resolveLabel(const MolObject& obj,
                                      const ObjectLabels& labels, int idx) const {
    const auto& atoms = obj.atoms();
    // A label set may outlive the atom it points at (object reload); guard.
    if (idx < 0 || idx >= static_cast<int>(atoms.size())) return {};
    const auto& a = atoms[idx];
    if (!labels.text.empty()) {
        auto it = labels.text.find(idx);
        if (it != labels.text.end()) return it->second;
    }
    if (!labelFormat_.empty()) return expandLabelTemplate(labelFormat_, a);
    return a.resName + std::to_string(a.resSeq);
}

void Application::addAtomLabel(const std::string& objName, int idx,
                               const std::string* text) {
    auto& ls = labelsByObject_[objName];
    if (std::find(ls.atoms.begin(), ls.atoms.end(), idx) == ls.atoms.end())
        ls.atoms.push_back(idx);
    // A per-atom override replaces any prior one; the no-`=` path clears it
    // so re-labelling returns the atom to the template/default text.
    if (text) ls.text[idx] = *text;
    else      ls.text.erase(idx);
}

size_t Application::removeAtomLabels(const std::string& objName,
                                     const std::set<int>& idxs) {
    auto it = labelsByObject_.find(objName);
    if (it == labelsByObject_.end()) return 0;
    auto& ls = it->second;
    size_t before = ls.atoms.size();
    ls.atoms.erase(std::remove_if(ls.atoms.begin(), ls.atoms.end(),
                   [&](int i) { return idxs.count(i) > 0; }), ls.atoms.end());
    for (int i : idxs) ls.text.erase(i);
    size_t removed = before - ls.atoms.size();
    if (ls.atoms.empty()) labelsByObject_.erase(it);
    return removed;
}

void Application::initRepresentations() {
    representations_[ReprType::Wireframe] = std::make_unique<WireframeRepr>();
    representations_[ReprType::BallStick] = std::make_unique<BallStickRepr>();
    representations_[ReprType::Backbone]  = std::make_unique<BackboneRepr>();
    representations_[ReprType::Spacefill] = std::make_unique<SpacefillRepr>();
    representations_[ReprType::Cartoon]   = std::make_unique<CartoonRepr>();
    representations_[ReprType::Ribbon]    = std::make_unique<RibbonRepr>();
    representations_[ReprType::Surface]   = std::make_unique<SurfaceRepr>();
}

Representation* Application::getRepr(ReprType type) {
    auto it = representations_.find(type);
    return (it != representations_.end()) ? it->second.get() : nullptr;
}

void Application::setRenderer(RendererType type) {
    rendererType_ = type;
    switch (type) {
        case RendererType::Ascii:
            canvas_ = std::make_unique<AsciiCanvas>();
            break;
        case RendererType::Braille:
            canvas_ = std::make_unique<BrailleCanvas>();
            break;
        case RendererType::Block:
            canvas_ = std::make_unique<BlockCanvas>();
            break;
        case RendererType::Pixel: {
            auto proto = forcedProtocol_ != GraphicsProtocol::None
                ? forcedProtocol_ : ProtocolPicker::detect();
            auto encoder = ProtocolPicker::createEncoder(proto);
            if (encoder) {
                canvas_ = std::make_unique<PixelCanvas>(std::move(encoder));
            } else {
                rendererType_ = RendererType::Braille;
                canvas_ = std::make_unique<BrailleCanvas>();
            }
            break;
        }
    }
}

int Application::run() {
    if (quitRequested_) return 0;  // script quit before the main loop ever started
    if (isHeadless()) return 0;    // no TUI: nothing to render or read input from
    running_ = true;
    layout_.markAllDirty();
    needsRedraw_ = true;
    framesToSkip_ = 0;

    while (running_) {
        if (g_resized) {
            g_resized = 0;
            onResize();
        }

        if (needsRedraw_) {
            if (framesToSkip_ > 0) {
                --framesToSkip_;
                // Don't clear needsRedraw_ — the skipped frame must still render
            } else {
                auto t0 = std::chrono::steady_clock::now();
                renderFrame();
                auto t1 = std::chrono::steady_clock::now();
                lastFrameMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

                // Only skip continuous camera motion frames, not discrete commands
                if (lastFrameMs_ > 100) {
                    framesToSkip_ = std::min(3, static_cast<int>(lastFrameMs_ / 50));
                }
                needsRedraw_ = false;
            }
        }

        processInput();
    }

    return 0;
}

void Application::quit(bool force) {
    (void)force;
    SessionSaver::saveSession(*this);
    {
        std::string hp = resolveHistoryPath();
        if (!hp.empty()) cmdLine_.saveHistory(hp);
    }
    MLOG_INFO("Quitting MolTerm");
    running_ = false;
    quitRequested_ = true;
}

std::string Application::loadFile(const std::string& path) {
    MLOG_INFO("Loading file: %s", path.c_str());

    // Re-runnable scripts call `:load same.cif` repeatedly; without dedup,
    // each call would silently create same_1, same_2, … and stack visible
    // copies (issue #27). Skip if a loaded object already points at the
    // same canonical path. To force a refresh, :delete the existing object
    // first.
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path want = fs::weakly_canonical(path, ec);
    if (!ec && !want.empty()) {
        for (const auto& [name, obj] : store_) {
            if (!obj || obj->sourcePath().empty()) continue;
            fs::path have = fs::weakly_canonical(obj->sourcePath(), ec);
            if (ec) { ec.clear(); continue; }
            if (have == want) {
                // Keep "Loaded " prefix so :loadalign/:fetch ok-detection
                // (which sniffs `result.rfind("Loaded ", 0) == 0`) still
                // treats this as success.
                std::string msg = "Loaded " + name + " (cached, same path)";
                MLOG_INFO("%s", msg.c_str());
                cmdLine_.setMessage(msg);
                return msg;
            }
        }
    }

    // Show loading message immediately
    cmdLine_.setMessage("Loading " + path + "...");
    if (!isHeadless()) {
        cmdLine_.render(layout_.commandLine());
        layout_.commandLine().refresh();
        doupdate();
    }

    try {
        auto obj = CifLoader::loadAuto(path);
        std::string name = obj->name();
        int atomCount = static_cast<int>(obj->atoms().size());
        int bondCount = static_cast<int>(obj->bonds().size());
        int stateCount = obj->stateCount();

        obj->applySmartDefaults();
        auto ptr = store_.add(std::move(obj));
        tabMgr_.currentTab().addObject(ptr);
        if (autoCenter_) tabMgr_.currentTab().centerView();
        layout_.markAllDirty();
        needsRedraw_ = true;

        std::string msg = "Loaded " + name + ": " +
               std::to_string(atomCount) + " atoms, " +
               std::to_string(bondCount) + " bonds";
        if (stateCount > 1)
            msg += ", " + std::to_string(stateCount) + " states";
        MLOG_INFO("%s", msg.c_str());
        return msg;
    } catch (const std::exception& e) {
        MLOG_ERROR("Load failed: %s — %s", path.c_str(), e.what());
        return std::string("Error loading ") + path + ": " + e.what();
    }
}

void Application::processInput() {
    int key = screen_.getKey();
    if (key == ERR) return;

    // Handle mouse events
    if (key == KEY_MOUSE) {
        handleMouse(key);
        return;
    }

    // Macro register selection: awaiting a register key after 'q' or '@'
    if (macroAwaitingRegister_) {
        macroAwaitingRegister_ = false;
        if (key >= 'a' && key <= 'z') {
            if (macroRecording_) {
                stopMacroRecord();
            } else {
                startMacroRecord(static_cast<char>(key));
            }
        } else {
            cmdLine_.setMessage("Invalid macro register (use a-z)");
        }
        layout_.markDirty(Layout::Component::CommandLine);
        layout_.markDirty(Layout::Component::StatusBar);
        needsRedraw_ = true;
        return;
    }
    if (macroPlayAwaitingRegister_) {
        macroPlayAwaitingRegister_ = false;
        if (key >= 'a' && key <= 'z') {
            playMacro(static_cast<char>(key));
            layout_.markAllDirty();  // macro playback can affect anything
        } else {
            cmdLine_.setMessage("Invalid macro register (use a-z)");
            layout_.markDirty(Layout::Component::CommandLine);
        }
        needsRedraw_ = true;
        return;
    }

    // Info/help overlay: scroll keys page through, explicit close keys
    // dismiss; everything else (resize, mouse, modifier prefixes, stray
    // alpha keys) is ignored so the overlay stays put.
    // lastVisibleRows is updated each frame by the renderer; used for paging.
    if (infoOverlay_.active) {
        const int rows = std::max(1, infoOverlay_.lastVisibleRows);
        // Wrapped line count populated by the renderer — narrow terminals
        // produce more wrapped lines than `lines.size()` would suggest, so
        // paging math must use the wrapped total to land on the real bottom.
        const int total = std::max(infoOverlay_.lastTotalLines,
                                   static_cast<int>(infoOverlay_.lines.size()));
        bool dismiss = false;
        switch (key) {
            case 'j': case KEY_DOWN:
                infoOverlay_.scrollOffset += 1;
                break;
            case 'k': case KEY_UP:
                infoOverlay_.scrollOffset -= 1;
                break;
            case KEY_NPAGE: case 4: /* Ctrl-D */
                infoOverlay_.scrollOffset += rows;
                break;
            case ' ':
                // Space: page down while content remains; dismiss at bottom.
                if (infoOverlay_.scrollOffset + rows >= total) dismiss = true;
                else infoOverlay_.scrollOffset += rows;
                break;
            case KEY_PPAGE: case 21: /* Ctrl-U */
                infoOverlay_.scrollOffset -= rows;
                break;
            case 'g':
                infoOverlay_.scrollOffset = 0;
                break;
            case 'G':
                infoOverlay_.scrollOffset = std::max(0, total - rows);
                break;
            case 'q': case 27: /* Esc */ case '?': case '\n': case KEY_ENTER:
                dismiss = true;
                break;
            default:
                // Ignore unrecognised keys (KEY_RESIZE, KEY_MOUSE,
                // bare modifiers, etc.) — closing the overlay on any
                // resize would be hostile.
                return;
        }
        if (dismiss) {
            infoOverlay_.active = false;
            infoOverlay_.scrollOffset = 0;
            // Overlay text is written via ncurses on top of the viewport
            // canvas, so the pixel renderer's frame-diff cache doesn't know
            // those cells changed. Without an explicit invalidate, the next
            // flush emits no pixels for that region and the area where the
            // overlay sat reads as a black stripe. markAllDirty also covers
            // any neighbouring component the overlay may have overlapped.
            if (canvas_) canvas_->invalidate();
            layout_.markAllDirty();
        } else {
            if (infoOverlay_.scrollOffset < 0) infoOverlay_.scrollOffset = 0;
            layout_.markDirty(Layout::Component::Viewport);
        }
        needsRedraw_ = true;
        return;
    }

    Mode mode = inputHandler_->mode();

    if (mode == Mode::Command || mode == Mode::Search) {
        Action action = inputHandler_->processKey(key);
        if (action != Action::None) {
            handleAction(action);
        } else {
            if (mode == Mode::Command) {
                handleCommandInput(key);
            } else {
                handleSearchInput(key);
            }
        }
    } else {
        Action action = inputHandler_->processKey(key);
        if (action != Action::None) {
            handleAction(action);
            recordAction(action);
        }
    }
}

void Application::handleMouse(int /*key*/) {
    MEVENT event;
    if (getmouse(&event) != OK) return;

    using C = Layout::Component;
    auto& tab = tabMgr_.currentTab();
    auto& cam = tab.camera();

    needsRedraw_ = true;

    // Scroll wheel for zoom — only viewport + status bar
    if (event.bstate & BUTTON4_PRESSED) {
        cam.zoomBy(1.15f);
        clearViewFit();   // manual zoom overrides any fit intent (issue #98)
        layout_.markDirty(C::Viewport);
        layout_.markDirty(C::StatusBar);
        return;
    }
#ifdef BUTTON5_PRESSED
    if (event.bstate & BUTTON5_PRESSED) {
        cam.zoomBy(1.0f / 1.15f);
        clearViewFit();
        layout_.markDirty(C::Viewport);
        layout_.markDirty(C::StatusBar);
        return;
    }
#endif

    // Non-scroll clicks: may affect all visible components
    layout_.markDirty(C::Viewport);
    layout_.markDirty(C::CommandLine);
    layout_.markDirty(C::StatusBar);
    layout_.markDirty(C::SeqBar);

    if (event.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED)) {
        // Check if click is in tab bar region (row 0)
        if (event.y == 0) {
            auto names = tabMgr_.tabNames();
            int x = 1;
            for (int i = 0; i < static_cast<int>(names.size()); ++i) {
                int labelLen = static_cast<int>(names[i].size()) + 2;
                if (event.x >= x && event.x < x + labelLen) {
                    tabMgr_.goToTab(i);
                    break;
                }
                x += labelLen + 1;
            }
        }
        // Click in viewport → inspect or select atom
        else if (event.y >= 1 && event.y < 1 + layout_.viewportHeight()) {
            int vpX = event.x;
            int vpY = event.y - 1;  // offset for tab bar row
            buildProjCache();
            int atomIdx = findNearestAtom(vpX, vpY);
            if (atomIdx < 0) return;

            auto obj = tabMgr_.currentTab().currentObject();
            if (!obj) return;
            const auto& atoms = obj->atoms();
            const auto& a = atoms[atomIdx];

            if (pickMode_ == PickMode::SelectAtom) {
                // Click toggles atom in $sele
                auto& sele = namedSelections_[kSele];
                if (sele.has(atomIdx)) {
                    sele.removeIndex(atomIdx);
                    cmdLine_.setMessage("sele(-1) = " + std::to_string(sele.size()) +
                        " atoms | " + a.chainId + "/" + a.resName + " " +
                        std::to_string(a.resSeq) + "/" + a.name);
                } else {
                    sele.addIndex(atomIdx);
                    cmdLine_.setMessage("sele(+1) = " + std::to_string(sele.size()) +
                        " atoms | " + a.chainId + "/" + a.resName + " " +
                        std::to_string(a.resSeq) + "/" + a.name);
                }
            } else if (pickMode_ == PickMode::SelectResidue) {
                // Click toggles entire residue in $sele
                auto& sele = namedSelections_[kSele];
                // Check if any atom of this residue is already selected
                bool alreadySelected = false;
                std::vector<int> resAtoms;
                for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
                    if (atoms[i].chainId == a.chainId &&
                        atoms[i].resSeq == a.resSeq &&
                        atoms[i].insCode == a.insCode) {
                        resAtoms.push_back(i);
                        if (sele.has(i)) alreadySelected = true;
                    }
                }
                if (alreadySelected) {
                    for (int i : resAtoms) sele.removeIndex(i);
                    cmdLine_.setMessage("sele(-" + std::to_string(resAtoms.size()) + ") = " +
                        std::to_string(sele.size()) + " atoms [" +
                        a.chainId + "/" + a.resName + " " + std::to_string(a.resSeq) + "]");
                } else {
                    sele.addIndices(resAtoms);
                    cmdLine_.setMessage("sele(+" + std::to_string(resAtoms.size()) + ") = " +
                        std::to_string(sele.size()) + " atoms [" +
                        a.chainId + "/" + a.resName + " " + std::to_string(a.resSeq) + "]");
                }
            } else if (pickMode_ == PickMode::SelectChain) {
                // Click toggles entire chain in $sele
                auto& sele = namedSelections_[kSele];
                bool alreadySelected = false;
                std::vector<int> chainAtoms;
                for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
                    if (atoms[i].chainId == a.chainId) {
                        chainAtoms.push_back(i);
                        if (sele.has(i)) alreadySelected = true;
                    }
                }
                if (alreadySelected) {
                    for (int i : chainAtoms) sele.removeIndex(i);
                    cmdLine_.setMessage("sele(-" + std::to_string(chainAtoms.size()) + ") = " +
                        std::to_string(sele.size()) + " atoms [chain " + a.chainId + "]");
                } else {
                    sele.addIndices(chainAtoms);
                    cmdLine_.setMessage("sele(+" + std::to_string(chainAtoms.size()) + ") = " +
                        std::to_string(sele.size()) + " atoms [chain " + a.chainId + "]");
                }
            } else if (pickMode_ == PickMode::Focus) {
                // Mol*-style click-to-focus. enterFocus picks up the new
                // bounding-sphere zoom automatically. Mode stays active so
                // a second click refocuses on a different residue (matches
                // gs/gS/gc convention — ESC to exit).
                std::vector<int> subject = expandByFocusGranularity(*obj, atomIdx);
                pickedAtomIdx_ = atomIdx;        // so subsequent F can refocus
                enterFocus(*obj, subject, residueInfoString(a));
            } else {
                // Inspect mode — show info at current level
                pickedAtomIdx_ = atomIdx;
                activeViewState().focusResi = a.resSeq;
                activeViewState().focusChain = a.chainId;
                // Store in pick register (pk1-pk4, rotating)
                pickRegs_[pickNext_] = atomIdx;
                int pkNum = pickNext_ + 1;  // 1-based
                pickNext_ = (pickNext_ + 1) % 4;
                // Save as named selection $pk1..$pk4
                namedSelections_["pk" + std::to_string(pkNum)] =
                    Selection({atomIdx}, "pk" + std::to_string(pkNum));

                switch (inspectLevel_) {
                    case InspectLevel::Atom:
                        cmdLine_.setMessage("pk" + std::to_string(pkNum) + ": " +
                            atomInfoString(*obj, atomIdx));
                        break;
                    case InspectLevel::Residue: {
                        int resCount = 0;
                        for (const auto& at : atoms)
                            if (at.chainId == a.chainId && at.resSeq == a.resSeq && at.insCode == a.insCode)
                                ++resCount;
                        std::string ssStr = (a.ssType == SSType::Helix) ? "helix" :
                                            (a.ssType == SSType::Sheet) ? "sheet" : "loop";
                        cmdLine_.setMessage("RES: " + a.chainId + "/" + a.resName + " " +
                            std::to_string(a.resSeq) + " (" + std::to_string(resCount) +
                            " atoms, " + ssStr + ")");
                        break;
                    }
                    case InspectLevel::Chain: {
                        int chainAtoms = 0;
                        std::set<int> residues;
                        for (const auto& at : atoms) {
                            if (at.chainId == a.chainId) {
                                ++chainAtoms;
                                residues.insert(at.resSeq);
                            }
                        }
                        cmdLine_.setMessage("CHAIN: " + a.chainId + " (" +
                            std::to_string(residues.size()) + " residues, " +
                            std::to_string(chainAtoms) + " atoms)");
                        break;
                    }
                    case InspectLevel::Object:
                        cmdLine_.setMessage("OBJ: " + obj->name() + " (" +
                            std::to_string(atoms.size()) + " atoms, " +
                            std::to_string(obj->bonds().size()) + " bonds)");
                        break;
                }
            }
        }
        // Click in seqbar → center on residue (or refocus, if focus is active
        // or the focus pick mode is engaged — then the seqbar acts as a
        // residue navigator).
        else if (layout_.seqBarVisible()) {
            int seqBarY = layout_.seqBar().posY();
            int seqBarX = layout_.seqBar().posX();
            int seqBarH = layout_.seqBar().height();
            int seqBarW = layout_.seqBar().width();
            if (event.y < seqBarY || event.y >= seqBarY + seqBarH ||
                event.x < seqBarX || event.x >= seqBarX + seqBarW) return;
            std::string clickChain;
            int hitAtom = -1;
            int resi = activeSeqBar().resSeqAtColumn(
                event.y - seqBarY, event.x - seqBarX,
                layout_.seqBarWrap(), seqBarW, &clickChain, &hitAtom);
            if (resi < 0) return;
            activeViewState().focusResi = resi;
            activeViewState().focusChain = clickChain;
            auto obj = tabMgr_.currentTab().currentObject();
            if (!obj) return;
            const auto& atoms = obj->atoms();
            if (hitAtom < 0 || hitAtom >= (int)atoms.size()) return;
            const auto& a = atoms[hitAtom];
            if (focusActive() || pickMode_ == PickMode::Focus) {
                auto subject = expandByFocusGranularity(*obj, hitAtom);
                pickedAtomIdx_ = hitAtom;
                enterFocus(*obj, subject, residueInfoString(a));
            } else {
                auto& seqCam = tabMgr_.currentTab().camera();
                seqCam.setCenter(a.x, a.y, a.z);
                if (seqCam.zoom() < 5.0f) seqCam.setZoom(8.0f);
                cmdLine_.setMessage(a.chainId + "/" +
                    a.resName + " " + std::to_string(resi));
            }
        }
    }
}

void Application::handleAction(Action action) {
    using C = Layout::Component;
    auto& tab = tabMgr_.currentTab();
    auto& cam = tab.camera();
    float rs = cam.rotationSpeed();

    // Helper: mark specific components dirty and request redraw
    auto dirty = [&](std::initializer_list<C> components) {
        for (auto c : components) layout_.markDirty(c);
        needsRedraw_ = true;
    };

    switch (action) {
        // Navigation — only viewport + status bar
        case Action::RotateLeft:   cam.rotateY(-rs);  dirty({C::Viewport, C::StatusBar}); break;
        case Action::RotateRight:  cam.rotateY(rs);   dirty({C::Viewport, C::StatusBar}); break;
        case Action::RotateUp:     cam.rotateX(-rs);  dirty({C::Viewport, C::StatusBar}); break;
        case Action::RotateDown:   cam.rotateX(rs);   dirty({C::Viewport, C::StatusBar}); break;
        case Action::RotateCW:    cam.rotateZ(rs);    dirty({C::Viewport, C::StatusBar}); break;
        case Action::RotateCCW:   cam.rotateZ(-rs);   dirty({C::Viewport, C::StatusBar}); break;
        // Manual pan/zoom/reset overrides any active :focus/:zoom/:orient
        // fit, so drop the intent (else the next :screenshot would re-fit
        // and clobber the user's framing). Issue #98.
        case Action::PanLeft:     cam.pan(-cam.panSpeed(), 0); clearViewFit(); dirty({C::Viewport, C::StatusBar}); break;
        case Action::PanRight:    cam.pan(cam.panSpeed(), 0);  clearViewFit(); dirty({C::Viewport, C::StatusBar}); break;
        case Action::PanUp:       cam.pan(0, -cam.panSpeed()); clearViewFit(); dirty({C::Viewport, C::StatusBar}); break;
        case Action::PanDown:     cam.pan(0, cam.panSpeed());  clearViewFit(); dirty({C::Viewport, C::StatusBar}); break;
        case Action::ZoomIn:      cam.zoomBy(1.2f); clearViewFit(); dirty({C::Viewport, C::StatusBar}); break;
        case Action::ZoomOut:     cam.zoomBy(1.0f/1.2f); clearViewFit(); dirty({C::Viewport, C::StatusBar}); break;
        case Action::ResetView:   cam.reset(); tab.centerView(); clearViewFit(); dirty({C::Viewport, C::StatusBar}); break;
        case Action::CenterSelection: tab.centerView(); dirty({C::Viewport, C::StatusBar}); break;
        case Action::Redraw:
            clearScreenAndRepaint();
            layout_.markAllDirty(); needsRedraw_ = true;
            framesToSkip_ = 0;
            break;

        // Objects — viewport + panels + seqbar + status
        case Action::NextObject:
            tab.selectNextObject();
            onCurrentObjectChanged();
            dirty({C::Viewport, C::ObjectPanel, C::SeqBar, C::StatusBar});
            break;
        case Action::PrevObject:
            tab.selectPrevObject();
            onCurrentObjectChanged();
            dirty({C::Viewport, C::ObjectPanel, C::SeqBar, C::StatusBar});
            break;
        case Action::ToggleVisible: {
            auto obj = tab.currentObject();
            if (obj) obj->toggleVisible();
            dirty({C::Viewport, C::ObjectPanel, C::StatusBar});
            break;
        }
        case Action::DeleteObject: {
            int idx = tab.selectedObjectIdx();
            if (idx >= 0) {
                auto obj = tab.currentObject();
                if (obj) store_.remove(obj->name());
                tab.removeObject(idx);
            }
            dirty({C::Viewport, C::ObjectPanel, C::SeqBar, C::StatusBar});
            break;
        }

        // Representations — viewport only
        // Repr-toggle hotkeys honor :set scope, so a single keystroke shows
        // / hides the same repr across every loaded object after a superpose.
        case Action::ShowWireframe: { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->showRepr(ReprType::Wireframe); return true; }); dirty({C::Viewport}); break; }
        case Action::ShowBallStick: { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->showRepr(ReprType::BallStick); return true; }); dirty({C::Viewport}); break; }
        case Action::ShowSpacefill: { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->showRepr(ReprType::Spacefill); return true; }); dirty({C::Viewport}); break; }
        case Action::ShowCartoon:   { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->showRepr(ReprType::Cartoon);   return true; }); dirty({C::Viewport}); break; }
        case Action::ShowRibbon:    { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->showRepr(ReprType::Ribbon);    return true; }); dirty({C::Viewport}); break; }
        case Action::ShowBackbone:  { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->showRepr(ReprType::Backbone);  return true; }); dirty({C::Viewport}); break; }
        case Action::HideWireframe: { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->hideRepr(ReprType::Wireframe); return true; }); dirty({C::Viewport}); break; }
        case Action::HideBallStick: { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->hideRepr(ReprType::BallStick); return true; }); dirty({C::Viewport}); break; }
        case Action::HideSpacefill: { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->hideRepr(ReprType::Spacefill); return true; }); dirty({C::Viewport}); break; }
        case Action::HideCartoon:   { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->hideRepr(ReprType::Cartoon);   return true; }); dirty({C::Viewport}); break; }
        case Action::HideRibbon:    { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->hideRepr(ReprType::Ribbon);    return true; }); dirty({C::Viewport}); break; }
        case Action::HideBackbone:  { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->hideRepr(ReprType::Backbone);  return true; }); dirty({C::Viewport}); break; }
        case Action::HideAll:       { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->hideAllRepr();                 return true; }); dirty({C::Viewport}); break; }

        // Coloring — viewport + seqbar
        case Action::ColorByElement: { auto obj = tab.currentObject(); if (obj) applyHeteroatomColors(*obj); dirty({C::Viewport, C::SeqBar}); break; }
        case Action::ColorByChain:   { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->setColorScheme(ColorScheme::Chain);              return true; }); dirty({C::Viewport, C::SeqBar}); break; }
        case Action::ColorBySS:      { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->setColorScheme(ColorScheme::SecondaryStructure); return true; }); dirty({C::Viewport, C::SeqBar}); break; }
        case Action::ColorByBFactor: { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->setColorScheme(ColorScheme::BFactor);            return true; }); dirty({C::Viewport, C::SeqBar}); break; }

        // Tabs — swap view state on switch (applyViewState marks changed components dirty)
        case Action::NextTab:
            tabMgr_.nextTab();
            layout_.applyViewState(activeViewState());
            layout_.markAllDirty(); needsRedraw_ = true;
            break;
        case Action::PrevTab:
            tabMgr_.prevTab();
            layout_.applyViewState(activeViewState());
            layout_.markAllDirty(); needsRedraw_ = true;
            break;
        case Action::NewTab:
            tabMgr_.addTab();
            dirty({C::TabBar});
            break;
        case Action::CloseTab:
            tabMgr_.closeCurrentTab();
            layout_.applyViewState(activeViewState());
            layout_.markAllDirty(); needsRedraw_ = true;
            break;

        // Panel — update both Layout and current tab's view state
        case Action::TogglePanel:
            layout_.togglePanel();
            activeViewState().panelVisible = layout_.panelVisible();
            dirty({C::Viewport, C::ObjectPanel});
            break;

        // Mode transitions — command line only
        case Action::EnterCommand:
            inputHandler_->setMode(Mode::Command);
            cmdLine_.activate(':');
            dirty({C::CommandLine, C::StatusBar});
            break;
        case Action::EnterSearch:
            inputHandler_->setMode(Mode::Search);
            cmdLine_.activate('/');
            dirty({C::CommandLine, C::StatusBar});
            break;
        case Action::EnterVisual:
            inputHandler_->setMode(Mode::Visual);
            dirty({C::StatusBar});
            break;
        case Action::ExitToNormal:
            // First, if we're in focus mode, exiting back to "normal" view
            // means leaving focus — restore camera + visibility.
            if (focusSnapshot_.active) {
                exitFocus();
                dirty({C::CommandLine, C::StatusBar, C::Viewport});
                break;
            }
            inputHandler_->setMode(Mode::Normal);
            cmdLine_.deactivate();
            pickedAtomIdx_ = -1;
            if (pickMode_ != PickMode::Inspect) {
                pickMode_ = PickMode::Inspect;
                cmdLine_.setMessage("Inspect mode | sele=" +
                    std::to_string(namedSelections_[kSele].size()));
            }
            dirty({C::CommandLine, C::StatusBar});
            break;

        // Command mode actions — commands can affect any component
        case Action::ExecuteCommand: {
            std::string input = cmdLine_.input();
            cmdLine_.pushHistory(input);
            cmdLine_.deactivate();
            inputHandler_->setMode(Mode::Normal);
            // Route an interactively typed/pasted line through the same
            // dispatcher scripts use, so it gets identical handling: ';'-
            // separated commands, ${var} expansion, '#' comments, and
            // user-defined :def functions. (Pasting a README block like
            // `:setenv A ; :setenv B` previously ran as one mangled command.)
            std::vector<std::string> lineBuf = {input};
            ScriptRunResult sr;
            dispatchScriptLines(lineBuf, 0, 1, sr, /*strict=*/false);
            std::string msg = !sr.firstFail.empty() ? sr.firstFail : sr.lastMsg;
            if (!msg.empty()) cmdLine_.setMessage(msg);
            // Keep an input/output transcript for the :messages overlay.
            recordTranscript(input, msg);
            layout_.markAllDirty(); needsRedraw_ = true;
            break;
        }
        case Action::Autocomplete: {
            std::string input = cmdLine_.input();
            // Find first space — determines if completing command or argument
            auto spacePos = input.find(' ');
            if (spacePos == std::string::npos) {
                // Completing command name
                auto completions = cmdRegistry_.complete(input);
                if (completions.size() == 1) {
                    cmdLine_.clearInput();
                    for (char c : completions[0]) cmdLine_.insertChar(c);
                    cmdLine_.insertChar(' ');
                } else if (completions.size() > 1) {
                    std::string msg;
                    for (const auto& c : completions) msg += c + " ";
                    cmdLine_.setMessage(msg);
                }
            } else {
                // Completing argument — context-dependent
                std::string cmdName = input.substr(0, spacePos);
                std::string partial = input.substr(input.rfind(' ') + 1);

                std::vector<std::string> candidates;

                // Selection-aware: if the immediately-preceding token is the
                // selector keyword `obj`, suggest loaded object names —
                // covers `:color red, obj <Tab>`, `:show cartoon obj <Tab>`,
                // `:select foo = obj <Tab>`, etc., regardless of cmdName.
                auto prevTokenLower = [](const std::string& s) -> std::string {
                    int i = static_cast<int>(s.size()) - 1;
                    while (i >= 0 && (s[i] == ' ' || s[i] == '\t')) --i;
                    int end = i + 1;
                    while (i >= 0 && s[i] != ' ' && s[i] != '\t' && s[i] != ',') --i;
                    std::string tok = s.substr(i + 1, end - i - 1);
                    std::transform(tok.begin(), tok.end(), tok.begin(), ::tolower);
                    return tok;
                };
                std::string beforePartial =
                    (input.rfind(' ') != std::string::npos)
                        ? input.substr(0, input.rfind(' '))
                        : "";
                bool selectorObjContext =
                    prevTokenLower(beforePartial) == "obj";

                if (selectorObjContext) {
                    for (const auto& n : store_.names()) {
                        if (n.find(partial) == 0) candidates.push_back(n);
                    }
                } else if (cmdName == "load" || cmdName == "e" || cmdName == "export" ||
                           cmdName == "loadalign" || cmdName == "run" ||
                           cmdName == "screenshot") {
                    // Filename completion via std::filesystem
                    namespace fs = std::filesystem;
                    std::string dir = ".";
                    std::string prefix = partial;
                    auto lastSlash = partial.rfind('/');
                    if (lastSlash != std::string::npos) {
                        dir = partial.substr(0, lastSlash);
                        if (dir.empty()) dir = "/";
                        prefix = partial.substr(lastSlash + 1);
                    }
                    try {
                        for (const auto& entry : fs::directory_iterator(dir)) {
                            std::string name = entry.path().filename().string();
                            if (name.find(prefix) == 0) {
                                std::string full = (lastSlash != std::string::npos)
                                    ? dir + "/" + name : name;
                                if (entry.is_directory()) full += "/";
                                candidates.push_back(full);
                            }
                        }
                    } catch (...) {}
                } else if (cmdName == "delete" || cmdName == "rename" ||
                           cmdName == "object" ||
                           cmdName == "align" || cmdName == "mmalign" || cmdName == "super" ||
                           cmdName == "alignto" || cmdName == "mmalignto") {
                    // Object name completion
                    for (const auto& n : store_.names()) {
                        if (n.find(partial) == 0) candidates.push_back(n);
                    }
                } else if (cmdName == "show" || cmdName == "hide") {
                    // Repr name completion
                    for (const auto& r : {"wireframe", "wire", "ballstick", "sticks",
                                           "spacefill", "spheres", "cartoon", "ribbon",
                                           "backbone", "trace", "surface", "all"}) {
                        std::string rs(r);
                        if (rs.find(partial) == 0) candidates.push_back(rs);
                    }
                } else if (cmdName == "color") {
                    // Color name/scheme completion
                    for (const auto& c : {"element", "cpk", "chain", "ss", "secondary",
                                           "bfactor", "b", "plddt", "rainbow", "restype", "type",
                                           "heteroatom", "clear",
                                           "red", "green", "blue", "yellow", "magenta",
                                           "cyan", "white", "orange", "pink", "lime",
                                           "teal", "purple", "salmon", "slate", "gray"}) {
                        std::string cs(c);
                        if (cs.find(partial) == 0) candidates.push_back(cs);
                    }
                } else if (cmdName == "set" || cmdName == "get") {
                    // Single source of truth: kSetOptionsLong (iterated by
                    // `:set` listing too) + kSetOptionsShort (aliases-only).
                    auto offer = [&](const char* o) {
                        std::string os(o);
                        if (os.find(partial) == 0) candidates.push_back(std::move(os));
                    };
                    for (const char* o : kSetOptionsLong)  offer(o);
                    for (const char* o : kSetOptionsShort) offer(o);
                }

                if (candidates.size() == 1) {
                    // Replace partial with completed word
                    std::string base = input.substr(0, input.rfind(' ') + 1);
                    cmdLine_.clearInput();
                    for (char c : base) cmdLine_.insertChar(c);
                    for (char c : candidates[0]) cmdLine_.insertChar(c);
                    cmdLine_.insertChar(' ');
                } else if (candidates.size() > 1) {
                    std::string msg;
                    for (const auto& c : candidates) msg += c + " ";
                    cmdLine_.setMessage(msg);
                }
            }
            dirty({C::CommandLine});
            break;
        }
        case Action::HistoryPrev: cmdLine_.historyPrev(); dirty({C::CommandLine}); break;
        case Action::HistoryNext: cmdLine_.historyNext(); dirty({C::CommandLine}); break;
        case Action::DeleteWord:  cmdLine_.deleteWord();  dirty({C::CommandLine}); break;
        case Action::ClearLine:   cmdLine_.clearInput();  dirty({C::CommandLine}); break;

        // Search — viewport (highlights) + command line + status
        case Action::ExecuteSearch: {
            std::string query = cmdLine_.input();
            cmdLine_.deactivate();
            inputHandler_->setMode(Mode::Normal);
            executeSearch(query);
            dirty({C::Viewport, C::CommandLine, C::StatusBar});
            break;
        }

        // Search navigation
        case Action::SearchNext: {
            if (searchMatches_.empty()) {
                cmdLine_.setMessage("No search results");
                dirty({C::CommandLine}); break;
            }
            auto obj = tab.currentObject();
            if (!obj) break;
            searchIdx_ = (searchIdx_ + 1) % static_cast<int>(searchMatches_.size());
            const auto& a = obj->atoms()[searchMatches_[searchIdx_]];
            cmdLine_.setMessage("/" + lastSearch_ + " [" + std::to_string(searchIdx_ + 1) +
                               "/" + std::to_string(searchMatches_.size()) + "] " +
                               a.chainId + " " + a.resName + " " + std::to_string(a.resSeq) +
                               " " + a.name);
            dirty({C::Viewport, C::CommandLine});
            break;
        }
        case Action::SearchPrev: {
            if (searchMatches_.empty()) {
                cmdLine_.setMessage("No search results");
                dirty({C::CommandLine}); break;
            }
            auto obj = tab.currentObject();
            if (!obj) break;
            searchIdx_--;
            if (searchIdx_ < 0) searchIdx_ = static_cast<int>(searchMatches_.size()) - 1;
            const auto& a = obj->atoms()[searchMatches_[searchIdx_]];
            cmdLine_.setMessage("/" + lastSearch_ + " [" + std::to_string(searchIdx_ + 1) +
                               "/" + std::to_string(searchMatches_.size()) + "] " +
                               a.chainId + " " + a.resName + " " + std::to_string(a.resSeq) +
                               " " + a.name);
            dirty({C::Viewport, C::CommandLine});
            break;
        }

        // Undo/Redo — can affect anything
        case Action::Undo: {
            std::string msg = undoStack_.undo();
            cmdLine_.setMessage(msg.empty() ? "Nothing to undo" : msg);
            layout_.markAllDirty(); needsRedraw_ = true;
            break;
        }
        case Action::Redo: {
            std::string msg = undoStack_.redo();
            cmdLine_.setMessage(msg.empty() ? "Nothing to redo" : msg);
            layout_.markAllDirty(); needsRedraw_ = true;
            break;
        }

        // Screenshot — command line message only
        case Action::Screenshot: {
            ExecResult result = cmdRegistry_.execute(*this, "screenshot");
            if (!result.msg.empty()) cmdLine_.setMessage(result.msg);
            dirty({C::CommandLine});
            break;
        }

        // Contact map toggle — affects panels + viewport
        case Action::ToggleContactMap: {
            ExecResult result = cmdRegistry_.execute(*this, "contactmap");
            if (!result.msg.empty()) cmdLine_.setMessage(result.msg);
            layout_.markAllDirty(); needsRedraw_ = true;
            break;
        }

        // Interface overlay toggle — viewport + panels
        // Sends explicit on/off rather than implicit toggle so the
        // command itself stays declarative (matches :set on|off style).
        case Action::ToggleInterface: {
            const char* arg = interfaceOverlay_ ? "interface off" : "interface on";
            ExecResult result = cmdRegistry_.execute(*this, arg);
            if (!result.msg.empty()) cmdLine_.setMessage(result.msg);
            layout_.markAllDirty(); needsRedraw_ = true;
            break;
        }

        // Focus mode (Mol*-style): F enters/exits.
        //   • Already focused → exit.
        //   • Fresh pick → focus on that residue.
        //   • No pick + named selection $sele exists → focus on it.
        case Action::FocusPick: {
            if (focusSnapshot_.active) {
                exitFocus();
                break;
            }
            auto obj = tabMgr_.currentTab().currentObject();
            if (!obj) { cmdLine_.setMessage("Focus: no object"); break; }
            const auto& atoms = obj->atoms();

            std::vector<int> subjectIdx;
            std::string desc;

            int target = pickedAtomIdx_;
            if (target < 0) target = pickRegs_[(pickNext_ + 3) % 4];   // most recent
            if (target >= 0 && target < (int)atoms.size()) {
                // Expand by configured granularity (default Residue —
                // matches prior behavior).
                subjectIdx = expandByFocusGranularity(*obj, target);
                desc = residueInfoString(atoms[target]);
            } else {
                // Fall back to the active named selection.
                auto it = namedSelections_.find(kSele);
                if (it == namedSelections_.end() || it->second.empty()) {
                    cmdLine_.setMessage(
                        "Focus: click an atom or run :select first");
                    break;
                }
                subjectIdx = it->second.indices();
                desc = "$sele";
            }
            enterFocus(*obj, subjectIdx, desc);
            break;
        }

        // Renderer toggle — full redraw
        case Action::TogglePixelRenderer: {
            clearScreenAndRepaint();
            framesToSkip_ = 0;
            if (rendererType_ == RendererType::Pixel) {
                rendererType_ = RendererType::Braille;
                canvas_ = std::make_unique<BrailleCanvas>();
                cmdLine_.setMessage("Renderer: BRAILLE");
            } else {
                setRenderer(RendererType::Pixel);
                auto* pc = dynamic_cast<PixelCanvas*>(canvas_.get());
                const char* name = (pc && pc->encoder()) ? pc->encoder()->name() : "PIXEL";
                cmdLine_.setMessage(std::string("Renderer: ") + name);
            }
            layout_.markAllDirty(); needsRedraw_ = true;
            break;
        }

        // Macro recording — command line message
        case Action::StartMacro:
            if (macroRecording_) {
                stopMacroRecord();
            } else {
                macroAwaitingRegister_ = true;
                cmdLine_.setMessage("Record macro: press register (a-z)");
            }
            dirty({C::CommandLine, C::StatusBar});
            break;
        case Action::PlayMacro:
            macroPlayAwaitingRegister_ = true;
            cmdLine_.setMessage("Play macro: press register (a-z)");
            dirty({C::CommandLine, C::StatusBar});
            break;

        // More coloring — viewport + seqbar
        case Action::ColorByPLDDT:   { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->setColorScheme(ColorScheme::PLDDT);   return true; }); dirty({C::Viewport, C::SeqBar}); break; }
        case Action::ColorByRainbow: { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->setColorScheme(ColorScheme::Rainbow); return true; }); dirty({C::Viewport, C::SeqBar}); break; }
        case Action::ColorByResType: { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->setColorScheme(ColorScheme::ResType); return true; }); dirty({C::Viewport, C::SeqBar}); break; }
        case Action::ColorBySASA:    { forEachInScope(*this, "", [](ScopedTarget& t){ t.obj->setColorScheme(ColorScheme::SASA);    return true; }); dirty({C::Viewport, C::SeqBar}); break; }

        // Multi-state cycling — viewport + seqbar + status
        case Action::NextState: {
            auto obj = tab.currentObject();
            if (obj && obj->stateCount() > 1) {
                obj->nextState();
                cmdLine_.setMessage("State " + std::to_string(obj->activeState() + 1) +
                                   "/" + std::to_string(obj->stateCount()));
            } else {
                cmdLine_.setMessage("Single-state structure");
            }
            dirty({C::Viewport, C::SeqBar, C::CommandLine, C::StatusBar});
            break;
        }
        case Action::PrevState: {
            auto obj = tab.currentObject();
            if (obj && obj->stateCount() > 1) {
                obj->prevState();
                cmdLine_.setMessage("State " + std::to_string(obj->activeState() + 1) +
                                   "/" + std::to_string(obj->stateCount()));
            } else {
                cmdLine_.setMessage("Single-state structure");
            }
            dirty({C::Viewport, C::SeqBar, C::CommandLine, C::StatusBar});
            break;
        }

        // Inspect — command line message
        case Action::Inspect:
            cmdLine_.setMessage(std::string("Click to inspect | gs/gS/gc to select | Level: ") +
                inspectLevelName(inspectLevel_));
            dirty({C::CommandLine});
            break;

        // Cycle inspect level
        case Action::CycleInspectLevel: {
            inspectLevel_ = static_cast<InspectLevel>(
                (static_cast<int>(inspectLevel_) + 1) % 4);
            cmdLine_.setMessage(std::string("Inspect level: ") + inspectLevelName(inspectLevel_));
            dirty({C::CommandLine});
            break;
        }

        // Pick mode
        case Action::EnterSelectAtom:
        case Action::EnterSelectResidue:
        case Action::EnterSelectChain: {
            PickMode target = (action == Action::EnterSelectAtom) ? PickMode::SelectAtom :
                              (action == Action::EnterSelectResidue) ? PickMode::SelectResidue :
                              PickMode::SelectChain;
            pickMode_ = (pickMode_ == target) ? PickMode::Inspect : target;
            if (pickMode_ != PickMode::Inspect)
                cmdLine_.setMessage(std::string(pickModeName(pickMode_)) +
                    " mode (click to add/remove, ESC to exit) sele=" +
                    std::to_string(namedSelections_[kSele].size()));
            else
                cmdLine_.setMessage("Inspect mode");
            dirty({C::CommandLine, C::StatusBar});
            break;
        }

        case Action::ClearSelection: {
            auto [hadSele, hadPk] = clearVisualSelection();
            if (hadSele || hadPk) {
                cmdLine_.setMessage("Cleared $sele (" + std::to_string(hadSele) +
                                    " atoms) and pk1-pk4");
            } else {
                cmdLine_.setMessage("Selection already empty");
            }
            dirty({C::Viewport, C::StatusBar, C::CommandLine});
            break;
        }

        case Action::EnterFocusPickMode: {
            // Toggle: a second `gf` while already in Focus mode returns to
            // Inspect (matches the gs/gS/gc convention).
            pickMode_ = (pickMode_ == PickMode::Focus) ? PickMode::Inspect
                                                       : PickMode::Focus;
            const char* gran =
                focusGranularity_ == FocusGranularity::Chain    ? "chain" :
                focusGranularity_ == FocusGranularity::Sidechain ? "sidechain" :
                                                                  "residue";
            if (pickMode_ == PickMode::Focus) {
                cmdLine_.setMessage(std::string("FOCUS pick mode — click an atom (granularity=") +
                                    gran + ", ESC to exit)");
            } else {
                cmdLine_.setMessage("Inspect mode");
            }
            dirty({C::CommandLine, C::StatusBar});
            break;
        }

        case Action::ToggleSeqBar: {
            // Force wrap=true so visible always means "all sequences shown."
            // The legacy single-line scroll mode is still reachable via
            // `:set seqwrap off` but no key cycles through it — three states
            // confused users with the camera-focus key.
            layout_.setSeqBarWrap(true);
            layout_.toggleSeqBar();
            cmdLine_.setMessage(layout_.seqBarVisible()
                                    ? "Sequence bar: visible"
                                    : "Sequence bar: hidden");
            activeViewState().seqBarVisible = layout_.seqBarVisible();
            activeViewState().seqBarWrap = layout_.seqBarWrap();
            if (canvas_) canvas_->invalidate();
            onResize();
            break;
        }

        case Action::SeqBarNextChain:
            if (layout_.seqBarVisible() && !layout_.seqBarWrap()) {
                activeSeqBar().nextChain();
                activeSeqBar().scrollToChain(activeSeqBar().activeChain());
                cmdLine_.setMessage("Chain: " + activeSeqBar().activeChain());
            }
            dirty({C::SeqBar, C::CommandLine});
            break;
        case Action::SeqBarPrevChain:
            if (layout_.seqBarVisible() && !layout_.seqBarWrap()) {
                activeSeqBar().prevChain();
                activeSeqBar().scrollToChain(activeSeqBar().activeChain());
                cmdLine_.setMessage("Chain: " + activeSeqBar().activeChain());
            }
            dirty({C::SeqBar, C::CommandLine});
            break;

        case Action::ShowOverlay:
            overlayVisible_ = true;
            cmdLine_.setMessage("Overlays visible");
            dirty({C::Viewport, C::CommandLine});
            break;
        case Action::HideOverlay:
            overlayVisible_ = false;
            cmdLine_.setMessage("Overlays hidden");
            dirty({C::Viewport, C::CommandLine});
            break;

        case Action::ApplyPreset: {
            auto obj = tab.currentObject();
            if (obj) {
                obj->applySmartDefaults();
                cmdLine_.setMessage("Applied default preset");
            }
            dirty({C::Viewport, C::CommandLine});
            break;
        }

        case Action::ShowHelp:
            showKeybindingHelp();
            dirty({C::Viewport});
            break;

        default:
            needsRedraw_ = false;
            break;
    }
}

void Application::handleLineEdit(int key) {
    if (key == KEY_BACKSPACE || key == 127 || key == 8) {
        cmdLine_.backspace();
    } else if (key == KEY_DC || key == 330) {
        cmdLine_.deleteForward();
    } else if (key == KEY_LEFT) {
        cmdLine_.cursorLeft();
    } else if (key == KEY_RIGHT) {
        cmdLine_.cursorRight();
    } else if (key == KEY_HOME || key == 1) {
        cmdLine_.cursorHome();
    } else if (key == KEY_END || key == 5) {
        cmdLine_.cursorEnd();
    } else {
        cmdLine_.insertChar(key);
    }
    cmdLine_.render(layout_.commandLine());
    layout_.commandLine().refresh();
    doupdate();
}

void Application::handleCommandInput(int key) {
    handleLineEdit(key);
}

void Application::handleSearchInput(int key) {
    handleLineEdit(key);
}

void Application::renderFrame() {
    ++frameCounter_;

    // Dirty flags are now set selectively by handleAction/handleMouse/onResize.
    // No blanket markAllDirty() here.

    // Tab bar
    if (layout_.isDirty(Layout::Component::TabBar))
        tabBar_.render(layout_.tabBar(), tabMgr_.tabNames(), tabMgr_.currentIndex());

    // Adjust seqbar height BEFORE rendering viewport (setSeqBarHeight rebuilds windows)
    if (layout_.seqBarVisible()) {
        auto obj = tabMgr_.currentTab().currentObject();
        if (obj) {
            activeSeqBar().update(*obj);
            if (layout_.seqBarWrap()) {
                int needed = std::min(activeSeqBar().wrapRows(layout_.seqBar().width()),
                                      screen_.height() / 4);
                if (needed != layout_.seqBar().height()) {
                    layout_.setSeqBarHeight(std::max(1, needed));
                    activeViewState().seqBarHeight = layout_.seqBar().height();
                    if (canvas_) canvas_->invalidate();
                }
            } else {
                if (layout_.seqBar().height() != 1) {
                    layout_.setSeqBarHeight(1);
                    activeViewState().seqBarHeight = 1;
                    if (canvas_) canvas_->invalidate();
                }
            }
        }
    }

    // Viewport — render to canvas but defer pixel flush
    bool viewportRendered = layout_.isDirty(Layout::Component::Viewport);
    if (viewportRendered)
        renderViewport();

    // Clear camera dirty flag after rendering (so caches like Spacefill sort work)
    tabMgr_.currentTab().camera().clearDirty();

    // Object panel
    if (layout_.panelVisible() && layout_.isDirty(Layout::Component::ObjectPanel)) {
        auto& tab = tabMgr_.currentTab();
        objectPanel_.render(layout_.objectPanel(), tab.objects(),
                           tab.selectedObjectIdx());
    }

    // Analysis panel (contact map etc.)
    if (layout_.analysisPanelVisible() && layout_.isDirty(Layout::Component::AnalysisPanel)) {
        renderAnalysisPanel();
    }

    // Sequence bar
    if (layout_.seqBarVisible() && layout_.isDirty(Layout::Component::SeqBar)) {
        auto obj = tabMgr_.currentTab().currentObject();
        if (obj) {
            const Selection* sele = nullptr;
            auto selIt = namedSelections_.find(kSele);
            if (selIt != namedSelections_.end()) sele = &selIt->second;
            activeSeqBar().render(layout_.seqBar(), activeViewState().focusResi, activeViewState().focusChain, sele,
                          obj->colorScheme(), layout_.seqBarWrap());
        }
    }

    // Status bar
    if (layout_.isDirty(Layout::Component::StatusBar))
        updateStatusBar();

    // Command line
    if (layout_.isDirty(Layout::Component::CommandLine))
        cmdLine_.render(layout_.commandLine());

    layout_.refreshAll();
    doupdate();

    // Pixel graphics must be written AFTER doupdate() so ncurses doesn't overwrite.
    if (rendererType_ == RendererType::Pixel && viewportRendered) {
        canvas_->flush(layout_.viewport());
    }
}

void Application::renderViewport() {
    auto& win = layout_.viewport();
    win.erase();

    int w = win.width(), h = win.height();
    canvas_->resize(w, h);
    if (auto* pc = dynamic_cast<PixelCanvas*>(canvas_.get())) applyBgMode(*pc);
    canvas_->clear();

    auto& tab = tabMgr_.currentTab();

    // Wireframe paints heteroatoms by element while keeping carbons on
    // the scheme color whenever the user is looking at an interface
    // (overlay or focus) — N/O/S/P need to pop as donors/acceptors there.
    if (auto* wf = dynamic_cast<WireframeRepr*>(getRepr(ReprType::Wireframe))) {
        wf->setHeteroatomCarbonScheme(interfaceOverlay_ || focusSnapshot_.active);
    }

    // Render reprs once per stereoscopic eye. setupStereoEye() handles the
    // single-pass, non-stereo case too (returns immediately after preparing
    // a normal full-canvas projection).
    auto* pcLive = dynamic_cast<PixelCanvas*>(canvas_.get());
    std::array<float, 9> savedRot{};
    for (int eye = 0; eye < stereoEyeCount(); ++eye) {
        savedRot = setupStereoEye(eye, canvas_->subW(), canvas_->subH(),
                                  canvas_->aspectYX());
        for (const auto& obj : tab.objects()) {
            if (!obj->visible()) continue;
            // Push the per-object alpha LUT so PixelCanvas can blend
            // transparent atoms in the upcoming repr->render() calls.
            if (pcLive) {
                pcLive->setAlphaLUT(obj->atomAlpha().empty() ? nullptr
                                                             : &obj->atomAlpha());
            }
            for (auto& [reprType, repr] : representations_) {
                if (obj->reprVisible(reprType)) {
                    repr->render(*obj, tab.camera(), *canvas_);
                }
            }
        }
        if (pcLive) pcLive->setAlphaLUT(nullptr);
    }
    restoreStereoCamera(savedRot);

    // ── ZoomGate auto-engage / disengage ────────────────────────────────
    // When the user has set :set interface_zoom <T>, crossing the
    // threshold simulates a :interface toggle. Manual `:interface on`
    // is preserved (gate falling does not disable a manually-engaged
    // overlay; only auto-engaged ones are auto-disengaged).
    if (interfaceZoomGate_.enabled()) {
        const bool wasActive = interfaceZoomGate_.active();
        const bool flipped   = interfaceZoomGate_.update(tab.camera().zoom());
        const bool isActive  = interfaceZoomGate_.active();
        if (flipped) {
            if (isActive && !interfaceOverlay_) {
                interfaceFromZoom_ = true;
                cmdRegistry_.execute(*this, "interface on");
            } else if (!isActive && interfaceFromZoom_ && interfaceOverlay_) {
                interfaceFromZoom_ = false;
                cmdRegistry_.execute(*this, "interface off");
            }
        }
        (void)wasActive;
    }

    // Apply post-processing on pixel canvas. InterfaceRepr is drawn
    // AFTER outline + fog + focus-dim so the colored overlay (sidechain
    // bonds, dashed interaction lines) stays vivid — fog would otherwise
    // wash it out, defeating the "highlight" intent.
    if (rendererType_ == RendererType::Pixel) {
        auto* pc = dynamic_cast<PixelCanvas*>(canvas_.get());
        if (pc) {
            if (outlineEnabled_) {
                uint8_t r = 0, g = 0, b = 0;
                if (outlineColor_) { r = (*outlineColor_)[0]; g = (*outlineColor_)[1]; b = (*outlineColor_)[2]; }
                pc->applyOutline(outlineThreshold_, outlineDarken_, outlineMode_, r, g, b);
            }
            if (fogStrength_ > 0.0f) pc->applyDepthFog(fogStrength_);

            // Focus-dim: prefer the interface mask when active, fall back
            // to the explicit :focus selection mask. Either is depth-
            // independent, so a non-focus atom in the foreground still
            // dims while the focus subject stays vivid.
            const std::vector<bool>* dimMask = nullptr;
            if (interfaceOverlay_ && !interfaceAtomMask_.empty()) {
                dimMask = &interfaceAtomMask_;
            } else if (!focusAtomMask_.empty()) {
                dimMask = &focusAtomMask_;
            }
            if (dimMask) pc->applyFocusDim(*dimMask, focusDimStrength_);
        }
    }

    // ── Interface overlay (sidechains + interaction lines) ─────────────
    // Drawn last so it sits on top of fog + focus-dim — vivid colors,
    // unattenuated. Fires under either:
    //   • global :interface overlay (full structure)
    //   • focus mode (filtered to the focus neighborhood)
    // Per-eye in stereo mode so each half gets its own vivid overlay.
    if ((interfaceOverlay_ || focusSnapshot_.active) &&
        interfaceRepr_.hasData()) {
        if (auto obj = tab.currentObject(); obj && obj->visible()) {
            std::array<float, 9> savedIfaceRot{};
            for (int eye = 0; eye < stereoEyeCount(); ++eye) {
                savedIfaceRot = setupStereoEye(eye, canvas_->subW(),
                                                canvas_->subH(),
                                                canvas_->aspectYX());
                interfaceRepr_.render(*obj, tab.camera(), *canvas_);
            }
            restoreStereoCamera(savedIfaceRot);
        }
    }

    // Pixel flush is deferred to after doupdate() in renderFrame().
    // Other renderers flush here (they go through ncurses).
    if (rendererType_ != RendererType::Pixel) {
        canvas_->flush(win);
    }

    // Centered modal overlay used by `?`, `:help`, `:help <cmd>`, and
    // `:interface legend`. Word-wraps to fit the terminal; scrolls when
    // content overflows. Key handling lives in processInput().
    if (infoOverlay_.active) {
        // Box width: clamp(min=20, preferred=60, max=terminal-4). Using
        // std::clamp(60, 20, w-4) is fine when w-4 >= 20; on tiny terminals
        // we fall back to whatever fits.
        int hi = std::max(20, w - 4);
        int ow = std::clamp(60, 20, hi);
        if (hi > ow) {
            int longest = static_cast<int>(infoOverlay_.title.size()) + 4;
            for (const auto& l : infoOverlay_.lines)
                longest = std::max(longest, static_cast<int>(l.size()) + 4);
            ow = std::min(std::max(ow, longest), hi);
        }
        int contentWidth = std::max(10, ow - 4);  // inside the box, minus padding

        // UTF-8 display width: count lead bytes (anything that isn't a
        // continuation byte 10xxxxxx). Treats every codepoint as one
        // column, which is right for the Latin/symbol/box-drawing chars
        // used by the help and legend overlays (Å, ↔, ≤, ─). Wide CJK
        // would over-estimate, but the overlay doesn't use any.
        auto cols = [](std::string_view s) {
            int n = 0;
            for (unsigned char c : s)
                if ((c & 0xC0) != 0x80) ++n;
            return n;
        };

        // Word-wrap a single line to `contentWidth` display columns.
        // Continuation lines indent to the original lead + 2 so wrapped
        // table rows stay visually aligned.
        auto wrapLine = [contentWidth, &cols](const std::string& line)
                          -> std::vector<std::string> {
            if (cols(line) <= contentWidth) return {line};
            size_t leadEnd = line.find_first_not_of(' ');
            std::string lead = (leadEnd == std::string::npos) ? std::string()
                                                              : line.substr(0, leadEnd);
            std::string indent = lead + "  ";
            std::vector<std::string> out;
            std::string cur = lead;
            std::string body = (leadEnd == std::string::npos) ? std::string()
                                                              : line.substr(leadEnd);
            std::istringstream iss(body);
            std::string word;
            bool first = true;
            while (iss >> word) {
                int sep = first ? 0 : 1;
                if (cols(cur) + sep + cols(word) > contentWidth) {
                    if (!cur.empty() && cur != lead) {
                        out.push_back(cur);
                        cur = indent;
                    }
                    // Hard-break a word that's still too long for one line.
                    while (cols(cur) + cols(word) > contentWidth) {
                        int avail = contentWidth - cols(cur);
                        if (avail <= 0) {
                            out.push_back(cur);
                            cur = indent;
                            avail = contentWidth - cols(cur);
                        }
                        // Walk codepoints to pick a byte-safe split point.
                        size_t cut = 0;
                        int taken = 0;
                        while (cut < word.size() && taken < avail) {
                            unsigned char c = static_cast<unsigned char>(word[cut]);
                            do { ++cut; } while (cut < word.size()
                                && (static_cast<unsigned char>(word[cut]) & 0xC0) == 0x80);
                            (void)c;
                            ++taken;
                        }
                        if (cut == 0) break;  // safety: avoid infinite loop
                        cur += word.substr(0, cut);
                        out.push_back(cur);
                        word.erase(0, cut);
                        cur = indent;
                    }
                    cur += word;
                } else {
                    if (!first) cur += ' ';
                    cur += word;
                }
                first = false;
            }
            if (!cur.empty()) out.push_back(cur);
            if (out.empty()) out.push_back(line);
            return out;
        };

        // Flatten input lines to wrapped lines, propagating per-line colors
        // (each wrapped fragment inherits the original line's color).
        std::vector<std::string> wrapped;
        std::vector<int> wrappedColors;
        wrapped.reserve(infoOverlay_.lines.size());
        wrappedColors.reserve(infoOverlay_.lines.size());
        for (size_t i = 0; i < infoOverlay_.lines.size(); ++i) {
            int origColor = (i < infoOverlay_.lineColors.size())
                            ? infoOverlay_.lineColors[i] : -1;
            for (auto& w : wrapLine(infoOverlay_.lines[i])) {
                wrapped.push_back(std::move(w));
                wrappedColors.push_back(origColor);
            }
        }
        int contentLines = static_cast<int>(wrapped.size());

        int oh = std::min(contentLines + 4, h - 2);
        int ox = (w - ow) / 2;
        int oy = (h - oh) / 2;

        int visibleRows = std::max(0, oh - 4);
        infoOverlay_.lastVisibleRows = visibleRows;
        infoOverlay_.lastTotalLines = contentLines;
        int maxScroll = std::max(0, contentLines - visibleRows);
        infoOverlay_.scrollOffset = std::clamp(infoOverlay_.scrollOffset, 0, maxScroll);

        // Background box
        for (int y = oy; y < oy + oh && y < h; ++y) {
            for (int x = ox; x < ox + ow && x < w; ++x)
                win.addCharColored(y, x, ' ', kColorStatusBar);
        }

        // Title (centered) — append scroll position when content overflows
        std::string title = "  " + infoOverlay_.title;
        if (maxScroll > 0) {
            int firstVisible = infoOverlay_.scrollOffset + 1;
            int lastVisible = std::min(infoOverlay_.scrollOffset + visibleRows, contentLines);
            title += "  (" + std::to_string(firstVisible) + "-" +
                     std::to_string(lastVisible) + "/" +
                     std::to_string(contentLines) + ")";
        }
        title += "  ";
        int titleX = ox + std::max(0, (ow - static_cast<int>(title.size())) / 2);
        win.printColored(oy, titleX, title, kColorTabActive);

        int row = oy + 2;
        int firstIdx = infoOverlay_.scrollOffset;
        int lastIdx = std::min(contentLines, firstIdx + visibleRows);
        for (int i = firstIdx; i < lastIdx; ++i) {
            if (row >= oy + oh - 1) break;
            int color = (wrappedColors[i] >= 0) ? wrappedColors[i] : kColorStatusBar;
            win.printColored(row++, ox + 2, wrapped[i], color);
        }

        const std::string footer = (maxScroll > 0)
            ? "  j/k scroll  q close  "
            : "  Press any key to close  ";
        int footerX = ox + std::max(0, (ow - static_cast<int>(footer.size())) / 2);
        win.printColored(std::min(row, oy + oh - 1), footerX, footer, kColorTabActive);
    }

    // Show history hint overlay when command line is active and empty
    cmdLine_.renderHistoryHint(win);

  if (overlayVisible_) {
    bool isPixel = (rendererType_ == RendererType::Pixel);
    // Auto-scale overlay sizes for the live canvas (issue #48). Pixels
    // mode = no-op; relative/physical adjust effective*() for this pass.
    int liveCanvasH = canvas_ ? canvas_->subH() : 0;
    Application::RenderScaleScope renderScale(*this, liveCanvasH, /*dpi*/ 0);

    // Stereo + pixel: drawPixelOverlay encapsulates labels, measurements,
    // and sele/pk rings against whatever projection the camera currently
    // has. Run it once per eye so each half gets its own overlay layer
    // aligned with the eye's slightly-rotated geometry.
    if (stereoMode_ != StereoMode::Off && isPixel) {
        if (auto* pc = dynamic_cast<PixelCanvas*>(canvas_.get())) {
            std::array<float, 9> savedOverlayRot{};
            for (int eye = 0; eye < stereoEyeCount(); ++eye) {
                savedOverlayRot = setupStereoEye(eye, canvas_->subW(),
                                                  canvas_->subH(),
                                                  canvas_->aspectYX());
                drawPixelOverlay(*pc);
            }
            restoreStereoCamera(savedOverlayRot);
        }
        win.refresh();
        return;
    }

    // Draw labels on viewport. Labels are keyed per object, so each visible
    // object's labels project against its own atoms — a label set on one
    // member of a multi-object overlay shows on that member regardless of
    // which object is current (issue #101). buildProjCache() primes the
    // camera projection for the frame; the atoms are projected directly.
    {
        buildProjCache();
        int scaleX = canvas_ ? canvas_->scaleX() : 1;
        int scaleY = canvas_ ? canvas_->scaleY() : 1;
        auto& cam = tabMgr_.currentTab().camera();
        auto* pc = isPixel ? dynamic_cast<PixelCanvas*>(canvas_.get()) : nullptr;
        for (const auto& [objName, labels] : labelsByObject_) {
            auto o = findObjectByName(*this, objName);
            if (!o || !o->visible()) continue;
            const auto& atoms = o->atoms();
            for (int idx : labels.atoms) {
                if (idx < 0 || idx >= static_cast<int>(atoms.size())) continue;
                float fsx, fsy, depth;
                cam.projectCached(atoms[idx].x, atoms[idx].y, atoms[idx].z,
                                  fsx, fsy, depth);
                int isx = static_cast<int>(fsx), isy = static_cast<int>(fsy);
                int tx = isx / scaleX, ty = isy / scaleY;
                if (tx < 0 || tx >= w - 6 || ty < 0 || ty >= h) continue;
                std::string lbl = resolveLabel(*o, labels, idx);
                if (pc) {
                    paintLabelText(*pc, isx + scaleX, isy, depth, lbl);
                } else {
                    int lx = std::min(tx + 1, w - static_cast<int>(lbl.size()));
                    win.printColored(ty, lx, lbl, kColorWhite);
                }
            }
        }
    }

    // Helper: draw a dashed line between two 3D atom positions.
    // In pixel mode, draws directly into the canvas (sub-pixel space).
    // In non-pixel mode, draws into the ncurses window (terminal space).
    int subW = canvas_ ? canvas_->subW() : w;
    int subH = canvas_ ? canvas_->subH() : h;
    int scaleX = canvas_ ? canvas_->scaleX() : 1;
    int scaleY = canvas_ ? canvas_->scaleY() : 1;

    // thickness: pixel-mode line thickness in sub-pixels (driven by
    // :set annotation_linewidth × overlay_scale); ignored in non-pixel.
    int annoLineThick = effectiveAnnotationLineWidth();
    // measurementLineColor_ overrides the legacy palette color in pixel
    // mode (issue #30); ncurses fallback always uses the palette `color`.
    PixelCanvas* pixelCanvas = isPixel ? dynamic_cast<PixelCanvas*>(canvas_.get()) : nullptr;
    const std::optional<ColorRGB>& mlc = measurementLineColor_;
    auto drawDashedLine = [&, annoLineThick](float sx1, float sy1, float d1,
                              float sx2, float sy2, float d2,
                              int color) {
        const int thickness = annoLineThick;
        if (isPixel) {
            int isx1 = static_cast<int>(sx1), isy1 = static_cast<int>(sy1);
            int isx2 = static_cast<int>(sx2), isy2 = static_cast<int>(sy2);
            int steps = std::max(std::abs(isx2 - isx1), std::abs(isy2 - isy1));
            if (steps == 0) steps = 1;
            int dashLen = std::max(4, thickness * 2);
            for (int s = 0; s <= steps; s += dashLen * 2) {
                // Draw dash segment
                for (int ds = 0; ds < dashLen && s + ds <= steps; ++ds) {
                    float t = static_cast<float>(s + ds) / static_cast<float>(steps);
                    int x = isx1 + static_cast<int>((isx2 - isx1) * t);
                    int y = isy1 + static_cast<int>((isy2 - isy1) * t);
                    float depth = d1 + (d2 - d1) * t;
                    // Draw thickness x thickness dot cluster
                    for (int dy = 0; dy < thickness; ++dy) {
                        for (int dx = 0; dx < thickness; ++dx) {
                            int px = x + dx - thickness / 2;
                            int py = y + dy - thickness / 2;
                            if (px < 0 || px >= subW || py < 0 || py >= subH) continue;
                            if (mlc && pixelCanvas)
                                pixelCanvas->drawDotRGB(px, py, depth,
                                                        (*mlc)[0], (*mlc)[1], (*mlc)[2]);
                            else
                                canvas_->drawDot(px, py, depth, color);
                        }
                    }
                }
            }
        } else {
            int tx1 = static_cast<int>(sx1) / scaleX, ty1 = static_cast<int>(sy1) / scaleY;
            int tx2 = static_cast<int>(sx2) / scaleX, ty2 = static_cast<int>(sy2) / scaleY;
            int steps = std::max(std::abs(tx2 - tx1), std::abs(ty2 - ty1));
            if (steps == 0) steps = 1;
            for (int s = 0; s <= steps; s += 2) {
                float t = static_cast<float>(s) / static_cast<float>(steps);
                int x = tx1 + static_cast<int>((tx2 - tx1) * t);
                int y = ty1 + static_cast<int>((ty2 - ty1) * t);
                if (x >= 0 && x < w && y >= 0 && y < h)
                    win.addCharColored(y, x, '-', color);
            }
        }
    };

    // Draw measurement dashed lines + labels. Each measurement carries its
    // owning object name; project against that object's atoms so a distance
    // pinned on a non-current overlay member still renders (issue #101). A
    // measurement with no object (legacy) falls back to the current object.
    if (!measurements_.empty()) {
        auto& cam = tabMgr_.currentTab().camera();
        auto cur = tabMgr_.currentTab().currentObject();
        for (const auto& m : measurements_) {
            if (m.atoms.size() < 2) continue;
            auto obj = m.obj.empty() ? cur : findObjectByName(*this, m.obj);
            if (!obj || !obj->visible()) continue;
            const auto& atoms = obj->atoms();
            const int na = static_cast<int>(atoms.size());
            if (!std::all_of(m.atoms.begin(), m.atoms.end(),
                             [na](int a) { return a >= 0 && a < na; })) continue;
            for (size_t mi = 0; mi + 1 < m.atoms.size(); ++mi) {
                int a1 = m.atoms[mi], a2 = m.atoms[mi + 1];
                float sx1, sy1, d1, sx2, sy2, d2;
                cam.projectCached(atoms[a1].x, atoms[a1].y, atoms[a1].z, sx1, sy1, d1);
                cam.projectCached(atoms[a2].x, atoms[a2].y, atoms[a2].z, sx2, sy2, d2);
                drawDashedLine(sx1, sy1, d1, sx2, sy2, d2, kColorYellow);
            }
            // Label at midpoint of first segment
            int a1 = m.atoms[0], a2 = m.atoms[1];
            float sx1, sy1, d1, sx2, sy2, d2;
            cam.projectCached(atoms[a1].x, atoms[a1].y, atoms[a1].z, sx1, sy1, d1);
            cam.projectCached(atoms[a2].x, atoms[a2].y, atoms[a2].z, sx2, sy2, d2);
            std::string text = m.displayLabel();
            if (isPixel) {
                auto* pc = dynamic_cast<PixelCanvas*>(canvas_.get());
                if (pc) {
                    int lx = (static_cast<int>(sx1) + static_cast<int>(sx2)) / 2;
                    int ly = (static_cast<int>(sy1) + static_cast<int>(sy2)) / 2;
                    float depth = (d1 + d2) / 2.0f;
                    paintAnnotationText(*pc, lx, ly, depth, text);
                }
            } else {
                int mx = (static_cast<int>(sx1) / scaleX + static_cast<int>(sx2) / scaleX) / 2;
                int my = (static_cast<int>(sy1) / scaleY + static_cast<int>(sy2) / scaleY) / 2;
                if (mx >= 0 && mx < w - static_cast<int>(text.size()) && my >= 0 && my < h)
                    win.printColored(my, mx, text, kColorYellow);
            }
        }
    }

    // Highlight overlay: $sele atoms plus pk1-pk4 pick registers.
    // Pixel: yellow ring — a dot vanishes at typical zoom and a filled
    // disc would occlude the atom's own color. Cell renderers: '*'
    // glyph, the chunkiest single-cell mark.
    {
        const std::vector<int>* selIdx = nullptr;
        auto selIt = namedSelections_.find(kSele);
        if (selIt != namedSelections_.end() && !selIt->second.empty())
            selIdx = &selIt->second.indices();

        int pks[4]; int nPk = 0;
        for (int p = 0; p < 4; ++p)
            if (pickRegs_[p] >= 0) pks[nPk++] = pickRegs_[p];
        std::sort(pks, pks + nPk);
        nPk = static_cast<int>(std::unique(pks, pks + nPk) - pks);

        if (selIdx || nPk > 0) {
            buildProjCache();
            const int ringR = isPixel
                ? std::max(3, static_cast<int>(std::lround(
                      4.0f * cameraZoomScale(tabMgr_.currentTab().camera().zoom())
                      * overlayScale_ * renderScaleHint_)))
                : 0;
            // Walk projCache_ as the master cursor and advance two
            // sub-cursors (sele, pk) past anything below the current
            // atom; an atom is highlit if either head matches. Keeps
            // the work to O(n + m + k) without copying or allocating.
            const size_t sn = selIdx ? selIdx->size() : 0;
            size_t si = 0;
            int    qi = 0;
            for (size_t pi = 0; pi < projCache_.size(); ++pi) {
                if (si >= sn && qi >= nPk) break;
                const auto& pa = projCache_[pi];
                while (si < sn && (*selIdx)[si] < pa.idx) ++si;
                while (qi < nPk && pks[qi] < pa.idx)      ++qi;
                bool match = (si < sn && (*selIdx)[si] == pa.idx) ||
                             (qi < nPk && pks[qi] == pa.idx);
                if (!match) continue;
                if (isPixel) {
                    if (pa.sx >= -ringR && pa.sx < subW + ringR &&
                        pa.sy >= -ringR && pa.sy < subH + ringR)
                        canvas_->drawCircle(pa.sx, pa.sy, pa.depth - 0.01f,
                                            ringR, kColorYellow, /*filled=*/false);
                } else {
                    int tx = pa.sx / scaleX;
                    int ty = pa.sy / scaleY;
                    if (tx >= 0 && tx < w && ty >= 0 && ty < h)
                        win.addCharColored(ty, tx, '*', kColorYellow);
                }
            }
        }
    }

    // Interface overlay (sidechain bonds + dashed interaction lines) is
    // rendered by InterfaceRepr earlier in the frame so depth-fog and
    // focus-dim post-passes see it. The legacy inline drawer was removed
    // when InterfaceRepr was introduced.
  } // overlayVisible_

    win.refresh();
}

void Application::renderAnalysisPanel() {
    auto obj = tabMgr_.currentTab().currentObject();
    if (obj) {
        contactMapPanel_.update(*obj);
    }
    contactMapPanel_.render(layout_.analysisPanel());
}

void Application::updateStatusBar() {
    auto& tab = tabMgr_.currentTab();
    std::string objInfo;
    std::string rightInfo;

    auto obj = tab.currentObject();
    if (obj) {
        objInfo = obj->name() + " [" +
                  std::to_string(obj->atoms().size()) + " atoms]";
        if (obj->stateCount() > 1)
            objInfo += " S:" + std::to_string(obj->activeState() + 1) +
                       "/" + std::to_string(obj->stateCount());
        if (!obj->visible()) objInfo += " (hidden)";
    }

    if (pickMode_ != PickMode::Inspect) {
        auto selIt = namedSelections_.find(kSele);
        int selCount = (selIt != namedSelections_.end()) ? static_cast<int>(selIt->second.size()) : 0;
        objInfo = std::string(pickModeName(pickMode_)) + " [" + std::to_string(selCount) + "] " + objInfo;
    }

    // Show renderer type on right
    std::string rendererName;
    switch (rendererType_) {
        case RendererType::Ascii:   rendererName = "ASCII"; break;
        case RendererType::Braille: rendererName = "BRAILLE"; break;
        case RendererType::Block:   rendererName = "BLOCK"; break;
        case RendererType::Pixel: {
            auto* pc = dynamic_cast<PixelCanvas*>(canvas_.get());
            rendererName = pc && pc->encoder() ? pc->encoder()->name() : "PIXEL";
            break;
        }
    }
    rightInfo = rendererName + " | " + tab.name();

    statusBar_.render(layout_.statusBar(), inputHandler_->mode(),
                      objInfo, rightInfo);
}

void Application::onResize() {
    fflush(stdout);
    fprintf(stdout, "\033[2J");
    fflush(stdout);

    endwin();
    refresh();
    layout_.resize(screen_.height(), screen_.width());
    layout_.markAllDirty();
    if (canvas_) canvas_->invalidate();
    framesToSkip_ = 0;
    needsRedraw_ = true;
}

std::pair<std::size_t, int> Application::clearVisualSelection() {
    std::size_t hadSele = 0;
    auto it = namedSelections_.find(kSele);
    if (it != namedSelections_.end()) {
        hadSele = it->second.size();
        it->second.clear();
    }
    int hadPk = 0;
    for (int p = 0; p < 4; ++p) {
        if (pickRegs_[p] >= 0) ++hadPk;
        pickRegs_[p] = -1;
    }
    pickNext_ = 0;
    return {hadSele, hadPk};
}

std::array<float, 9> Application::setupStereoEye(int eyePass,
                                                  int totalSubW, int subH,
                                                  float aspectYX) {
    auto& cam = tabMgr_.currentTab().camera();
    if (stereoMode_ == StereoMode::Off) {
        cam.prepareProjection(totalSubW, subH, aspectYX);
        return cam.rotation();
    }
    auto savedRot = cam.rotation();
    float halfAngle = stereoAngle_ * 0.5f;
    bool crosseye = (stereoMode_ == StereoMode::Crosseye);
    // Walleye: left eye sees the LEFT image — left half gets the camera
    // rotated to the left (negative Y) so the molecule shifts right toward
    // the user's left visual field. Crosseye swaps the halves.
    float angle;
    if (eyePass == 0) angle = crosseye ? +halfAngle : -halfAngle;
    else              angle = crosseye ? -halfAngle : +halfAngle;
    cam.rotateY(angle);
    int halfW = totalSubW / 2;
    cam.prepareProjection(halfW, subH, aspectYX);
    cam.setProjOffsetX((eyePass == 0) ? halfW * 0.5f : halfW * 1.5f);
    return savedRot;
}

void Application::restoreStereoCamera(const std::array<float, 9>& savedRot) {
    if (stereoMode_ == StereoMode::Off) return;
    tabMgr_.currentTab().camera().setRotation(savedRot);
}

void Application::drawArrowsPixel(PixelCanvas& pc, int subW, int subH,
                                   Camera& cam) {
    if (arrows_.empty()) return;
    int thickness = effectiveArrowThickness();
    int headSize  = effectiveArrowHeadSize();
    // Base color: global :set arrow_color override, else default yellow. Each
    // arrow may override it per-overlay (issue #104); r/g/b are reset at the
    // top of every loop iteration and captured by `plot` by reference.
    uint8_t baseR = 255, baseG = 255, baseB = 50;
    if (arrowColor_) { baseR = (*arrowColor_)[0]; baseG = (*arrowColor_)[1]; baseB = (*arrowColor_)[2]; }
    uint8_t r = baseR, g = baseG, b = baseB;

    // Plot a thickness × thickness pixel block at (x, y, depth). Re-uses
    // PixelCanvas::drawDotRGB to skip the colorIds_ tag (overlays don't
    // participate in outline / fog post-passes).
    auto plot = [&](int x, int y, float depth) {
        for (int dy = 0; dy < thickness; ++dy) {
            for (int dx = 0; dx < thickness; ++dx) {
                int px = x + dx - thickness / 2;
                int py = y + dy - thickness / 2;
                if (px < 0 || px >= subW || py < 0 || py >= subH) continue;
                pc.drawDotRGB(px, py, depth, r, g, b);
            }
        }
    };

    for (const auto& arr : arrows_) {
        r = baseR; g = baseG; b = baseB;
        if (arr.color) { r = (*arr.color)[0]; g = (*arr.color)[1]; b = (*arr.color)[2]; }
        float fxa, fya, fda, fxb, fyb, fdb;
        cam.projectCached(arr.a[0], arr.a[1], arr.a[2], fxa, fya, fda);
        cam.projectCached(arr.b[0], arr.b[1], arr.b[2], fxb, fyb, fdb);
        int x1 = static_cast<int>(fxa), y1 = static_cast<int>(fya);
        int x2 = static_cast<int>(fxb), y2 = static_cast<int>(fyb);
        int steps = std::max(std::abs(x2 - x1), std::abs(y2 - y1));
        if (steps == 0) continue;
        // Solid shaft (vs :measure's dashed pattern).
        for (int s = 0; s <= steps; ++s) {
            float t = static_cast<float>(s) / static_cast<float>(steps);
            int x = x1 + static_cast<int>((x2 - x1) * t);
            int y = y1 + static_cast<int>((y2 - y1) * t);
            float depth = fda + (fdb - fda) * t;
            plot(x, y, depth);
        }
        // Triangular arrowhead at endpoint B. Compute the unit shaft
        // direction in screen space, then fill a triangle whose apex is
        // at (x2, y2) and whose base is `headSize` pixels back along the
        // shaft, perpendicular spread = headSize/2.
        float dxs = static_cast<float>(x2 - x1);
        float dys = static_cast<float>(y2 - y1);
        float L = std::sqrt(dxs*dxs + dys*dys);
        if (L > 1e-3f) {
            float ux = dxs / L, uy = dys / L;
            // Perpendicular (90° CCW in screen coords).
            float px = -uy, py = ux;
            float baseX = x2 - ux * headSize;
            float baseY = y2 - uy * headSize;
            float halfBase = headSize * 0.5f;
            // Scan the triangle by stepping from apex back to base, widening
            // linearly. Cheap rasterizer; arrowheads are tiny.
            for (int i = 0; i <= headSize; ++i) {
                float ti = static_cast<float>(i) / static_cast<float>(headSize);
                float cx = x2 + (baseX - x2) * ti;
                float cy = y2 + (baseY - y2) * ti;
                float halfWidth = halfBase * ti;
                int wHalf = static_cast<int>(halfWidth);
                for (int j = -wHalf; j <= wHalf; ++j) {
                    int hx = static_cast<int>(cx + px * j);
                    int hy = static_cast<int>(cy + py * j);
                    if (hx < 0 || hx >= subW || hy < 0 || hy >= subH) continue;
                    pc.drawDotRGB(hx, hy, fdb, r, g, b);
                }
            }
        }
        // Caption at midpoint, slightly offset perpendicular so it doesn't
        // overlap the shaft.
        if (!arr.caption.empty()) {
            int mx = (x1 + x2) / 2;
            int my = (y1 + y2) / 2;
            paintAnnotationText(pc, mx, my, (fda + fdb) * 0.5f, arr.caption);
        }
    }
}

void Application::drawFreeLabelsPixel(PixelCanvas& pc, int subW, int subH,
                                       Camera& cam) {
    if (freeLabels_.empty()) return;
    int fsize = effectiveLabelFontSize();
    // Inset corner labels by one font-height so they don't kiss the edge.
    int inset = std::max(4, fsize / 2);
    auto paint = [&](int sx, int sy, float depth, const std::string& text) {
        paintLabelText(pc, sx, sy, depth, text);
    };
    for (const auto& fl : freeLabels_) {
        int sx = 0, sy = 0;
        // Depth = 0 places free labels in front of all geometry (smaller
        // depth wins under reverse-z; matches how atom labels are layered
        // after fog/dim post-passes).
        float depth = 0.0f;
        switch (fl.anchor) {
            case FreeLabelAnchor::Corner: {
                int approxW = static_cast<int>(fl.text.size()) * fsize / 2;
                switch (fl.corner) {
                    case FreeLabelCorner::TopLeft:     sx = inset;            sy = inset;          break;
                    case FreeLabelCorner::TopRight:    sx = subW - inset - approxW; sy = inset;    break;
                    case FreeLabelCorner::BottomLeft:  sx = inset;            sy = subH - inset - fsize; break;
                    case FreeLabelCorner::BottomRight: sx = subW - inset - approxW; sy = subH - inset - fsize; break;
                }
                break;
            }
            case FreeLabelAnchor::Screen:
                sx = static_cast<int>(fl.fx * subW);
                sy = static_cast<int>(fl.fy * subH);
                break;
            case FreeLabelAnchor::World: {
                float fsx, fsy, fdepth;
                cam.projectCached(fl.wx, fl.wy, fl.wz, fsx, fsy, fdepth);
                sx = static_cast<int>(fsx);
                sy = static_cast<int>(fsy);
                depth = fdepth;
                break;
            }
        }
        if (sx < -static_cast<int>(fl.text.size()) * fsize ||
            sy < -fsize || sx >= subW || sy >= subH) continue;
        paint(sx, sy, depth, fl.text);
    }
}

void Application::paintLabelText(PixelCanvas& pc, int sx, int sy, float depth,
                                  const std::string& text) {
    uint8_t r = 255, g = 255, b = 255;          // default white
    if (labelColor_) { r = (*labelColor_)[0]; g = (*labelColor_)[1]; b = (*labelColor_)[2]; }
    int fsize = effectiveLabelFontSize();
    if (!labelOutline_ || labelOutlineThickness_ <= 0) {
        pc.drawTextRGB(sx, sy, depth, text, r, g, b, fsize);
        return;
    }
    auto oc = labelOutlineColor_.value_or(autoOutlineColor(r, g, b));
    pc.drawTextOutlinedRGB(sx, sy, depth, text, r, g, b,
                           oc[0], oc[1], oc[2],
                           labelOutlineThickness_, fsize);
}

void Application::paintAnnotationText(PixelCanvas& pc, int sx, int sy, float depth,
                                       const std::string& text) {
    uint8_t r = 255, g = 255, b = 50;           // default yellow
    if (annotationColor_) { r = (*annotationColor_)[0]; g = (*annotationColor_)[1]; b = (*annotationColor_)[2]; }
    int fsize = effectiveAnnotationFontSize();
    if (!annotationOutline_ || annotationOutlineThickness_ <= 0) {
        pc.drawTextRGB(sx, sy, depth, text, r, g, b, fsize);
        return;
    }
    auto oc = annotationOutlineColor_.value_or(autoOutlineColor(r, g, b));
    pc.drawTextOutlinedRGB(sx, sy, depth, text, r, g, b,
                           oc[0], oc[1], oc[2],
                           annotationOutlineThickness_, fsize);
}

void Application::drawPixelOverlay(PixelCanvas& pc, bool includeSeleHighlights) {
    if (!overlayVisible_) return;
    auto& tab = tabMgr_.currentTab();
    int subW = pc.subW();
    int subH = pc.subH();

    // Free-position labels + arrows first — they don't need an object
    // loaded, so a figure with only an axis arrow + corner caption
    // still renders.
    drawFreeLabelsPixel(pc, subW, subH, tab.camera());
    drawArrowsPixel(pc, subW, subH, tab.camera());

    // Labels and measurements are keyed by object and render across every
    // visible object (not just the current one), so an overlay of superposed
    // structures can carry per-structure annotations (issue #101). The
    // $sele/pk highlight below stays scoped to the current object.
    auto& cam = tab.camera();
    int scaleX = pc.scaleX();

    // Residue labels — text rendered in white next to each labeled atom.
    // Text comes from resolveLabel(): per-atom override > label_format > default.
    for (const auto& [objName, labels] : labelsByObject_) {
        auto o = findObjectByName(*this, objName);
        if (!o || !o->visible()) continue;
        const auto& oatoms = o->atoms();
        for (int idx : labels.atoms) {
            if (idx < 0 || idx >= static_cast<int>(oatoms.size())) continue;
            const auto& a = oatoms[idx];
            float fsx, fsy, depth;
            cam.projectCached(a.x, a.y, a.z, fsx, fsy, depth);
            int isx = static_cast<int>(fsx);
            int isy = static_cast<int>(fsy);
            if (isx < 0 || isx >= subW || isy < 0 || isy >= subH) continue;
            paintLabelText(pc, isx + scaleX, isy, depth, resolveLabel(*o, labels, idx));
        }
    }

    // Measurement dashed lines + midpoint distance/angle/dihedral labels.
    if (!measurements_.empty()) {
        const auto& mlc = measurementLineColor_;
        auto drawDash = [&](float sx1, float sy1, float d1,
                            float sx2, float sy2, float d2,
                            int color, int thickness) {
            int isx1 = static_cast<int>(sx1), isy1 = static_cast<int>(sy1);
            int isx2 = static_cast<int>(sx2), isy2 = static_cast<int>(sy2);
            int steps = std::max(std::abs(isx2 - isx1), std::abs(isy2 - isy1));
            if (steps == 0) steps = 1;
            int dashLen = std::max(4, thickness * 2);
            for (int s = 0; s <= steps; s += dashLen * 2) {
                for (int ds = 0; ds < dashLen && s + ds <= steps; ++ds) {
                    float t = static_cast<float>(s + ds) / static_cast<float>(steps);
                    int x = isx1 + static_cast<int>((isx2 - isx1) * t);
                    int y = isy1 + static_cast<int>((isy2 - isy1) * t);
                    float depth = d1 + (d2 - d1) * t;
                    for (int dy = 0; dy < thickness; ++dy) {
                        for (int dx = 0; dx < thickness; ++dx) {
                            int px = x + dx - thickness / 2;
                            int py = y + dy - thickness / 2;
                            if (px < 0 || px >= subW || py < 0 || py >= subH) continue;
                            if (mlc) pc.drawDotRGB(px, py, depth,
                                                   (*mlc)[0], (*mlc)[1], (*mlc)[2]);
                            else     pc.drawDot(px, py, depth, color);
                        }
                    }
                }
            }
        };

        auto cur = tab.currentObject();
        for (const auto& m : measurements_) {
            if (m.atoms.size() < 2) continue;
            auto o = m.obj.empty() ? cur : findObjectByName(*this, m.obj);
            if (!o || !o->visible()) continue;
            const auto& oatoms = o->atoms();
            const int na = static_cast<int>(oatoms.size());
            if (!std::all_of(m.atoms.begin(), m.atoms.end(),
                             [na](int a) { return a >= 0 && a < na; })) continue;
            for (size_t mi = 0; mi + 1 < m.atoms.size(); ++mi) {
                int a1 = m.atoms[mi], a2 = m.atoms[mi + 1];
                float sx1, sy1, d1, sx2, sy2, d2;
                cam.projectCached(oatoms[a1].x, oatoms[a1].y, oatoms[a1].z, sx1, sy1, d1);
                cam.projectCached(oatoms[a2].x, oatoms[a2].y, oatoms[a2].z, sx2, sy2, d2);
                drawDash(sx1, sy1, d1, sx2, sy2, d2, kColorYellow,
                         effectiveAnnotationLineWidth());
            }
            int a1 = m.atoms[0], a2 = m.atoms[1];
            float sx1, sy1, d1, sx2, sy2, d2;
            cam.projectCached(oatoms[a1].x, oatoms[a1].y, oatoms[a1].z, sx1, sy1, d1);
            cam.projectCached(oatoms[a2].x, oatoms[a2].y, oatoms[a2].z, sx2, sy2, d2);
            int lx = (static_cast<int>(sx1) + static_cast<int>(sx2)) / 2;
            int ly = (static_cast<int>(sy1) + static_cast<int>(sy2)) / 2;
            float depth = (d1 + d2) / 2.0f;
            paintAnnotationText(pc, lx, ly, depth, m.displayLabel());
        }
    }

    // $sele highlight + pk1..pk4 pick registers as yellow rings. Skipped
    // when the caller opts out (screenshots default to no halo so the
    // editor's transient selection cue doesn't bake into figures —
    // issue #96).
    if (includeSeleHighlights) {
        auto obj = tab.currentObject();
        if (!obj || !obj->visible()) return;
        const auto& atoms = obj->atoms();
        const std::vector<int>* selIdx = nullptr;
        auto selIt = namedSelections_.find(kSele);
        if (selIt != namedSelections_.end() && !selIt->second.empty())
            selIdx = &selIt->second.indices();

        int pks[4]; int nPk = 0;
        for (int p = 0; p < 4; ++p)
            if (pickRegs_[p] >= 0) pks[nPk++] = pickRegs_[p];
        std::sort(pks, pks + nPk);
        nPk = static_cast<int>(std::unique(pks, pks + nPk) - pks);

        if (selIdx || nPk > 0) {
            int ringR = std::max(3, static_cast<int>(std::lround(
                4.0f * cameraZoomScale(cam.zoom()) * overlayScale_ * renderScaleHint_)));
            std::set<int> highlight;
            if (selIdx) highlight.insert(selIdx->begin(), selIdx->end());
            for (int i = 0; i < nPk; ++i) highlight.insert(pks[i]);
            for (int idx : highlight) {
                if (idx < 0 || idx >= static_cast<int>(atoms.size())) continue;
                const auto& a = atoms[idx];
                float fsx, fsy, depth;
                cam.projectCached(a.x, a.y, a.z, fsx, fsy, depth);
                int isx = static_cast<int>(fsx);
                int isy = static_cast<int>(fsy);
                if (isx < -ringR || isx >= subW + ringR ||
                    isy < -ringR || isy >= subH + ringR) continue;
                pc.drawCircle(isx, isy, depth - 0.01f, ringR, kColorYellow, false);
            }
        }
    }
}

void Application::buildProjCache() {
    if (projCacheFrame_ == frameCounter_ && !projCache_.empty()) return;
    projCacheFrame_ = frameCounter_;
    projCache_.clear();
    pickGrid_.clear();

    auto& tab = tabMgr_.currentTab();
    auto obj = tab.currentObject();
    if (!obj || !obj->visible()) return;

    int sw = canvas_ ? canvas_->subW() : layout_.viewportWidth();
    int sh = canvas_ ? canvas_->subH() : layout_.viewportHeight();
    float aspect = canvas_ ? canvas_->aspectYX() : 2.0f;
    int scaleX = canvas_ ? canvas_->scaleX() : 1;
    int scaleY = canvas_ ? canvas_->scaleY() : 1;
    int w = layout_.viewportWidth();
    int h = layout_.viewportHeight();

    const auto& atoms = obj->atoms();
    auto& cam = tab.camera();
    cam.prepareProjection(sw, sh, aspect);

    projCache_.reserve(atoms.size() / 2);
    for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
        float fsx, fsy, depth;
        cam.projectCached(atoms[i].x, atoms[i].y, atoms[i].z, fsx, fsy, depth);
        int tx = static_cast<int>(fsx) / scaleX;
        int ty = static_cast<int>(fsy) / scaleY;
        if (tx >= 0 && tx < w && ty >= 0 && ty < h) {
            int cacheIdx = static_cast<int>(projCache_.size());
            projCache_.push_back({i, static_cast<int>(fsx), static_cast<int>(fsy), depth});
            pickGrid_[pickGridKey(static_cast<int>(fsx), static_cast<int>(fsy))].push_back(cacheIdx);
        }
    }
}

int Application::findNearestAtom(int termX, int termY) const {
    int scaleX = canvas_ ? canvas_->scaleX() : 1;
    int scaleY = canvas_ ? canvas_->scaleY() : 1;
    int subX = termX * scaleX + scaleX / 2;
    int subY = termY * scaleY + scaleY / 2;

    int bestIdx = -1;
    float bestDist2 = std::numeric_limits<float>::max();
    float bestDepth = std::numeric_limits<float>::max();

    // Query 3x3 neighborhood in spatial hash
    int cx = subX / kPickCellSize, cy = subY / kPickCellSize;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int key = (cy + dy) * 10000 + (cx + dx);
            auto it = pickGrid_.find(key);
            if (it == pickGrid_.end()) continue;
            for (int ci : it->second) {
                const auto& pa = projCache_[ci];
                float ddx = static_cast<float>(pa.sx - subX);
                float ddy = static_cast<float>(pa.sy - subY);
                float dist2 = ddx * ddx + ddy * ddy;
                if (dist2 < bestDist2 - 0.5f ||
                    (dist2 < bestDist2 + 0.5f && pa.depth < bestDepth)) {
                    bestDist2 = dist2;
                    bestDepth = pa.depth;
                    bestIdx = pa.idx;
                }
            }
        }
    }

    float maxRange = static_cast<float>(10 * std::max(scaleX, scaleY));
    if (bestDist2 > maxRange * maxRange) return -1;
    return bestIdx;
}

std::string Application::residueInfoString(const AtomData& a) const {
    char buf[80];
    std::snprintf(buf, sizeof(buf),
        "%s %d%s (chain %s)",
        a.resName.c_str(), a.resSeq,
        (a.insCode == ' ' ? "" : std::string(1, a.insCode).c_str()),
        a.chainId.c_str());
    return std::string(buf);
}

std::string Application::atomInfoString(const MolObject& mol, int atomIdx) const {
    if (atomIdx < 0 || atomIdx >= static_cast<int>(mol.atoms().size()))
        return "";
    const auto& a = mol.atoms()[atomIdx];
    char buf[256];
    std::string insStr = (a.insCode != ' ') ? std::string(1, a.insCode) : "";
    snprintf(buf, sizeof(buf),
        "%s/%s %d%s/%s (%s) B=%.1f occ=%.2f [%.2f, %.2f, %.2f]",
        a.chainId.c_str(), a.resName.c_str(), a.resSeq,
        insStr.c_str(),
        a.name.c_str(), a.element.c_str(),
        a.bFactor, a.occupancy,
        a.x, a.y, a.z);
    return std::string(buf);
}

bool Application::recomputeInterface() {
    auto obj = tabMgr_.currentTab().currentObject();
    if (!obj) {
        interfaceContacts_.clear();
        interfaceAtomMask_.clear();
        interfaceRepr_.clear();
        return false;
    }

    contactMapPanel_.update(*obj);
    contactMapPanel_.contactMap().computeInterface(*obj, interfaceCutoff_);
    interfaceContacts_ = contactMapPanel_.contactMap().interfaceContacts();
    if (interfaceContacts_.empty()) {
        interfaceAtomMask_.clear();
        interfaceRepr_.clear();
        return false;
    }

    const auto& atoms = obj->atoms();
    interfaceAtomMask_.assign(atoms.size(), false);
    std::set<std::tuple<std::string,int,char>> interfaceResidues;
    for (const auto& c : interfaceContacts_) {
        if (c.atom1 >= 0 && c.atom1 < (int)atoms.size()) {
            const auto& a = atoms[c.atom1];
            interfaceResidues.emplace(a.chainId, a.resSeq, a.insCode);
        }
        if (c.atom2 >= 0 && c.atom2 < (int)atoms.size()) {
            const auto& a = atoms[c.atom2];
            interfaceResidues.emplace(a.chainId, a.resSeq, a.insCode);
        }
    }
    for (size_t i = 0; i < atoms.size(); ++i) {
        const auto& a = atoms[i];
        if (interfaceResidues.count({a.chainId, a.resSeq, a.insCode}))
            interfaceAtomMask_[i] = true;
    }
    interfaceRepr_.setData(interfaceAtomMask_, interfaceContacts_);
    interfaceRepr_.setDrawSidechains(interfaceSidechains_);
    interfaceRepr_.setInteractionThickness(interfaceThickness_);
    interfaceRepr_.setLineThickness(std::max(1, interfaceThickness_ - 1));
    interfaceRepr_.setShowMask(interfaceShowMask_);
    return true;
}

void Application::onCurrentObjectChanged() {
    // Stale-overlay guard: when the user switches objects, any state
    // built against the prior mol's atom indices (interface mask /
    // contacts) no longer corresponds to what's being rendered. Refresh
    // it so dashes + sidechain hairs match the new current object.
    if (interfaceOverlay_) {
        if (!recomputeInterface()) {
            // New object has no inter-chain contacts — turn the overlay
            // off rather than leave a dangling on-state with empty data.
            interfaceOverlay_ = false;
            interfaceFromZoom_ = false;
        }
    }
    if (canvas_) canvas_->invalidate();
}

Selection Application::parseSelection(const std::string& expr, const MolObject& mol) {
    auto resolver = [this](const std::string& name) -> const Selection* {
        auto it = namedSelections_.find(name);
        return (it != namedSelections_.end()) ? &it->second : nullptr;
    };
    auto sel = Selection::parse(expr, mol, resolver);
    // Auto-save latest result under kSele so `$sele` references it.
    namedSelections_[kSele] = sel;
    return sel;
}

// ── Focus Selection mode ────────────────────────────────────────────────────
//
// Mol*-style click-to-focus. enterFocus snapshots camera + per-repr
// visibility, snaps the camera to the subject centroid, hides
// non-neighborhood atoms for atom-direct reprs (Wireframe/BallStick/
// Spacefill), and forces ball-stick visible on the neighborhood so
// sidechains pop. exitFocus restores everything.

std::vector<int> Application::expandByFocusGranularity(const MolObject& mol,
                                                       int atomIdx) const {
    std::vector<int> out;
    const auto& atoms = mol.atoms();
    if (atomIdx < 0 || atomIdx >= (int)atoms.size()) return out;
    const auto& a = atoms[atomIdx];

    if (focusGranularity_ == FocusGranularity::Chain) {
        for (int i = 0; i < (int)atoms.size(); ++i) {
            if (atoms[i].chainId == a.chainId) out.push_back(i);
        }
        return out;
    }

    if (focusGranularity_ == FocusGranularity::Sidechain) {
        for (int i = 0; i < (int)atoms.size(); ++i) {
            const auto& b = atoms[i];
            if (b.chainId != a.chainId || b.resSeq != a.resSeq ||
                b.insCode != a.insCode) continue;
            const std::string& nm = b.name;
            if (nm == "N" || nm == "CA" || nm == "C" || nm == "O") continue;
            out.push_back(i);
        }
        if (!out.empty()) return out;
        // Empty sidechain (e.g. Gly) → fall through to Residue.
    }

    // Residue (default + Sidechain fallback)
    for (int i = 0; i < (int)atoms.size(); ++i) {
        const auto& b = atoms[i];
        if (b.chainId == a.chainId && b.resSeq == a.resSeq &&
            b.insCode == a.insCode) {
            out.push_back(i);
        }
    }
    return out;
}

// ── View-fit (issue #98) ────────────────────────────────────────────────
// Best sub-pixel dimensions to fit against when no real canvas is active
// (e.g. a --no-tui script before the first screenshot). The screenshot
// command re-fits to the actual output size, so this only needs to be a
// reasonable default for the live preview.
static void bestFitCanvasDims(const Canvas* c, int& W, int& H, float& ay) {
    if (c && c->subW() > 0 && c->subH() > 0) {
        W = c->subW(); H = c->subH(); ay = c->aspectYX();
    } else {
        W = 1440; H = 1080; ay = 1.0f;   // 4:3, square sub-pixels
    }
}

float Application::computeFitZoom(int W, int H, float aspectYX) const {
    const auto& cam = tabMgr_.currentTab().camera();
    if (!viewFit_.active || viewFit_.xs.empty() || W <= 0 || H <= 0)
        return cam.zoom();

    // Project each subject atom through the current rotation+center and
    // take the half-extents along screen X and Y (world Å).
    const auto& R = cam.rotation();
    const float cx = cam.centerX(), cy = cam.centerY(), cz = cam.centerZ();
    float ex = 0.0f, ey = 0.0f;
    for (size_t i = 0; i < viewFit_.xs.size(); ++i) {
        const float x = viewFit_.xs[i] - cx;
        const float y = viewFit_.ys[i] - cy;
        const float z = viewFit_.zs[i] - cz;
        ex = std::max(ex, std::fabs(R[0]*x + R[1]*y + R[2]*z));
        ey = std::max(ey, std::fabs(R[3]*x + R[4]*y + R[5]*z));
    }
    ex = std::max(ex + viewFit_.pad, viewFit_.minExtent);
    ey = std::max(ey + viewFit_.pad, viewFit_.minExtent);

    // projectCached() maps Å→sub-pixels via Camera::scaleFromZoom on X and
    // scale/aspectYX on Y. Solve for the scale that keeps the projected
    // half-extent within fill*frame/2 in both dimensions, then invert through
    // Camera::zoomFromScale so the two formulas stay locked together.
    const float fW = viewFit_.fill * static_cast<float>(W) * 0.5f;
    const float fH = viewFit_.fill * static_cast<float>(H) * 0.5f * aspectYX;
    const float scale = std::min(fW / ex, fH / ey);
    return std::clamp(Camera::zoomFromScale(scale, W, H), 0.01f, 100.0f);
}

void Application::applyViewFit(int W, int H, float aspectYX) {
    if (!viewFit_.active) return;
    tabMgr_.currentTab().camera().setZoom(computeFitZoom(W, H, aspectYX));
}

void Application::setViewFit(std::vector<float> xs, std::vector<float> ys,
                             std::vector<float> zs, float fill, float pad,
                             float minExtent) {
    viewFit_.active = true;
    viewFit_.xs = std::move(xs);
    viewFit_.ys = std::move(ys);
    viewFit_.zs = std::move(zs);
    viewFit_.fill = fill;
    viewFit_.pad = pad;
    viewFit_.minExtent = minExtent;
    int W, H; float ay;
    bestFitCanvasDims(canvas_.get(), W, H, ay);
    applyViewFit(W, H, ay);
}

void Application::clearViewFit() {
    viewFit_.active = false;
    viewFit_.xs.clear();
    viewFit_.ys.clear();
    viewFit_.zs.clear();
}

void Application::enterFocus(MolObject& mol,
                             const std::vector<int>& subjectIndices,
                             const std::string& exprDesc) {
    if (subjectIndices.empty()) return;
    if (focusSnapshot_.active) exitFocus();   // refocus → exit then re-enter

    const auto& atoms = mol.atoms();

    // Snapshot camera state.
    auto& cam = tabMgr_.currentTab().camera();
    focusSnapshot_.active = true;
    focusSnapshot_.rot    = cam.rotation();
    focusSnapshot_.cx     = cam.centerX();
    focusSnapshot_.cy     = cam.centerY();
    focusSnapshot_.cz     = cam.centerZ();
    focusSnapshot_.panX   = cam.panXOffset();
    focusSnapshot_.panY   = cam.panYOffset();
    focusSnapshot_.zoom   = cam.zoom();

    // Snapshot per-repr visibility for the atom-direct reprs we touch.
    static const ReprType kTouchedReprs[] = {
        ReprType::Wireframe, ReprType::BallStick, ReprType::Spacefill,
    };
    focusSnapshot_.reprs.clear();
    for (ReprType r : kTouchedReprs) {
        FocusSavedRepr s;
        s.type        = r;
        s.objectLevel = mol.reprVisible(r);
        s.atomMask    = mol.atomVisMask(r);    // empty if all-visible
        focusSnapshot_.reprs.push_back(std::move(s));
    }
    // Spline reprs are hidden during focus (they obscure the close-up
    // sidechain/wireframe view); save their object-level state so we can
    // put them back on exit.
    focusSnapshot_.cartoonVisible  = mol.reprVisible(ReprType::Cartoon);
    focusSnapshot_.ribbonVisible   = mol.reprVisible(ReprType::Ribbon);
    focusSnapshot_.backboneVisible = mol.reprVisible(ReprType::Backbone);
    // Snapshot wireframe thickness so we can bump it during focus.
    if (auto* wf = dynamic_cast<WireframeRepr*>(getRepr(ReprType::Wireframe))) {
        focusSnapshot_.wireframeThickness = wf->thickness();
    }

    // Compute subject centroid (for camera snap) + enclosing radius
    // (for subject-size aware zoom — Mol*-style).
    float sx = 0, sy = 0, sz = 0;
    int n = 0;
    std::vector<float> fxs, fys, fzs;
    fxs.reserve(subjectIndices.size());
    fys.reserve(subjectIndices.size());
    fzs.reserve(subjectIndices.size());
    for (int idx : subjectIndices) {
        if (idx < 0 || idx >= (int)atoms.size()) continue;
        sx += atoms[idx].x; sy += atoms[idx].y; sz += atoms[idx].z;
        fxs.push_back(atoms[idx].x);
        fys.push_back(atoms[idx].y);
        fzs.push_back(atoms[idx].z);
        ++n;
    }
    if (n == 0) { focusSnapshot_.active = false; return; }
    sx /= n; sy /= n; sz /= n;

    // Snap the camera to the subject centroid, then fit its *projected*
    // extent to the frame (issue #98). The fit is recomputed per output
    // canvas (live + each :screenshot), so it fills the frame at any
    // resolution/aspect instead of a fixed reference viewport.
    cam.focusOn(sx, sy, sz, cam.zoom());
    setViewFit(fxs, fys, fzs, focusFillFraction_, focusExtraRadius_,
               focusMinRadius_);

    // Build subject mask first.
    focusAtomMask_.assign(atoms.size(), false);
    for (int idx : subjectIndices) {
        if (idx >= 0 && idx < (int)atoms.size()) focusAtomMask_[idx] = true;
    }

    // Spatial neighborhood: every atom within focus_radius of any
    // subject atom. This catches the close-pocket geometry — backbone
    // + sidechains touching the subject — but it's distance-based, so
    // a long sidechain reaching the pocket may have its CA outside.
    const float r2 = focusRadius_ * focusRadius_;
    focusNbhdMask_.assign(atoms.size(), false);
    for (size_t i = 0; i < atoms.size(); ++i) {
        const auto& ai = atoms[i];
        for (int j : subjectIndices) {
            if (j < 0 || j >= (int)atoms.size()) continue;
            const auto& aj = atoms[j];
            float dx = ai.x - aj.x, dy = ai.y - aj.y, dz = ai.z - aj.z;
            if (dx*dx + dy*dy + dz*dz <= r2) { focusNbhdMask_[i] = true; break; }
        }
    }

    // Ensure interface contacts are cached. We need them before building
    // nbhdIndices so the partner-residue expansion below can promote
    // whole interacting residues into the neighborhood.
    if (interfaceContacts_.empty()) {
        contactMapPanel_.update(mol);
        contactMapPanel_.contactMap().computeInterface(mol, 4.5f);
        interfaceContacts_ = contactMapPanel_.contactMap().interfaceContacts();
        focusComputedInterface_ = true;
    }

    // Promote any residue that has a contact reaching into the subject:
    // the user expects to see the whole interacting residue, not the
    // truncated portion that happens to fall inside focus_radius. Each
    // partner is identified by (chainId, resSeq, insCode), then every
    // atom sharing that key is added to the neighborhood mask.
    std::set<std::tuple<std::string, int, char>> partnerResidues;
    for (const auto& c : interfaceContacts_) {
        if (c.atom1 < 0 || c.atom2 < 0) continue;
        if (c.atom1 >= (int)atoms.size() || c.atom2 >= (int)atoms.size()) continue;
        bool s1 = focusAtomMask_[c.atom1];
        bool s2 = focusAtomMask_[c.atom2];
        if (s1 == s2) continue;     // both in subject, or neither — no partner edge
        int partner = s1 ? c.atom2 : c.atom1;
        const auto& a = atoms[partner];
        partnerResidues.emplace(a.chainId, a.resSeq, a.insCode);
    }
    // Fused pass: for each atom, promote it into the mask if its residue
    // matched a partner, then collect all in-mask atoms into nbhdIndices.
    std::vector<int> nbhdIndices;
    nbhdIndices.reserve(atoms.size());
    for (size_t i = 0; i < atoms.size(); ++i) {
        const auto& a = atoms[i];
        if (!focusNbhdMask_[i] && !partnerResidues.empty()
            && partnerResidues.count({a.chainId, a.resSeq, a.insCode})) {
            focusNbhdMask_[i] = true;
        }
        if (focusNbhdMask_[i]) nbhdIndices.push_back((int)i);
    }

    mol.hideRepr(ReprType::Cartoon);
    mol.hideRepr(ReprType::Ribbon);
    mol.hideRepr(ReprType::Backbone);

    // Bump wireframe thickness modestly so the local scaffold reads;
    // the zoom-scaling in WireframeRepr::render does the rest.
    if (auto* wf = dynamic_cast<WireframeRepr*>(getRepr(ReprType::Wireframe))) {
        wf->setThickness(std::max(0.14f, focusSnapshot_.wireframeThickness * 1.4f));
    }
    mol.showRepr(ReprType::Wireframe);
    mol.showReprForAtoms(ReprType::Wireframe, nbhdIndices);

    mol.showRepr(ReprType::BallStick);
    mol.showReprForAtoms(ReprType::BallStick, nbhdIndices);

    if (mol.reprVisible(ReprType::Spacefill)) {
        mol.showReprForAtoms(ReprType::Spacefill, nbhdIndices);
    }

    // Filter contacts to ones whose endpoints are both in the (now
    // residue-expanded) neighborhood, so the dashed lines render only
    // for what's visible in the pocket.
    std::vector<InterfaceContact> filtered;
    filtered.reserve(interfaceContacts_.size());
    for (const auto& c : interfaceContacts_) {
        if (c.atom1 < 0 || c.atom2 < 0) continue;
        if (c.atom1 >= (int)atoms.size() || c.atom2 >= (int)atoms.size()) continue;
        if (focusNbhdMask_[c.atom1] && focusNbhdMask_[c.atom2])
            filtered.push_back(c);
    }
    interfaceRepr_.setData(focusNbhdMask_, std::move(filtered));
    interfaceRepr_.setDrawSidechains(false);   // wireframe already covers it
    interfaceRepr_.setInteractionThickness(interfaceThickness_);
    interfaceRepr_.setShowMask(interfaceShowMask_);

    focusExpr_ = exprDesc.empty() ? std::string("focus") : exprDesc;
    char msg[160];
    std::snprintf(msg, sizeof(msg),
        "Focus: %d atoms (radius=%.1fA zoom=%.2f) — F or Esc to exit",
        (int)nbhdIndices.size(), focusRadius_,
        tabMgr_.currentTab().camera().zoom());
    cmdLine_.setMessage(msg);
    needsRedraw_ = true;
}

void Application::exitFocus() {
    if (!focusSnapshot_.active) return;
    clearViewFit();   // restoring the saved zoom — drop the fit intent
    auto obj = tabMgr_.currentTab().currentObject();
    if (!obj) {
        focusSnapshot_.active = false;
        focusAtomMask_.clear();
        focusNbhdMask_.clear();
        focusExpr_.clear();
        return;
    }

    // Restore camera.
    auto& cam = tabMgr_.currentTab().camera();
    cam.setRotation(focusSnapshot_.rot);
    cam.setCenter(focusSnapshot_.cx, focusSnapshot_.cy, focusSnapshot_.cz);
    cam.setPan(focusSnapshot_.panX, focusSnapshot_.panY);
    cam.setZoom(focusSnapshot_.zoom);

    // Restore per-repr visibility atom-for-atom.
    for (const auto& s : focusSnapshot_.reprs) {
        if (s.atomMask.empty()) {
            // Pre-focus state was "all visible" → clear any per-atom mask.
            // showRepr with no per-atom args resets the mask in current API.
            if (s.objectLevel) obj->showRepr(s.type);
            else               obj->hideRepr(s.type);
        } else {
            // Pre-focus state had a custom mask — re-apply it via
            // showReprForAtoms with the previously-visible indices.
            std::vector<int> idxs;
            idxs.reserve(s.atomMask.size());
            for (size_t i = 0; i < s.atomMask.size(); ++i)
                if (s.atomMask[i]) idxs.push_back((int)i);
            obj->showReprForAtoms(s.type, idxs);
            if (!s.objectLevel) obj->hideRepr(s.type);
        }
    }

    // Restore spline reprs.
    if (focusSnapshot_.cartoonVisible)  obj->showRepr(ReprType::Cartoon);
    else                                obj->hideRepr(ReprType::Cartoon);
    if (focusSnapshot_.ribbonVisible)   obj->showRepr(ReprType::Ribbon);
    else                                obj->hideRepr(ReprType::Ribbon);
    if (focusSnapshot_.backboneVisible) obj->showRepr(ReprType::Backbone);
    else                                obj->hideRepr(ReprType::Backbone);

    // Restore wireframe thickness.
    if (auto* wf = dynamic_cast<WireframeRepr*>(getRepr(ReprType::Wireframe))) {
        wf->setThickness(focusSnapshot_.wireframeThickness);
    }

    // Put the unfiltered interface contact list back if the global
    // overlay was on. If focus computed interactions on demand (no
    // pre-existing :interface), drop them entirely on exit.
    if (focusComputedInterface_) {
        interfaceContacts_.clear();
        interfaceAtomMask_.clear();
        interfaceRepr_.clear();
        focusComputedInterface_ = false;
    } else if (interfaceOverlay_ && !interfaceContacts_.empty()) {
        interfaceRepr_.setData(interfaceAtomMask_, interfaceContacts_);
        interfaceRepr_.setDrawSidechains(interfaceSidechains_);
        interfaceRepr_.setShowMask(interfaceShowMask_);
    } else {
        interfaceRepr_.clear();
    }

    focusSnapshot_.active = false;
    focusSnapshot_.reprs.clear();
    focusAtomMask_.clear();
    focusNbhdMask_.clear();
    focusExpr_.clear();
    cmdLine_.setMessage("Focus exited");
    needsRedraw_ = true;
}

void Application::executeSearch(const std::string& query) {
    lastSearch_ = query;
    searchMatches_.clear();
    searchIdx_ = -1;

    auto obj = tabMgr_.currentTab().currentObject();
    if (!obj) {
        cmdLine_.setMessage("No object selected");
        return;
    }

    // Parse as selection expression
    auto sel = parseSelection(query, *obj);
    searchMatches_ = std::vector<int>(sel.indices().begin(), sel.indices().end());

    if (searchMatches_.empty()) {
        cmdLine_.setMessage("/" + query + ": no matches");
    } else {
        searchIdx_ = 0;
        const auto& a = obj->atoms()[searchMatches_[0]];
        cmdLine_.setMessage("/" + query + ": " + std::to_string(searchMatches_.size()) +
                           " atoms [1/" + std::to_string(searchMatches_.size()) + "] " +
                           a.chainId + " " + a.resName + " " + std::to_string(a.resSeq) +
                           " " + a.name);
    }
}

// ── Macro recording ─────────────────────────────────────────────────────────

void Application::startMacroRecord(char reg) {
    macroRecording_ = true;
    macroRegister_ = reg;
    currentMacro_.clear();
    cmdLine_.setMessage(std::string("Recording @") + reg + "...");
}

void Application::stopMacroRecord() {
    if (!macroRecording_) return;
    macros_[macroRegister_] = std::move(currentMacro_);
    cmdLine_.setMessage(std::string("Recorded @") + macroRegister_ +
                        " (" + std::to_string(macros_[macroRegister_].size()) + " actions)");
    macroRecording_ = false;
    macroRegister_ = '\0';
    currentMacro_.clear();
}

void Application::recordAction(Action action) {
    if (!macroRecording_) return;
    // Don't record meta-actions that control recording itself
    if (action == Action::StartMacro || action == Action::PlayMacro) return;
    currentMacro_.push_back(action);
}

void Application::playMacro(char reg) {
    auto it = macros_.find(reg);
    if (it == macros_.end() || it->second.empty()) {
        cmdLine_.setMessage(std::string("Macro @") + reg + " is empty");
        return;
    }
    for (Action a : it->second) {
        handleAction(a);
    }
    cmdLine_.setMessage(std::string("Played @") + reg +
                        " (" + std::to_string(it->second.size()) + " actions)");
}

void Application::registerCommands() {
    // Per-area command families live in src/cmd/commands/*.cpp (see
    // cmd/commands/Commands.h). registerCommands() is mid-decomposition: the
    // groups below have moved out; the rest is still inline pending migration.
    registerSessionCommands(cmdRegistry_);

    registerFilesCommands(cmdRegistry_);

    registerDisplayCommands(cmdRegistry_);
    registerViewCommands(cmdRegistry_);
    registerSettingsCommands(cmdRegistry_);
    registerObjectCommands(cmdRegistry_);
    registerScriptingCommands(cmdRegistry_);
    registerSelectionCommands(cmdRegistry_);
    registerAlignmentCommands(cmdRegistry_);
    registerFetchCommands(cmdRegistry_);
    registerAnnotationCommands(cmdRegistry_);
    registerMeasurementCommands(cmdRegistry_);
}

// ----------------------------------------------------------------------
// Help overlay population
// ----------------------------------------------------------------------

void Application::activateOverlay(std::string title,
                                  std::vector<std::string> lines,
                                  std::vector<int> colors,
                                  std::string headlessTitle) {
    if (isHeadless() && !headlessTitle.empty()) {
        std::cout << headlessTitle << "\n\n";
        for (const auto& l : lines) std::cout << l << "\n";
        return;
    }
    infoOverlay_.title = std::move(title);
    infoOverlay_.lines = std::move(lines);
    infoOverlay_.lineColors = std::move(colors);
    infoOverlay_.scrollOffset = 0;
    infoOverlay_.active = true;
}

void Application::showKeybindingHelp() {
    activateOverlay("MolTerm Keybindings", {
        "NAVIGATION",
        " h/j/k/l   Rotate molecule",
        " W/A/S/D   Pan view",
        " +/-       Zoom in/out",
        " </>       Z-axis rotation",
        " 0         Reset view",
        "",
        "REPRESENTATIONS (s=show, x=hide)",
        " sw/sb/ss/sc/sr/sk   wire/ball/fill/cartoon/ribbon/bone",
        " xw/xb/xs/xc/xr/xk   hide each      xa  hide all",
        "",
        "COLORING (c + key)",
        " ce element  cc chain  cs SS  cb B-factor",
        " cp pLDDT    cr rainbow  ct restype  ca SASA",
        "",
        "OBJECTS & TABS",
        " Tab/S-Tab  Next/prev object   Space  Toggle visible",
        " gt/gT      Next/prev tab      dd     Delete object",
        " o panel   i inspect   / search   n/N results",
        " gs/gS/gC  Atom/residue/chain pick   gf  Focus pick",
        " gx        Clear $sele + pk1-pk4",
        "",
        "ANALYSIS & STATE",
        " I  toggle :interface overlay     F  focus picked residue",
        " [/] prev/next state              b  toggle sequence bar",
        " {/} seqbar prev/next chain",
        "",
        "MACROS / OTHER",
        " q+a-z record   @+a-z play    m  toggle pixel renderer",
        " P  screenshot  u/Ctrl-r undo/redo  .  repeat last action",
        "",
        ":help [cmd]   :load   :fetch   :align   :measure",
        ":focus   :dssp   :interface   :export   :screenshot",
    });
}

void Application::recordTranscript(const std::string& input, const std::string& output) {
    static constexpr size_t kTranscriptMax = 1000;
    if (input.empty()) return;
    cmdTranscript_.push_back(":" + input);
    if (!output.empty()) {
        std::istringstream iss(output);
        std::string line;
        while (std::getline(iss, line)) cmdTranscript_.push_back("  " + line);
    }
    if (cmdTranscript_.size() > kTranscriptMax)
        cmdTranscript_.erase(cmdTranscript_.begin(),
                             cmdTranscript_.begin() +
                                 (cmdTranscript_.size() - kTranscriptMax));
}

void Application::showCommandTranscript() {
    std::vector<std::string> lines = cmdTranscript_;
    if (lines.empty()) lines.push_back("(no command history yet)");
    activateOverlay("Command history  (\":\" = input, indented = output)",
                    lines, {}, "Command history");
    // Start at the bottom so the most recent commands are visible; the render
    // path clamps scrollOffset to the last page.
    infoOverlay_.scrollOffset = static_cast<int>(lines.size());
}

void Application::showCommandIndex() {
    // Group commands by category. Aliases (commands sharing usage with a
    // canonical entry, like :e for :load) appear in the same group; we
    // keep them all so the index shows every dispatchable name.
    // Category "Hidden" is reserved for back-compat aliases that are
    // dispatchable but should not surface in the index (still reachable
    // via :help :<name>).
    std::map<std::string, std::vector<const CommandInfo*>> groups;
    for (const auto& [name, info] : cmdRegistry_.all()) {
        if (info.category == "Hidden") continue;
        const std::string& cat = info.category.empty() ? "Misc" : info.category;
        groups[cat].push_back(&info);
    }
    // Stable category ordering: most user-facing first, then alphabetical.
    static const std::vector<std::string> ordered = {
        "Files", "Display", "Coloring", "View", "Selection",
        "Measurement", "Analysis", "Session", "Window", "Help"
    };

    std::vector<std::string> lines;
    auto emitGroup = [&](const std::string& cat,
                         std::vector<const CommandInfo*>& cmds) {
        std::sort(cmds.begin(), cmds.end(),
                  [](const CommandInfo* a, const CommandInfo* b) {
                      return a->name < b->name;
                  });
        lines.push_back(cat);
        for (const auto* info : cmds) {
            std::string usage = info->usage.empty() ? (":" + info->name) : info->usage;
            std::string row = " " + usage;
            const int kPad = 36;
            if (static_cast<int>(row.size()) < kPad)
                row.append(kPad - row.size(), ' ');
            else
                row += "  ";
            row += info->description;
            lines.push_back(row);
        }
        lines.push_back("");
    };

    for (const auto& cat : ordered) {
        auto it = groups.find(cat);
        if (it == groups.end()) continue;
        emitGroup(cat, it->second);
        groups.erase(it);
    }
    // Anything uncategorized falls through alphabetically.
    for (auto& [cat, cmds] : groups) emitGroup(cat, cmds);

    if (!lines.empty() && lines.back().empty()) lines.pop_back();
    lines.push_back(":help <cmd>  for usage, description, and examples");

    activateOverlay("MolTerm Commands", std::move(lines), {}, "MolTerm Commands");
}

void Application::showCommandHelp(const CommandInfo& info) {
    std::vector<std::string> lines;
    lines.push_back("Usage:");
    lines.push_back("  " + (info.usage.empty() ? (":" + info.name) : info.usage));
    lines.push_back("");
    if (!info.description.empty()) {
        lines.push_back("Description:");
        lines.push_back("  " + info.description);
        lines.push_back("");
    }
    if (!info.category.empty()) {
        lines.push_back("Category: " + info.category);
        lines.push_back("");
    }
    if (!info.examples.empty()) {
        lines.push_back("Examples:");
        for (const auto& ex : info.examples) lines.push_back("  " + ex);
    }
    if (!lines.empty() && lines.back().empty()) lines.pop_back();

    activateOverlay(":" + info.name, std::move(lines), {}, ":" + info.name);
}

void Application::showInterfaceLegend() {
    // Color each legend swatch row in the interaction-type's render color so
    // the modal directly mirrors what the viewport draws.
    static constexpr const char* kSwatch = "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80 "
                                            "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80 "
                                            "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80";  // ─── ─── ───
    static constexpr const char* kAng = "\xC3\x85";  // Å

    std::vector<std::string> lines;
    std::vector<int> colors;
    auto add = [&](std::string text, int color = -1) {
        lines.push_back(std::move(text));
        colors.push_back(color);
    };

    add("Interaction types (dashed inter-chain lines):");
    add("");
    add(std::string("  ") + kSwatch + "  H-bond        N/O \xE2\x86\x94 N/O,           \xE2\x89\xA4 3.5 " + kAng,
        interactionColor(InteractionType::HBond));
    add(std::string("  ") + kSwatch + "  Salt bridge   charged,             \xE2\x89\xA4 4.0 " + kAng,
        interactionColor(InteractionType::SaltBridge));
    add(std::string("  ") + kSwatch + "  Hydrophobic   C \xE2\x86\x94 C,               \xE2\x89\xA4 4.5 " + kAng,
        interactionColor(InteractionType::Hydrophobic));
    add(std::string("  ") + kSwatch + "  Other         heavy-atom pair below cutoff",
        interactionColor(InteractionType::Other));
    add("");

    if (interfaceContacts_.empty()) {
        add("Statistics:");
        add("  No interface overlay active.");
        add("  Run :interface on  to compute and display contacts.");
    } else {
        // When focus mode is active, restrict statistics to contacts that
        // touch the focus subject — the user is looking at one binding site,
        // so the legend should reflect that site, not the whole structure.
        const bool focusFiltered = focusSnapshot_.active && !focusAtomMask_.empty();
        auto inFocus = [&](int atomIdx) {
            return atomIdx >= 0 && atomIdx < (int)focusAtomMask_.size()
                   && focusAtomMask_[atomIdx];
        };

        struct TypeStats {
            int n = 0;
            float dMin = std::numeric_limits<float>::infinity();
            float dMax = -std::numeric_limits<float>::infinity();
            float dSum = 0.0f;
        };
        std::array<TypeStats, 4> ts{};
        std::set<std::tuple<std::string, int, char>> residues;
        auto obj = tabMgr_.currentTab().currentObject();
        const std::vector<AtomData> empty;
        const std::vector<AtomData>& atoms = obj ? obj->atoms() : empty;
        for (const auto& c : interfaceContacts_) {
            if (focusFiltered && !inFocus(c.atom1) && !inFocus(c.atom2)) continue;
            int idx = static_cast<int>(c.type);
            if (idx >= 0 && idx < 4) {
                ts[idx].n++;
                ts[idx].dMin = std::min(ts[idx].dMin, c.distance);
                ts[idx].dMax = std::max(ts[idx].dMax, c.distance);
                ts[idx].dSum += c.distance;
            }
            if (c.atom1 >= 0 && c.atom1 < (int)atoms.size()) {
                const auto& a = atoms[c.atom1];
                residues.emplace(a.chainId, a.resSeq, a.insCode);
            }
            if (c.atom2 >= 0 && c.atom2 < (int)atoms.size()) {
                const auto& a = atoms[c.atom2];
                residues.emplace(a.chainId, a.resSeq, a.insCode);
            }
        }
        int total = 0;
        for (const auto& s : ts) total += s.n;

        add("Statistics:");
        char buf[200];
        if (focusFiltered) {
            const std::string& expr = focusExpr_.empty() ? std::string("focus subject") : focusExpr_;
            std::snprintf(buf, sizeof(buf), "  Scope: focus subject  (%.*s)",
                          static_cast<int>(std::min<size_t>(expr.size(), 80)), expr.c_str());
            add(buf);
        } else {
            add("  Scope: whole structure");
        }
        std::snprintf(buf, sizeof(buf), "  Total contacts:        %d", total);
        add(buf);
        std::snprintf(buf, sizeof(buf), "  Residues at interface: %zu", residues.size());
        add(buf);
        add("");
        add("By type:");
        struct Row { const char* name; InteractionType type; };
        const Row rows[4] = {
            {"H-bond",      InteractionType::HBond},
            {"Salt bridge", InteractionType::SaltBridge},
            {"Hydrophobic", InteractionType::Hydrophobic},
            {"Other",       InteractionType::Other},
        };
        for (const auto& r : rows) {
            const auto& s = ts[static_cast<int>(r.type)];
            const bool drawn = interfaceShowMask_ & interactionBit(r.type);
            const char* hidden = drawn ? "" : "  [hidden]";
            if (s.n == 0) {
                std::snprintf(buf, sizeof(buf),
                              "  %-12s %4d%s", r.name, 0, hidden);
            } else {
                float pct = total ? (100.0f * s.n / total) : 0.0f;
                float avg = s.dSum / s.n;
                std::snprintf(buf, sizeof(buf),
                              "  %-12s %4d  (%4.1f%%)  avg %.2f %s   (%.2f - %.2f)%s",
                              r.name, s.n, pct, avg, kAng, s.dMin, s.dMax, hidden);
            }
            add(buf, interactionColor(r.type));
        }
        if (interfaceShowMask_ != kInterfaceShowAll) {
            add("");
            add("  :set interface_show all  to draw every type");
        }
    }

    activateOverlay("Interface Overlay", std::move(lines),
                    std::move(colors), "Interface Overlay");
}

} // namespace molterm
