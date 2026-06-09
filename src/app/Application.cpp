#include "molterm/app/Application.h"

#include "molterm/app/CommandScope.h"
#include "molterm/app/ScriptRunner.h"
#include "molterm/app/PathPatterns.h"
#include "molterm/cmd/CommandHelpers.h"
#include "molterm/cmd/CommandParser.h"
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
#include "molterm/io/SessionExporter.h"
#include "molterm/config/ConfigParser.h"
#include "molterm/core/Logger.h"
#include "molterm/core/Selection.h"
#include "molterm/io/SessionSaver.h"

#include <chrono>
#include <cmath>
#include <cstdio>
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

// Global resize flag, set by the SIGWINCH handler and drained in run().
static volatile sig_atomic_t g_resized = 0;
static void resizeHandler(int) { g_resized = 1; }

// Clear pixel graphics artifacts and force full ncurses repaint
void clearScreenAndRepaint() {
    fprintf(stdout, "\033[2J");
    fflush(stdout);
    clearok(curscr, TRUE);
    wrefresh(curscr);
}

Application::Application() : script_(std::make_unique<ScriptRunner>(*this)) {}
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
    return script_->run(in, strict, args, sourcePath);
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
        bgModeName(view_.bgMode()), view_.outlineEnabled() ? "on" : "off",
        view_.fogStrength(), elapsedSec, visibleAtoms);
}

void Application::applyBgMode(PixelCanvas& pc) const {
    switch (view_.bgMode()) {
        case BgMode::Transparent: pc.setBackground(0,   0,   0,   true);  break;
        case BgMode::White:       pc.setBackground(255, 255, 255, false); break;
        case BgMode::Black:       pc.setBackground(0,   0,   0,   false); break;
        case BgMode::Custom:      pc.setBackground(view_.bgCustomR(), view_.bgCustomG(), view_.bgCustomB(), false); break;
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
