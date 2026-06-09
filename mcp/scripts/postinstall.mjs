#!/usr/bin/env node
// Fetch the platform-matched molterm binary (and shipped recipe lib) from the
// GitHub release and stage it under vendor/, so `npx molterm-mcp` works without
// the user building the C++ core themselves.
//
// Layout written here mirrors scripts/update.sh so the binary's own lib lookup
// (`<prefix>/share/molterm/lib`) resolves the same way it does for a normal
// install:
//   vendor/bin/molterm
//   vendor/share/molterm/lib/*.mt
//
// Override knobs (env):
//   MOLTERM_BIN              — already have a binary; skip the download entirely
//   MOLTERM_SKIP_DOWNLOAD=1  — skip (e.g. offline CI; runtime will error clearly)
//   MOLTERM_FORCE_DOWNLOAD=1 — download even if a local dev build is present
//   MOLTERM_BINARY_VERSION   — pin a different release tag (default below)
//   MOLTERM_REPO             — owner/name of the GitHub repo (default below)

import { createWriteStream, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import https from "node:https";

const DEFAULT_VERSION = "v0.49.0"; // bump alongside the MCP package version
const REPO = process.env.MOLTERM_REPO || "vv137/molterm";
const VERSION = process.env.MOLTERM_BINARY_VERSION || DEFAULT_VERSION;

const PKG_ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const VENDOR = join(PKG_ROOT, "vendor");
const BIN_PATH = join(VENDOR, "bin", "molterm");
const LIB_DIR = join(VENDOR, "share", "molterm", "lib");
const STAMP = join(VENDOR, ".version");

// Map uname-style platform to the release asset suffix (matches update.sh).
function assetSuffix() {
  const p = `${process.platform}-${process.arch}`;
  switch (p) {
    case "darwin-arm64": return "macos-arm64.tar.gz";
    case "linux-x64":    return "linux-x86_64.tar.gz";
    case "linux-arm64":  return "linux-aarch64.tar.gz";
    default:             return null;
  }
}

function note(msg) { console.log(`[molterm-mcp] ${msg}`); }
function warn(msg) { console.warn(`[molterm-mcp] ${msg}`); }

// Follow GitHub's redirect chain (releases -> objects CDN) and stream to dest.
function download(url, dest, redirects = 0) {
  return new Promise((resolve, reject) => {
    if (redirects > 5) return reject(new Error("too many redirects"));
    https.get(url, { headers: { "User-Agent": "molterm-mcp-postinstall" } }, (res) => {
      if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
        res.resume();
        return resolve(download(res.headers.location, dest, redirects + 1));
      }
      if (res.statusCode !== 200) {
        res.resume();
        return reject(new Error(`HTTP ${res.statusCode} for ${url}`));
      }
      const file = createWriteStream(dest);
      res.pipe(file);
      file.on("finish", () => file.close(() => resolve()));
      file.on("error", reject);
    }).on("error", reject);
  });
}

async function main() {
  // Already satisfied? Don't touch anything.
  if (process.env.MOLTERM_SKIP_DOWNLOAD === "1") {
    return note("MOLTERM_SKIP_DOWNLOAD=1 — skipping binary download.");
  }
  if (process.env.MOLTERM_BIN && existsSync(process.env.MOLTERM_BIN)) {
    return note(`Using MOLTERM_BIN=${process.env.MOLTERM_BIN} — skipping download.`);
  }
  // In-repo dev build present: prefer the freshly compiled binary over a release.
  const devBuild = join(PKG_ROOT, "..", "build", "molterm");
  if (existsSync(devBuild) && process.env.MOLTERM_FORCE_DOWNLOAD !== "1") {
    return note(`Local dev build found (${devBuild}) — skipping download.`);
  }
  // Correct version already vendored.
  if (existsSync(BIN_PATH) && existsSync(STAMP) &&
      readFileSync(STAMP, "utf8").trim() === VERSION) {
    return note(`molterm ${VERSION} already vendored.`);
  }

  const suffix = assetSuffix();
  if (!suffix) {
    // Don't fail the install on unsupported platforms — let the runtime emit an
    // actionable error. (No macOS-x64 or Windows release assets exist yet.)
    warn(`No prebuilt molterm for ${process.platform}-${process.arch}. ` +
         `Build from source and set MOLTERM_BIN. https://github.com/${REPO}`);
    return;
  }

  const base = `molterm-${VERSION}-${suffix.replace(/\.tar\.gz$/, "")}`;
  const url = `https://github.com/${REPO}/releases/download/${VERSION}/molterm-${VERSION}-${suffix}`;
  const tmp = join(tmpdir(), `molterm-mcp-dl-${process.pid}`);
  mkdirSync(tmp, { recursive: true });
  const tgz = join(tmp, "molterm.tar.gz");

  try {
    note(`Downloading molterm ${VERSION} (${suffix})...`);
    await download(url, tgz);
    execFileSync("tar", ["-xzf", tgz, "-C", tmp]);

    const stage = join(tmp, base);
    const stagedBin = join(stage, "molterm");
    if (!existsSync(stagedBin)) throw new Error(`binary missing in archive (${base}/molterm)`);

    // Stage into vendor/ with the bin/ + share/molterm/lib/ layout molterm expects.
    rmSync(VENDOR, { recursive: true, force: true });
    mkdirSync(dirname(BIN_PATH), { recursive: true });
    execFileSync("install", ["-m", "0755", stagedBin, BIN_PATH]);

    const stagedLib = join(stage, "lib");
    if (existsSync(stagedLib)) {
      mkdirSync(LIB_DIR, { recursive: true });
      execFileSync("cp", ["-R", `${stagedLib}/.`, LIB_DIR]);
    }

    writeFileSync(STAMP, VERSION + "\n");
    const v = execFileSync(BIN_PATH, ["--version"], { encoding: "utf8" }).trim();
    note(`Installed ${v} -> ${BIN_PATH}`);
  } catch (err) {
    // Supported platform but the fetch failed — fail loudly so the user notices
    // and can rerun `npm install` or set MOLTERM_BIN, rather than hitting opaque
    // tool failures at runtime.
    warn(`Failed to fetch molterm ${VERSION}: ${err.message}`);
    warn(`Retry the install, or set MOLTERM_BIN to a local molterm binary. ` +
         `Manual builds: https://github.com/${REPO}`);
    process.exitCode = 1;
  } finally {
    rmSync(tmp, { recursive: true, force: true });
  }
}

main().catch((err) => {
  warn(`postinstall error: ${err.message}`);
  process.exitCode = 1;
});
