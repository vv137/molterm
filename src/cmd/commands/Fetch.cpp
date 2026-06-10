// Remote / IO: :fetch/:assembly/:screenshot.


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
#include <filesystem>
#include "molterm/render/PixelCanvas.h"
#include "molterm/io/CifLoader.h"
#include "molterm/repr/WireframeRepr.h"
#include "molterm/core/Logger.h"

namespace molterm {

void Application::registerFetchCommands(CommandRegistry& reg) {
    // :fetch <pdb_id> — download from RCSB PDB or AlphaFold DB
    reg.registerCmd("fetch", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
    reg.registerCmd("assembly", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
    reg.registerCmd("screenshot", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
            auto dpi = parseInt(positional.back());
            if (!dpi) {
                return {false, "Usage: :screenshot [file.png] [W H [DPI]]"};
            }
            reqDpi = *dpi;
            positional.pop_back();
            if (reqDpi < 1 || reqDpi > 4800) {
                return {false, "DPI out of range (1..4800)"};
            }
        }
        if (positional.size() >= 3) {
            auto w = parseInt(positional[positional.size() - 2]);
            auto h = parseInt(positional[positional.size() - 1]);
            if (!w || !h) {
                return {false, "Usage: :screenshot [file.png] [W H [DPI]]"};
            }
            reqPixW = *w;
            reqPixH = *h;
            positional.pop_back();
            positional.pop_back();
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
            // The live framebuffer can still hold the *previous* frame: a
            // screenshot runs inside processInput(), before the loop renders
            // the deferred frame. When a redraw is pending, reconcile it first
            // so pending state — :set bg, repr toggles, camera moves made this
            // turn — lands in the PNG instead of lagging a frame behind. Gated
            // on needsRedraw_ so the steady state isn't double-rendered.
            if (pc && app.needsRedraw_) app.renderViewport();
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

        // Re-fit a :focus/:zoom/:orient view to the *actual* output size so
        // the subject fills the frame at any resolution/aspect (issue #98).
        // Saved and restored around the render so the live viewport keeps
        // its own zoom.
        const float savedFitZoom = tab.camera().zoom();
        const bool reFitForShot = app.hasViewFit();
        if (reFitForShot)
            app.applyViewFit(offscreen.subW(), offscreen.subH(),
                             offscreen.aspectYX());

        if (auto* wf = dynamic_cast<WireframeRepr*>(app.getRepr(ReprType::Wireframe))) {
            wf->setHeteroatomCarbonScheme(app.interfaceOverlay_ || app.focus().snapshot.active);
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
        } else if (!app.focus().atomMask.empty()) {
            dimMask = &app.focus().atomMask;
        }
        if (dimMask) offscreen.applyFocusDim(*dimMask, app.focus().dimStrength);

        if ((app.interfaceOverlay_ || app.focus().snapshot.active) &&
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

        // Restore the live viewport's zoom (the re-fit above was for the
        // screenshot's dimensions only) and projection. Issue #98.
        if (reFitForShot) tab.camera().setZoom(savedFitZoom);
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

}

}  // namespace molterm
