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

mkdir -p "$(dirname "$DEST")"
install -m 0755 "$TMP/molterm-${TAG}-${ASSET_SUFFIX%.tar.gz}/molterm" "$DEST"

echo "installed $TAG → $DEST"
"$DEST" --version
