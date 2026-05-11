#!/usr/bin/env bash
set -euo pipefail

REPO="vv137/molterm"
DEST="${1:-${MOLTERM_BIN:-$HOME/.local/bin/molterm}}"

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64)        ASSET_SUFFIX="macos-arm64.tar.gz" ;;
    Linux-x86_64)        ASSET_SUFFIX="linux-x86_64.tar.gz" ;;
    Linux-aarch64|Linux-arm64) ASSET_SUFFIX="linux-aarch64.tar.gz" ;;
    *) echo "unsupported platform: $(uname -s) $(uname -m)" >&2; exit 1 ;;
esac

TAG="$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest" | grep -m1 '"tag_name"' | cut -d'"' -f4)"
[ -n "$TAG" ] || { echo "could not resolve latest tag" >&2; exit 1; }

URL="https://github.com/${REPO}/releases/download/${TAG}/molterm-${TAG}-${ASSET_SUFFIX}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "fetching $TAG ($ASSET_SUFFIX)"
curl -fsSL "$URL" | tar -xzf - -C "$TMP"

STAGE="$TMP/molterm-${TAG}-${ASSET_SUFFIX%.tar.gz}"

# Install layout (matches the lookup chain in resolveAtLibPath):
#   <prefix>/bin/molterm                — DEST
#   <prefix>/share/molterm/lib/*.mt     — shipped recipe baseline
# `~/.molterm/lib/` stays reserved for user-fork overrides and is
# never touched by the updater.
BIN_DIR="$(dirname "$DEST")"
PREFIX="$(dirname "$BIN_DIR")"
LIB_DIR="$PREFIX/share/molterm/lib"

mkdir -p "$BIN_DIR"
install -m 0755 "$STAGE/molterm" "$DEST"

if [ -d "$STAGE/lib" ]; then
    mkdir -p "$LIB_DIR"
    # Mirror the shipped recipes — overwrite (rsync-style) so a stale
    # script from a prior tag doesn't shadow a renamed/removed one.
    find "$LIB_DIR" -maxdepth 1 -name '*.mt' -delete 2>/dev/null || true
    cp "$STAGE/lib/"*.mt "$STAGE/lib/README.md" "$LIB_DIR/" 2>/dev/null || true
    echo "installed lib   → $LIB_DIR"
fi

echo "installed $TAG → $DEST"
"$DEST" --version
