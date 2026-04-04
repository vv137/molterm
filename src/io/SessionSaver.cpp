#include "molterm/io/SessionSaver.h"
#include "molterm/app/Application.h"
#include "molterm/core/Logger.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace molterm {

std::string SessionSaver::sessionPath() {
    const char* home = std::getenv("HOME");
    if (!home) return "";
    return std::string(home) + "/.molterm/autosave.toml";
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

            // Active representations
            std::string reprs;
            if (obj->reprVisible(ReprType::Wireframe)) reprs += "wireframe,";
            if (obj->reprVisible(ReprType::BallStick)) reprs += "ballstick,";
            if (obj->reprVisible(ReprType::Spacefill)) reprs += "spacefill,";
            if (obj->reprVisible(ReprType::Cartoon))   reprs += "cartoon,";
            if (obj->reprVisible(ReprType::Ribbon))    reprs += "ribbon,";
            if (obj->reprVisible(ReprType::Backbone))  reprs += "backbone,";
            if (!reprs.empty()) reprs.pop_back();  // remove trailing comma
            out << "reprs = \"" << reprs << "\"\n";

            // Multi-state
            if (obj->stateCount() > 1)
                out << "active_state = " << obj->activeState() << "\n";
        }
        out << "\n";
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
            continue;
        }
        if (line == "[[tab.object]]") {
            if (!curTab) { tabs.emplace_back(); curTab = &tabs.back(); }
            curTab->objects.emplace_back();
            curObj = &curTab->objects.back();
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

            // Restore representations
            obj->hideAllRepr();
            if (os.reprs.find("wireframe") != std::string::npos) obj->showRepr(ReprType::Wireframe);
            if (os.reprs.find("ballstick") != std::string::npos) obj->showRepr(ReprType::BallStick);
            if (os.reprs.find("spacefill") != std::string::npos) obj->showRepr(ReprType::Spacefill);
            if (os.reprs.find("cartoon") != std::string::npos)   obj->showRepr(ReprType::Cartoon);
            if (os.reprs.find("ribbon") != std::string::npos)    obj->showRepr(ReprType::Ribbon);
            if (os.reprs.find("backbone") != std::string::npos)  obj->showRepr(ReprType::Backbone);

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

    std::string result = "Restored session: " + std::to_string(filesLoaded) + " files";
    if (filesFailed > 0) result += " (" + std::to_string(filesFailed) + " failed)";
    MLOG_INFO("%s", result.c_str());
    return result;
}

} // namespace molterm
