// Camera / framing: :center/:zoom/:orient/:turn/:camera/:focus.


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
#include <fstream>

namespace molterm {

void Application::registerViewCommands(CommandRegistry& reg) {
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
    reg.registerCmd("center", [collectAtomCoords, computeUnion](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
    reg.registerCmd("zoom", [collectAtomCoords, computeUnion](Application& app, const ParsedCommand& cmd) -> ExecResult {
        auto g = collectAtomCoords(app, cmd);
        if (g.xs.empty()) return {false, "No atoms match"};
        auto [cx, cy, cz, span] = computeUnion(g);
        app.tabs().currentTab().camera().setCenter(cx, cy, cz);
        // Fit the projected extent to the frame, re-fittable per output
        // canvas so a hi-DPI :screenshot keeps the same framing (issue #98).
        app.setViewFit(g.xs, g.ys, g.zs, /*fill*/ 0.9f, /*pad*/ 1.0f,
                       /*minExtent*/ 1.0f);
        std::string msg = "Zoomed to " + std::to_string(g.xs.size()) + " atoms";
        if (g.objs > 1) msg += " (" + std::to_string(g.objs) + " objects)";
        app.logViewState(cmd, static_cast<int>(g.xs.size()));
        return {true, msg};
    }, ":zoom [selection]", "Center and zoom to fit the selection (or whole object)",
       {":zoom", ":zoom chain A", ":zoom resi 50-80"}, "View");

    // :orient [view <vx>,<vy>,<vz>] [selection] — align PCA axes, optionally view from a
    // direction expressed in the PCA frame (e1=longest, e2=mid, e3=shortest).
    // Default v_pca = (0,0,1): look down the shortest axis (flat face on screen).
    reg.registerCmd("orient", [collectAtomCoords, computeUnion](Application& app, const ParsedCommand& cmd) -> ExecResult {
        double vx = 0.0, vy = 0.0, vz = 1.0;
        int selStart = 0;
        // `view` interprets (vx,vy,vz) as coefficients in the PCA eigenbasis
        // of the selection (0 0 1 = down the shortest axis = default). `dir`
        // interprets them as a world-space view direction, so a script that
        // already holds a world axis (a pca().axis3, a bond vector) can aim
        // the camera straight down it (issue #105).
        bool worldDir = false;
        if (!cmd.args.empty() && (cmd.args[0] == "view" || cmd.args[0] == "dir")) {
            worldDir = (cmd.args[0] == "dir");
            if (cmd.args.size() >= 2 && !cmd.args[1].empty() && cmd.args[1][0] == '$') {
                // `:orient dir $reg [selection]` — a vec3 register supplies the
                // direction in one token (issue #105), so a script can aim the
                // camera down an axis it already computed (e.g. a groove normal
                // from `:run @lib/tcr_angles`).
                auto it = app.registers().find(cmd.args[1].substr(1));
                if (it == app.registers().end())
                    return {false, "no such register: " + cmd.args[1]};
                if (it->second.kind != Register::Kind::Vec3)
                    return {false, "register is not a vec3: " + cmd.args[1]};
                vx = it->second.vec[0]; vy = it->second.vec[1]; vz = it->second.vec[2];
                selStart = 2;
            } else {
                // Parser splits on whitespace AND commas, so "view 1,1,0" and
                // "view 1 1 0" both arrive as separate args.
                if (cmd.args.size() < 4) {
                    return {false, "Usage: :orient " + cmd.args[0] + " <vx> <vy> <vz> | $reg [selection]"};
                }
                auto px = parseDouble(cmd.args[1]);
                auto py = parseDouble(cmd.args[2]);
                auto pz = parseDouble(cmd.args[3]);
                if (!px || !py || !pz) {
                    return {false, "Invalid view vector: " + cmd.args[1] + " " + cmd.args[2] + " " + cmd.args[3]};
                }
                vx = *px; vy = *py; vz = *pz;
                selStart = 4;
            }
            double vlen = std::sqrt(vx*vx + vy*vy + vz*vz);
            if (vlen < 1e-10) return {false, "View vector cannot be zero"};
            vx /= vlen; vy /= vlen; vz /= vlen;
        }

        auto g = collectAtomCoords(app, cmd, selStart);
        if (g.xs.empty()) return {false, "No atoms match"};
        auto [cx, cy, cz, span] = computeUnion(g);
        auto& cam = app.tabs().currentTab().camera();
        cam.setCenter(cx, cy, cz);
        // Fit the projected extent to the frame (issue #98), recomputed per
        // output canvas. Applied under whatever rotation is current, so the
        // <2-atom and degenerate-PCA early returns fit before the rotation
        // and the main path fits after.
        auto fitToFrame = [&] { app.setViewFit(g.xs, g.ys, g.zs, 0.9f, 1.0f, 1.0f); };
        if (g.xs.size() < 2) {
            fitToFrame();
            return {true, "Centered (need >=2 atoms for orientation)"};
        }

        // PCA over the union of atom positions across in-scope objects.
        // The same primitive is exposed to scripts via `:let G = pca(...)`
        // (issue #33); both code paths come through geom::pcaOf so the
        // PCA frame chosen by `:orient` always matches the one a script
        // sees.
        auto pca = geom::pcaOf(g.xs, g.ys, g.zs);
        if (!pca.valid) {
            fitToFrame();
            return {true, "Centered (need >=2 atoms for orientation)"};
        }
        const auto& e1 = pca.axis1;
        const auto& e2 = pca.axis2;
        const auto& e3 = pca.axis3;

        // View direction in world space. `dir` is already a world vector;
        // `view`/default combine the eigenbasis coefficients into world space.
        double sz[3];
        if (worldDir) {
            sz[0] = vx; sz[1] = vy; sz[2] = vz;
        } else {
            sz[0] = vx*e1[0] + vy*e2[0] + vz*e3[0];
            sz[1] = vx*e1[1] + vy*e2[1] + vz*e3[1];
            sz[2] = vx*e1[2] + vy*e2[2] + vz*e3[2];
        }

        double up[3];
        if (worldDir) {
            // No eigenbasis frame to lean on — use the structure's major axis
            // (e1) as the up reference so its longest dimension tends toward
            // screen-vertical; fall back to e2 if e1 ≈ parallel to the view.
            const auto* ref = &e1;
            double d1 = std::abs(e1[0]*sz[0] + e1[1]*sz[1] + e1[2]*sz[2]);
            if (d1 > 0.9) ref = &e2;
            up[0] = (*ref)[0]; up[1] = (*ref)[1]; up[2] = (*ref)[2];
        } else {
            // Up reference in PCA frame: prefer e2; if view is too close to e2, fall back to e1
            double up_pca[3] = {0, 1, 0};
            if (std::abs(vy) > 0.9) { up_pca[0] = 1; up_pca[1] = 0; up_pca[2] = 0; }
            up[0] = up_pca[0]*e1[0] + up_pca[1]*e2[0] + up_pca[2]*e3[0];
            up[1] = up_pca[0]*e1[1] + up_pca[1]*e2[1] + up_pca[2]*e3[1];
            up[2] = up_pca[0]*e1[2] + up_pca[1]*e2[2] + up_pca[2]*e3[2];
        }

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
        fitToFrame();   // measure the extent in the final screen frame (issue #98)

        std::string msg = "Oriented " + std::to_string(g.xs.size()) + " atoms";
        if (g.objs > 1) msg += " (" + std::to_string(g.objs) + " objects)";
        app.logViewState(cmd, static_cast<int>(g.xs.size()));
        return {true, msg};
    }, ":orient [view <vx vy vz> | dir <vx vy vz> | dir $reg] [selection]",
       "Center, zoom, and align principal axes. Default = view down the "
       "shortest PCA axis. `view <vx vy vz>` aims along a combination of the "
       "selection's PCA eigen-axes (0 0 1 = shortest/e3, 0 1 0 = middle/e2, "
       "1 0 0 = longest/e1). `dir <vx vy vz>` (or `dir $reg` for a vec3 "
       "register) aims down a world-space direction instead. Pair with a "
       "follow-up :zoom to frame a different selection than the rotation basis.",
       {":orient", ":orient view 0 1 0 chain A and resi 50-85",
        ":orient dir 1 0 0", ":orient dir $groove_normal"}, "View");

    // :turn x|y|z <deg>  — incremental camera rotation around screen axes,
    // no PCA, no recompute. Mirrors PyMOL's `turn` and is the cheap path
    // for spinning animations: orient once, then turn N° per frame.
    reg.registerCmd("turn", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
        if (cmd.args.size() < 2) {
            return {false, "Usage: :turn x|y|z <degrees>"};
        }
        const auto& axis = cmd.args[0];
        auto degOpt = parseFloat(cmd.args[1]);
        if (!degOpt) {
            return {false, "Invalid angle: " + cmd.args[1]};
        }
        float deg = *degOpt;
        auto& cam = app.tabs().currentTab().camera();
        if      (axis == "x" || axis == "X") cam.rotateX(deg);
        else if (axis == "y" || axis == "Y") cam.rotateY(deg);
        else if (axis == "z" || axis == "Z") cam.rotateZ(deg);
        else return {false, "Axis must be x, y, or z (got '" + axis + "')"};
        app.logViewState(cmd);
        return {true, "Turned " + axis + " by " + std::to_string(deg) + " deg"};
    }, ":turn x|y|z <deg>", "Rotate camera around a screen axis (no PCA recompute)",
       {":turn y 90", ":turn x -45"}, "View");

    // :camera — print | save <file> | load <file> | reset
    //
    // The 15-float camera state (rotation 3x3, center XYZ, zoom, pan XY) is
    // serialized as a small key=value text file so figure scripts can be
    // bit-reproducible across renders. Without this, every re-render starts
    // from a freshly-PCA'd pose and tiny structural changes silently shift
    // the camera (issue #39c). File format is line-oriented and forgiving
    // about whitespace; the version header lets future revisions stay
    // backwards-compatible.
    reg.registerCmd("camera", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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
            app.clearViewFit();   // explicit camera state, not a fit (issue #98)
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
            app.clearViewFit();   // restore an exact saved pose (issue #98)
            return {true, "Camera loaded from " + path};
        }

        return {false, "Usage: :camera | :camera save <file> | :camera load <file> | :camera reset"};
    }, ":camera [save|load|reset] [file]",
       "Save/load/reset the camera (15-float state: rotation 3x3, center XYZ, zoom, pan XY) — bit-reproducible figures",
       {":camera", ":camera save fig30.cam", ":camera load fig30.cam", ":camera reset"}, "View");

    // :focus <selection>  → Mol*-style focus on selection
    //                        (camera + hide non-neighborhood + show
    //                        sidechains + dim cartoon context).
    // :focus off           → restore pre-focus camera + visibility.
    reg.registerCmd("focus", [](Application& app, const ParsedCommand& cmd) -> ExecResult {
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

}

}  // namespace molterm
