#!/usr/bin/env bash
# scripts/render-all.sh — batch-render every .pdb (or .cif) to a high-
# quality PNG. Drives a single molterm --no-tui process: load → orient →
# screenshot → clear, repeat per file. One process, no flicker, no
# per-file startup cost.
#
# Render preset matches the README's hero-figure recipe:
#   csd 24          — cartoon spline subdivisions (def 14)
#   outline on      — silhouette outlines
#   fog 0.4         — atmospheric depth fog
#   2048×2048       — large enough for journal print at 300 DPI
# These are tuned for quality; on slower machines drop SIZE or set
# CSD=14 OUTLINE=off FOG=0.0 for ~3-4× faster renders.
#
# Usage:
#   ./scripts/render-all.sh [INPUT_DIR] [OUTPUT_DIR]
#   ./scripts/render-all.sh                  # cwd → cwd
#   ./scripts/render-all.sh ./pdbs ./out
#
# Env knobs (all optional):
#   MOLTERM   path to the binary             (default: molterm on PATH)
#   SIZE      WxH output dimensions          (default: 2048x2048)
#   DPI       DPI metadata stamp             (default: 300)
#   PATTERNS  space-sep extensions to glob   (default: "pdb cif pdb.gz cif.gz")
#   COLOR     :color scheme to apply         (default: secondary)
#   CSD       cartoon spline subdivisions    (default: 24)
#   OUTLINE   silhouette outlines on|off     (default: on)
#   FOG       depth fog 0..1                 (default: 0.4)
#   STRICT    abort on first error           (default: off)

set -euo pipefail

INPUT_DIR="${1:-.}"
OUTPUT_DIR="${2:-.}"
MOLTERM="${MOLTERM:-molterm}"
SIZE="${SIZE:-2048x2048}"
DPI="${DPI:-300}"
PATTERNS="${PATTERNS:-pdb cif pdb.gz cif.gz}"
COLOR="${COLOR:-secondary}"
CSD="${CSD:-24}"
OUTLINE="${OUTLINE:-on}"
FOG="${FOG:-0.4}"
STRICT="${STRICT:-}"
W="${SIZE%x*}"
H="${SIZE#*x}"

mkdir -p "$OUTPUT_DIR"

read -ra exts <<< "$PATTERNS"
shopt -s nullglob
files=()
for ext in "${exts[@]}"; do
    files+=("$INPUT_DIR"/*."$ext")
done
shopt -u nullglob

if [ ${#files[@]} -eq 0 ]; then
    echo "no {${PATTERNS// /,}} files in ${INPUT_DIR}" >&2
    exit 1
fi

echo "rendering ${#files[@]} structure(s) → ${OUTPUT_DIR} (${SIZE} @ ${DPI}dpi, csd=${CSD} outline=${OUTLINE} fog=${FOG})"

{
    # One-time global render settings.
    printf 'set renderer pixel\nset csd %s\nset outline %s\nset fog %s\n' \
        "$CSD" "$OUTLINE" "$FOG"
    for f in "${files[@]}"; do
        stem="$(basename "$f")"
        stem="${stem%.gz}"     # double-extension: strip .gz before dropping .pdb/.cif
        stem="${stem%.*}"
        # `clear all` (not `clear`) so the global ObjectStore drops the
        # entry — otherwise an N-file batch's memory peaks at Σ structures.
        printf 'load %s\ncolor %s\norient\nscreenshot %s %s %s %s\nclear all\n' \
            "$f" "$COLOR" "$OUTPUT_DIR/$stem.png" "$W" "$H" "$DPI"
    done
    echo quit
} | "$MOLTERM" -s - --no-tui ${STRICT:+--strict}
