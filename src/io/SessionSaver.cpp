#include "molterm/io/SessionSaver.h"
#include "molterm/app/Application.h"
#include "molterm/cmd/Register.h"
#include "molterm/core/Logger.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

namespace molterm {

std::string SessionSaver::sessionPath() {
    const char* home = std::getenv("HOME");
    if (!home) return "";
    return std::string(home) + "/.molterm/autosave.toml";
}

// Stable token for a repr, used as the `repr_mask_<token>` autosave key and to
// match against the comma-joined `reprs` list.
static const char* reprKeyName(ReprType r) {
    switch (r) {
        case ReprType::Wireframe: return "wireframe";
        case ReprType::BallStick: return "ballstick";
        case ReprType::Spacefill: return "spacefill";
        case ReprType::Cartoon:   return "cartoon";
        case ReprType::Ribbon:    return "ribbon";
        case ReprType::Backbone:  return "backbone";
        case ReprType::Surface:   return "surface";
    }
    return "";
}

// Every repr, in a fixed order — so save/restore iterate identically.
static constexpr ReprType kAllReprs[] = {
    ReprType::Wireframe, ReprType::BallStick, ReprType::Spacefill,
    ReprType::Cartoon, ReprType::Ribbon, ReprType::Backbone, ReprType::Surface};

// Autosave key prefixes for per-atom state — shared by save + restore so the
// length stripped on parse stays in sync with the emitted key.
static constexpr char kReprMaskPrefix[] = "repr_mask_";
static constexpr char kColorOvrPrefix[] = "color_override_";

// Encode the true bits of a per-atom mask as comma-separated inclusive index
// ranges ("12,40-211") — compact because ligand/polymer atoms are contiguous.
static std::string encodeRanges(const std::vector<bool>& mask) {
    std::string s;
    const int n = static_cast<int>(mask.size());
    for (int i = 0; i < n; ) {
        if (!mask[i]) { ++i; continue; }
        int j = i;
        while (j + 1 < n && mask[j + 1]) ++j;
        if (!s.empty()) s += ",";
        s += (i == j) ? std::to_string(i)
                      : std::to_string(i) + "-" + std::to_string(j);
        i = j + 1;
    }
    return s;
}

// Same, over an ascending index list (per-color groups are naturally sorted
// since atoms are scanned in order) — avoids a full N-bool mask per color.
static std::string encodeRanges(const std::vector<int>& idx) {
    std::string s;
    for (size_t k = 0; k < idx.size(); ) {
        int a = idx[k], b = a;
        size_t m = k;
        while (m + 1 < idx.size() && idx[m + 1] == idx[m] + 1) b = idx[++m];
        if (!s.empty()) s += ",";
        s += (a == b) ? std::to_string(a)
                      : std::to_string(a) + "-" + std::to_string(b);
        k = m + 1;
    }
    return s;
}

static std::vector<int> decodeRanges(const std::string& s) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty()) continue;
        auto dash = tok.find('-');
        if (dash == std::string::npos) {
            out.push_back(std::stoi(tok));
        } else {
            int a = std::stoi(tok.substr(0, dash));
            int b = std::stoi(tok.substr(dash + 1));
            for (int k = a; k <= b; ++k) out.push_back(k);
        }
    }
    return out;
}

bool SessionSaver::saveSession(const Application& app) {
    std::string path = sessionPath();
    if (path.empty()) return false;

    std::filesystem::create_directories(std::filesystem::path(path).parent_path());

    std::ofstream out(path);
    if (!out) return false;

    out << "# MolTerm autosave session\n\n";

    // Renderer
    out << "[session]\n";
    switch (app.rendererType()) {
        case RendererType::Ascii:   out << "renderer = \"ascii\"\n"; break;
        case RendererType::Block:   out << "renderer = \"block\"\n"; break;
        case RendererType::Pixel:   out << "renderer = \"pixel\"\n"; break;
        default:                    out << "renderer = \"braille\"\n"; break;
    }
    out << "fog = " << app.fogStrength() << "\n";
    out << "\n";

    // Save each tab
    auto& tabMgr = app.tabs();
    for (int t = 0; t < static_cast<int>(tabMgr.count()); ++t) {
        const auto& tab = tabMgr.tab(t);
        out << "[[tab]]\n";
        out << "name = \"" << tab.name() << "\"\n";

        // Camera state
        const auto& cam = tab.camera();
        const auto& rot = cam.rotation();
        out << "camera_rot = [";
        for (int i = 0; i < 9; ++i) {
            if (i > 0) out << ", ";
            out << rot[i];
        }
        out << "]\n";
        out << "camera_center = [" << cam.centerX() << ", "
            << cam.centerY() << ", " << cam.centerZ() << "]\n";
        out << "camera_zoom = " << cam.zoom() << "\n";
        out << "camera_pan = [" << cam.panXOffset() << ", " << cam.panYOffset() << "]\n";

        // Objects in this tab
        for (const auto& obj : tab.objects()) {
            out << "\n[[tab.object]]\n";
            out << "name = \"" << obj->name() << "\"\n";
            out << "path = \"" << obj->sourcePath() << "\"\n";
            out << "visible = " << (obj->visible() ? "true" : "false") << "\n";
            out << "atom_count = " << obj->atoms().size() << "\n";

            // Color scheme
            switch (obj->colorScheme()) {
                case ColorScheme::Element:            out << "color_scheme = \"element\"\n"; break;
                case ColorScheme::Chain:              out << "color_scheme = \"chain\"\n"; break;
                case ColorScheme::SecondaryStructure: out << "color_scheme = \"ss\"\n"; break;
                case ColorScheme::BFactor:            out << "color_scheme = \"bfactor\"\n"; break;
                case ColorScheme::PLDDT:              out << "color_scheme = \"plddt\"\n"; break;
                case ColorScheme::Rainbow:            out << "color_scheme = \"rainbow\"\n"; break;
                case ColorScheme::ResType:            out << "color_scheme = \"restype\"\n"; break;
                default:                              out << "color_scheme = \"element\"\n"; break;
            }

            // Active representations (object-level flags)
            std::string reprs;
            for (ReprType r : kAllReprs)
                if (obj->reprVisible(r)) { reprs += reprKeyName(r); reprs += ","; }
            if (!reprs.empty()) reprs.pop_back();  // remove trailing comma
            out << "reprs = \"" << reprs << "\"\n";

            // Per-atom repr masks (e.g. `show wire ligand`) — saved as index
            // ranges so a ligand-only repr round-trips on :resume instead of
            // expanding to the whole object. Only emitted for a real subset.
            for (ReprType r : kAllReprs) {
                if (!obj->reprVisible(r)) continue;
                auto mask = obj->atomVisMask(r);
                if (mask.empty()) continue;  // no per-atom state → whole object
                if (std::all_of(mask.begin(), mask.end(), [](bool b) { return b; }))
                    continue;                // effectively whole object
                out << kReprMaskPrefix << reprKeyName(r) << " = \""
                    << encodeRanges(mask) << "\"\n";
            }

            // Per-atom color overrides (e.g. `color element ligand`) — saved as
            // index ranges per colorPair id (the stable kColor*/packed-RGB
            // value, no name round-trip). Without this a ligand resumes in the
            // scheme color, not CPK. Grouping shared with the PyMOL exporter.
            for (const auto& [cid, idx] : obj->colorGroups())
                out << kColorOvrPrefix << cid << " = \"" << encodeRanges(idx) << "\"\n";

            // Multi-state
            if (obj->stateCount() > 1)
                out << "active_state = " << obj->activeState() << "\n";
        }
        out << "\n";
    }

    // Typed registers (`:let` from issues #32, #33, #35). Each register
    // round-trips its kind + value so `:resume` recovers exact values
    // without re-evaluating the original expression — important when the
    // source structure has changed since the autosave (re-evaluating
    // `pos(A:1:CA)` against a different model would silently drift).
    for (const auto& [name, r] : app.registers()) {
        out << "\n[[register]]\n";
        out << "name = \"" << name << "\"\n";
        if (!r.expr.empty()) out << "expr = \"" << r.expr << "\"\n";
        switch (r.kind) {
            case Register::Kind::Scalar:
                out << "kind = \"scalar\"\n";
                out << "scalar = " << r.scalar << "\n";
                break;
            case Register::Kind::Vec3:
                out << "kind = \"vec3\"\n";
                out << "vec = [" << r.vec[0] << ", " << r.vec[1] << ", " << r.vec[2] << "]\n";
                break;
            case Register::Kind::Pca:
                out << "kind = \"pca\"\n";
                out << "center = [" << r.pca.center[0] << ", " << r.pca.center[1] << ", " << r.pca.center[2] << "]\n";
                out << "axis1 = ["  << r.pca.axis1[0]  << ", " << r.pca.axis1[1]  << ", " << r.pca.axis1[2]  << "]\n";
                out << "axis2 = ["  << r.pca.axis2[0]  << ", " << r.pca.axis2[1]  << ", " << r.pca.axis2[2]  << "]\n";
                out << "axis3 = ["  << r.pca.axis3[0]  << ", " << r.pca.axis3[1]  << ", " << r.pca.axis3[2]  << "]\n";
                out << "eigvals = ["<< r.pca.eigvals[0]<< ", " << r.pca.eigvals[1]<< ", " << r.pca.eigvals[2]<< "]\n";
                // superpose_axis() stows its rotation angle + post-fit residual
                // here (not in eigvals); persist them so `$reg.angle`/`$reg.rmsd`
                // survive a session round-trip. 0 for pca()/helix_axis().
                out << "angle = " << r.pca.angle << "\n";
                out << "rmsd = "  << r.pca.rmsd  << "\n";
                // Producer tag (0 = pca, 1 = helix, 2 = superpose) so the
                // angle/eig field-access rules survive a session round-trip.
                out << "source = " << static_cast<int>(r.pca.source) << "\n";
                break;
        }
    }

    MLOG_INFO("Session saved to %s", path.c_str());
    return true;
}

std::string SessionSaver::restoreSession(Application& app) {
    std::string path = sessionPath();
    if (path.empty() || !std::filesystem::exists(path))
        return "No saved session found";

    // Simple line-by-line parser (avoid toml++ dependency for this small file)
    std::ifstream in(path);
    if (!in) return "Cannot read " + path;

    MLOG_INFO("Restoring session from %s", path.c_str());

    struct ObjState {
        std::string name;
        std::string filePath;
        bool visible = true;
        std::string colorScheme = "element";
        std::string reprs = "wireframe";
        std::map<std::string, std::string> reprMasks;  // repr token → index ranges
        std::map<int, std::string> colorOverrides;     // colorPair id → index ranges
        int atomCount = -1;                            // -1 = unknown (old autosave)
        int activeState = 0;
    };
    struct TabState {
        std::string name;
        std::array<float, 9> rot = {1,0,0, 0,1,0, 0,0,1};
        float cx = 0, cy = 0, cz = 0;
        float zoom = 1;
        float panX = 0, panY = 0;
        std::vector<ObjState> objects;
    };

    std::string renderer = "braille";
    float fog = 0.35f;
    std::vector<TabState> tabs;
    TabState* curTab = nullptr;
    ObjState* curObj = nullptr;
    // Pending registers — accumulated as we walk the file and applied
    // to app.registers() after the tab-level state restore so the
    // typed-value snapshot wins regardless of section order.
    struct PendingReg {
        std::string name;
        std::string kind = "scalar";
        std::string expr;
        double scalar = 0;
        std::array<double, 3> vec{};
        geom::PcaResult pca;
    };
    std::vector<PendingReg> pendingRegs;
    PendingReg* curReg = nullptr;
    auto parseTriple = [](const std::string& v, std::array<double, 3>& out) {
        std::string s = v;
        if (!s.empty() && s.front() == '[') s = s.substr(1);
        if (!s.empty() && s.back() == ']')  s.pop_back();
        std::istringstream ss(s);
        for (int i = 0; i < 3; ++i) {
            ss >> out[i]; if (ss.peek() == ',') ss.ignore();
        }
    };

    std::string line;
    while (std::getline(in, line)) {
        // Trim
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue;
        line = line.substr(start);

        if (line == "[[tab]]") {
            tabs.emplace_back();
            curTab = &tabs.back();
            curObj = nullptr;
            curReg = nullptr;
            continue;
        }
        if (line == "[[tab.object]]") {
            if (!curTab) { tabs.emplace_back(); curTab = &tabs.back(); }
            curTab->objects.emplace_back();
            curObj = &curTab->objects.back();
            curReg = nullptr;
            continue;
        }
        if (line == "[[register]]") {
            pendingRegs.emplace_back();
            curReg = &pendingRegs.back();
            curObj = nullptr;
            // Stays at register-section regardless of which tab it lives
            // in — registers are app-level, not tab-level.
            continue;
        }

        // Parse key = value
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // Trim key/val
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val = val.substr(1);
        // Strip quotes
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
            val = val.substr(1, val.size() - 2);

        // Register-level keys take precedence over session-level when
        // we're inside a [[register]] section (they appear after at
        // least one [[tab]]; curReg is the explicit gate).
        if (curReg) {
            if      (key == "name")    curReg->name = val;
            else if (key == "kind")    curReg->kind = val;
            else if (key == "expr")    curReg->expr = val;
            else if (key == "scalar")  curReg->scalar = std::stod(val);
            else if (key == "vec")     parseTriple(val, curReg->vec);
            else if (key == "center")  parseTriple(val, curReg->pca.center);
            else if (key == "axis1")   parseTriple(val, curReg->pca.axis1);
            else if (key == "axis2")   parseTriple(val, curReg->pca.axis2);
            else if (key == "axis3")   parseTriple(val, curReg->pca.axis3);
            else if (key == "eigvals") parseTriple(val, curReg->pca.eigvals);
            else if (key == "angle")   curReg->pca.angle = std::stod(val);
            else if (key == "rmsd")    curReg->pca.rmsd = std::stod(val);
            else if (key == "source")  curReg->pca.source =
                static_cast<geom::PcaResult::Source>(std::stoi(val));
            continue;
        }

        // Session-level keys
        if (!curTab) {
            if (key == "renderer") renderer = val;
            else if (key == "fog") fog = std::stof(val);
            continue;
        }

        // Object-level keys
        if (curObj) {
            if (key == "name") curObj->name = val;
            else if (key == "path") curObj->filePath = val;
            else if (key == "visible") curObj->visible = (val == "true");
            else if (key == "color_scheme") curObj->colorScheme = val;
            else if (key == "reprs") curObj->reprs = val;
            else if (key == "atom_count") curObj->atomCount = std::stoi(val);
            else if (key.rfind(kReprMaskPrefix, 0) == 0)
                curObj->reprMasks[key.substr(sizeof(kReprMaskPrefix) - 1)] = val;
            else if (key.rfind(kColorOvrPrefix, 0) == 0)
                curObj->colorOverrides[std::stoi(key.substr(sizeof(kColorOvrPrefix) - 1))] = val;
            else if (key == "active_state") curObj->activeState = std::stoi(val);
            continue;
        }

        // Tab-level keys
        if (key == "name") curTab->name = val;
        else if (key == "camera_zoom") curTab->zoom = std::stof(val);
        else if (key == "camera_rot") {
            // Parse [f, f, f, ...]
            std::string inner = val;
            if (!inner.empty() && inner.front() == '[') inner = inner.substr(1);
            if (!inner.empty() && inner.back() == ']') inner.pop_back();
            std::istringstream ss(inner);
            for (int i = 0; i < 9; ++i) {
                ss >> curTab->rot[i];
                if (ss.peek() == ',') ss.ignore();
            }
        } else if (key == "camera_center") {
            std::string inner = val;
            if (!inner.empty() && inner.front() == '[') inner = inner.substr(1);
            if (!inner.empty() && inner.back() == ']') inner.pop_back();
            std::istringstream ss(inner);
            ss >> curTab->cx; if (ss.peek() == ',') ss.ignore();
            ss >> curTab->cy; if (ss.peek() == ',') ss.ignore();
            ss >> curTab->cz;
        } else if (key == "camera_pan") {
            std::string inner = val;
            if (!inner.empty() && inner.front() == '[') inner = inner.substr(1);
            if (!inner.empty() && inner.back() == ']') inner.pop_back();
            std::istringstream ss(inner);
            ss >> curTab->panX; if (ss.peek() == ',') ss.ignore();
            ss >> curTab->panY;
        }
    }

    // Apply renderer
    if (renderer == "ascii")       app.setRenderer(RendererType::Ascii);
    else if (renderer == "block")  app.setRenderer(RendererType::Block);
    else if (renderer == "pixel")  app.setRenderer(RendererType::Pixel);
    else                           app.setRenderer(RendererType::Braille);
    app.setFogStrength(fog);

    int filesLoaded = 0;
    int filesFailed = 0;

    for (size_t ti = 0; ti < tabs.size(); ++ti) {
        const auto& ts = tabs[ti];
        // Use existing first tab, create new ones after
        if (ti > 0) {
            app.tabs().addTab(ts.name);
            app.tabs().goToTab(static_cast<int>(ti));
        } else if (!ts.name.empty()) {
            app.tabs().currentTab().setName(ts.name);
        }

        for (const auto& os : ts.objects) {
            if (os.filePath.empty()) { ++filesFailed; continue; }
            std::string msg = app.loadFile(os.filePath);
            if (msg.find("Error") != std::string::npos) { ++filesFailed; continue; }
            ++filesLoaded;

            auto obj = app.tabs().currentTab().currentObject();
            if (!obj) continue;

            // Restore name
            if (!os.name.empty() && os.name != obj->name())
                app.store().rename(obj->name(), os.name);

            // Restore visibility
            obj->setVisible(os.visible);

            // Restore color scheme
            if (os.colorScheme == "chain")        obj->setColorScheme(ColorScheme::Chain);
            else if (os.colorScheme == "ss")      obj->setColorScheme(ColorScheme::SecondaryStructure);
            else if (os.colorScheme == "bfactor") obj->setColorScheme(ColorScheme::BFactor);
            else if (os.colorScheme == "plddt")   obj->setColorScheme(ColorScheme::PLDDT);
            else if (os.colorScheme == "rainbow") obj->setColorScheme(ColorScheme::Rainbow);
            else if (os.colorScheme == "restype") obj->setColorScheme(ColorScheme::ResType);
            else                                  obj->setColorScheme(ColorScheme::Element);

            // Per-atom state is positional (atom index), so only trust it when
            // the reloaded file still has the same atom count — otherwise a
            // changed source would silently mis-show/mis-paint. On mismatch fall
            // back to object-level reprs (the pre-per-atom behavior).
            const bool perAtomOk =
                os.atomCount < 0 ||
                os.atomCount == static_cast<int>(obj->atoms().size());
            if (!perAtomOk)
                MLOG_INFO("resume: %s atom count changed (%d -> %zu); "
                          "restoring object-level reprs only",
                          obj->name().c_str(), os.atomCount, obj->atoms().size());

            // Restore representations, honoring per-atom masks when valid: a
            // saved `repr_mask_<token>` re-applies the subset (e.g. ligand-only
            // wireframe) instead of showing the repr on every atom.
            obj->hideAllRepr();
            for (ReprType r : kAllReprs) {
                const char* token = reprKeyName(r);
                if (os.reprs.find(token) == std::string::npos) continue;
                auto it = os.reprMasks.find(token);
                if (perAtomOk && it != os.reprMasks.end())
                    obj->showReprForAtoms(r, decodeRanges(it->second));
                else
                    obj->showRepr(r);
            }

            // Restore per-atom color overrides (cleared first so loader smart-
            // defaults don't bleed through). Only when valid and recorded —
            // keeps older autosaves and `color clear` states intact.
            if (perAtomOk && !os.colorOverrides.empty()) {
                obj->clearAtomColors();
                for (const auto& [cid, ranges] : os.colorOverrides)
                    obj->setAtomColors(decodeRanges(ranges), cid);
            }

            // Restore multi-state
            if (os.activeState > 0 && obj->stateCount() > 1)
                obj->setActiveState(os.activeState);
        }

        // Restore camera
        auto& cam = app.tabs().currentTab().camera();
        cam.setRotation(ts.rot);
        cam.setCenter(ts.cx, ts.cy, ts.cz);
        cam.setZoom(ts.zoom);
        cam.pan(ts.panX, ts.panY);
    }

    // Go back to first tab
    if (tabs.size() > 1) app.tabs().goToTab(0);

    // Apply registers — pca.valid is set true so consumers don't have to
    // know that the snapshot bypasses pcaOf().
    for (const auto& pr : pendingRegs) {
        if (pr.name.empty()) continue;
        Register r;
        r.expr = pr.expr;
        if      (pr.kind == "scalar") { r.kind = Register::Kind::Scalar; r.scalar = pr.scalar; }
        else if (pr.kind == "vec3")   { r.kind = Register::Kind::Vec3;   r.vec    = pr.vec; }
        else if (pr.kind == "pca") {
            r.kind = Register::Kind::Pca;
            r.pca = pr.pca;
            r.pca.valid = true;
        } else continue;  // unknown kind — skip rather than poison the table
        app.registers()[pr.name] = std::move(r);
    }

    std::string result = "Restored session: " + std::to_string(filesLoaded) + " files";
    if (filesFailed > 0) result += " (" + std::to_string(filesFailed) + " failed)";
    if (!pendingRegs.empty())
        result += ", " + std::to_string(pendingRegs.size()) + " registers";
    MLOG_INFO("%s", result.c_str());
    return result;
}

} // namespace molterm
