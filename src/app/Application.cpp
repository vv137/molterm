#include "molterm/app/Application.h"

#include "molterm/app/CommandScope.h"
#include "molterm/app/PathPatterns.h"
#include "molterm/cmd/CommandParser.h"
#include "molterm/cmd/RegisterExpr.h"
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
#include "molterm/repr/ReprUtil.h"
#include "molterm/io/SessionExporter.h"
#include "molterm/config/ConfigParser.h"
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
static constexpr const char* kAllToken = "all";
static constexpr const char* kAllGlob  = "*";

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

inline constexpr std::string_view kAtLibPrefix = "@lib/";

// Resolve `@lib/<name>` (with or without trailing .mt) to a concrete
// file path for `:run` — issue #56. Lookup chain, first match wins:
//   1. $MOLTERM_LIB_DIR/<name>.mt        (per-invocation override)
//   2. ~/.molterm/lib/<name>.mt          (per-user library)
//   3. <exe>/../share/molterm/lib/<name>.mt   (install layout)
//   4. <exe>/../lib/<name>.mt           (build-tree layout — running from build/)
//   5. <source>/lib/<name>.mt           (dev fallback via MOLTERM_SOURCE_DIR)
// Returns empty string when no candidate exists, so the caller can
// surface a "Cannot resolve @lib/foo" message with the chain it tried.
static std::string resolveAtLibPath(const std::string& spec) {
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

static int applyHeteroatomColors(MolObject& obj) {
    const auto& atoms = obj.atoms();
    int count = 0;
    for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
        if (atoms[i].element != "C") {
            obj.setAtomColor(i, ColorMapper::colorForElement(atoms[i].element));
            ++count;
        }
    }
    return count;
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
static std::optional<bool> parseBool(const std::string& v) {
    if (v == "on" || v == "1" || v == "true" || v == "yes")  return true;
    if (v == "off" || v == "0" || v == "false" || v == "no") return false;
    return std::nullopt;
}

// Global resize flag
static volatile sig_atomic_t g_resized = 0;
static void resizeHandler(int) { g_resized = 1; }

// Clear pixel graphics artifacts and force full ncurses repaint
static void clearScreenAndRepaint() {
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
// In-place trim of leading + trailing ASCII whitespace. Duplicated across
// half a dozen sites in this TU before this consolidation.
void trimWhitespace(std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) { s.clear(); return; }
    size_t b = s.find_last_not_of(" \t\r");
    s = s.substr(a, b - a + 1);
}

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

bool Application::dispatchScriptLines(const std::vector<std::string>& lines,
                                       size_t lo, size_t hi,
                                       ScriptRunResult& result, bool strict) {
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
            RegisterExpr::Context ctx;
            ctx.regs = &registers();
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
                if (strict) { result.stopped = true; return false; }
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
                if (strict) { result.stopped = true; return false; }
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
                if (strict) { result.stopped = true; return false; }
                i = endIdx;
                continue;
            }
            std::string var = expanded.substr(0, inPos);
            std::string rangeExpr = expanded.substr(inPos + 4);
            trimWhitespace(var);
            if (var.empty()) { i = endIdx; continue; }
            // parse `LO..HI`
            auto dotPos = rangeExpr.find("..");
            if (dotPos == std::string::npos) {
                recordFailure(i, srcLine, "bad :foreach range (expected LO..HI): " + rangeExpr);
                if (strict) { result.stopped = true; return false; }
                i = endIdx;
                continue;
            }
            int loVal = 0, hiVal = 0;
            try {
                loVal = std::stoi(rangeExpr.substr(0, dotPos));
                hiVal = std::stoi(rangeExpr.substr(dotPos + 2));
            } catch (const std::exception&) {
                recordFailure(i, srcLine, "bad :foreach numeric range: " + rangeExpr);
                if (strict) { result.stopped = true; return false; }
                i = endIdx;
                continue;
            }
            for (int v = loVal; v <= hiVal; ++v) {
                Register r; r.kind = Register::Kind::Scalar; r.scalar = v;
                registers()[var] = r;
                if (!dispatchScriptLines(lines, i + 1, endIdx, result, strict)) {
                    return false;
                }
            }
            i = endIdx;       // past the :end
            continue;
        }
        if (isControlKeyword(srcLine, "end", body)) {
            // Stray :end (foreach consumed its own); ignore.
            continue;
        }

        // Non-control line. Skip if any enclosing :if branch is inactive.
        if (anyInactive()) continue;
        if (!runLine(srcLine, i)) return false;
    }
    return true;
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

// `[A-Za-z_][A-Za-z0-9_]*` — accepted in both ${NAME} expansion and :setenv.
bool isValidEnvName(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
        return false;
    for (size_t k = 1; k < s.size(); ++k) {
        char c = s[k];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            return false;
    }
    return true;
}

// Join args[lo..hi) with single-space separators.
std::string joinArgs(const std::vector<std::string>& args, size_t lo, size_t hi) {
    std::string out;
    for (size_t i = lo; i < hi && i < args.size(); ++i) {
        if (i > lo) out += ' ';
        out += args[i];
    }
    return out;
}

// Split args at literal "=" token. Returns (left, right_joined). When no "="
// is present, right is empty and left is the original args vector. Used by
// :label and the :measure/:angle/:dihedral caption parsers.
std::pair<std::vector<std::string>, std::string>
splitAtToken(const std::vector<std::string>& args, std::string_view token) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == token) {
            return {std::vector<std::string>(args.begin(), args.begin() + i),
                    joinArgs(args, i + 1, args.size())};
        }
    }
    return {args, ""};
}

// Back-compat wrapper for the original "=" caller. New callers should
// invoke splitAtToken directly with whichever keyword they want
// (`as` for :copy / :extract, `vs` for :cmp, etc.).
std::pair<std::vector<std::string>, std::string>
splitAtEqToken(const std::vector<std::string>& args) {
    return splitAtToken(args, "=");
}

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

// Canonical option names enumerated by `:set` (no args) / `:set all` AND
// by tab-completion (which extends with kSetOptionsShort below). Long
// forms only — listing should not double-print under each alias.
// Adding a new knob: append here so the listing surfaces it; if the knob
// has a short alias, append that to kSetOptionsShort too.
inline constexpr const char* kSetOptionsLong[] = {
    "renderer",
    "bg",
    "fog",
    "outline",
    "outline_threshold",
    "outline_darken",
    "outline_mode",
    "outline_color",
    "label_color",
    "annotation_color",
    "measurement_line_color",
    "arrow_color",
    "label_outline",
    "label_outline_color",
    "label_outline_thickness",
    "annotation_outline",
    "annotation_outline_color",
    "annotation_outline_thickness",
    "arrow_thickness",
    "arrow_head_size",
    "label_font_size",
    "annotation_font_size",
    "annotation_linewidth",
    "overlay_scale",
    "size_mode",
    "reference_canvas_height",
    "live_dpi",
    "label_format",
    "verbose",
    "backbone_thickness",
    "wireframe_thickness",
    "ball_radius",
    "pan_speed",
    "cartoon_helix",
    "cartoon_sheet",
    "cartoon_loop",
    "cartoon_subdiv",
    "cartoon_aspect",
    "cartoon_helix_radial",
    "cartoon_tubular_helix",
    "cartoon_tubular_radius",
    "nucleic_backbone",
    "bs_units",
    "bs_factor",
    "spacefill_scale",
    "lod_medium",
    "lod_low",
    "backbone_cutoff",
    "auto_center",
    "panel",
    "seqbar",
    "seqwrap",
    "interface_color",
    "interface_thickness",
    "interface_classify",
    "interface_sidechains",
    "interface_show",
    "stereo",
    "stereo_angle",
    "scope",
    "focus_radius",
    "focus_extra",
    "focus_min_radius",
    "focus_dim",
    "focus_granularity",
};

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

// Parse a bg color spec into (r,g,b). Accepts "#RGB", "#RRGGBB", and
// "rgb(R,G,B)" — the parser is forgiving on whitespace and case so that
// `:set bg "#202020"`, `:set bg "rgb(32,32,32)"`, and PyMOL-style
// `bg "#FFF"` all round-trip. Returns std::nullopt on malformed input;
// callers fall back to the named-mode parser before erroring out.
std::optional<std::array<uint8_t, 3>> parseHexColor(std::string s) {
    auto trim = [](std::string& v) {
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(v.begin());
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back())))  v.pop_back();
    };
    trim(s);
    if (s.empty()) return std::nullopt;

    // rgb(R,G,B) form: optional whitespace inside, decimal channels 0..255.
    if (s.size() > 4 &&
        (s[0] == 'r' || s[0] == 'R') && (s[1] == 'g' || s[1] == 'G') &&
        (s[2] == 'b' || s[2] == 'B') && s[3] == '(' && s.back() == ')') {
        std::string body = s.substr(4, s.size() - 5);
        int r = -1, g = -1, b = -1;
        if (std::sscanf(body.c_str(), " %d , %d , %d ", &r, &g, &b) != 3) return std::nullopt;
        if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) return std::nullopt;
        return std::array<uint8_t, 3>{static_cast<uint8_t>(r),
                                      static_cast<uint8_t>(g),
                                      static_cast<uint8_t>(b)};
    }

    // #RGB / #RRGGBB form.
    if (s[0] != '#') return std::nullopt;
    std::string hex = s.substr(1);
    auto fromHex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    if (hex.size() == 3) {
        int r = fromHex(hex[0]), g = fromHex(hex[1]), b = fromHex(hex[2]);
        if (r < 0 || g < 0 || b < 0) return std::nullopt;
        return std::array<uint8_t, 3>{static_cast<uint8_t>(r * 17),
                                      static_cast<uint8_t>(g * 17),
                                      static_cast<uint8_t>(b * 17)};
    }
    if (hex.size() == 6) {
        int rh = fromHex(hex[0]), rl = fromHex(hex[1]);
        int gh = fromHex(hex[2]), gl = fromHex(hex[3]);
        int bh = fromHex(hex[4]), bl = fromHex(hex[5]);
        if (rh < 0 || rl < 0 || gh < 0 || gl < 0 || bh < 0 || bl < 0) return std::nullopt;
        return std::array<uint8_t, 3>{static_cast<uint8_t>((rh << 4) | rl),
                                      static_cast<uint8_t>((gh << 4) | gl),
                                      static_cast<uint8_t>((bh << 4) | bl)};
    }
    return std::nullopt;
}

// Resolve a color spec — named ("red", "white"), hex ("#RRGGBB", "#RGB"),
// or rgb(R,G,B) — to an RGB triple. Used by :set label_color /
// annotation_color / measurement_line_color / outline_color so all four
// share one accept-list and stay in sync.
std::optional<std::array<uint8_t, 3>> parseColorSpec(const std::string& s) {
    if (s.empty()) return std::nullopt;
    if (auto rgb = parseHexColor(s)) return rgb;
    int pair = ColorMapper::colorByName(s);
    if (pair > 0) {
        auto rgb = PixelCanvas::colorPairToRGB(pair);
        return std::array<uint8_t, 3>{rgb.r, rgb.g, rgb.b};
    }
    return std::nullopt;
}

}  // namespace

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

std::string Application::resolveLabel(int atomIdx) const {
    auto obj = tabMgr_.currentTab().currentObject();
    if (!obj) return {};
    const auto& atoms = obj->atoms();
    // labelAtoms_ may outlive an object swap; a stale idx is possible.
    if (atomIdx < 0 || atomIdx >= static_cast<int>(atoms.size())) return {};
    const auto& a = atoms[atomIdx];
    // Common case: no overrides and no template — skip the hash lookup.
    if (labelText_.empty() && labelFormat_.empty())
        return a.resName + std::to_string(a.resSeq);
    auto it = labelText_.find(atomIdx);
    if (it != labelText_.end()) return it->second;
    if (!labelFormat_.empty()) return expandLabelTemplate(labelFormat_, a);
    return a.resName + std::to_string(a.resSeq);
}

void Application::initRepresentations() {
    representations_[ReprType::Wireframe] = std::make_unique<WireframeRepr>();
    representations_[ReprType::BallStick] = std::make_unique<BallStickRepr>();
    representations_[ReprType::Backbone]  = std::make_unique<BackboneRepr>();
    representations_[ReprType::Spacefill] = std::make_unique<SpacefillRepr>();
    representations_[ReprType::Cartoon]   = std::make_unique<CartoonRepr>();
    representations_[ReprType::Ribbon]    = std::make_unique<RibbonRepr>();
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
        layout_.markDirty(C::Viewport);
        layout_.markDirty(C::StatusBar);
        return;
    }
#ifdef BUTTON5_PRESSED
    if (event.bstate & BUTTON5_PRESSED) {
        cam.zoomBy(1.0f / 1.15f);
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
        case Action::PanLeft:     cam.pan(-cam.panSpeed(), 0);  dirty({C::Viewport, C::StatusBar}); break;
        case Action::PanRight:    cam.pan(cam.panSpeed(), 0);   dirty({C::Viewport, C::StatusBar}); break;
        case Action::PanUp:       cam.pan(0, -cam.panSpeed());  dirty({C::Viewport, C::StatusBar}); break;
        case Action::PanDown:     cam.pan(0, cam.panSpeed());   dirty({C::Viewport, C::StatusBar}); break;
        case Action::ZoomIn:      cam.zoomBy(1.2f);  dirty({C::Viewport, C::StatusBar}); break;
        case Action::ZoomOut:     cam.zoomBy(1.0f/1.2f); dirty({C::Viewport, C::StatusBar}); break;
        case Action::ResetView:   cam.reset(); tab.centerView(); dirty({C::Viewport, C::StatusBar}); break;
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
            input = expandScriptVars(input);
            ExecResult result = cmdRegistry_.execute(*this, input);
            if (!result.msg.empty()) cmdLine_.setMessage(result.msg);
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
                                           "backbone", "trace", "all"}) {
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

    // Draw labels on viewport
    {
        buildProjCache();
        int scaleX = canvas_ ? canvas_->scaleX() : 1;
        int scaleY = canvas_ ? canvas_->scaleY() : 1;
        auto obj = tabMgr_.currentTab().currentObject();
        if (obj && !labelAtoms_.empty()) {
            // Build fast lookup set from label list
            std::set<int> labelSet(labelAtoms_.begin(), labelAtoms_.end());
            for (const auto& pa : projCache_) {
                if (!labelSet.count(pa.idx)) continue;

                int tx = pa.sx / scaleX;
                int ty = pa.sy / scaleY;
                if (tx < 0 || tx >= w - 6 || ty < 0 || ty >= h) continue;

                std::string lbl = resolveLabel(pa.idx);
                if (isPixel) {
                    auto* pc = dynamic_cast<PixelCanvas*>(canvas_.get());
                    if (pc) paintLabelText(*pc, pa.sx + scaleX, pa.sy, pa.depth, lbl);
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

    // Draw measurement dashed lines + labels
    {
        auto obj = tabMgr_.currentTab().currentObject();
        if (obj && obj->visible() && !measurements_.empty()) {
            const auto& atoms = obj->atoms();
            auto& cam = tabMgr_.currentTab().camera();
            for (const auto& m : measurements_) {
                if (m.atoms.size() < 2) continue;
                for (size_t mi = 0; mi + 1 < m.atoms.size(); ++mi) {
                    int a1 = m.atoms[mi], a2 = m.atoms[mi + 1];
                    if (a1 < 0 || a1 >= static_cast<int>(atoms.size())) continue;
                    if (a2 < 0 || a2 >= static_cast<int>(atoms.size())) continue;
                    float sx1, sy1, d1, sx2, sy2, d2;
                    cam.projectCached(atoms[a1].x, atoms[a1].y, atoms[a1].z, sx1, sy1, d1);
                    cam.projectCached(atoms[a2].x, atoms[a2].y, atoms[a2].z, sx2, sy2, d2);
                    drawDashedLine(sx1, sy1, d1, sx2, sy2, d2, kColorYellow);
                }
                // Label at midpoint of first segment
                {
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
    uint8_t r = 255, g = 255, b = 50;          // default yellow
    if (arrowColor_) { r = (*arrowColor_)[0]; g = (*arrowColor_)[1]; b = (*arrowColor_)[2]; }

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

    auto obj = tab.currentObject();
    if (!obj || !obj->visible()) return;
    auto& cam = tab.camera();
    const auto& atoms = obj->atoms();
    int scaleX = pc.scaleX();

    // Residue labels — text rendered in white next to each labeled atom.
    // Text comes from resolveLabel(): per-atom override > label_format > default.
    if (!labelAtoms_.empty()) {
        std::set<int> labelSet(labelAtoms_.begin(), labelAtoms_.end());
        for (int idx : labelSet) {
            if (idx < 0 || idx >= static_cast<int>(atoms.size())) continue;
            const auto& a = atoms[idx];
            float fsx, fsy, depth;
            cam.projectCached(a.x, a.y, a.z, fsx, fsy, depth);
            int isx = static_cast<int>(fsx);
            int isy = static_cast<int>(fsy);
            if (isx < 0 || isx >= subW || isy < 0 || isy >= subH) continue;
            std::string lbl = resolveLabel(idx);
            paintLabelText(pc, isx + scaleX, isy, depth, lbl);
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

        for (const auto& m : measurements_) {
            if (m.atoms.size() < 2) continue;
            for (size_t mi = 0; mi + 1 < m.atoms.size(); ++mi) {
                int a1 = m.atoms[mi], a2 = m.atoms[mi + 1];
                if (a1 < 0 || a1 >= static_cast<int>(atoms.size())) continue;
                if (a2 < 0 || a2 >= static_cast<int>(atoms.size())) continue;
                float sx1, sy1, d1, sx2, sy2, d2;
                cam.projectCached(atoms[a1].x, atoms[a1].y, atoms[a1].z, sx1, sy1, d1);
                cam.projectCached(atoms[a2].x, atoms[a2].y, atoms[a2].z, sx2, sy2, d2);
                drawDash(sx1, sy1, d1, sx2, sy2, d2, kColorYellow,
                         effectiveAnnotationLineWidth());
            }
            int a1 = m.atoms[0], a2 = m.atoms[1];
            if (a1 < 0 || a1 >= static_cast<int>(atoms.size())) continue;
            if (a2 < 0 || a2 >= static_cast<int>(atoms.size())) continue;
            float sx1, sy1, d1, sx2, sy2, d2;
            cam.projectCached(atoms[a1].x, atoms[a1].y, atoms[a1].z, sx1, sy1, d1);
            cam.projectCached(atoms[a2].x, atoms[a2].y, atoms[a2].z, sx2, sy2, d2);
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
    for (int idx : subjectIndices) {
        if (idx < 0 || idx >= (int)atoms.size()) continue;
        sx += atoms[idx].x; sy += atoms[idx].y; sz += atoms[idx].z;
        ++n;
    }
    if (n == 0) { focusSnapshot_.active = false; return; }
    sx /= n; sy /= n; sz /= n;

    float r2max = 0.0f;
    for (int idx : subjectIndices) {
        if (idx < 0 || idx >= (int)atoms.size()) continue;
        float dx = atoms[idx].x - sx;
        float dy = atoms[idx].y - sy;
        float dz = atoms[idx].z - sz;
        float r2 = dx*dx + dy*dy + dz*dz;
        if (r2 > r2max) r2max = r2;
    }
    const float rEnc = std::sqrt(r2max);
    const float rPad = std::max(rEnc + focusExtraRadius_, focusMinRadius_);
    // K=20 calibrates fillFraction=1.0 to the existing `:zoom` formula
    // (40 / span ≈ 20 / R) so :zoom and a "full-fill" focus agree.
    const float targetZoom = (rPad > 0.0f)
        ? focusFillFraction_ * 20.0f / rPad
        : focusZoom_;

    cam.focusOn(sx, sy, sz, targetZoom);

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
        wf->setThickness(std::max(0.5f, focusSnapshot_.wireframeThickness * 1.4f));
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
        "Focus: %d atoms (radius=%.1fA zoom=%.1f) — F or Esc to exit",
        (int)nbhdIndices.size(), focusRadius_, focusZoom_);
    cmdLine_.setMessage(msg);
    needsRedraw_ = true;
}

void Application::exitFocus() {
    if (!focusSnapshot_.active) return;
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
    // :q / :quit
    cmdRegistry_.registerCmd("q", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        app.quit(cmd.forced);
        return {true, ""};
    }, ":q[!]", "Quit MolTerm (use :q! to skip auto-save)",
       {":q", ":q!"}, "Help");
    cmdRegistry_.registerCmd("quit", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        app.quit(cmd.forced);
        return {true, ""};
    }, ":quit[!]", "Quit MolTerm (alias for :q)",
       {":quit"}, "Help");
    cmdRegistry_.registerCmd("qa", [](Application& app, const ParsedCommand&) -> ExecResult {
        app.quit(true);
        return {true, ""};
    }, ":qa", "Quit all tabs and exit",
       {":qa"}, "Help");

    // :load <file>
    auto loadPatterns = [](Application& app, const ParsedCommand& cmd,
                           const char* usage) -> ExecResult {
        if (cmd.args.empty()) return {false, usage};
        // Fast path for the common single-literal case: skip globbing
        // entirely so a path with no metacharacters and no brace range
        // never has to round-trip through glob() / readdir().
        const bool singleLiteral =
            cmd.args.size() == 1 &&
            cmd.args[0].find_first_of("*?[{") == std::string::npos;
        std::vector<std::string> matches =
            singleLiteral ? std::vector<std::string>{cmd.args[0]}
                          : expandPathPatterns(cmd.args);
        if (matches.empty()) {
            std::string joined;
            for (size_t i = 0; i < cmd.args.size(); ++i) {
                if (i) joined += ' ';
                joined += cmd.args[i];
            }
            return {false, "No files matched pattern(s): " + joined};
        }
        if (matches.size() == 1) {
            std::string msg = app.loadFile(matches[0]);
            bool ok = msg.rfind("Loaded ", 0) == 0;
            return {ok, msg};
        }
        int loaded = 0;
        std::string firstError;
        for (const auto& p : matches) {
            std::string msg = app.loadFile(p);
            if (msg.rfind("Loaded ", 0) == 0) ++loaded;
            else if (firstError.empty()) firstError = msg;
        }
        if (loaded == 0)
            return {false, "All loads failed; first error: " + firstError};
        std::string summary = "Loaded " + std::to_string(loaded) +
                              " structure(s)";
        const int failed = static_cast<int>(matches.size()) - loaded;
        if (failed > 0) summary += " (" + std::to_string(failed) + " failed)";
        return {true, summary};
    };

    cmdRegistry_.registerCmd("load",
        [loadPatterns](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return loadPatterns(app, cmd, "Usage: :load <pattern>...");
        },
        ":load <pattern>...",
        "Load structure file(s); supports shell globs and brace ranges",
        {":load protein.pdb", ":load 1bna.cif",
         ":load *.pdb", ":load model_{1..5}.cif",
         ":load relaxed_*.pdb confident_*.cif"},
        "Files");
    cmdRegistry_.registerCmd("e",
        [loadPatterns](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return loadPatterns(app, cmd, "Usage: :e <pattern>...");
        },
        ":e <pattern>...",
        "Load structure file(s) (alias for :load)",
        {":e protein.pdb", ":e *.cif"},
        "Files");

    // :tabnew
    cmdRegistry_.registerCmd("tabnew", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        std::string name = cmd.args.empty() ? "" : cmd.args[0];
        app.tabs().addTab(name);
        app.tabs().goToTab(static_cast<int>(app.tabs().count()) - 1);
        return {true, "New tab created"};
    }, ":tabnew [name]", "Create a new tab (optionally named)",
       {":tabnew", ":tabnew analysis"}, "Window");

    // :tabclose
    cmdRegistry_.registerCmd("tabclose", [](Application& app, const ParsedCommand&) -> ExecResult {
        if (app.tabs().count() <= 1) return {false, "Cannot close last tab"};
        app.tabs().closeCurrentTab();
        return {true, ""};
    }, ":tabclose", "Close the current tab",
       {":tabclose"}, "Window");

    // :objects
    cmdRegistry_.registerCmd("objects", [](Application& app, const ParsedCommand&) -> ExecResult {
        auto names = app.store().names();
        if (names.empty()) return {true, "No objects loaded"};
        std::string result = "Objects:";
        for (const auto& n : names) result += " " + n;
        return {true, result};
    }, ":objects", "List objects loaded in the current tab",
       {":objects"}, "Window");

    // :object — make a specific object the "current" one (drives the info
    // panel, structure-mutating commands, and per-tab seq bar). It does
    // NOT gate :color/:show/:hide; those follow :set scope.
    cmdRegistry_.registerCmd("object", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto& tab = app.tabs().currentTab();
        const auto& objs = tab.objects();
        if (cmd.args.empty()) {
            auto cur = tab.currentObject();
            if (!cur) return {true, "No current object (tab is empty)"};
            return {true, "Current object: " + cur->name() + " [" +
                          std::to_string(tab.selectedObjectIdx() + 1) + "/" +
                          std::to_string(objs.size()) + "]"};
        }
        if (objs.empty()) return {false, "No objects loaded"};

        const auto& a = cmd.args[0];
        if (a == "next") {
            tab.selectNextObject();
        } else if (a == "prev" || a == "previous") {
            tab.selectPrevObject();
        } else {
            // Try as 1-based index first (matches the listing in :objects),
            // fall back to name lookup.
            int newIdx = -1;
            try {
                size_t parsed = 0;
                int n = std::stoi(a, &parsed);
                if (parsed == a.size() && n >= 1 &&
                    n <= static_cast<int>(objs.size())) {
                    newIdx = n - 1;
                }
            } catch (...) {}
            if (newIdx < 0) {
                for (size_t i = 0; i < objs.size(); ++i) {
                    if (objs[i] && objs[i]->name() == a) {
                        newIdx = static_cast<int>(i);
                        break;
                    }
                }
            }
            if (newIdx < 0) return {false, "No such object: " + a};
            tab.selectObject(newIdx);
        }

        // Per-object overlays (interface mask + contacts) reference the
        // prior object's atom indices; refresh against the new mol so the
        // overlay matches what's actually being rendered.
        app.onCurrentObjectChanged();
        auto cur = tab.currentObject();
        // Refresh the panel + seqbar so the UI follows the switch.
        app.layout().markDirty(Layout::Component::ObjectPanel);
        app.layout().markDirty(Layout::Component::SeqBar);
        if (!cur) return {true, "No current object"};
        return {true, "Current object: " + cur->name() + " [" +
                      std::to_string(tab.selectedObjectIdx() + 1) + "/" +
                      std::to_string(objs.size()) + "]"};
    }, ":object [name|index|next|prev]",
       "Show or set the current object in the active tab",
       {":object", ":object 1ubq", ":object 2", ":object next"},
       "Window");

    // Helper: resolve repr name to ReprType. Returns false if unknown.
    auto resolveRepr = [](const std::string& name, ReprType& out) -> bool {
        if (name == "wireframe" || name == "wire" || name == "lines") { out = ReprType::Wireframe; return true; }
        if (name == "ballstick" || name == "sticks" || name == "bs")  { out = ReprType::BallStick; return true; }
        if (name == "spacefill" || name == "spheres" || name == "cpk") { out = ReprType::Spacefill; return true; }
        if (name == "cartoon" || name == "tube")     { out = ReprType::Cartoon; return true; }
        if (name == "ribbon")                        { out = ReprType::Ribbon; return true; }
        if (name == "backbone" || name == "trace" || name == "ca") { out = ReprType::Backbone; return true; }
        return false;
    };

    // :show <repr> [selection]
    cmdRegistry_.registerCmd("show", [resolveRepr](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
    cmdRegistry_.registerCmd("hide", [resolveRepr](Application& app, const ParsedCommand& cmd) -> ExecResult {
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

    cmdRegistry_.registerCmd("enable",
        [setObjectVisibility](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return setObjectVisibility(app, cmd, true);
        },
        ":enable [name|index|all]",
        "Make object visible (default: current object)",
        {":enable", ":enable 1ubq", ":enable 2", ":enable all"}, "Display");

    cmdRegistry_.registerCmd("disable",
        [setObjectVisibility](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return setObjectVisibility(app, cmd, false);
        },
        ":disable [name|index|all]",
        "Hide object entirely (default: current object)",
        {":disable", ":disable 1ubq", ":disable 2", ":disable all"}, "Display");

    // :color <scheme>
    cmdRegistry_.registerCmd("color", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty())
            return {false, "Usage: :color <scheme> or :color <name> <selection> | Colors: " + ColorMapper::availableColors()};

        const auto& first = cmd.args[0];

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
            int totalCount = 0;
            int objs = forEachInScope(app, "", [&](ScopedTarget& t) {
                totalCount += applyHeteroatomColors(*t.obj);
                return true;
            });
            if (objs == 0) return {false, "No object selected"};
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

        std::string expr;
        for (size_t i = 1; i < cmd.args.size(); ++i) {
            if (i > 1) expr += " ";
            expr += cmd.args[i];
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
       "Set coloring scheme (element, chain, ss, bfactor, plddt, rainbow, restype, heteroatom) or named color",
       {":color ss", ":color chain", ":color red chain A", ":color rainbow"}, "Coloring");

    // :zoom / :center / :orient — atoms gathered across every in-scope
    // object, so a multi-object scope frames the *union* (the natural
    // behavior after a :loadalign superpose). MolObject coordinates are
    // already world-aligned (Aligner mutates atoms in place), so summing
    // x/y/z directly across objects is correct.
    //
    // Broadcast mode (scope=all) skips :disable'd objects: the user has
    // signaled the object isn't on screen, and bounding the camera to its
    // atoms collapses the visible structure off-canvas (issue #23). Scope
    // = current still honors the active object even when it's disabled —
    // the user explicitly targeted it.
    struct ScopeAtomXYZ {
        std::vector<float> xs, ys, zs;
        int objs = 0;
    };
    auto collectAtomCoords = [](Application& app, const ParsedCommand& cmd,
                                int startArg = 0) -> ScopeAtomXYZ {
        std::string expr;
        for (size_t i = startArg; i < cmd.args.size(); ++i) {
            if (i > static_cast<size_t>(startArg)) expr += " ";
            expr += cmd.args[i];
        }
        ScopeAtomXYZ out;
        bool broadcast = app.effectiveCommandScope() == ScopeMode::All;
        forEachInScope(app, expr, [&](ScopedTarget& t) {
            if (broadcast && !t.obj->visible()) return true;
            const auto& atoms = t.obj->atoms();
            for (int i : t.sel.indices()) {
                out.xs.push_back(atoms[i].x);
                out.ys.push_back(atoms[i].y);
                out.zs.push_back(atoms[i].z);
            }
            ++out.objs;
            return true;
        });
        return out;
    };

    auto computeUnion = [](const ScopeAtomXYZ& g)
        -> std::tuple<float, float, float, float> {
        float cx = 0, cy = 0, cz = 0;
        float minX = std::numeric_limits<float>::max(), maxX = std::numeric_limits<float>::lowest();
        float minY = minX, maxY = maxX, minZ = minX, maxZ = maxX;
        const size_t n = g.xs.size();
        for (size_t i = 0; i < n; ++i) {
            cx += g.xs[i]; cy += g.ys[i]; cz += g.zs[i];
            if (g.xs[i] < minX) minX = g.xs[i];
            if (g.xs[i] > maxX) maxX = g.xs[i];
            if (g.ys[i] < minY) minY = g.ys[i];
            if (g.ys[i] > maxY) maxY = g.ys[i];
            if (g.zs[i] < minZ) minZ = g.zs[i];
            if (g.zs[i] > maxZ) maxZ = g.zs[i];
        }
        if (n > 0) {
            const float fn = static_cast<float>(n);
            cx /= fn; cy /= fn; cz /= fn;
        }
        float span = std::max({maxX - minX, maxY - minY, maxZ - minZ});
        return {cx, cy, cz, span};
    };

    // :center [selection]
    cmdRegistry_.registerCmd("center", [collectAtomCoords, computeUnion](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto g = collectAtomCoords(app, cmd);
        if (g.xs.empty()) return {false, "No atoms match"};
        auto [cx, cy, cz, span] = computeUnion(g);
        app.tabs().currentTab().camera().setCenter(cx, cy, cz);
        std::string msg = "Centered on " + std::to_string(g.xs.size()) + " atoms";
        if (g.objs > 1) msg += " (" + std::to_string(g.objs) + " objects)";
        app.logViewState(cmd, static_cast<int>(g.xs.size()));
        return {true, msg};
    }, ":center [selection]", "Center the view on a selection (or whole object)",
       {":center", ":center chain A", ":center resn HEM"}, "View");

    // :zoom [selection]
    cmdRegistry_.registerCmd("zoom", [collectAtomCoords, computeUnion](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto g = collectAtomCoords(app, cmd);
        if (g.xs.empty()) return {false, "No atoms match"};
        auto [cx, cy, cz, span] = computeUnion(g);
        app.tabs().currentTab().camera().setCenter(cx, cy, cz);
        if (span > 0.0f) app.tabs().currentTab().camera().setZoom(40.0f / span);
        std::string msg = "Zoomed to " + std::to_string(g.xs.size()) + " atoms";
        if (g.objs > 1) msg += " (" + std::to_string(g.objs) + " objects)";
        app.logViewState(cmd, static_cast<int>(g.xs.size()));
        return {true, msg};
    }, ":zoom [selection]", "Center and zoom to fit the selection (or whole object)",
       {":zoom", ":zoom chain A", ":zoom resi 50-80"}, "View");

    // :orient [view <vx>,<vy>,<vz>] [selection] — align PCA axes, optionally view from a
    // direction expressed in the PCA frame (e1=longest, e2=mid, e3=shortest).
    // Default v_pca = (0,0,1): look down the shortest axis (flat face on screen).
    cmdRegistry_.registerCmd("orient", [collectAtomCoords, computeUnion](Application& app, const ParsedCommand& cmd) -> ExecResult {
        double vx = 0.0, vy = 0.0, vz = 1.0;
        int selStart = 0;
        if (!cmd.args.empty() && cmd.args[0] == "view") {
            // Parser splits on whitespace AND commas, so "view 1,1,0" and "view 1 1 0"
            // both arrive as four separate args.
            if (cmd.args.size() < 4) {
                return {false, "Usage: :orient view <vx> <vy> <vz> [selection]"};
            }
            try {
                vx = std::stod(cmd.args[1]);
                vy = std::stod(cmd.args[2]);
                vz = std::stod(cmd.args[3]);
            } catch (...) {
                return {false, "Invalid view vector: " + cmd.args[1] + " " + cmd.args[2] + " " + cmd.args[3]};
            }
            double vlen = std::sqrt(vx*vx + vy*vy + vz*vz);
            if (vlen < 1e-10) return {false, "View vector cannot be zero"};
            vx /= vlen; vy /= vlen; vz /= vlen;
            selStart = 4;
        }

        auto g = collectAtomCoords(app, cmd, selStart);
        if (g.xs.empty()) return {false, "No atoms match"};
        auto [cx, cy, cz, span] = computeUnion(g);
        auto& cam = app.tabs().currentTab().camera();
        cam.setCenter(cx, cy, cz);
        if (span > 0.0f) cam.setZoom(40.0f / span);

        if (g.xs.size() < 2) {
            return {true, "Centered (need >=2 atoms for orientation)"};
        }

        // PCA over the union of atom positions across in-scope objects.
        // The same primitive is exposed to scripts via `:let G = pca(...)`
        // (issue #33); both code paths come through geom::pcaOf so the
        // PCA frame chosen by `:orient` always matches the one a script
        // sees.
        auto pca = geom::pcaOf(g.xs, g.ys, g.zs);
        if (!pca.valid) return {true, "Centered (need >=2 atoms for orientation)"};
        const auto& e1 = pca.axis1;
        const auto& e2 = pca.axis2;
        const auto& e3 = pca.axis3;

        // View direction in world space
        double sz[3] = {
            vx*e1[0] + vy*e2[0] + vz*e3[0],
            vx*e1[1] + vy*e2[1] + vz*e3[1],
            vx*e1[2] + vy*e2[2] + vz*e3[2],
        };

        // Up reference in PCA frame: prefer e2; if view is too close to e2, fall back to e1
        double up_pca[3] = {0, 1, 0};
        if (std::abs(vy) > 0.9) { up_pca[0] = 1; up_pca[1] = 0; up_pca[2] = 0; }
        double up[3] = {
            up_pca[0]*e1[0] + up_pca[1]*e2[0] + up_pca[2]*e3[0],
            up_pca[0]*e1[1] + up_pca[1]*e2[1] + up_pca[2]*e3[1],
            up_pca[0]*e1[2] + up_pca[1]*e2[2] + up_pca[2]*e3[2],
        };

        // Project up onto plane perpendicular to view → screen Y
        double dot_uv = up[0]*sz[0] + up[1]*sz[1] + up[2]*sz[2];
        double sy[3] = {up[0] - dot_uv*sz[0], up[1] - dot_uv*sz[1], up[2] - dot_uv*sz[2]};
        double sylen = std::sqrt(sy[0]*sy[0] + sy[1]*sy[1] + sy[2]*sy[2]);
        sy[0] /= sylen; sy[1] /= sylen; sy[2] /= sylen;

        // screen X = screen Y × screen Z (right-handed)
        double sx[3] = {
            sy[1]*sz[2] - sy[2]*sz[1],
            sy[2]*sz[0] - sy[0]*sz[2],
            sy[0]*sz[1] - sy[1]*sz[0],
        };

        std::array<float, 9> rot;
        rot[0] = (float)sx[0]; rot[1] = (float)sx[1]; rot[2] = (float)sx[2];
        rot[3] = (float)sy[0]; rot[4] = (float)sy[1]; rot[5] = (float)sy[2];
        rot[6] = (float)sz[0]; rot[7] = (float)sz[1]; rot[8] = (float)sz[2];
        cam.setRotation(rot);

        std::string msg = "Oriented " + std::to_string(g.xs.size()) + " atoms";
        if (g.objs > 1) msg += " (" + std::to_string(g.objs) + " objects)";
        app.logViewState(cmd, static_cast<int>(g.xs.size()));
        return {true, msg};
    }, ":orient [view <vx> <vy> <vz>] [selection]",
       "Center, zoom, and align principal axes (default: view down shortest axis)",
       {":orient", ":orient chain A", ":orient view 1 0 0"}, "View");

    // :turn x|y|z <deg>  — incremental camera rotation around screen axes,
    // no PCA, no recompute. Mirrors PyMOL's `turn` and is the cheap path
    // for spinning animations: orient once, then turn N° per frame.
    cmdRegistry_.registerCmd("turn", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.size() < 2) {
            return {false, "Usage: :turn x|y|z <degrees>"};
        }
        const auto& axis = cmd.args[0];
        float deg;
        try {
            deg = std::stof(cmd.args[1]);
        } catch (...) {
            return {false, "Invalid angle: " + cmd.args[1]};
        }
        auto& cam = app.tabs().currentTab().camera();
        if      (axis == "x" || axis == "X") cam.rotateX(deg);
        else if (axis == "y" || axis == "Y") cam.rotateY(deg);
        else if (axis == "z" || axis == "Z") cam.rotateZ(deg);
        else return {false, "Axis must be x, y, or z (got '" + axis + "')"};
        app.logViewState(cmd);
        return {true, "Turned " + axis + " by " + std::to_string(deg) + " deg"};
    }, ":turn x|y|z <deg>", "Rotate camera around a screen axis (no PCA recompute)",
       {":turn y 90", ":turn x -45"}, "View");

    // :set <option> [value]
    cmdRegistry_.registerCmd("set", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty() || cmd.args[0] == "all") {
            // Vim-style `:set` / `:set all` — print every queryable option's
            // current value, one line each. Each option's formatting stays
            // in the `:get` handler; we look it up once and reuse its handler
            // directly to skip per-option string tokenization.
            const auto* getCmd = app.cmdRegistry().lookup("get");
            if (!getCmd) return {false, "internal: :get command missing"};
            std::string out;
            ParsedCommand pc;
            pc.name = "get";
            pc.args.resize(1);
            for (const char* o : kSetOptionsLong) {
                pc.args[0] = o;
                auto r = getCmd->handler(app, pc);
                if (r.ok) {
                    if (!out.empty()) out += '\n';
                    out += r.msg;
                }
            }
            if (out.empty()) return {true, "(no options)"};
            return {true, out};
        }
        const auto& opt = cmd.args[0];
        if (opt == "panel") {
            if (cmd.args.size() < 2) return {false, "Usage: :set panel on|off"};
            auto v = parseBool(cmd.args[1]);
            if (!v) return {false, "Usage: :set panel on|off"};
            app.layout().setPanel(*v);
            app.tabs().currentTab().viewState().panelVisible = app.layout().panelVisible();
            return {true, app.layout().panelVisible() ? "Panel visible" : "Panel hidden"};
        }
        if (opt == "scope") {
            if (cmd.args.size() < 2) return {false, "Usage: :set scope all|current"};
            const auto& val = cmd.args[1];
            if (val == kAllToken)      app.setCommandScope(ScopeMode::All);
            else if (val == "current") app.setCommandScope(ScopeMode::Current);
            else return {false, "Usage: :set scope all|current"};
            return {true, "Command scope: " + val};
        }
        if (opt == "renderer" || opt == "render") {
            if (cmd.args.size() < 2) return {false, "Usage: :set renderer <ascii|braille|block|sixel|pixel|auto|kitty|iterm2>"};
            const auto& val = cmd.args[1];
            if (val == "ascii")        app.setRenderer(RendererType::Ascii);
            else if (val == "braille") app.setRenderer(RendererType::Braille);
            else if (val == "block")   app.setRenderer(RendererType::Block);
            else if (val == "pixel" || val == "auto") {
                app.setForcedProtocol(GraphicsProtocol::None);
                app.setRenderer(RendererType::Pixel);
            } else if (val == "sixel") {
                app.setForcedProtocol(GraphicsProtocol::Sixel);
                app.setRenderer(RendererType::Pixel);
            } else if (val == "kitty") {
                app.setForcedProtocol(GraphicsProtocol::Kitty);
                app.setRenderer(RendererType::Pixel);
            } else if (val == "iterm2") {
                app.setForcedProtocol(GraphicsProtocol::ITerm2);
                app.setRenderer(RendererType::Pixel);
            }
            else return {false, "Unknown renderer: " + val};
            clearScreenAndRepaint();
            return {true, "Renderer set to " + val};
        }
        if (opt == "backbone_thickness" || opt == "bt") {
            if (cmd.args.size() < 2) return {false, "Usage: :set backbone_thickness <0.5-10>"};
            float val = std::stof(cmd.args[1]);
            auto* bb = dynamic_cast<BackboneRepr*>(app.getRepr(ReprType::Backbone));
            if (bb) {
                bb->setThickness(val);
                return {true, "Backbone thickness set to " + std::to_string(bb->thickness())};
            }
            return {false, "Backbone repr not found"};
        }
        if (opt == "wireframe_thickness" || opt == "wt") {
            if (cmd.args.size() < 2) return {false, "Usage: :set wireframe_thickness <0.5-10>"};
            float val = std::stof(cmd.args[1]);
            auto* wf = dynamic_cast<WireframeRepr*>(app.getRepr(ReprType::Wireframe));
            if (wf) {
                wf->setThickness(val);
                return {true, "Wireframe thickness set to " + std::to_string(wf->thickness())};
            }
            return {false, "Wireframe repr not found"};
        }
        if (opt == "ball_radius" || opt == "br") {
            if (cmd.args.size() < 2) return {false, "Usage: :set ball_radius <1-10>"};
            int val = std::stoi(cmd.args[1]);
            auto* bs = dynamic_cast<BallStickRepr*>(app.getRepr(ReprType::BallStick));
            if (bs) {
                bs->setBallRadius(val);
                return {true, "Ball radius set to " + std::to_string(val)};
            }
            return {false, "BallStick repr not found"};
        }
        if (opt == "pan_speed" || opt == "ps") {
            if (cmd.args.size() < 2) return {false, "Usage: :set pan_speed <1-20>"};
            float val = std::stof(cmd.args[1]);
            app.tabs().currentTab().camera().setPanSpeed(val);
            return {true, "Pan speed set to " + std::to_string(val)};
        }
        if (opt == "fog") {
            if (cmd.args.size() < 2) return {false, "Usage: :set fog <0.0-1.0> (0=off)"};
            float val = std::stof(cmd.args[1]);
            app.setFogStrength(val);
            return {true, "Fog strength set to " + std::to_string(val)};
        }
        if (opt == "outline") {
            if (cmd.args.size() < 2) return {false, "Usage: :set outline on|off"};
            auto v = parseBool(cmd.args[1]);
            if (!v) return {false, "Usage: :set outline on|off"};
            app.setOutlineEnabled(*v);
            return {true, std::string("Outline: ") + (*v ? "on" : "off")};
        }
        if (opt == "screenshot_overlay") {
            if (cmd.args.size() < 2) return {false, "Usage: :set screenshot_overlay on|off"};
            auto v = parseBool(cmd.args[1]);
            if (!v) return {false, "Usage: :set screenshot_overlay on|off"};
            app.setScreenshotOverlay(*v);
            return {true, std::string("screenshot_overlay: ") + (*v ? "on" : "off")};
        }
        if (opt == "outline_threshold" || opt == "ot") {
            if (cmd.args.size() < 2) return {false, "Usage: :set outline_threshold <0.0-1.0>"};
            app.setOutlineThreshold(std::stof(cmd.args[1]));
            return {true, "Outline threshold set to " + cmd.args[1]};
        }
        if (opt == "outline_darken" || opt == "od") {
            if (cmd.args.size() < 2) return {false, "Usage: :set outline_darken <0.0-1.0>"};
            app.setOutlineDarken(std::stof(cmd.args[1]));
            return {true, "Outline darken set to " + cmd.args[1]};
        }
        if (opt == "label_font_size" || opt == "lfs") {
            if (cmd.args.size() < 2) return {false, "Usage: :set label_font_size <8..72>"};
            int px = std::stoi(cmd.args[1]);
            if (px < 8 || px > 72) return {false, "label_font_size out of range (8..72)"};
            app.setLabelFontSize(px);
            return {true, "label_font_size = " + std::to_string(px) + " px"};
        }
        if (opt == "annotation_font_size" || opt == "anf") {
            if (cmd.args.size() < 2) return {false, "Usage: :set annotation_font_size <8..72>"};
            int px = std::stoi(cmd.args[1]);
            if (px < 8 || px > 72) return {false, "annotation_font_size out of range (8..72)"};
            app.setAnnotationFontSize(px);
            return {true, "annotation_font_size = " + std::to_string(px) + " px"};
        }
        if (opt == "annotation_linewidth" || opt == "anlw") {
            if (cmd.args.size() < 2) return {false, "Usage: :set annotation_linewidth <1..8>"};
            int px = std::stoi(cmd.args[1]);
            if (px < 1 || px > 8) return {false, "annotation_linewidth out of range (1..8)"};
            app.setAnnotationLineWidth(px);
            return {true, "annotation_linewidth = " + std::to_string(px) + " px"};
        }
        if (opt == "overlay_scale" || opt == "scale") {
            if (cmd.args.size() < 2) return {false, "Usage: :set overlay_scale <0.5..4.0>"};
            float s = std::stof(cmd.args[1]);
            if (s < 0.5f || s > 4.0f) return {false, "overlay_scale out of range (0.5..4.0)"};
            app.setOverlayScale(s);
            return {true, "overlay_scale = " + cmd.args[1] + "x"};
        }
        if (opt == "size_mode" || opt == "sm") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set size_mode pixels|physical|relative"};
            const std::string& v = cmd.args[1];
            if      (v == "pixels"   || v == "px")  app.setOverlaySizeMode(Application::OverlaySizeMode::Pixels);
            else if (v == "physical" || v == "pt")  app.setOverlaySizeMode(Application::OverlaySizeMode::Physical);
            else if (v == "relative" || v == "rel") app.setOverlaySizeMode(Application::OverlaySizeMode::Relative);
            else return {false, "Usage: :set size_mode pixels|physical|relative"};
            return {true, "size_mode = " + v};
        }
        if (opt == "reference_canvas_height") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set reference_canvas_height <240..8192>"};
            int h = std::stoi(cmd.args[1]);
            if (h < 240 || h > 8192)
                return {false, "reference_canvas_height out of range (240..8192)"};
            app.setReferenceCanvasHeight(h);
            return {true, "reference_canvas_height = " + std::to_string(h) + " px"};
        }
        if (opt == "live_dpi") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set live_dpi <36..600>"};
            int d = std::stoi(cmd.args[1]);
            if (d < 36 || d > 600)
                return {false, "live_dpi out of range (36..600)"};
            app.setLiveDpi(d);
            return {true, "live_dpi = " + std::to_string(d)};
        }
        // ── Overlay color knobs (issues #30, #31) ──
        // Each takes a color spec (named, #hex, or rgb(R,G,B)). Setting
        // to "default" / "auto" / "" clears the override so the renderer
        // falls back to the legacy constant (white for labels, yellow
        // for annotation captions / measurement lines, depth darken for
        // outlines).
        auto applyColorOpt = [&](const char* name,
                                 void (Application::*setter)(std::optional<Application::ColorRGB>))
            -> std::optional<ExecResult> {
            if (opt != name) return std::nullopt;
            if (cmd.args.size() < 2)
                return ExecResult{false, std::string("Usage: :set ") + name + " <named|#RRGGBB|rgb(R,G,B)|default>"};
            // Rejoin in case rgb(R,G,B) was comma-split by the parser.
            std::string v;
            for (size_t i = 1; i < cmd.args.size(); ++i) {
                if (i > 1) v += ',';
                v += cmd.args[i];
            }
            if (v == "default" || v == "auto" || v == "off" || v == "clear") {
                (app.*setter)(std::nullopt);
                return ExecResult{true, std::string(name) + " reset to default"};
            }
            auto rgb = parseColorSpec(v);
            if (!rgb) return ExecResult{false, std::string("Bad color: ") + v};
            (app.*setter)(*rgb);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", (*rgb)[0], (*rgb)[1], (*rgb)[2]);
            return ExecResult{true, std::string(name) + " = " + buf};
        };
        if (auto r = applyColorOpt("label_color",                &Application::setLabelColor))             return *r;
        if (auto r = applyColorOpt("annotation_color",           &Application::setAnnotationColor))        return *r;
        if (auto r = applyColorOpt("measurement_line_color",     &Application::setMeasurementLineColor))   return *r;
        if (auto r = applyColorOpt("outline_color",              &Application::setOutlineColor))           return *r;
        if (auto r = applyColorOpt("arrow_color",                &Application::setArrowColor))             return *r;
        if (auto r = applyColorOpt("label_outline_color",        &Application::setLabelOutlineColor))      return *r;
        if (auto r = applyColorOpt("annotation_outline_color",   &Application::setAnnotationOutlineColor)) return *r;
        // ── Text outline / halo (issue #49) ──
        // on/off toggle + thickness for the contrasting halo around
        // label and annotation glyphs. Auto-color picks white-on-dark /
        // black-on-light against the body color when *outline_color is
        // unset, so the toggle alone is usually enough.
        if (opt == "label_outline") {
            if (cmd.args.size() < 2) return {false, "Usage: :set label_outline on|off"};
            auto on = parseBool(cmd.args[1]);
            if (!on) return {false, "Usage: :set label_outline on|off"};
            app.setLabelOutline(*on);
            return {true, std::string("label_outline = ") + (*on ? "on" : "off")};
        }
        if (opt == "label_outline_thickness") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set label_outline_thickness <1..6>"};
            int t = std::stoi(cmd.args[1]);
            if (t < 1 || t > 6)
                return {false, "label_outline_thickness out of range (1..6)"};
            app.setLabelOutlineThickness(t);
            return {true, "label_outline_thickness = " + std::to_string(t)};
        }
        if (opt == "annotation_outline") {
            if (cmd.args.size() < 2) return {false, "Usage: :set annotation_outline on|off"};
            auto on = parseBool(cmd.args[1]);
            if (!on) return {false, "Usage: :set annotation_outline on|off"};
            app.setAnnotationOutline(*on);
            return {true, std::string("annotation_outline = ") + (*on ? "on" : "off")};
        }
        if (opt == "annotation_outline_thickness") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set annotation_outline_thickness <1..6>"};
            int t = std::stoi(cmd.args[1]);
            if (t < 1 || t > 6)
                return {false, "annotation_outline_thickness out of range (1..6)"};
            app.setAnnotationOutlineThickness(t);
            return {true, "annotation_outline_thickness = " + std::to_string(t)};
        }
        if (opt == "arrow_thickness" || opt == "at") {
            if (cmd.args.size() < 2) return {false, "Usage: :set arrow_thickness <1..10>"};
            int t = std::stoi(cmd.args[1]);
            if (t < 1 || t > 10) return {false, "arrow_thickness out of range (1..10)"};
            app.setArrowThickness(t);
            return {true, "arrow_thickness = " + std::to_string(t)};
        }
        if (opt == "arrow_head_size" || opt == "ahs") {
            if (cmd.args.size() < 2) return {false, "Usage: :set arrow_head_size <2..32>"};
            int s = std::stoi(cmd.args[1]);
            if (s < 2 || s > 32) return {false, "arrow_head_size out of range (2..32)"};
            app.setArrowHeadSize(s);
            return {true, "arrow_head_size = " + std::to_string(s)};
        }
        if (opt == "outline_mode") {
            if (cmd.args.size() < 2) return {false, "Usage: :set outline_mode edge|silhouette|both"};
            const std::string& v = cmd.args[1];
            if      (v == "edge")       app.setOutlineMode(OutlineMode::Edge);
            else if (v == "silhouette") app.setOutlineMode(OutlineMode::Silhouette);
            else if (v == "both")       app.setOutlineMode(OutlineMode::Both);
            else return {false, "Usage: :set outline_mode edge|silhouette|both"};
            return {true, "outline_mode = " + v};
        }
        if (opt == "stereo") {
            if (cmd.args.size() < 2) return {false, "Usage: :set stereo off|walleye|crosseye|on"};
            const auto& val = cmd.args[1];
            if (val == "off" || val == "0")        app.setStereoMode(StereoMode::Off);
            else if (val == "walleye" || val == "parallel" ||
                     val == "on" || val == "sidebyside")
                                                   app.setStereoMode(StereoMode::Walleye);
            else if (val == "crosseye" || val == "cross")
                                                   app.setStereoMode(StereoMode::Crosseye);
            else return {false, "Usage: :set stereo off|walleye|crosseye"};
            return {true, "Stereo: " + val};
        }
        if (opt == "stereo_angle" || opt == "sa") {
            if (cmd.args.size() < 2) return {false, "Usage: :set stereo_angle <degrees>"};
            float deg = std::stof(cmd.args[1]);
            deg = std::max(0.5f, std::min(20.0f, deg));
            app.setStereoAngle(deg);
            return {true, "Stereo angle: " + std::to_string(deg) + " deg"};
        }
        if (opt == "focus_dim" || opt == "fd") {
            if (cmd.args.size() < 2) return {false, "Usage: :set focus_dim <0.0-1.0>"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.0f, std::min(1.0f, v));
            app.focusDimStrength_ = v;
            return {true, "Focus dim strength: " + std::to_string(v)};
        }
        if (opt == "focus_radius" || opt == "fr") {
            if (cmd.args.size() < 2) return {false, "Usage: :set focus_radius <Å>"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.5f, std::min(50.0f, v));
            app.focusRadius_ = v;
            return {true, "Focus radius: " + std::to_string(v) + " Å"};
        }
        if (opt == "focus_zoom" || opt == "fz") {
            if (cmd.args.size() < 2) return {false, "Usage: :set focus_zoom <float>"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.1f, std::min(50.0f, v));
            app.focusZoom_ = v;
            return {true, "Focus zoom (fallback): " + std::to_string(v) +
                          " (subject-size aware zoom is now used; tune via focus_fill)"};
        }
        if (opt == "focus_fill" || opt == "ff") {
            if (cmd.args.size() < 2) return {false, "Usage: :set focus_fill <0.05-1.0>"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.05f, std::min(1.0f, v));
            app.focusFillFraction_ = v;
            return {true, "Focus fill fraction: " + std::to_string(v)};
        }
        if (opt == "focus_extra" || opt == "fe") {
            if (cmd.args.size() < 2) return {false, "Usage: :set focus_extra <Å>"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.0f, std::min(50.0f, v));
            app.focusExtraRadius_ = v;
            return {true, "Focus extra radius: " + std::to_string(v) + " Å"};
        }
        if (opt == "focus_min_radius" || opt == "fmr") {
            if (cmd.args.size() < 2) return {false, "Usage: :set focus_min_radius <Å>"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.1f, std::min(50.0f, v));
            app.focusMinRadius_ = v;
            return {true, "Focus min radius: " + std::to_string(v) + " Å"};
        }
        if (opt == "focus_granularity" || opt == "fg") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set focus_granularity <residue|chain|sidechain>"};
            const std::string& g = cmd.args[1];
            if (g == "residue" || g == "res") {
                app.focusGranularity_ = FocusGranularity::Residue;
            } else if (g == "chain" || g == "c") {
                app.focusGranularity_ = FocusGranularity::Chain;
            } else if (g == "sidechain" || g == "sc") {
                app.focusGranularity_ = FocusGranularity::Sidechain;
            } else {
                return {false, "Unknown granularity: " + g + " (use residue|chain|sidechain)"};
            }
            return {true, "Focus granularity: " + g};
        }
        if (opt == "wf_thickness" || opt == "wft") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set wf_thickness <0.1-2.0>"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.1f, std::min(2.0f, v));
            auto* wf = dynamic_cast<WireframeRepr*>(
                app.getRepr(ReprType::Wireframe));
            if (!wf) return {false, "Wireframe repr not found"};
            wf->setThickness(v);
            return {true, "Wireframe thickness: " + std::to_string(v)};
        }
        if (opt == "interface_zoom" || opt == "iz") {
            if (cmd.args.size() < 2) {
                app.interfaceZoomGate_.setEnabled(false);
                return {true, "interface_zoom disabled"};
            }
            const std::string& v = cmd.args[1];
            if (v == "off" || v == "none") {
                app.interfaceZoomGate_.setEnabled(false);
                return {true, "interface_zoom disabled"};
            }
            float thresh = std::stof(v);
            app.interfaceZoomGate_.setThreshold(thresh);
            app.interfaceZoomGate_.setEnabled(true);
            return {true, "interface_zoom threshold: " + v};
        }
        if (opt == "interface_sidechains" || opt == "isc") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set interface_sidechains on|off"};
            auto vb = parseBool(cmd.args[1]);
            if (!vb) return {false, "Usage: :set interface_sidechains on|off"};
            app.interfaceSidechains_ = *vb;
            app.interfaceRepr_.setDrawSidechains(*vb);
            return {true, std::string("Interface sidechains: ") +
                          (*vb ? "on" : "off")};
        }
        if (opt == "interface_thickness" || opt == "ith") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set interface_thickness <1-6>"};
            int t = std::stoi(cmd.args[1]);
            t = std::max(1, std::min(6, t));
            app.interfaceThickness_ = t;
            app.interfaceRepr_.setInteractionThickness(t);
            app.interfaceRepr_.setLineThickness(std::max(1, t - 1));
            return {true, "Interface thickness: " + std::to_string(t)};
        }
        if (opt == "cartoon_helix" || opt == "ch") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_helix <0.1-3.0>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (ct) { ct->setHelixRadius(std::stof(cmd.args[1])); return {true, "Cartoon helix radius: " + cmd.args[1]}; }
            return {false, "Cartoon repr not found"};
        }
        if (opt == "cartoon_sheet" || opt == "csh") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_sheet <0.1-3.0>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (ct) { ct->setSheetRadius(std::stof(cmd.args[1])); return {true, "Cartoon sheet radius: " + cmd.args[1]}; }
            return {false, "Cartoon repr not found"};
        }
        if (opt == "cartoon_loop" || opt == "cl") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_loop <0.05-1.0>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (ct) { ct->setLoopRadius(std::stof(cmd.args[1])); return {true, "Cartoon loop radius: " + cmd.args[1]}; }
            return {false, "Cartoon repr not found"};
        }
        if (opt == "cartoon_subdiv" || opt == "csd") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_subdiv <2-16>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (ct) { ct->setSubdivisions(std::stoi(cmd.args[1])); return {true, "Cartoon subdivisions: " + cmd.args[1]}; }
            return {false, "Cartoon repr not found"};
        }
        if (opt == "cartoon_aspect" || opt == "csa") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_aspect <1.0-12.0>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            float v = std::stof(cmd.args[1]);
            v = std::max(1.0f, std::min(12.0f, v));
            ct->setHelixAspect(v);
            return {true, "Cartoon helix aspect (W:H): " + std::to_string(v)};
        }
        if (opt == "cartoon_helix_radial" || opt == "chr") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_helix_radial <4-64>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            int v = std::stoi(cmd.args[1]);
            ct->setHelixRadialSegments(v);
            return {true, "Cartoon helix radial vertices: " +
                          std::to_string(ct->helixRadialSegments())};
        }
        if (opt == "cartoon_tubular_helix" || opt == "cth") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_tubular_helix on|off"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            auto on = parseBool(cmd.args[1]);
            if (!on) return {false, "Use on|off|true|false"};
            ct->setTubularHelix(*on);
            return {true, std::string("Cartoon tubular helix: ") +
                          (*on ? "on (circular tube)" : "off (elliptical ribbon)")};
        }
        if (opt == "cartoon_tubular_radius" || opt == "ctr") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_tubular_radius <0.1-3.0>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.1f, std::min(3.0f, v));
            ct->setTubularRadius(v);
            return {true, "Cartoon tubular helix radius: " + std::to_string(v) + " Å"};
        }
        if (opt == "bs_units") {
            if (cmd.args.size() < 2) return {false, "Usage: :set bs_units vdw|cell"};
            auto* bs = dynamic_cast<BallStickRepr*>(app.getRepr(ReprType::BallStick));
            if (!bs) return {false, "BallStick repr not found"};
            const std::string& v = cmd.args[1];
            if (v == "vdw" || v == "a") {
                bs->setUseVdwSize(true);
                return {true, "BallStick units: vdw (Å × bs_factor)"};
            }
            if (v == "cell" || v == "subpx") {
                bs->setUseVdwSize(false);
                return {true, "BallStick units: cell (legacy ball_radius)"};
            }
            return {false, "Unknown bs_units: " + v + " (use vdw|cell)"};
        }
        if (opt == "bs_factor" || opt == "bsf") {
            if (cmd.args.size() < 2) return {false, "Usage: :set bs_factor <0.05-1.0>"};
            auto* bs = dynamic_cast<BallStickRepr*>(app.getRepr(ReprType::BallStick));
            if (!bs) return {false, "BallStick repr not found"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.05f, std::min(1.0f, v));
            bs->setSizeFactor(v);
            return {true, "BallStick size factor (×vdW): " + std::to_string(v)};
        }
        if (opt == "spacefill_scale" || opt == "ss_scale" || opt == "sfs") {
            if (cmd.args.size() < 2) return {false, "Usage: :set spacefill_scale <0.1-2.0>"};
            auto* sf = dynamic_cast<SpacefillRepr*>(app.getRepr(ReprType::Spacefill));
            if (!sf) return {false, "Spacefill repr not found"};
            float v = std::stof(cmd.args[1]);
            v = std::max(0.1f, std::min(2.0f, v));
            sf->setScale(v);
            return {true, "Spacefill scale (×vdW): " + std::to_string(v)};
        }
        if (opt == "nucleic_backbone" || opt == "nb") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set nucleic_backbone p|c4"};
            auto* ct = dynamic_cast<CartoonRepr*>(
                app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            const std::string& v = cmd.args[1];
            if (v == "p" || v == "P") {
                ct->setNucleicBackbone(CartoonRepr::NucleicBackbone::P);
                return {true, "Nucleic backbone trace: P (phosphate)"};
            }
            if (v == "c4" || v == "C4" || v == "c4'" || v == "C4'") {
                ct->setNucleicBackbone(CartoonRepr::NucleicBackbone::C4);
                return {true, "Nucleic backbone trace: C4'"};
            }
            return {false, "Unknown nucleic backbone: " + v + " (use p or c4)"};
        }
        if (opt == "lod_medium") {
            if (cmd.args.size() < 2) return {false, "Usage: :set lod_medium <atom_count>"};
            Representation::lodMediumThreshold = std::stoul(cmd.args[1]);
            return {true, "LOD medium threshold: " + cmd.args[1]};
        }
        if (opt == "lod_low") {
            if (cmd.args.size() < 2) return {false, "Usage: :set lod_low <atom_count>"};
            Representation::lodLowThreshold = std::stoul(cmd.args[1]);
            return {true, "LOD low threshold: " + cmd.args[1]};
        }
        if (opt == "backbone_cutoff") {
            if (cmd.args.size() < 2) return {false, "Usage: :set backbone_cutoff <atom_count>"};
            Representation::backboneCutoff = std::stoul(cmd.args[1]);
            return {true, "Backbone fallback cutoff: " + cmd.args[1]};
        }
        if (opt == "auto_center") {
            if (cmd.args.size() < 2) return {false, "Usage: :set auto_center on|off"};
            auto v = parseBool(cmd.args[1]);
            if (!v) return {false, "Usage: :set auto_center on|off"};
            app.setAutoCenter(*v);
            return {true, std::string("Auto-center on load: ") + (*v ? "on" : "off")};
        }
        if (opt == "seqbar") {
            if (cmd.args.size() < 2) return {false, "Usage: :set seqbar on|off"};
            auto v = parseBool(cmd.args[1]);
            if (!v) return {false, "Usage: :set seqbar on|off"};
            app.layout().setSeqBar(*v);
            auto& vs = app.tabs().currentTab().viewState();
            vs.seqBarVisible = app.layout().seqBarVisible();
            if (app.canvas()) app.canvas()->invalidate();
            app.onResize();
            return {true, app.layout().seqBarVisible() ? "Sequence bar visible" : "Sequence bar hidden"};
        }
        if (opt == "seqwrap") {
            if (cmd.args.size() < 2) return {false, "Usage: :set seqwrap on|off"};
            auto v = parseBool(cmd.args[1]);
            if (!v) return {false, "Usage: :set seqwrap on|off"};
            app.layout().setSeqBarWrap(*v);
            auto& vs = app.tabs().currentTab().viewState();
            vs.seqBarWrap = app.layout().seqBarWrap();
            if (app.canvas()) app.canvas()->invalidate();
            app.onResize();
            return {true, app.layout().seqBarWrap() ? "Sequence bar: wrap" : "Sequence bar: scroll"};
        }
        if (opt == "interface_color" || opt == "ic") {
            if (cmd.args.size() < 2) return {false, "Usage: :set interface_color <color_name>"};
            int c = ColorMapper::colorByName(cmd.args[1]);
            if (c < 0) return {false, "Unknown color: " + cmd.args[1] + " (" + ColorMapper::availableColors() + ")"};
            app.interfaceColor_ = c;
            return {true, "Interface color: " + cmd.args[1]};
        }
        if (opt == "interface_thickness" || opt == "it") {
            if (cmd.args.size() < 2) return {false, "Usage: :set interface_thickness <1-4>"};
            int val = std::stoi(cmd.args[1]);
            app.interfaceThickness_ = std::max(1, std::min(4, val));
            return {true, "Interface thickness: " + std::to_string(app.interfaceThickness_)};
        }
        if (opt == "interface_classify" || opt == "iclass") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set interface_classify on|off"};
            auto v = parseBool(cmd.args[1]);
            if (!v) return {false, "Usage: :set interface_classify on|off"};
            app.interfaceClassify_ = *v;
            return {true, *v ? "Interface classification: on (cyan H-bond, "
                               "red salt, yellow hydrophobic, gray other)"
                             : "Interface classification: off (single color)"};
        }
        if (opt == "interface_show" || opt == "is") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set interface_show all|specific|none|<list>"};
            // Reassemble: the parser splits on both spaces and commas,
            // so `set is salt,hbond` and `set is salt hbond` both arrive
            // as separate args. parseInterfaceShowSpec re-tokenizes by
            // comma, so we just rejoin with commas.
            std::string spec = cmd.args[1];
            for (size_t i = 2; i < cmd.args.size(); ++i) spec += "," + cmd.args[i];
            int parsed = parseInterfaceShowSpec(spec);
            if (parsed < 0)
                return {false, "Usage: :set interface_show all|specific|none|<list of hbond,salt,hydrophobic,other>"};
            app.interfaceShowMask_ = static_cast<std::uint8_t>(parsed);
            app.interfaceRepr_.setShowMask(app.interfaceShowMask_);
            return {true, "Interface show: " + formatInterfaceShowSpec(app.interfaceShowMask_)};
        }
        if (opt == "label_format" || opt == "lf") {
            if (cmd.args.size() < 2) {
                app.setLabelFormat("");
                return {true, "label_format cleared (default <resname><resseq>)"};
            }
            std::string fmt = joinArgs(cmd.args, 1, cmd.args.size());
            app.setLabelFormat(fmt);
            return {true, "label_format = " + fmt};
        }
        if (opt == "bg" || opt == "background_color") {
            // Accept: transparent | white | black | "#RRGGBB" | "#RGB" |
            //         "rgb(R,G,B)". Custom forms route through parseHexColor
            //         and set BgMode::Custom + the rgb triple.
            if (cmd.args.size() < 2)
                return {false, "Usage: :set bg transparent|white|black|#RRGGBB|rgb(R,G,B)"};
            // Args may have been split on commas/whitespace by the parser
            // (e.g. `rgb(32,32,32)` → `rgb(32` `32` `32)`); rejoin everything
            // after the option name into a single value string for parseHexColor.
            std::string v;
            for (size_t i = 1; i < cmd.args.size(); ++i) {
                if (i > 1) v += ',';   // commas were stripped during split
                v += cmd.args[i];
            }
            if      (v == "transparent" || v == "trans") app.setBgMode(BgMode::Transparent);
            else if (v == "white")                        app.setBgMode(BgMode::White);
            else if (v == "black")                        app.setBgMode(BgMode::Black);
            else if (auto rgb = parseHexColor(v))         app.setBgCustomRGB((*rgb)[0], (*rgb)[1], (*rgb)[2]);
            else return {false, "Bad bg value: " + v +
                                " (expected transparent|white|black|#RRGGBB|rgb(R,G,B))"};
            return {true, "bg = " + v};
        }
        if (opt == "verbose" || opt == "v") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set verbose on|off"};
            auto b = parseBool(cmd.args[1]);
            if (!b) return {false, "Usage: :set verbose on|off"};
            app.setVerbose(*b);
            return {true, std::string("verbose = ") + (*b ? "on" : "off")};
        }
        if (opt == "transparency" || opt == "transp") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :set transparency <0..1> [selection]"};
            float v;
            try { v = std::stof(cmd.args[1]); } catch (...) {
                return {false, "Invalid transparency: " + cmd.args[1]};
            }
            if (v < 0.0f || v > 1.0f)
                return {false, "Transparency out of range (0..1)"};
            // 1.0 - v: PyMOL-style "transparency" is 1 = fully invisible,
            // but PixelCanvas blending uses alpha (1 = opaque). Convert.
            float alpha = 1.0f - v;
            if (cmd.args.size() == 2) {
                // No selection — operate on current object's whole atom set,
                // matching the legacy "set transparency 0.5" shorthand.
                auto obj = app.tabs().currentTab().currentObject();
                if (!obj) return {false, "No object selected"};
                if (v <= 0.0f) { obj->clearAtomAlpha(); return {true, "transparency cleared"}; }
                obj->setAtomAlphaAll(alpha);
                return {true, "transparency = " + cmd.args[1] +
                              " (whole object, " + std::to_string(obj->atoms().size()) + " atoms)"};
            }
            // With a selection, use forEachInScope so obj-qualified forms
            // (issue #55, e.g. `reference/(chain D)`) hit the named object
            // instead of silently failing on the current object.
            std::string expr = joinArgs(cmd.args, 2, cmd.args.size());
            int totalAtoms = 0;
            int objs = forEachInScope(app, expr, [&](ScopedTarget& t) {
                t.obj->setAtomAlphas(t.sel.indices(), alpha);
                totalAtoms += static_cast<int>(t.sel.size());
                return true;
            });
            if (objs == 0) return {false, "No atoms match: " + expr};
            std::string msg = "transparency = " + cmd.args[1] +
                              " on " + std::to_string(totalAtoms) + " atoms (" + expr + ")";
            if (objs > 1) msg += " in " + std::to_string(objs) + " objects";
            return {true, msg};
        }
        return {false, "Unknown option: " + opt};
    }, ":set <option> [value]",
       "Set a runtime option (renderer, fog, outline, cartoon_*, focus_*, panel, seqbar, ...)",
       {":set renderer pixel", ":set outline on", ":set fog 0.5"}, "Session");

    // :get — query current value of a :set option (for scripting)
    cmdRegistry_.registerCmd("get", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :get <option>"};
        const auto& opt = cmd.args[0];
        auto onoff = [](bool b) { return std::string(b ? "on" : "off"); };

        if (opt == "panel")        return {true, "panel = " + onoff(app.layout().panelVisible())};
        if (opt == "scope")
            return {true, std::string("scope = ") +
                          (app.commandScope() == ScopeMode::All ? kAllToken : "current")};
        if (opt == "outline")      return {true, "outline = " + onoff(app.outlineEnabled())};
        if (opt == "screenshot_overlay")
            return {true, "screenshot_overlay = " + onoff(app.screenshotOverlay())};
        if (opt == "auto_center")  return {true, "auto_center = " + onoff(app.autoCenter())};
        if (opt == "seqbar")       return {true, "seqbar = " + onoff(app.layout().seqBarVisible())};
        if (opt == "seqwrap")      return {true, "seqwrap = " + onoff(app.layout().seqBarWrap())};
        if (opt == "interface_classify" || opt == "iclass")
            return {true, "interface_classify = " + onoff(app.interfaceClassify_)};
        if (opt == "interface_sidechains" || opt == "isc")
            return {true, "interface_sidechains = " + onoff(app.interfaceSidechains_)};
        if (opt == "interface_show" || opt == "is")
            return {true, "interface_show = " +
                          formatInterfaceShowSpec(app.interfaceShowMask_)};
        if (opt == "label_format" || opt == "lf")
            return {true, "label_format = " +
                          (app.labelFormat().empty() ? std::string("(default)")
                                                     : app.labelFormat())};
        if (opt == "bg" || opt == "background_color") {
            if (app.bgMode() == BgMode::Custom) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "#%02X%02X%02X",
                              app.bgCustomR(), app.bgCustomG(), app.bgCustomB());
                return {true, std::string("bg = ") + buf};
            }
            return {true, std::string("bg = ") + bgModeName(app.bgMode())};
        }
        if (opt == "verbose" || opt == "v")
            return {true, std::string("verbose = ") + (app.verbose() ? "on" : "off")};
        if (opt == "transparency" || opt == "transp") {
            auto obj = app.tabs().currentTab().currentObject();
            if (!obj || obj->atomAlpha().empty())
                return {true, "transparency = 0.0 (all atoms opaque)"};
            int n = static_cast<int>(obj->atomAlpha().size());
            int touched = 0;
            for (float a : obj->atomAlpha()) if (a < 0.999f) ++touched;
            return {true, "transparency: " + std::to_string(touched) +
                          " / " + std::to_string(n) + " atoms have <1.0 alpha"};
        }

        if (opt == "renderer" || opt == "render") {
            const char* n = "?";
            switch (app.rendererType()) {
                case RendererType::Ascii:   n = "ascii";   break;
                case RendererType::Braille: n = "braille"; break;
                case RendererType::Block:   n = "block";   break;
                case RendererType::Pixel:   n = "pixel";   break;
            }
            return {true, std::string("renderer = ") + n};
        }
        if (opt == "fog")              return {true, "fog = " + std::to_string(app.fogStrength())};
        if (opt == "outline_threshold" || opt == "ot")
            return {true, "outline_threshold = " + std::to_string(app.outlineThreshold())};
        if (opt == "outline_darken" || opt == "od")
            return {true, "outline_darken = " + std::to_string(app.outlineDarken())};
        if (opt == "label_font_size" || opt == "lfs")
            return {true, "label_font_size = " + std::to_string(app.labelFontSize()) + " px"};
        if (opt == "annotation_font_size" || opt == "anf")
            return {true, "annotation_font_size = " + std::to_string(app.annotationFontSize()) + " px"};
        if (opt == "annotation_linewidth" || opt == "anlw")
            return {true, "annotation_linewidth = " + std::to_string(app.annotationLineWidth()) + " px"};
        if (opt == "overlay_scale" || opt == "scale")
            return {true, "overlay_scale = " + std::to_string(app.overlayScale()) + "x"};
        if (opt == "size_mode" || opt == "sm") {
            const char* m = "pixels";
            if (app.overlaySizeMode() == Application::OverlaySizeMode::Physical) m = "physical";
            if (app.overlaySizeMode() == Application::OverlaySizeMode::Relative) m = "relative";
            return {true, std::string("size_mode = ") + m};
        }
        if (opt == "reference_canvas_height")
            return {true, "reference_canvas_height = " + std::to_string(app.referenceCanvasHeight()) + " px"};
        if (opt == "live_dpi")
            return {true, "live_dpi = " + std::to_string(app.liveDpi())};
        auto fmtColor = [](const std::optional<Application::ColorRGB>& c, const char* dflt) {
            if (!c) return std::string(dflt);
            char buf[16];
            std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", (*c)[0], (*c)[1], (*c)[2]);
            return std::string(buf);
        };
        if (opt == "label_color")
            return {true, "label_color = " + fmtColor(app.labelColor(), "default (white)")};
        if (opt == "annotation_color")
            return {true, "annotation_color = " + fmtColor(app.annotationColor(), "default (yellow)")};
        if (opt == "measurement_line_color")
            return {true, "measurement_line_color = " + fmtColor(app.measurementLineColor(), "default (yellow)")};
        if (opt == "outline_color")
            return {true, "outline_color = " + fmtColor(app.outlineColor(), "default (depth-darken)")};
        if (opt == "arrow_color")
            return {true, "arrow_color = " + fmtColor(app.arrowColor(), "default (yellow)")};
        if (opt == "label_outline")
            return {true, std::string("label_outline = ") + (app.labelOutline() ? "on" : "off")};
        if (opt == "label_outline_thickness")
            return {true, "label_outline_thickness = " + std::to_string(app.labelOutlineThickness())};
        if (opt == "label_outline_color")
            return {true, "label_outline_color = " + fmtColor(app.labelOutlineColor(), "auto (contrast)")};
        if (opt == "annotation_outline")
            return {true, std::string("annotation_outline = ") + (app.annotationOutline() ? "on" : "off")};
        if (opt == "annotation_outline_thickness")
            return {true, "annotation_outline_thickness = " + std::to_string(app.annotationOutlineThickness())};
        if (opt == "annotation_outline_color")
            return {true, "annotation_outline_color = " + fmtColor(app.annotationOutlineColor(), "auto (contrast)")};
        if (opt == "arrow_thickness" || opt == "at")
            return {true, "arrow_thickness = " + std::to_string(app.arrowThickness())};
        if (opt == "arrow_head_size" || opt == "ahs")
            return {true, "arrow_head_size = " + std::to_string(app.arrowHeadSize())};
        if (opt == "outline_mode") {
            const char* m = "edge";
            if (app.outlineMode() == OutlineMode::Silhouette) m = "silhouette";
            if (app.outlineMode() == OutlineMode::Both)       m = "both";
            return {true, std::string("outline_mode = ") + m};
        }
        if (opt == "stereo") {
            const char* m = "off";
            if (app.stereoMode() == StereoMode::Walleye)  m = "walleye";
            if (app.stereoMode() == StereoMode::Crosseye) m = "crosseye";
            return {true, std::string("stereo = ") + m};
        }
        if (opt == "stereo_angle" || opt == "sa")
            return {true, "stereo_angle = " + std::to_string(app.stereoAngle())};
        if (opt == "pan_speed" || opt == "ps")
            return {true, "pan_speed = " + std::to_string(app.tabs().currentTab().camera().panSpeed())};
        if (opt == "backbone_thickness" || opt == "bt") {
            auto* bb = dynamic_cast<BackboneRepr*>(app.getRepr(ReprType::Backbone));
            if (!bb) return {false, "Backbone repr not found"};
            return {true, "backbone_thickness = " + std::to_string(bb->thickness())};
        }
        if (opt == "wireframe_thickness" || opt == "wt") {
            auto* wf = dynamic_cast<WireframeRepr*>(app.getRepr(ReprType::Wireframe));
            if (!wf) return {false, "Wireframe repr not found"};
            return {true, "wireframe_thickness = " + std::to_string(wf->thickness())};
        }
        if (opt == "ball_radius" || opt == "br") {
            auto* bs = dynamic_cast<BallStickRepr*>(app.getRepr(ReprType::BallStick));
            if (!bs) return {false, "BallStick repr not found"};
            return {true, "ball_radius = " + std::to_string(bs->ballRadius())};
        }
        if (opt == "cartoon_helix" || opt == "ch") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_helix = " + std::to_string(ct->helixRadius())};
        }
        if (opt == "cartoon_sheet" || opt == "csh") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_sheet = " + std::to_string(ct->sheetRadius())};
        }
        if (opt == "cartoon_loop" || opt == "cl") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_loop = " + std::to_string(ct->loopRadius())};
        }
        if (opt == "cartoon_subdiv" || opt == "csd") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_subdiv = " + std::to_string(ct->subdivisions())};
        }
        if (opt == "cartoon_aspect" || opt == "csa") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_aspect = " + std::to_string(ct->helixAspect())};
        }
        if (opt == "cartoon_helix_radial" || opt == "chr") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_helix_radial = " +
                          std::to_string(ct->helixRadialSegments())};
        }
        if (opt == "cartoon_tubular_helix" || opt == "cth") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, std::string("cartoon_tubular_helix = ") +
                          (ct->tubularHelix() ? "on" : "off")};
        }
        if (opt == "cartoon_tubular_radius" || opt == "ctr") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_tubular_radius = " +
                          std::to_string(ct->tubularRadius())};
        }
        if (opt == "bs_units") {
            auto* bs = dynamic_cast<BallStickRepr*>(app.getRepr(ReprType::BallStick));
            if (!bs) return {false, "BallStick repr not found"};
            return {true, std::string("bs_units = ") + (bs->useVdwSize() ? "vdw" : "cell")};
        }
        if (opt == "bs_factor" || opt == "bsf") {
            auto* bs = dynamic_cast<BallStickRepr*>(app.getRepr(ReprType::BallStick));
            if (!bs) return {false, "BallStick repr not found"};
            return {true, "bs_factor = " + std::to_string(bs->sizeFactor())};
        }
        if (opt == "spacefill_scale" || opt == "ss_scale" || opt == "sfs") {
            auto* sf = dynamic_cast<SpacefillRepr*>(app.getRepr(ReprType::Spacefill));
            if (!sf) return {false, "Spacefill repr not found"};
            return {true, "spacefill_scale = " + std::to_string(sf->scale())};
        }
        if (opt == "nucleic_backbone" || opt == "nb") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            const char* n = (ct->nucleicBackbone() == CartoonRepr::NucleicBackbone::P) ? "p" : "c4";
            return {true, std::string("nucleic_backbone = ") + n};
        }
        if (opt == "lod_medium")
            return {true, "lod_medium = " + std::to_string(Representation::lodMediumThreshold)};
        if (opt == "lod_low")
            return {true, "lod_low = " + std::to_string(Representation::lodLowThreshold)};
        if (opt == "backbone_cutoff")
            return {true, "backbone_cutoff = " + std::to_string(Representation::backboneCutoff)};
        if (opt == "interface_color" || opt == "ic")
            return {true, "interface_color = " + std::to_string(app.interfaceColor_)};
        if (opt == "interface_thickness" || opt == "it")
            return {true, "interface_thickness = " + std::to_string(app.interfaceThickness_)};

        return {false, "Unknown option: " + opt};
    }, ":get <option>", "Print the current value of a :set option (handy for scripting)",
       {":get renderer", ":get fog", ":get focus_radius"}, "Session");

    // :help [cmd] — overview overlay (no args) or per-command help (one arg).
    // Per-command help displays usage, description, and registered examples.
    cmdRegistry_.registerCmd("help", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
    cmdRegistry_.registerCmd("clear", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
    cmdRegistry_.registerCmd("delete", deleteObjectCmd,
        ":delete [name]", "Delete an object (defaults to the currently selected one)",
        {":delete", ":delete 1bna"}, "Window");
    // :rm — vim/unix-style alias for :delete.
    cmdRegistry_.registerCmd("rm", deleteObjectCmd,
        ":rm [name]", "Remove an object (alias for :delete)",
        {":rm", ":rm 1bna"}, "Window");

    // :copy [<src-obj-or-sel>] [as <newname>]
    // Non-destructive clone. The first token is treated as an object
    // name iff it matches an object loaded in the store; otherwise the
    // whole pre-`as` chunk is parsed as a selection expression, and a
    // new MolObject is built from the matching atoms via subset().
    // Selection-form bonds are kept iff both endpoints survive, and
    // per-atom state (color, alpha, repr masks) carries over.
    cmdRegistry_.registerCmd("copy", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
    cmdRegistry_.registerCmd("extract", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
    cmdRegistry_.registerCmd("split", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
    cmdRegistry_.registerCmd("rename", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
    cmdRegistry_.registerCmd("info", [](Application& app, const ParsedCommand&) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        return {true, obj->name() + ": " +
               std::to_string(obj->atoms().size()) + " atoms, " +
               std::to_string(obj->bonds().size()) + " bonds"};
    }, ":info", "Show atom/bond counts and metadata for the current object",
       {":info"}, "Session");

    // :camera — print | save <file> | load <file> | reset
    //
    // The 15-float camera state (rotation 3x3, center XYZ, zoom, pan XY) is
    // serialized as a small key=value text file so figure scripts can be
    // bit-reproducible across renders. Without this, every re-render starts
    // from a freshly-PCA'd pose and tiny structural changes silently shift
    // the camera (issue #39c). File format is line-oriented and forgiving
    // about whitespace; the version header lets future revisions stay
    // backwards-compatible.
    cmdRegistry_.registerCmd("camera", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto& cam = app.tabs().currentTab().camera();
        auto formatState = [&]() {
            char buf[512];
            const auto& r = cam.rotation();
            std::snprintf(buf, sizeof(buf),
                "# molterm camera v1\n"
                "center = %.6f %.6f %.6f\n"
                "zoom = %.6f\n"
                "pan = %.6f %.6f\n"
                "rot = %.6f %.6f %.6f  %.6f %.6f %.6f  %.6f %.6f %.6f\n",
                cam.centerX(), cam.centerY(), cam.centerZ(),
                cam.zoom(), cam.panXOffset(), cam.panYOffset(),
                r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8]);
            return std::string(buf);
        };

        if (cmd.args.empty()) {
            // Print current state to the command line — handy for quick
            // copy/paste into a script as a fixed-camera snapshot.
            return {true, formatState()};
        }

        const std::string& sub = cmd.args[0];

        if (sub == "reset") {
            cam.reset();
            return {true, "Camera reset"};
        }

        if (sub == "save") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :camera save <file>"};
            const std::string& path = cmd.args[1];
            std::ofstream out(path);
            if (!out) return {false, "Cannot write " + path};
            out << formatState();
            return {true, "Camera saved to " + path};
        }

        if (sub == "load") {
            if (cmd.args.size() < 2)
                return {false, "Usage: :camera load <file>"};
            const std::string& path = cmd.args[1];
            std::ifstream in(path);
            if (!in) return {false, "Cannot read " + path};
            float cx = cam.centerX(), cy = cam.centerY(), cz = cam.centerZ();
            float zoom = cam.zoom();
            float panX = cam.panXOffset(), panY = cam.panYOffset();
            std::array<float, 9> rot = cam.rotation();
            std::string line;
            while (std::getline(in, line)) {
                // Strip leading whitespace + skip blanks/comments.
                size_t s = line.find_first_not_of(" \t");
                if (s == std::string::npos || line[s] == '#') continue;
                std::string content = line.substr(s);
                auto eq = content.find('=');
                if (eq == std::string::npos) continue;
                std::string key = content.substr(0, eq);
                std::string val = content.substr(eq + 1);
                while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
                if      (key == "center") std::sscanf(val.c_str(), " %f %f %f", &cx, &cy, &cz);
                else if (key == "zoom")   std::sscanf(val.c_str(), " %f", &zoom);
                else if (key == "pan")    std::sscanf(val.c_str(), " %f %f", &panX, &panY);
                else if (key == "rot")    std::sscanf(val.c_str(),
                    " %f %f %f %f %f %f %f %f %f",
                    &rot[0], &rot[1], &rot[2], &rot[3], &rot[4],
                    &rot[5], &rot[6], &rot[7], &rot[8]);
                // Unknown keys silently ignored — keeps forward-compat
                // when newer files add fields the current binary doesn't
                // recognize.
            }
            cam.setCenter(cx, cy, cz);
            cam.setZoom(zoom);
            cam.setPan(panX, panY);
            cam.setRotation(rot);
            return {true, "Camera loaded from " + path};
        }

        return {false, "Usage: :camera | :camera save <file> | :camera load <file> | :camera reset"};
    }, ":camera [save|load|reset] [file]",
       "Save/load/reset the camera (15-float state: rotation 3x3, center XYZ, zoom, pan XY) — bit-reproducible figures",
       {":camera", ":camera save fig30.cam", ":camera load fig30.cam", ":camera reset"}, "View");

    // :let <name> = <expr> — typed registers (#32, #33, #35).
    //
    // The expression evaluator (RegisterExpr) supports scalars, vec3
    // literals (`[x,y,z]`), atom positions via `pos(<chain:resi:name>)`,
    // PCA results via `pca(<selection>)`, and the usual vector algebra
    // (+, -, *, /, dot, cross, length, normalize, midpoint, angle).
    // Field access on registers: `$g.axis1`, `$g.center`, `$v.length`,
    // etc. — see Register::getVec/getScalar.
    cmdRegistry_.registerCmd("let", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        // Find `=` separator. Name is the single token before; RHS is
        // everything after, rejoined with spaces (the command parser
        // already split on whitespace AND commas, so `[1, 0, 0]` arrives
        // as `[1` `0` `0]` — we re-emit commas between adjacent tokens
        // so the expression lexer sees a clean stream).
        int eqIdx = -1;
        for (int i = 0; i < static_cast<int>(cmd.args.size()); ++i) {
            if (cmd.args[i] == "=") { eqIdx = i; break; }
        }
        if (eqIdx != 1) {
            return {false, "Usage: :let <name> = <expr>"};
        }
        const std::string& name = cmd.args[0];
        if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) {
            return {false, "Register name must start with letter or underscore"};
        }
        // Rejoin RHS. The command parser already split on whitespace AND
        // commas, so we re-emit commas between adjacent bare tokens (so
        // `[1 2 3]` becomes `[1,2,3]` and `angle(v d)` becomes `angle(v,d)`
        // for the expression lexer). Free-text inside pca(...)/pos(...)
        // would over-comma here, but the builtin handlers normalize
        // commas back to spaces before resolving the selection / atom-spec.
        std::string rhs;
        for (int i = eqIdx + 1; i < static_cast<int>(cmd.args.size()); ++i) {
            const std::string& t = cmd.args[i];
            if (!rhs.empty()) {
                char last = rhs.back();
                char first = t.empty() ? ' ' : t[0];
                bool lastIsOp   = (last  == '+' || last  == '-' || last  == '*' ||
                                   last  == '/' || last  == '(' || last  == '[' ||
                                   last  == ',' || last  == '.');
                bool firstIsOp  = (first == '+' || first == '-' || first == '*' ||
                                   first == '/' || first == ')' || first == ']' ||
                                   first == ',' || first == '.');
                if (!lastIsOp && !firstIsOp) rhs += ',';
                else                          rhs += ' ';
            }
            rhs += t;
        }

        RegisterExpr::Context ctx;
        ctx.regs = &app.registers();
        ctx.resolveAtomPos = [&app](const std::string& spec, double* x, double* y, double* z) -> bool {
            // Atom-spec resolver — accepts the same `obj/chain:resi:name`
            // qualifier the selection grammar uses since #55 (issue #66).
            // Forms:
            //   chain:resi:name              → current object
            //   obj/chain:resi:name          → named object (cross-object)
            // `obj/` is recognised by the first '/' in the spec; everything
            // before it is the object name, everything after is the
            // legacy three-part atom spec.
            std::string objName;
            std::string remainder = spec;
            auto slash = spec.find('/');
            if (slash != std::string::npos) {
                objName = spec.substr(0, slash);
                remainder = spec.substr(slash + 1);
            }
            std::shared_ptr<MolObject> obj;
            if (objName.empty()) {
                obj = app.tabs().currentTab().currentObject();
            } else {
                obj = app.store().get(objName);
            }
            if (!obj) return false;
            std::string chain, resi, name;
            int part = 0;
            for (char c : remainder) {
                if (c == ':') { ++part; continue; }
                if (std::isspace(static_cast<unsigned char>(c))) continue;
                if      (part == 0) chain += c;
                else if (part == 1) resi += c;
                else                name += c;
            }
            int rs = -1;
            try { rs = std::stoi(resi); } catch (...) { return false; }
            const auto& atoms = obj->atoms();
            for (const auto& a : atoms) {
                if (!chain.empty() && a.chainId != chain) continue;
                if (a.resSeq != rs) continue;
                if (!name.empty() && a.name != name) continue;
                *x = a.x; *y = a.y; *z = a.z;
                return true;
            }
            return false;
        };
        ctx.collectSelectionXYZ = [&app](const std::string& expr,
                                         std::vector<float>* xs,
                                         std::vector<float>* ys,
                                         std::vector<float>* zs) -> int {
            auto obj = app.tabs().currentTab().currentObject();
            if (!obj) return 0;
            // pca(<sel>) gets comma-injected by the :let RHS rejoin (since
            // the command parser pre-split on commas + whitespace). The
            // selection grammar uses spaces, never commas, so it's safe
            // to swap them back out before parsing.
            std::string normalized = expr;
            for (char& c : normalized) if (c == ',') c = ' ';
            auto sel = app.parseSelection(normalized, *obj);
            const auto& atoms = obj->atoms();
            for (int i : sel.indices()) {
                if (i < 0 || i >= (int)atoms.size()) continue;
                xs->push_back(atoms[i].x);
                ys->push_back(atoms[i].y);
                zs->push_back(atoms[i].z);
            }
            return static_cast<int>(xs->size());
        };

        auto r = RegisterExpr::eval(rhs, ctx);
        if (!r.ok) return {false, ":let " + name + ": " + r.error};
        app.registers()[name] = r.value;
        return {true, formatRegister(name, r.value)};
    }, ":let <name> = <expr>",
       "Bind a typed register (scalar, vec3, or pca-result) for reuse later. "
       "Expression supports +,-,*,/, vec3 literals [x,y,z], $reg.field access, "
       "and builtins pos()/pca()/dot()/cross()/length()/normalize()/midpoint()/angle().",
       {":let v_axis = pos(A:43:CA) - pos(B:23:CA)",
        ":let G = pca(chain A and helix)",
        ":let theta = angle($v_axis, $G.axis1)"}, "Registers");

    // :unlet <name> | :unlet * — drop registers.
    cmdRegistry_.registerCmd("unlet", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
    cmdRegistry_.registerCmd("registers", [](Application& app, const ParsedCommand&) -> ExecResult {
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

    // :expose <name> [<name> ...] — mark registers for export from the
    // current script frame (issue #67). Each name will be copied into
    // the caller's register frame when the script exits. Names starting
    // with `_` are private and silently rejected. Outside a scope=local
    // script the command is a no-op (top-level frame has no caller).
    // Named :expose to avoid collision with the existing :export
    // commands for PML session export and PDB file export.
    cmdRegistry_.registerCmd("expose", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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

    // :select <expression>
    cmdRegistry_.registerCmd("select", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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

        // Check for named selection: "name = expr"
        // Find "=" in args
        int eqIdx = -1;
        for (int i = 0; i < static_cast<int>(cmd.args.size()); ++i) {
            if (cmd.args[i] == "=") { eqIdx = i; break; }
        }

        std::string name;
        std::string expr;

        if (eqIdx == 1) {
            // "select name = expr..."
            name = cmd.args[0];
            for (int i = eqIdx + 1; i < static_cast<int>(cmd.args.size()); ++i) {
                if (!expr.empty()) expr += " ";
                expr += cmd.args[i];
            }
        } else {
            // "select expr..."
            for (size_t i = 0; i < cmd.args.size(); ++i) {
                if (i > 0) expr += " ";
                expr += cmd.args[i];
            }
        }

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
    cmdRegistry_.registerCmd("count", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
    cmdRegistry_.registerCmd("cmp", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
    cmdRegistry_.registerCmd(kSele, [](Application& app, const ParsedCommand&) -> ExecResult {
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
    enum class AlignMode { Auto, ForceTM, ForceMM };

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

    cmdRegistry_.registerCmd("align",
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

    cmdRegistry_.registerCmd("alignto",
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
    cmdRegistry_.registerCmd("super",
        [doAlignByName](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return doAlignByName(app, cmd, AlignMode::ForceTM);
        },
        ":super <obj> [sel] to <obj> [sel]",
        "Superpose structures (alias for :align tm)",
        {":super mob to ref"},
        "Analysis");

    // Hidden back-compat aliases that hard-force MM mode.
    cmdRegistry_.registerCmd("mmalign",
        [doAlignByName](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return doAlignByName(app, cmd, AlignMode::ForceMM);
        },
        ":mmalign <obj> [sel] to <obj> [sel]",
        "Deprecated alias for ':align ... mm'",
        {":mmalign complex1 to complex2"},
        "Hidden");

    cmdRegistry_.registerCmd("mmalignto",
        [doAlignTo](Application& app, const ParsedCommand& cmd) -> ExecResult {
            return doAlignTo(app, cmd, AlignMode::ForceMM);
        },
        ":mmalignto <target> [sel]",
        "Deprecated alias for ':alignto ... mm'",
        {":mmalignto complex2"},
        "Hidden");

    cmdRegistry_.registerCmd("loadalign",
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

    // :fetch <pdb_id> — download from RCSB PDB or AlphaFold DB
    cmdRegistry_.registerCmd("fetch", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) return {false, "Usage: :fetch <pdb_id> or :fetch afdb:<uniprot_id>"};

        std::string id = cmd.args[0];
        std::string url;
        std::string filename;

        // AlphaFold DB: "afdb:P12345" or "AFDB:P12345" or "AF-P12345"
        bool isAF = false;
        if (id.size() > 5 && (id.substr(0, 5) == "afdb:" || id.substr(0, 5) == "AFDB:")) {
            std::string uniprotId = id.substr(5);
            url = "https://alphafold.ebi.ac.uk/files/AF-" + uniprotId + "-F1-model_v6.cif";
            filename = "AF-" + uniprotId + ".cif";
            isAF = true;
        } else if (id.size() > 3 && id.substr(0, 3) == "AF-") {
            // Direct AF ID like AF-P12345-F1
            url = "https://alphafold.ebi.ac.uk/files/" + id + "-model_v6.cif";
            filename = id + ".cif";
            isAF = true;
        } else {
            // RCSB PDB: 4-character PDB ID
            std::string lower = id;
            for (auto& c : lower) c = static_cast<char>(std::tolower(c));
            url = "https://files.rcsb.org/download/" + lower + ".cif";
            filename = lower + ".cif";
        }

        // Write to current working directory, but never overwrite an existing file
        std::filesystem::path outPath = std::filesystem::current_path() / filename;
        std::string src = isAF ? "AlphaFold DB" : "RCSB PDB";

        if (std::filesystem::exists(outPath)) {
            std::string result = app.loadFile(outPath.string());
            bool ok = result.rfind("Loaded ", 0) == 0;
            return {ok, "Loaded existing " + outPath.string() + " (skipped fetch from " + src + ") | " + result};
        }

        std::string curlCmd = "curl -sL -o '" + outPath.string() + "' -w '%{http_code}' '" + url + "'";
        FILE* pipe = popen(curlCmd.c_str(), "r");
        if (!pipe) return {false, "Failed to run curl"};

        char buf[64];
        std::string httpCode;
        while (fgets(buf, sizeof(buf), pipe)) httpCode += buf;
        int ret = pclose(pipe);

        if (ret != 0 || httpCode.find("200") == std::string::npos) {
            std::error_code ec;
            std::filesystem::remove(outPath, ec);
            return {false, "Failed to fetch " + id + " from " + src + " (HTTP " + httpCode + ")"};
        }

        std::string result = app.loadFile(outPath.string());
        bool ok = result.rfind("Loaded ", 0) == 0;
        return {ok, "Fetched " + id + " from " + src + " -> " + outPath.string() + " | " + result};
    }, ":fetch <pdb_id|afdb:uniprot_id>", "Download a structure from RCSB PDB (4-letter ID) or AlphaFold DB (afdb: prefix)",
       {":fetch 1bna", ":fetch afdb:P00533"}, "Files");

    // :assembly [id|list] — generate biological assembly
    cmdRegistry_.registerCmd("assembly", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        std::string path = obj->sourcePath();
        if (path.empty()) return {false, "No source file for " + obj->name()};

        if (!cmd.args.empty() && cmd.args[0] == "list") {
            auto assemblies = CifLoader::listAssemblies(path);
            if (assemblies.empty()) return {true, "No assemblies found in " + obj->name()};
            std::string result = "Assemblies:";
            for (const auto& a : assemblies)
                result += " " + a.name + "(" + std::to_string(a.oligomericCount) + "-mer)";
            return {true, result};
        }

        std::string asmId = cmd.args.empty() ? "1" : cmd.args[0];
        try {
            auto asmObj = CifLoader::loadAssembly(path, asmId);
            int atomCount = static_cast<int>(asmObj->atoms().size());
            int bondCount = static_cast<int>(asmObj->bonds().size());
            std::string name = asmObj->name();
            auto ptr = app.store().add(std::move(asmObj));
            app.tabs().currentTab().addObject(ptr);
            if (app.autoCenter()) app.tabs().currentTab().centerView();
            MLOG_INFO("Generated assembly %s: %d atoms, %d bonds", asmId.c_str(), atomCount, bondCount);
            return {true, "Assembly " + asmId + " → " + name + ": " +
                   std::to_string(atomCount) + " atoms, " + std::to_string(bondCount) + " bonds"};
        } catch (const std::exception& e) {
            return {false, std::string("Assembly error: ") + e.what()};
        }
    }, ":assembly [id|list]", "Generate a biological assembly (defaults to assembly 1; 'list' shows available IDs)",
       {":assembly", ":assembly 1", ":assembly list"}, "Files");

    // (PML/PDB :export handlers were registered separately and the second
    // one silently overwrote the first under CommandRegistry's last-write-
    // wins semantics. They now live as a single extension-dispatching
    // handler below near the PDB writer call site.)

    // :screenshot <file.png>
    cmdRegistry_.registerCmd("screenshot", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        std::string path;
        // Optional pixel dimensions: `:screenshot file.png 1920 1080`.
        // Optional DPI for PNG pHYs metadata: `:screenshot file.png 1800 1200 300`.
        // Useful when running from a script with no real terminal — the
        // active viewport may otherwise default to a small fallback size.
        int reqPixW = 0, reqPixH = 0, reqDpi = 0;
        std::vector<std::string> positional;
        for (const auto& a : cmd.args) positional.push_back(a);

        // Last token is DPI iff we have ≥4 args; W H are then args[-3..-2].
        if (positional.size() >= 4) {
            try {
                reqDpi = std::stoi(positional.back());
                positional.pop_back();
            } catch (...) {
                return {false, "Usage: :screenshot [file.png] [W H [DPI]]"};
            }
            if (reqDpi < 1 || reqDpi > 4800) {
                return {false, "DPI out of range (1..4800)"};
            }
        }
        if (positional.size() >= 3) {
            try {
                reqPixW = std::stoi(positional[positional.size() - 2]);
                reqPixH = std::stoi(positional[positional.size() - 1]);
                positional.pop_back();
                positional.pop_back();
            } catch (...) {
                return {false, "Usage: :screenshot [file.png] [W H [DPI]]"};
            }
            if (reqPixW < 64 || reqPixH < 64 ||
                reqPixW > 8192 || reqPixH > 8192) {
                return {false, "Screenshot size out of range (64..8192 px)"};
            }
        }

        if (positional.empty()) {
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            char fname[64];
            std::strftime(fname, sizeof(fname), "molterm_%Y%m%d_%H%M%S.png", std::localtime(&t));
            path = fname;
        } else {
            path = positional[0];
        }

        auto savedMsg = [&](int w, int h) {
            std::string msg = "Saved " + std::to_string(w) + "x" + std::to_string(h);
            if (reqDpi > 0) msg += " @ " + std::to_string(reqDpi) + " dpi";
            msg += " to " + path;
            return msg;
        };

        // Surface 0-visible-atom screenshots unconditionally — silent
        // empty PNGs are the most common headless-script footgun.
        int visibleAtoms = app.countVisibleAtoms();
        if (visibleAtoms == 0) {
            std::fprintf(stderr,
                "[warn] :screenshot — 0 visible atoms; PNG will be empty\n");
        }

        auto t0 = std::chrono::steady_clock::now();

        // If already in pixel mode and no explicit size was requested,
        // grab the live framebuffer. The canvas already carries bgMode_
        // since renderViewport() applies it before clear().
        // Skip when the live frame carries a $sele/pk halo the user
        // doesn't want in the PNG — fall through to the offscreen
        // re-render path which honors `screenshot_overlay`. Issue #96.
        bool fastPathOk = !app.hasSelectionHighlight() || app.screenshotOverlay();
        if (app.rendererType() == RendererType::Pixel && reqPixW == 0 && fastPathOk) {
            auto* pc = dynamic_cast<PixelCanvas*>(app.canvas());
            if (pc && pc->savePNG(path, reqDpi)) {
                double dt = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - t0).count();
                app.logRenderStats(pc->pixelWidth(), pc->pixelHeight(),
                                   reqDpi, visibleAtoms, dt);
                return ExecResult{true, savedMsg(pc->pixelWidth(), pc->pixelHeight())};
            }
            return {false, "Failed to save " + path};
        }

        // Offscreen render: create a temporary PixelCanvas, render into it, save PNG
        auto proto = ProtocolPicker::detect();
        auto encoder = ProtocolPicker::createEncoder(proto);
        if (!encoder) {
            // Create a dummy SixelEncoder just for offscreen rendering
            encoder = ProtocolPicker::createEncoder(GraphicsProtocol::Sixel);
        }
        if (!encoder) return {false, "Cannot create offscreen renderer"};

        PixelCanvas offscreen(std::move(encoder));
        if (reqPixW > 0) {
            // Honor the exact pixel dimensions the user asked for —
            // converting through cells and back would silently truncate
            // when the request isn't a multiple of the terminal's cell size.
            offscreen.resizePixels(reqPixW, reqPixH);
        } else {
            offscreen.resize(app.layout().viewportWidth(),
                             app.layout().viewportHeight());
        }
        app.applyBgMode(offscreen);
        offscreen.clear();

        auto& tab = app.tabs().currentTab();

        if (auto* wf = dynamic_cast<WireframeRepr*>(app.getRepr(ReprType::Wireframe))) {
            wf->setHeteroatomCarbonScheme(app.interfaceOverlay_ || app.focusSnapshot_.active);
        }

        // Render reprs once per stereoscopic eye (single-pass when
        // stereo is off). Mirrors renderViewport().
        std::array<float, 9> savedScreenshotRot{};
        for (int eye = 0; eye < app.stereoEyeCount(); ++eye) {
            savedScreenshotRot = app.setupStereoEye(eye, offscreen.subW(),
                                                     offscreen.subH(),
                                                     offscreen.aspectYX());
            for (const auto& obj : tab.objects()) {
                if (!obj->visible()) continue;
                offscreen.setAlphaLUT(obj->atomAlpha().empty() ? nullptr
                                                               : &obj->atomAlpha());
                for (auto& [reprType, repr] : app.representations()) {
                    if (obj->reprVisible(reprType)) {
                        repr->render(*obj, tab.camera(), offscreen);
                    }
                }
            }
            offscreen.setAlphaLUT(nullptr);
        }
        app.restoreStereoCamera(savedScreenshotRot);

        if (app.outlineEnabled()) {
            uint8_t r = 0, g = 0, b = 0;
            if (auto& oc = app.outlineColor()) { r = (*oc)[0]; g = (*oc)[1]; b = (*oc)[2]; }
            offscreen.applyOutline(app.outlineThreshold(), app.outlineDarken(),
                                   app.outlineMode(), r, g, b);
        }
        if (app.fogStrength() > 0.0f)
            offscreen.applyDepthFog(app.fogStrength());

        // Mirror the live render pipeline: focus-dim (mask-driven) +
        // interface overlay so the captured PNG matches what's on screen.
        const std::vector<bool>* dimMask = nullptr;
        if (app.interfaceOverlay_ && !app.interfaceAtomMask_.empty()) {
            dimMask = &app.interfaceAtomMask_;
        } else if (!app.focusAtomMask_.empty()) {
            dimMask = &app.focusAtomMask_;
        }
        if (dimMask) offscreen.applyFocusDim(*dimMask, app.focusDimStrength_);

        if ((app.interfaceOverlay_ || app.focusSnapshot_.active) &&
            app.interfaceRepr_.hasData()) {
            if (auto obj = tab.currentObject()) {
                for (int eye = 0; eye < app.stereoEyeCount(); ++eye) {
                    savedScreenshotRot = app.setupStereoEye(
                        eye, offscreen.subW(), offscreen.subH(),
                        offscreen.aspectYX());
                    app.interfaceRepr_.render(*obj, tab.camera(), offscreen);
                }
                app.restoreStereoCamera(savedScreenshotRot);
            }
        }

        // Match the live render's overlay layer: residue labels,
        // measurement dashed lines + values, $sele/pk highlight rings.
        // Drawn after fog/dim so labels stay legible against dimmed atoms.
        // RenderScaleScope auto-scales label/annotation sizes for the
        // screenshot resolution / DPI per :set size_mode (issue #48).
        {
            Application::RenderScaleScope screenshotScale(
                app, offscreen.pixelHeight(), reqDpi);
            for (int eye = 0; eye < app.stereoEyeCount(); ++eye) {
                savedScreenshotRot = app.setupStereoEye(
                    eye, offscreen.subW(), offscreen.subH(),
                    offscreen.aspectYX());
                app.drawPixelOverlay(offscreen, app.screenshotOverlay());
            }
            app.restoreStereoCamera(savedScreenshotRot);
        }

        // Restore projection for the active canvas
        auto* canvas = app.canvas();
        if (canvas)
            tab.camera().prepareProjection(canvas->subW(), canvas->subH(), canvas->aspectYX());

        if (offscreen.savePNG(path, reqDpi)) {
            double dt = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t0).count();
            app.logRenderStats(offscreen.pixelWidth(), offscreen.pixelHeight(),
                               reqDpi, visibleAtoms, dt);
            return ExecResult{true, savedMsg(offscreen.pixelWidth(), offscreen.pixelHeight())};
        }
        return {false, "Failed to save " + path};
    }, ":screenshot [file.png] [W H [DPI]]",
       "Save a PNG; optional explicit size (W H) and DPI metadata for "
       "figure prep. Excludes the active-selection halo by default; opt "
       "back in with ':set screenshot_overlay on'.",
       {":screenshot", ":screenshot fig.png", ":screenshot fig.png 1920 1080 300"}, "Files");

    // :preset — apply smart default representation
    cmdRegistry_.registerCmd("preset", [](Application& app, const ParsedCommand&) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        obj->applySmartDefaults();
        return {true, "Applied default preset (cartoon + ballstick ligands)"};
    }, ":preset", "Apply smart default representations (cartoon for protein, ballstick for ligands)",
       {":preset"}, "Display");

    cmdRegistry_.registerCmd("label", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty())
            return {false, "Usage: :label <sel|corner CORNER|screen FX FY|world X Y Z> [= \"text\"] or :label clear"};
        if (cmd.args[0] == "clear") {
            app.labelAtoms().clear();
            app.labelText().clear();
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
        auto sel = app.parseSelection(expr, *obj);
        if (sel.empty()) return {false, "No atoms match: " + expr};
        auto& labels = app.labelAtoms();
        auto& textMap = app.labelText();
        for (int idx : sel.indices()) {
            bool found = false;
            for (int li : labels) { if (li == idx) { found = true; break; } }
            if (!found) labels.push_back(idx);
            // Drop any prior override on the no-`=` path so re-labelling
            // returns the atom to the template/default text.
            if (hasCustom) textMap[idx] = customText;
            else           textMap.erase(idx);
        }
        if (hasCustom)
            return {true, "Labeled " + std::to_string(sel.size()) +
                          " atoms as \"" + customText + "\""};
        return {true, "Labeled " + std::to_string(sel.size()) + " atoms"};
    }, ":label <selection|clear> [= \"text\"]",
       "Show residue labels (default <resname><resseq>; '= text' overrides per-atom; 'clear' removes)",
       {":label resi 50-60", ":label chain E and resi 1 = \"P1\"", ":label clear"}, "Display");

    cmdRegistry_.registerCmd("unlabel", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) {
            // "Remove all labels" must cover free labels too — corner /
            // screen / world anchors live in a separate store but read
            // as "labels" to the user.
            int n = static_cast<int>(app.labelAtoms().size());
            int fn = static_cast<int>(app.freeLabels().size());
            app.labelAtoms().clear();
            app.labelText().clear();
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
        auto sel = app.parseSelection(expr, *obj);
        if (sel.empty()) return {false, "No atoms match: " + expr};
        std::set<int> drop(sel.indices().begin(), sel.indices().end());
        auto& labels = app.labelAtoms();
        auto& textMap = app.labelText();
        size_t before = labels.size();
        labels.erase(std::remove_if(labels.begin(), labels.end(),
                     [&](int idx) { return drop.count(idx) > 0; }),
                     labels.end());
        for (int idx : drop) textMap.erase(idx);
        return {true, "Removed " + std::to_string(before - labels.size()) +
                      " label(s)"};
    }, ":unlabel [selection|corner [<which>]|screen|world]",
       "Remove labels (no arg: all atom + free; selection: matching atoms; corner/screen/world: that anchor kind)",
       {":unlabel", ":unlabel chain E and resi 1",
        ":unlabel corner topleft", ":unlabel corner", ":unlabel screen"}, "Display");

    // :overlay on|off | :overlay clear
    cmdRegistry_.registerCmd("overlay", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) {
            return {false, "Usage: :overlay on|off | :overlay clear"};
        }
        if (cmd.args[0] == "clear") {
            int mc = static_cast<int>(app.measurements().size());
            int lc = static_cast<int>(app.labelAtoms().size());
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

    // :run [--strict] [--fresh] <script.mt> [KEY=VALUE ...] — execute a command script
    cmdRegistry_.registerCmd("run", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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

    cmdRegistry_.registerCmd("setenv", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
    cmdRegistry_.registerCmd("echo", [](Application&, const ParsedCommand& cmd) -> ExecResult {
        std::string text = joinArgs(cmd.args, 0, cmd.args.size());
        std::printf("%s\n", text.c_str());
        std::fflush(stdout);
        return {true, ""};
    }, ":echo <text>",
       "Print text to stdout (after ${var} / ${reg:fmt} expansion). Useful for "
       "machine-readable script output and LLM-agent telemetry.",
       {":echo crossing = ${crossing:.2f}",
        ":echo workspace = ${WS}",
        ":echo \"label A\\tlabel B\""}, "Session");

    cmdRegistry_.registerCmd("save", [](Application& app, const ParsedCommand&) -> ExecResult {
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
    cmdRegistry_.registerCmd("export", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
                exportMs.push_back({m.atoms, m.label, m.caption});
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

    cmdRegistry_.registerCmd("resume", [](Application& app, const ParsedCommand&) -> ExecResult {
        std::string msg = SessionSaver::restoreSession(app);
        bool ok = msg.find("Failed") == std::string::npos &&
                  msg.find("Cannot") == std::string::npos &&
                  msg.find("not found") == std::string::npos;
        return {ok, msg};
    }, ":resume", "Restore the last session from ~/.molterm/autosave.toml",
       {":resume"}, "Session");

    // Helper: resolve atom index from serial number string or pick register
    auto resolveAtomIdx = [](Application& app, const std::string& s) -> int {
        // Pick register: pk1..pk4
        if (s.size() >= 3 && s[0] == 'p' && s[1] == 'k' && s[2] >= '1' && s[2] <= '4') {
            return app.pickReg(s[2] - '1');
        }
        // Named selection reference: $name → first atom
        if (!s.empty() && s[0] == '$') {
            auto it = app.namedSelections().find(s.substr(1));
            if (it != app.namedSelections().end() && !it->second.empty())
                return it->second.indices()[0];
            return -1;
        }
        // Serial number
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return -1;
        const auto& atoms = obj->atoms();
        try {
            int serial = std::stoi(s);
            for (int i = 0; i < static_cast<int>(atoms.size()); ++i) {
                if (atoms[i].serial == serial) return i;
            }
        } catch (...) {}
        return -1;
    };

    // Helper: format atom label for measurement display
    auto atomLabel = [](const AtomData& a) -> std::string {
        return a.chainId + "/" + a.resName + std::to_string(a.resSeq) + "/" + a.name;
    };


    // :measure [serial1 serial2] — distance (no args = pk1↔pk2)
    cmdRegistry_.registerCmd("measure", [resolveAtomIdx, atomLabel](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object selected"};
        const auto& atoms = obj->atoms();
        int n = static_cast<int>(atoms.size());
        auto [pos, caption] = splitAtEqToken(cmd.args);

        int i1, i2;
        if (pos.empty()) {
            i1 = app.pickRegs_[0]; i2 = app.pickRegs_[1];
            if (i1 < 0 || i2 < 0) return {false, "Click two atoms first (pk1, pk2), then :measure"};
        } else if (pos.size() >= 2) {
            i1 = resolveAtomIdx(app, pos[0]);
            i2 = resolveAtomIdx(app, pos[1]);
            if (i1 < 0) return {false, "Atom not found: serial " + pos[0]};
            if (i2 < 0) return {false, "Atom not found: serial " + pos[1]};
        } else {
            return {false, "Usage: :measure [serial1 serial2] [= \"caption\"]"};
        }
        if (i1 >= n || i2 >= n) return {false, "Invalid atom index"};

        float dx = atoms[i1].x - atoms[i2].x;
        float dy = atoms[i1].y - atoms[i2].y;
        float dz = atoms[i1].z - atoms[i2].z;
        float dist = std::sqrt(dx*dx + dy*dy + dz*dz);

        char dbuf[16]; std::snprintf(dbuf, sizeof(dbuf), "%.2f", dist);
        std::string shortLabel = std::string(dbuf) + "A";
        std::string msg = "Distance " + atomLabel(atoms[i1]) + " — " +
            atomLabel(atoms[i2]) + " = " + shortLabel;
        if (!caption.empty()) msg += "  \"" + caption + "\"";
        app.measurements().push_back({{i1, i2}, shortLabel, caption});
        MLOG_INFO("%s", msg.c_str());
        return {true, msg};
    }, ":measure [serial1 serial2 | pk1 pk2] [= \"caption\"]",
       "Measure a distance and persist a dashed line + value. Endpoints are "
       "atom serials (PDB serial, not selections) or the pk1/pk2 picks; "
       "for a selection, resolve to a serial first.",
       {":measure", ":measure pk1 pk2",
        ":measure 12 47 = \"Glu-OE1 ↔ Lys-Nζ\""}, "Measurement");

    // :arrow — solid arrow with caption (issue #38). Distinct from :measure
    // (dashed + auto distance) because the visual semantics matter: a solid
    // arrow reads as "this is an axis vector", not "the author measured
    // here". Two endpoint forms:
    //   :arrow <serial1> <serial2> [= "label"]   # atom-to-atom, resolved once
    //   :arrow $regA $regB         [= "label"]   # vec3 / point register endpoints
    cmdRegistry_.registerCmd("arrow", [resolveAtomIdx](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto [pos, caption] = splitAtEqToken(cmd.args);
        if (pos.size() < 2) return {false, "Usage: :arrow <s1> <s2> [= \"label\"] | :arrow $regA $regB"};
        ArrowOverlay arr;
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
    }, ":arrow <s1> <s2> [= \"label\"]",
       "Persistent solid arrow + caption between two atoms / vec3 registers (use $reg for non-atom endpoints)",
       {":arrow pk1 pk2 = \"V-V axis\"",
        ":arrow $p1 $p2 = \"helix axis\""}, "Measurement");

    // :axis $reg [= "label"] — render the principal axis of a PCA register
    // as an arrow centered on its `center`, length proportional to the
    // square root of the eigenvalue (≈ 1σ along that axis). Provides the
    // natural composition over `:let G = pca(...); :axis $G`.
    cmdRegistry_.registerCmd("axis", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto [pos, caption] = splitAtEqToken(cmd.args);
        if (pos.empty() || pos[0].empty() || pos[0][0] != '$')
            return {false, "Usage: :axis $pcaRegister [= \"label\"]"};
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
        for (int i = 0; i < 3; ++i) {
            float c = static_cast<float>(p.center[i]);
            float d = static_cast<float>(p.axis1[i]);
            arr.a[i] = c - halfLen * d;
            arr.b[i] = c + halfLen * d;
        }
        app.arrows().push_back(std::move(arr));
        return {true, "Axis added"};
    }, ":axis $pcaReg [= \"label\"]",
       "Draw the major axis (axis1) of a pca-result register as an arrow of length ±1σ centered on its centroid",
       {":axis $G = \"groove axis\""}, "Measurement");
    cmdRegistry_.registerCmd("angle", [resolveAtomIdx, atomLabel](Application& app, const ParsedCommand& cmd) -> ExecResult {
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

        float v1x = atoms[i1].x - atoms[i2].x, v1y = atoms[i1].y - atoms[i2].y, v1z = atoms[i1].z - atoms[i2].z;
        float v2x = atoms[i3].x - atoms[i2].x, v2y = atoms[i3].y - atoms[i2].y, v2z = atoms[i3].z - atoms[i2].z;
        float dot = v1x*v2x + v1y*v2y + v1z*v2z;
        float len1 = std::sqrt(v1x*v1x + v1y*v1y + v1z*v1z);
        float len2 = std::sqrt(v2x*v2x + v2y*v2y + v2z*v2z);
        float cosA = (len1 > 0 && len2 > 0) ? dot / (len1 * len2) : 0;
        cosA = std::max(-1.0f, std::min(1.0f, cosA));
        float deg = std::acos(cosA) * 180.0f / static_cast<float>(M_PI);

        char buf[128];
        std::snprintf(buf, sizeof(buf), "%.1f", deg);
        std::string shortLabel = std::string(buf) + "°";
        std::string msg = "Angle " + atomLabel(atoms[i1]) + " — " +
            atomLabel(atoms[i2]) + " — " + atomLabel(atoms[i3]) + " = " + buf + " deg";
        if (!caption.empty()) msg += "  \"" + caption + "\"";
        app.measurements().push_back({{i1, i2, i3}, shortLabel, caption});
        MLOG_INFO("%s", msg.c_str());
        return {true, msg};
    }, ":angle [serial1 serial2 serial3 | pk1 pk2 pk3] [= \"caption\"]",
       "Measure the angle at the middle atom; endpoints are atom serials "
       "(PDB serial, not selections) or the pk1/pk2/pk3 picks.",
       {":angle", ":angle pk1 pk2 pk3 = \"φ1\""}, "Measurement");

    // :dihedral [s1 s2 s3 s4] — dihedral (no args = pk1-pk4)
    cmdRegistry_.registerCmd("dihedral", [resolveAtomIdx, atomLabel](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
        std::string msg = "Dihedral " + atomLabel(atoms[i1]) + " — " +
            atomLabel(atoms[i2]) + " — " + atomLabel(atoms[i3]) + " — " +
            atomLabel(atoms[i4]) + " = " + buf + " deg";
        if (!caption.empty()) msg += "  \"" + caption + "\"";
        app.measurements().push_back({{i1, i2, i3, i4}, shortLabel, caption});
        MLOG_INFO("%s", msg.c_str());
        return {true, msg};
    }, ":dihedral [serial1..serial4 | pk1..pk4] [= \"caption\"]",
       "Measure a dihedral angle; endpoints are atom serials (PDB serial, "
       "not selections) or the pk1..pk4 picks.",
       {":dihedral", ":dihedral pk1 pk2 pk3 pk4 = \"χ1\""}, "Measurement");

    // :contactmap [cutoff] — toggle contact map panel
    cmdRegistry_.registerCmd("contactmap", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        float cutoff = 8.0f;
        if (!cmd.args.empty()) {
            try { cutoff = std::stof(cmd.args[0]); } catch (...) {}
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
    cmdRegistry_.registerCmd("cmap", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        return app.cmdRegistry().execute(app, cmd.args.empty() ? "contactmap" :
            "contactmap " + cmd.args[0]);
    }, ":cmap [cutoff]", "Toggle contact map panel (alias for :contactmap)",
       {":cmap", ":cmap 6"}, "Analysis");

    cmdRegistry_.registerCmd("interface", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
            try { cutoff = std::stof(cmd.args[0]); wantOn = true; cutoffArgIdx = 99; }
            catch (...) { return {false, "Usage: :interface on|off|legend [cutoff]"}; }
        }
        if (cmd.args.size() > cutoffArgIdx) {
            try { cutoff = std::stof(cmd.args[cutoffArgIdx]); }
            catch (...) { return {false, "Cutoff must be a number"}; }
        }
        app.interfaceOverlay_ = wantOn;
        if (app.interfaceOverlay_) {
            auto obj = app.tabs().currentTab().currentObject();
            if (!obj) {
                app.interfaceOverlay_ = false;
                return {false, "No object loaded"};
            }
            app.interfaceCutoff_ = cutoff;
            if (!app.recomputeInterface()) {
                app.interfaceOverlay_ = false;
                char buf[80];
                std::snprintf(buf, sizeof(buf),
                              "No inter-chain contacts found (cutoff=%.1fA)",
                              cutoff);
                return {false, buf};
            }

            int nHB = 0, nSalt = 0, nHyd = 0, nOther = 0;
            for (const auto& c : app.interfaceContacts_) {
                switch (c.type) {
                    case InteractionType::HBond:       ++nHB;    break;
                    case InteractionType::SaltBridge:  ++nSalt;  break;
                    case InteractionType::Hydrophobic: ++nHyd;   break;
                    case InteractionType::Other:       ++nOther; break;
                }
            }
            auto tag = [&](InteractionType t) {
                return (app.interfaceShowMask_ & interactionBit(t))
                       ? "" : "*";
            };
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                "Interface: %zu residue pairs (cutoff=%.1fA) — "
                "salt %d%s, H-bond %d%s, hydrophobic %d%s, other %d%s"
                "%s",
                app.interfaceContacts_.size(), cutoff,
                nSalt,  tag(InteractionType::SaltBridge),
                nHB,    tag(InteractionType::HBond),
                nHyd,   tag(InteractionType::Hydrophobic),
                nOther, tag(InteractionType::Other),
                app.interfaceShowMask_ == kInterfaceShowAll
                    ? "" : "  [* hidden — :set interface_show all]");
            return {true, buf};
        }
        app.interfaceContacts_.clear();
        app.interfaceAtomMask_.clear();
        app.interfaceRepr_.clear();
        app.interfaceFromZoom_ = false;
        return {true, "Interface overlay hidden"};
    }, ":interface on|off|legend [cutoff]",
       "Show/hide classified inter-chain contact overlay; 'legend' opens a color/stats overlay (default cutoff: 4.5 \xC3\x85)",
       {":interface on", ":interface off", ":interface on 5.0", ":interface legend"}, "Analysis");

    // :focus <selection>  → Mol*-style focus on selection
    //                        (camera + hide non-neighborhood + show
    //                        sidechains + dim cartoon context).
    // :focus off           → restore pre-focus camera + visibility.
    cmdRegistry_.registerCmd("focus", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.empty()) {
            return {false, "Usage: :focus <selection>  |  :focus off"};
        }
        if (cmd.args.size() == 1 &&
            (cmd.args[0] == "off" || cmd.args[0] == "none" || cmd.args[0] == "clear")) {
            if (!app.focusActive()) return {true, "Focus already off"};
            app.exitFocus();
            app.logViewState(cmd);
            return {true, "Focus exited"};
        }
        auto obj = app.tabs().currentTab().currentObject();
        if (!obj) return {false, "No object loaded"};
        std::string expr;
        for (size_t i = 0; i < cmd.args.size(); ++i) {
            if (i) expr += ' ';
            expr += cmd.args[i];
        }
        Selection sel = app.parseSelection(expr, *obj);
        if (sel.empty()) return {false, "Empty selection: " + expr};
        app.enterFocus(*obj, sel.indices(), expr);
        app.logViewState(cmd, static_cast<int>(sel.size()));
        return {true, "Focus: " + std::to_string(sel.size()) +
                      " atoms (" + expr + ")"};
    }, ":focus <selection>|off",
       "Mol*-style click-to-focus: zoom in, hide occluders, show sidechains (use 'off' to exit)",
       {":focus chain A and resi 80",
        ":focus $sele",
        ":focus same residue as $sele",
        ":focus same chain as resn HEM",
        ":focus within 5 of resn HEM",
        ":focus off"}, "View");

    // :dssp — Re-run DSSP secondary-structure assignment on the current
    // state. Useful for trajectory frames where the loader's initial SS
    // (from headers, if any) doesn't reflect the current conformation.
    cmdRegistry_.registerCmd("dssp", [](Application& app, const ParsedCommand&) -> ExecResult {
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
        " cp pLDDT    cr rainbow",
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
