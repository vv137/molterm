// Options: :set / :get.


#include "molterm/app/Application.h"
#include "molterm/cmd/CommandParser.h"
#include "molterm/cmd/CommandRegistry.h"
#include "molterm/cmd/CommandHelpers.h"
#include "molterm/cmd/Register.h"
#include "molterm/cmd/RegisterExpr.h"
#include "molterm/cmd/ExprContext.h"
#include "molterm/core/MolObject.h"
#include "molterm/core/Selection.h"
#include "molterm/repr/CartoonRepr.h"
#include "molterm/repr/SpacefillRepr.h"
#include "molterm/repr/SurfaceRepr.h"
#include "molterm/repr/RibbonRepr.h"
#include "molterm/repr/WireframeRepr.h"
#include "molterm/repr/BallStickRepr.h"
#include "molterm/repr/BackboneRepr.h"

namespace molterm {

void Application::registerSettingsCommands(CommandRegistry& reg) {
    // :set <option> [value]
    reg.registerCmd("set", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
            if (cmd.args.size() < 2) return {false, "Usage: :set wireframe_thickness <0.01-1.0>"};
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
        if (opt == "transcript_lines") {
            if (cmd.args.size() < 2) return {false, "Usage: :set transcript_lines <1..30>"};
            int n = std::stoi(cmd.args[1]);
            if (n < 1 || n > 30) return {false, "transcript_lines out of range (1..30)"};
            app.setTranscriptHintLines(n);
            return {true, "transcript_lines = " + std::to_string(n)};
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
                return {false, "Usage: :set wf_thickness <0.01-1.0>"};
            auto* wf = dynamic_cast<WireframeRepr*>(
                app.getRepr(ReprType::Wireframe));
            if (!wf) return {false, "Wireframe repr not found"};
            wf->setThickness(std::stof(cmd.args[1]));
            return {true, "Wireframe thickness: " + std::to_string(wf->thickness())};
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
        if (opt == "cartoon_sheet_smooth" || opt == "css") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_sheet_smooth <0-10>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            ct->setSheetSmooth(std::stoi(cmd.args[1]));
            return {true, "Cartoon sheet smoothing passes: " +
                          std::to_string(ct->sheetSmooth())};
        }
        if (opt == "cartoon_sheet_height" || opt == "cshh") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_sheet_height <0.01-2.0>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            ct->setSheetHeight(std::stof(cmd.args[1]));
            return {true, "Cartoon sheet half-height: " +
                          std::to_string(ct->sheetHeight()) + " Å"};
        }
        if (opt == "cartoon_tension" || opt == "cstn") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_tension <0.0-1.0>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            ct->setTension(std::stof(cmd.args[1]));
            return {true, "Cartoon loop/sheet tension: " +
                          std::to_string(ct->tension())};
        }
        if (opt == "cartoon_helix_tension" || opt == "chtn") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_helix_tension <0.0-1.0>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            ct->setHelixTension(std::stof(cmd.args[1]));
            return {true, "Cartoon helix tension: " +
                          std::to_string(ct->helixTension())};
        }
        if (opt == "cartoon_sheet_flat" || opt == "csf") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_sheet_flat <0.0-1.0>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            ct->setSheetFlat(std::stof(cmd.args[1]));
            return {true, "Cartoon sheet flatten (C→O blend): " +
                          std::to_string(ct->sheetFlat())};
        }
        if (opt == "cartoon_arrow_width" || opt == "caw") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_arrow_width <1.0-4.0>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            ct->setArrowWidth(std::stof(cmd.args[1]));
            return {true, "Cartoon arrowhead width scale: " +
                          std::to_string(ct->arrowWidth()) + "×"};
        }
        if (opt == "cartoon_frame_smooth" || opt == "cfs") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_frame_smooth <0-10>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            ct->setFrameSmooth(std::stoi(cmd.args[1]));
            return {true, "Cartoon frame smoothing passes: " +
                          std::to_string(ct->frameSmooth())};
        }
        if (opt == "cartoon_width_smooth" || opt == "cws") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_width_smooth <0-10>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            ct->setWidthSmooth(std::stoi(cmd.args[1]));
            return {true, "Cartoon width smoothing passes: " +
                          std::to_string(ct->widthSmooth())};
        }
        if (opt == "cartoon_nucleic_width" || opt == "cnw") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_nucleic_width <0.05-2.0>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            ct->setNucleicWidth(std::stof(cmd.args[1]));
            return {true, "Cartoon nucleic ribbon half-width: " +
                          std::to_string(ct->nucleicWidth()) + " Å"};
        }
        if (opt == "cartoon_nucleic_height" || opt == "cnh") {
            if (cmd.args.size() < 2) return {false, "Usage: :set cartoon_nucleic_height <0.05-2.0>"};
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            ct->setNucleicHeight(std::stof(cmd.args[1]));
            return {true, "Cartoon nucleic ribbon half-height: " +
                          std::to_string(ct->nucleicHeight()) + " Å"};
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
        if (opt == "surface_mode" || opt == "surf_mode") {
            if (cmd.args.size() < 2) return {false, "Usage: :set surface_mode ses|sas|vdw|gaussian"};
            auto* sr = dynamic_cast<SurfaceRepr*>(app.getRepr(ReprType::Surface));
            if (!sr) return {false, "Surface repr not found"};
            const std::string& m = cmd.args[1];
            if (m == "ses")           sr->setMode(SurfaceRepr::Mode::Ses);
            else if (m == "sas")      sr->setMode(SurfaceRepr::Mode::Sas);
            else if (m == "vdw")      sr->setMode(SurfaceRepr::Mode::Vdw);
            else if (m == "gaussian" || m == "gauss")
                                      sr->setMode(SurfaceRepr::Mode::Gaussian);
            else return {false, "surface_mode: ses|sas|vdw|gaussian"};
            return {true, "Surface mode: " + m};
        }
        if (opt == "surface_probe" || opt == "surf_probe") {
            if (cmd.args.size() < 2) return {false, "Usage: :set surface_probe <0.0-3.0>"};
            auto* sr = dynamic_cast<SurfaceRepr*>(app.getRepr(ReprType::Surface));
            if (!sr) return {false, "Surface repr not found"};
            sr->setProbe(std::stof(cmd.args[1]));
            return {true, "Surface probe radius: " + std::to_string(sr->probe()) + " Å"};
        }
        if (opt == "surface_resolution" || opt == "surf_res") {
            if (cmd.args.size() < 2) return {false, "Usage: :set surface_resolution <0.2-3.0>"};
            auto* sr = dynamic_cast<SurfaceRepr*>(app.getRepr(ReprType::Surface));
            if (!sr) return {false, "Surface repr not found"};
            sr->setResolution(std::stof(cmd.args[1]));
            return {true, "Surface grid spacing: " + std::to_string(sr->resolution()) + " Å"};
        }
        if (opt == "surface_scale" || opt == "surf_scale") {
            if (cmd.args.size() < 2) return {false, "Usage: :set surface_scale <0.2-3.0>"};
            auto* sr = dynamic_cast<SurfaceRepr*>(app.getRepr(ReprType::Surface));
            if (!sr) return {false, "Surface repr not found"};
            sr->setScale(std::stof(cmd.args[1]));
            return {true, "Surface blob radius (×vdW): " + std::to_string(sr->scale())};
        }
        if (opt == "surface_smoothness" || opt == "surf_smooth") {
            if (cmd.args.size() < 2) return {false, "Usage: :set surface_smoothness <0.5-8.0>"};
            auto* sr = dynamic_cast<SurfaceRepr*>(app.getRepr(ReprType::Surface));
            if (!sr) return {false, "Surface repr not found"};
            sr->setSmoothness(std::stof(cmd.args[1]));
            return {true, "Surface smoothness (kernel k): " + std::to_string(sr->smoothness())};
        }
        if (opt == "surface_iso" || opt == "surf_iso") {
            if (cmd.args.size() < 2) return {false, "Usage: :set surface_iso <0.05-5.0>"};
            auto* sr = dynamic_cast<SurfaceRepr*>(app.getRepr(ReprType::Surface));
            if (!sr) return {false, "Surface repr not found"};
            sr->setIsoValue(std::stof(cmd.args[1]));
            return {true, "Surface iso-level: " + std::to_string(sr->isoValue())};
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
    reg.registerCmd("get", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
        if (opt == "transcript_lines")
            return {true, "transcript_lines = " + std::to_string(app.transcriptHintLines())};
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
        if (opt == "cartoon_sheet_smooth" || opt == "css") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_sheet_smooth = " + std::to_string(ct->sheetSmooth())};
        }
        if (opt == "cartoon_sheet_height" || opt == "cshh") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_sheet_height = " + std::to_string(ct->sheetHeight())};
        }
        if (opt == "cartoon_tension" || opt == "cstn") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_tension = " + std::to_string(ct->tension())};
        }
        if (opt == "cartoon_helix_tension" || opt == "chtn") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_helix_tension = " + std::to_string(ct->helixTension())};
        }
        if (opt == "cartoon_sheet_flat" || opt == "csf") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_sheet_flat = " + std::to_string(ct->sheetFlat())};
        }
        if (opt == "cartoon_arrow_width" || opt == "caw") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_arrow_width = " + std::to_string(ct->arrowWidth())};
        }
        if (opt == "cartoon_frame_smooth" || opt == "cfs") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_frame_smooth = " + std::to_string(ct->frameSmooth())};
        }
        if (opt == "cartoon_width_smooth" || opt == "cws") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_width_smooth = " + std::to_string(ct->widthSmooth())};
        }
        if (opt == "cartoon_nucleic_width" || opt == "cnw") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_nucleic_width = " + std::to_string(ct->nucleicWidth())};
        }
        if (opt == "cartoon_nucleic_height" || opt == "cnh") {
            auto* ct = dynamic_cast<CartoonRepr*>(app.getRepr(ReprType::Cartoon));
            if (!ct) return {false, "Cartoon repr not found"};
            return {true, "cartoon_nucleic_height = " + std::to_string(ct->nucleicHeight())};
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
        if (opt == "surface_mode" || opt == "surf_mode") {
            auto* sr = dynamic_cast<SurfaceRepr*>(app.getRepr(ReprType::Surface));
            if (!sr) return {false, "Surface repr not found"};
            const char* m = "ses";
            switch (sr->mode()) {
                case SurfaceRepr::Mode::Ses:      m = "ses"; break;
                case SurfaceRepr::Mode::Sas:      m = "sas"; break;
                case SurfaceRepr::Mode::Vdw:      m = "vdw"; break;
                case SurfaceRepr::Mode::Gaussian: m = "gaussian"; break;
            }
            return {true, std::string("surface_mode = ") + m};
        }
        if (opt == "surface_probe" || opt == "surf_probe") {
            auto* sr = dynamic_cast<SurfaceRepr*>(app.getRepr(ReprType::Surface));
            if (!sr) return {false, "Surface repr not found"};
            return {true, "surface_probe = " + std::to_string(sr->probe())};
        }
        if (opt == "surface_resolution" || opt == "surf_res") {
            auto* sr = dynamic_cast<SurfaceRepr*>(app.getRepr(ReprType::Surface));
            if (!sr) return {false, "Surface repr not found"};
            return {true, "surface_resolution = " + std::to_string(sr->resolution())};
        }
        if (opt == "surface_scale" || opt == "surf_scale") {
            auto* sr = dynamic_cast<SurfaceRepr*>(app.getRepr(ReprType::Surface));
            if (!sr) return {false, "Surface repr not found"};
            return {true, "surface_scale = " + std::to_string(sr->scale())};
        }
        if (opt == "surface_smoothness" || opt == "surf_smooth") {
            auto* sr = dynamic_cast<SurfaceRepr*>(app.getRepr(ReprType::Surface));
            if (!sr) return {false, "Surface repr not found"};
            return {true, "surface_smoothness = " + std::to_string(sr->smoothness())};
        }
        if (opt == "surface_iso" || opt == "surf_iso") {
            auto* sr = dynamic_cast<SurfaceRepr*>(app.getRepr(ReprType::Surface));
            if (!sr) return {false, "Surface repr not found"};
            return {true, "surface_iso = " + std::to_string(sr->isoValue())};
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

}

}  // namespace molterm
