#include "molterm/app/Application.h"

#include "molterm/render/AsciiCanvas.h"
#include "molterm/render/BrailleCanvas.h"
#include "molterm/render/BlockCanvas.h"
#include "molterm/render/PixelCanvas.h"
#include "molterm/render/Camera.h"
#include "molterm/render/Canvas.h"
#include "molterm/render/ColorMapper.h"
#include "molterm/repr/WireframeRepr.h"
#include "molterm/repr/InterfaceRepr.h"
#include "molterm/repr/ReprUtil.h"
#include "molterm/core/Logger.h"
#include "molterm/core/MolObject.h"
#include "molterm/core/Selection.h"
#include "molterm/tui/Layout.h"
#include "molterm/tui/Screen.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// Frame rendering, viewport rasterization, stereo, pixel overlays (labels /
// annotations / arrows / selection halo), status bar, and the picking
// projection cache. Split out of Application.cpp (god-class decomposition).
// Same Application class; these are its method definitions in their own TU.
namespace molterm {

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
        wf->setHeteroatomCarbonScheme(interface_.active || focus_.active());
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
    if (interface_.zoomGate.enabled()) {
        const bool wasActive = interface_.zoomGate.active();
        const bool flipped   = interface_.zoomGate.update(tab.camera().zoom());
        const bool isActive  = interface_.zoomGate.active();
        if (flipped) {
            if (isActive && !interface_.active) {
                interface_.fromZoom = true;
                cmdRegistry_.execute(*this, "interface on");
            } else if (!isActive && interface_.fromZoom && interface_.active) {
                interface_.fromZoom = false;
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
            if (outlineEnabled()) {
                uint8_t r = 0, g = 0, b = 0;
                if (outlineColor()) { r = (*outlineColor())[0]; g = (*outlineColor())[1]; b = (*outlineColor())[2]; }
                pc->applyOutline(outlineThreshold(), outlineDarken(), outlineMode(), r, g, b);
            }
            if (fogStrength() > 0.0f) pc->applyDepthFog(fogStrength());

            // Focus-dim: prefer the interface mask when active, fall back
            // to the explicit :focus selection mask. Either is depth-
            // independent, so a non-focus atom in the foreground still
            // dims while the focus subject stays vivid.
            const std::vector<bool>* dimMask = nullptr;
            if (interface_.active && !interface_.atomMask.empty()) {
                dimMask = &interface_.atomMask;
            } else if (!focus_.atomMask.empty()) {
                dimMask = &focus_.atomMask;
            }
            if (dimMask) pc->applyFocusDim(*dimMask, focus_.dimStrength);
        }
    }

    // ── Interface overlay (sidechains + interaction lines) ─────────────
    // Drawn last so it sits on top of fog + focus-dim — vivid colors,
    // unattenuated. Fires under either:
    //   • global :interface overlay (full structure)
    //   • focus mode (filtered to the focus neighborhood)
    // Per-eye in stereo mode so each half gets its own vivid overlay.
    if ((interface_.active || focus_.active()) &&
        interface_.repr.hasData()) {
        if (auto obj = tab.currentObject(); obj && obj->visible()) {
            std::array<float, 9> savedIfaceRot{};
            for (int eye = 0; eye < stereoEyeCount(); ++eye) {
                savedIfaceRot = setupStereoEye(eye, canvas_->subW(),
                                                canvas_->subH(),
                                                canvas_->aspectYX());
                interface_.repr.render(*obj, tab.camera(), *canvas_);
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

    // Live transcript (input + output) above the active command line. The
    // renderer clamps and returns the scroll offset (it knows the height).
    transcriptHintScroll_ = cmdLine_.renderHistoryHint(
        win, cmdTranscript_, transcriptHintScroll_, transcriptHintLines_);

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
    if (stereoMode() != StereoMode::Off && isPixel) {
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
    const std::optional<ColorRGB>& mlc = measurementLineColor();
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
                      * overlayScale() * renderScaleHint())))
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
    if (stereoMode() == StereoMode::Off) {
        cam.prepareProjection(totalSubW, subH, aspectYX);
        return cam.rotation();
    }
    auto savedRot = cam.rotation();
    float halfAngle = stereoAngle() * 0.5f;
    bool crosseye = (stereoMode() == StereoMode::Crosseye);
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
    if (stereoMode() == StereoMode::Off) return;
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
    if (arrowColor()) { baseR = (*arrowColor())[0]; baseG = (*arrowColor())[1]; baseB = (*arrowColor())[2]; }
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
    if (labelColor()) { r = (*labelColor())[0]; g = (*labelColor())[1]; b = (*labelColor())[2]; }
    int fsize = effectiveLabelFontSize();
    if (!labelOutline() || labelOutlineThickness() <= 0) {
        pc.drawTextRGB(sx, sy, depth, text, r, g, b, fsize);
        return;
    }
    auto oc = labelOutlineColor().value_or(autoOutlineColor(r, g, b));
    pc.drawTextOutlinedRGB(sx, sy, depth, text, r, g, b,
                           oc[0], oc[1], oc[2],
                           labelOutlineThickness(), fsize);
}

void Application::paintAnnotationText(PixelCanvas& pc, int sx, int sy, float depth,
                                       const std::string& text) {
    uint8_t r = 255, g = 255, b = 50;           // default yellow
    if (annotationColor()) { r = (*annotationColor())[0]; g = (*annotationColor())[1]; b = (*annotationColor())[2]; }
    int fsize = effectiveAnnotationFontSize();
    if (!annotationOutline() || annotationOutlineThickness() <= 0) {
        pc.drawTextRGB(sx, sy, depth, text, r, g, b, fsize);
        return;
    }
    auto oc = annotationOutlineColor().value_or(autoOutlineColor(r, g, b));
    pc.drawTextOutlinedRGB(sx, sy, depth, text, r, g, b,
                           oc[0], oc[1], oc[2],
                           annotationOutlineThickness(), fsize);
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
        const auto& mlc = measurementLineColor();
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
                4.0f * cameraZoomScale(cam.zoom()) * overlayScale() * renderScaleHint())));
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

} // namespace molterm
