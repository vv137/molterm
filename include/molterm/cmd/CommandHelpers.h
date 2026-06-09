#pragma once

// Helpers shared by command handlers across the src/cmd/commands/ modules
// (and by the script runner in Application.cpp). These used to live in an
// anonymous namespace in Application.cpp; they were lifted here so the
// per-area command translation units can reuse them without duplication.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace molterm {

class MolObject;

// "all" token shared by scope-taking commands (:color all, :set all, ...).
inline constexpr const char* kAllToken = "all";

// "*" glob token (scope-everything) and the "@lib/" recipe-path prefix.
inline constexpr const char* kAllGlob = "*";
inline constexpr std::string_view kAtLibPrefix = "@lib/";

// Recolor heteroatom (non-standard-residue) carbons distinctly; returns the
// number of atoms recolored. Overload without indices does the whole object.
int applyHeteroatomColors(MolObject& obj, const std::vector<int>& indices);
int applyHeteroatomColors(MolObject& obj);

// Resolve an `@lib/<name>` recipe spec to an absolute path (searching
// $MOLTERM_LIB_DIR, the config dir, install dirs, and the source tree).
// Empty string when no candidate exists. Definition in Application.cpp.
std::string resolveAtLibPath(const std::string& spec);

// Parse a boolean option value: on/1/true/yes -> true, off/0/false/no -> false,
// anything else -> nullopt.
std::optional<bool> parseBool(const std::string& v);

// In-place trim of leading + trailing ASCII whitespace.
void trimWhitespace(std::string& s);

// Join args[lo..hi) with single-space separators.
std::string joinArgs(const std::vector<std::string>& args, size_t lo, size_t hi);

// Split args at the first token equal to `token`: returns {left, right} where
// `left` is the args before it and `right` is everything after, space-joined.
// When the token is absent, `right` is empty and `left` is the original args.
// Used by :label and the :measure/:angle/:dihedral caption parsers.
std::pair<std::vector<std::string>, std::string>
splitAtToken(const std::vector<std::string>& args, std::string_view token);

// Back-compat wrapper for the original "=" caller.
std::pair<std::vector<std::string>, std::string>
splitAtEqToken(const std::vector<std::string>& args);

// `[A-Za-z_][A-Za-z0-9_]*` — accepted in both ${NAME} expansion and :setenv.
bool isValidEnvName(const std::string& s);

// A bare binding name for `:let <name> =` / `:select <name> =`: starts with a
// letter or underscore and holds no whitespace. Looser than isValidEnvName()
// (the rest of the character set is unconstrained).
bool isBareName(const std::string& s);

// Parse "#RGB" / "#RRGGBB" / "rgb(R,G,B)" to an RGB triple (forgiving on
// whitespace and case). std::nullopt on malformed input.
std::optional<std::array<uint8_t, 3>> parseHexColor(std::string s);

// Resolve a color spec — named ("red"), hex ("#RRGGBB"/"#RGB"), or rgb(R,G,B)
// — to an RGB triple, so every color-accepting option shares one accept-list.
std::optional<std::array<uint8_t, 3>> parseColorSpec(const std::string& s);

// Canonical :set option names, enumerated by `:set` (no args) / `:set all` AND
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
    "surface_mode",
    "surface_probe",
    "surface_resolution",
    "surface_scale",
    "surface_smoothness",
    "surface_iso",
    "lod_medium",
    "lod_low",
    "backbone_cutoff",
    "auto_center",
    "panel",
    "seqbar",
    "seqwrap",
    "transcript_lines",
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

// Clear pixel-graphics artifacts and force a full ncurses repaint.
void clearScreenAndRepaint();

}  // namespace molterm
