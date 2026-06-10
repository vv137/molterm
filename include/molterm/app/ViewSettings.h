#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

#include "molterm/render/OutlineMode.h"
#include "molterm/render/StereoMode.h"

namespace molterm {

// Background fill applied before clear()/savePNG().
//   Transparent : leave the alpha channel clear (default).
//   White/Black : opaque flat fills.
//   Custom      : user-supplied RGB triple in bgCustom{R,G,B}, set via
//                 `:set bg "#RRGGBB"` / `:set bg "rgb(R,G,B)"`. Always opaque;
//                 for transparency use BgMode::Transparent.
enum class BgMode { Transparent, White, Black, Custom };

// Visual / overlay presentation state, factored out of Application so the
// dozens of `:set`-tunable appearance knobs (fog, outlines, stereo, labels,
// annotations, arrows, overlay sizing, custom colors) live as one cohesive,
// copyable unit instead of a flat field bag on the app object. Holds the data
// plus only the derived-value helpers that depend solely on these fields;
// Application reaches it through view().
class ViewSettings {
public:
    using ColorRGB = std::array<uint8_t, 3>;

    // Per-render auto-scale model for overlay sizes (issue #48). With Pixels
    // the *_font_size knobs are raw screen pixels (pre-#48 behavior). Physical
    // treats them as point sizes (1 pt = liveDpi/72 px live) and rescales by
    // the screenshot DPI. Relative (default) treats them as pixels at a
    // referenceCanvasHeight-tall canvas and rescales by canvasH/refH so rough
    // and hi-DPI renders show labels at the same fraction of the canvas.
    enum class OverlaySizeMode { Pixels, Physical, Relative };

    BgMode bgMode() const { return bgMode_; }
    void setBgMode(BgMode m) { bgMode_ = m; }
    // Custom bg RGB used when bgMode_ == BgMode::Custom. Setter also flips the
    // mode so callers don't have to remember to.
    void setBgCustomRGB(uint8_t r, uint8_t g, uint8_t b) {
        bgCustomR_ = r; bgCustomG_ = g; bgCustomB_ = b;
        bgMode_ = BgMode::Custom;
    }
    uint8_t bgCustomR() const { return bgCustomR_; }
    uint8_t bgCustomG() const { return bgCustomG_; }
    uint8_t bgCustomB() const { return bgCustomB_; }

    float fogStrength() const { return fogStrength_; }
    void setFogStrength(float s) { fogStrength_ = s; }
    bool outlineEnabled() const { return outlineEnabled_; }
    void setOutlineEnabled(bool v) { outlineEnabled_ = v; }
    bool screenshotOverlay() const { return screenshotOverlay_; }
    void setScreenshotOverlay(bool v) { screenshotOverlay_ = v; }
    float outlineThreshold() const { return outlineThreshold_; }
    void setOutlineThreshold(float t) { outlineThreshold_ = t; }
    float outlineDarken() const { return outlineDarken_; }
    void setOutlineDarken(float d) { outlineDarken_ = d; }

    StereoMode stereoMode() const { return stereoMode_; }
    void setStereoMode(StereoMode m) { stereoMode_ = m; }
    float stereoAngle() const { return stereoAngle_; }
    void setStereoAngle(float deg) { stereoAngle_ = deg; }

    // Overlay sizing — labels, measurement dashes/captions, selection rings.
    // The cell-derived defaults render fine on small canvases but vanish at
    // hi-DPI screenshot sizes (issue #25). Each knob has its own setting;
    // overlayScale_ is a global multiplier applied on top.
    int labelFontSize() const { return labelFontSize_; }
    void setLabelFontSize(int px) { labelFontSize_ = px; }
    int annotationFontSize() const { return annotationFontSize_; }
    void setAnnotationFontSize(int px) { annotationFontSize_ = px; }
    int annotationLineWidth() const { return annotationLineWidth_; }
    void setAnnotationLineWidth(int px) { annotationLineWidth_ = px; }
    float overlayScale() const { return overlayScale_; }
    void setOverlayScale(float s) { overlayScale_ = s; }

    OverlaySizeMode overlaySizeMode() const { return overlaySizeMode_; }
    void setOverlaySizeMode(OverlaySizeMode m) { overlaySizeMode_ = m; }
    int referenceCanvasHeight() const { return referenceCanvasHeight_; }
    void setReferenceCanvasHeight(int h) { referenceCanvasHeight_ = h; }
    int liveDpi() const { return liveDpi_; }
    void setLiveDpi(int dpi) { liveDpi_ = dpi; }
    // Compute the auto-scale multiplier for the given render context. Returns
    // 1.0 in Pixels mode (back-compat) or when canvas/dpi look bogus, so
    // callers don't have to special-case those.
    float computeRenderScale(int canvasHeight, int dpi) const {
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

    // Text outline / halo for label and annotation legibility (issue #49).
    // When enabled, drawTextOutlinedRGB paints a `*OutlineThickness_`-wide
    // halo around each glyph in `*OutlineColor_` before the body color, so
    // text reads cleanly against any background. Auto color (nullopt) picks
    // white-on-dark / black-on-light against the body color.
    bool labelOutline() const { return labelOutline_; }
    void setLabelOutline(bool on) { labelOutline_ = on; }
    int labelOutlineThickness() const { return labelOutlineThickness_; }
    void setLabelOutlineThickness(int t) { labelOutlineThickness_ = t; }
    bool annotationOutline() const { return annotationOutline_; }
    void setAnnotationOutline(bool on) { annotationOutline_ = on; }
    int annotationOutlineThickness() const { return annotationOutlineThickness_; }
    void setAnnotationOutlineThickness(int t) { annotationOutlineThickness_ = t; }

    int arrowThickness() const { return arrowThickness_; }
    void setArrowThickness(int t) { arrowThickness_ = t; }
    int arrowHeadSize() const { return arrowHeadSize_; }
    void setArrowHeadSize(int s) { arrowHeadSize_ = s; }

    // Custom overlay colors (issue #30, #31). Each is std::optional — unset
    // means "use the legacy default color constant" (white for labels, yellow
    // for annotation captions / measurement lines, plain darken for outlines).
    // Set via :set <kind>_color <named|#hex|rgb()>.
    const std::optional<ColorRGB>& labelColor()             const { return labelColor_; }
    const std::optional<ColorRGB>& annotationColor()        const { return annotationColor_; }
    const std::optional<ColorRGB>& arrowColor()             const { return arrowColor_; }
    const std::optional<ColorRGB>& measurementLineColor()   const { return measurementLineColor_; }
    const std::optional<ColorRGB>& outlineColor()           const { return outlineColor_; }
    const std::optional<ColorRGB>& labelOutlineColor()      const { return labelOutlineColor_; }
    const std::optional<ColorRGB>& annotationOutlineColor() const { return annotationOutlineColor_; }
    void setLabelColor(std::optional<ColorRGB> c)             { labelColor_             = c; }
    void setAnnotationColor(std::optional<ColorRGB> c)        { annotationColor_        = c; }
    void setArrowColor(std::optional<ColorRGB> c)             { arrowColor_             = c; }
    void setMeasurementLineColor(std::optional<ColorRGB> c)   { measurementLineColor_   = c; }
    void setOutlineColor(std::optional<ColorRGB> c)           { outlineColor_           = c; }
    void setLabelOutlineColor(std::optional<ColorRGB> c)      { labelOutlineColor_      = c; }
    void setAnnotationOutlineColor(std::optional<ColorRGB> c) { annotationOutlineColor_ = c; }
    // Pick the auto outline color from a body color: white on dark bodies,
    // black on light bodies. Used when *OutlineColor_ is unset.
    static ColorRGB autoOutlineColor(uint8_t r, uint8_t g, uint8_t b) {
        int luma = (r * 299 + g * 587 + b * 114) / 1000;
        return luma < 128 ? ColorRGB{255, 255, 255} : ColorRGB{0, 0, 0};
    }
    OutlineMode outlineMode() const { return outlineMode_; }
    void setOutlineMode(OutlineMode m) { outlineMode_ = m; }

    // Pre-multiplied effective values used by the overlay renderer. The
    // renderScaleHint_ factor is set transiently by the render entry points
    // (live and screenshot) based on overlaySizeMode_ + the current canvas
    // size + DPI — see computeRenderScale().
    int effectiveLabelFontSize() const {
        return std::max(4, static_cast<int>(std::lround(labelFontSize_ * overlayScale_ * renderScaleHint_)));
    }
    int effectiveAnnotationFontSize() const {
        return std::max(4, static_cast<int>(std::lround(annotationFontSize_ * overlayScale_ * renderScaleHint_)));
    }
    int effectiveAnnotationLineWidth() const {
        return std::max(1, static_cast<int>(std::lround(annotationLineWidth_ * overlayScale_ * renderScaleHint_)));
    }
    int effectiveArrowThickness() const {
        return std::max(1, static_cast<int>(std::lround(arrowThickness_ * overlayScale_ * renderScaleHint_)));
    }
    int effectiveArrowHeadSize() const {
        return std::max(2, static_cast<int>(std::lround(arrowHeadSize_ * overlayScale_ * renderScaleHint_)));
    }
    float renderScaleHint() const { return renderScaleHint_; }
    void setRenderScaleHint(float s) { renderScaleHint_ = s; }

private:
    BgMode bgMode_ = BgMode::Transparent;
    uint8_t bgCustomR_ = 0, bgCustomG_ = 0, bgCustomB_ = 0;

    float fogStrength_ = 0.35f;
    bool outlineEnabled_ = true;
    bool screenshotOverlay_ = false;
    float outlineThreshold_ = 0.3f;
    StereoMode stereoMode_ = StereoMode::Off;
    float stereoAngle_ = 6.0f;  // total parallax in degrees (eyes ±half)
    float outlineDarken_ = 0.15f;
    int labelFontSize_ = 14;
    int annotationFontSize_ = 14;
    int annotationLineWidth_ = 2;
    float overlayScale_ = 1.0f;
    OverlaySizeMode overlaySizeMode_ = OverlaySizeMode::Relative;
    int referenceCanvasHeight_ = 1080;
    int liveDpi_ = 96;
    float renderScaleHint_ = 1.0f;
    int arrowThickness_ = 2;
    int arrowHeadSize_  = 8;
    std::optional<ColorRGB> labelColor_;
    std::optional<ColorRGB> annotationColor_;
    std::optional<ColorRGB> arrowColor_;
    std::optional<ColorRGB> measurementLineColor_;
    std::optional<ColorRGB> outlineColor_;
    std::optional<ColorRGB> labelOutlineColor_;
    std::optional<ColorRGB> annotationOutlineColor_;
    bool labelOutline_ = true;   // halo on atom labels by default (readability)
    int  labelOutlineThickness_ = 2;
    bool annotationOutline_ = false;
    int  annotationOutlineThickness_ = 2;
    OutlineMode outlineMode_ = OutlineMode::Edge;
};

} // namespace molterm
