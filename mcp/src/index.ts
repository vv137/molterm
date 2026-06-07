#!/usr/bin/env node

import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod/v4";
import { basename } from "node:path";
import { runMolterm, runWithScreenshot, runShell, REPO_ROOT } from "./molterm.js";

/** Extract molterm object name from a file path (stem without extensions). */
function objName(filePath: string): string {
  let name = basename(filePath);
  name = name.replace(/\.(cif\.gz|cif|pdb|ent|mmcif)$/i, "");
  return name;
}

/** Detect if a string is a PDB ID vs a file path. */
function isPdbId(s: string): boolean {
  return /^[a-zA-Z0-9]{4}$/.test(s) || /^afdb:/i.test(s) || /^AF-/i.test(s);
}

/** Return the appropriate load command for a PDB ID or file path. */
function loadCmd(s: string): string {
  return isPdbId(s) ? `fetch ${s}` : `load ${s}`;
}

/** Return the object name for a PDB ID or file path. */
function nameOf(s: string): string {
  return isPdbId(s) ? s.replace(/^afdb:/i, "AF-").toLowerCase() : objName(s);
}

const server = new McpServer({
  name: "molterm",
  version: "0.2.0",
});

// ═══════════════════════════════════════════════════════════════════════
//  FILES
// ═══════════════════════════════════════════════════════════════════════

server.registerTool("fetch", {
  description: "Download a molecular structure from RCSB PDB (4-letter code) or AlphaFold DB (afdb:UniProtID) and return basic info",
  inputSchema: { id: z.string().describe("PDB ID (e.g. 1ubq) or afdb:<UniProt> (e.g. afdb:P00533)") },
}, async ({ id }) => {
  const result = await runMolterm([`fetch ${id}`, "info"]);
  return { content: [{ type: "text", text: result.stdout || result.stderr }] };
});

server.registerTool("info", {
  description: "Load a structure file and return summary information (atom count, bonds, chains, etc.)",
  inputSchema: { file: z.string().describe("Path to structure file (.pdb, .cif, .cif.gz)") },
}, async ({ file }) => {
  const result = await runMolterm([`load ${file}`, "info"]);
  return { content: [{ type: "text", text: result.stdout || result.stderr }] };
});

// ═══════════════════════════════════════════════════════════════════════
//  SELECTION
// ═══════════════════════════════════════════════════════════════════════

server.registerTool("select", {
  description: "Run a selection expression and return matching atom count. " +
    "Supports boolean algebra: 'chain A and helix', 'resi 50-60 or name CA', " +
    "'within 5 of resn HEM', 'byres ...', etc.",
  inputSchema: {
    file: z.string().describe("Path to structure file or PDB ID"),
    expression: z.string().describe("Selection expression"),
  },
}, async ({ file, expression }) => {
  const result = await runMolterm([loadCmd(file), `select ${expression}`, `count $sele`]);
  return { content: [{ type: "text", text: result.stdout || result.stderr }] };
});

server.registerTool("count", {
  description: "Count atoms matching a selection expression in a structure",
  inputSchema: {
    file: z.string().describe("Path to structure file or PDB ID"),
    selection: z.string().default("all").describe("Selection expression (e.g. 'chain A', 'helix', 'name CA')"),
  },
}, async ({ file, selection }) => {
  const result = await runMolterm([loadCmd(file), `count ${selection}`]);
  return { content: [{ type: "text", text: result.stdout || result.stderr }] };
});

// ═══════════════════════════════════════════════════════════════════════
//  RENDERING & SCREENSHOTS
// ═══════════════════════════════════════════════════════════════════════

server.registerTool("screenshot", {
  description: "Render a molecular structure to a PNG image. Returns the image inline and optionally saves to a file path.",
  inputSchema: {
    file: z.string().describe("Path to structure file or PDB ID"),
    commands: z.array(z.string()).default([]).describe(
      "Molterm commands to run before capture (e.g. ['show cartoon', 'color chain', 'orient'])"
    ),
    width: z.number().int().min(64).max(4096).default(800),
    height: z.number().int().min(64).max(4096).default(600),
    dpi: z.number().int().min(72).max(600).default(150),
    output: z.string().optional().describe("File path to save the PNG"),
  },
}, async ({ file, commands, width, height, dpi, output }) => {
  const allCmds = [loadCmd(file), ...commands];
  const { result, image, path } = await runWithScreenshot(allCmds, width, height, dpi, output);
  if (image) {
    const msg = output ? `Saved to ${path}` : (result.stdout || "Screenshot captured");
    return {
      content: [
        { type: "image", data: image.toString("base64"), mimeType: "image/png" },
        { type: "text", text: msg },
      ],
    };
  }
  return { content: [{ type: "text", text: `Screenshot failed: ${result.stderr || result.stdout}` }] };
});

server.registerTool("render_pdb", {
  description: "Fetch a PDB structure by ID and render it as a PNG image. Optionally save to a file.",
  inputSchema: {
    id: z.string().describe("PDB ID (e.g. 1ubq, 4hhb)"),
    commands: z.array(z.string()).default(["preset", "orient"]).describe(
      "Commands to run after fetch (default: preset + orient)"
    ),
    width: z.number().int().min(64).max(4096).default(800),
    height: z.number().int().min(64).max(4096).default(600),
    dpi: z.number().int().min(72).max(600).default(150),
    output: z.string().optional().describe("File path to save the PNG"),
  },
}, async ({ id, commands, width, height, dpi, output }) => {
  const allCmds = [`fetch ${id}`, ...commands];
  const { result, image, path } = await runWithScreenshot(allCmds, width, height, dpi, output);
  if (image) {
    const msg = output ? `Saved to ${path}` : (result.stdout || `Rendered ${id}`);
    return {
      content: [
        { type: "image", data: image.toString("base64"), mimeType: "image/png" },
        { type: "text", text: msg },
      ],
    };
  }
  return { content: [{ type: "text", text: `Render failed: ${result.stderr || result.stdout}` }] };
});

// ═══════════════════════════════════════════════════════════════════════
//  MEASUREMENT
// ═══════════════════════════════════════════════════════════════════════

server.registerTool("measure", {
  description: "Measure distance between two atoms. Endpoints are parenthesized selections that each resolve to exactly one atom.",
  inputSchema: {
    file: z.string().describe("Path to structure file or PDB ID"),
    sel1: z.string().describe("Selection for first atom (e.g. 'resi 10 and name CA')"),
    sel2: z.string().describe("Selection for second atom"),
  },
}, async ({ file, sel1, sel2 }) => {
  const result = await runMolterm([loadCmd(file), `measure (${sel1}) (${sel2})`]);
  return { content: [{ type: "text", text: result.stdout || result.stderr }] };
});

// ═══════════════════════════════════════════════════════════════════════
//  ANALYSIS
// ═══════════════════════════════════════════════════════════════════════

server.registerTool("dssp", {
  description: "Run DSSP secondary structure assignment on a loaded structure",
  inputSchema: { file: z.string().describe("Path to structure file or PDB ID") },
}, async ({ file }) => {
  const result = await runMolterm([loadCmd(file), "dssp"]);
  return { content: [{ type: "text", text: result.stdout || result.stderr }] };
});

server.registerTool("align", {
  description: "Structurally align two structures using TM-align (or MM-align for multi-chain). " +
    "Reports TM-score, RMSD, and aligned length. Accepts file paths or PDB IDs.",
  inputSchema: {
    mobile: z.string().describe("Mobile structure: file path or PDB ID (e.g. '1crn' or '/path/to/file.cif')"),
    target: z.string().describe("Target/reference structure: file path or PDB ID"),
    mode: z.enum(["auto", "tm", "mm"]).default("auto").describe("Alignment mode: auto (default), tm (TM-align), mm (MM-align for multi-chain)"),
  },
}, async ({ mobile, target, mode }) => {
  const mob = nameOf(mobile);
  const ref = nameOf(target);
  const modeFlag = mode === "auto" ? "" : ` ${mode}`;
  const result = await runMolterm([
    loadCmd(target),
    loadCmd(mobile),
    `align ${mob} to ${ref}${modeFlag}`,
  ]);
  return { content: [{ type: "text", text: result.stdout || result.stderr }] };
});

// ═══════════════════════════════════════════════════════════════════════
//  SCRIPTING & HELP
// ═══════════════════════════════════════════════════════════════════════

server.registerTool("run_script", {
  description: "Execute a sequence of arbitrary molterm commands in headless mode. " +
    "Use this for advanced workflows not covered by other tools, or to chain multiple operations.",
  inputSchema: {
    commands: z.array(z.string()).describe("List of molterm commands to execute sequentially"),
    cwd: z.string().optional().describe("Working directory"),
  },
}, async ({ commands, cwd }) => {
  const result = await runMolterm(commands, { cwd });
  const parts: Array<{ type: "text"; text: string }> = [];
  if (result.stdout) parts.push({ type: "text", text: result.stdout });
  if (result.stderr) parts.push({ type: "text", text: `[stderr] ${result.stderr}` });
  if (parts.length === 0) parts.push({ type: "text", text: "(no output)" });
  return { content: parts };
});

server.registerTool("help", {
  description: "Get help for molterm commands. Without arguments, lists all available commands grouped by category. " +
    "With a command name, shows usage, description, and examples.",
  inputSchema: {
    command: z.string().optional().describe("Command name to get help for (omit for full command index)"),
  },
}, async ({ command }) => {
  const cmd = command ? `help ${command}` : "help";
  const result = await runMolterm([cmd]);
  return { content: [{ type: "text", text: result.stdout || result.stderr }] };
});

// ═══════════════════════════════════════════════════════════════════════
//  UPDATE & VERSION
// ═══════════════════════════════════════════════════════════════════════

server.registerTool("version", {
  description: "Show the installed molterm version and git status",
  inputSchema: {},
}, async () => {
  const [ver, git] = await Promise.all([
    runMolterm(["echo molterm ${MOLTERM_VERSION}"]).catch(() => ({ stdout: "", stderr: "molterm binary not found" })),
    runShell("git log --oneline -5 && echo '---' && git describe --tags --always"),
  ]);
  const parts: string[] = [];
  if (ver.stdout) parts.push(ver.stdout);
  if (git.stdout) parts.push(git.stdout);
  if (ver.stderr) parts.push(`[stderr] ${ver.stderr}`);
  return { content: [{ type: "text", text: parts.join("\n") }] };
});

server.registerTool("update", {
  description: "Update molterm: git pull, rebuild the C++ binary, and rebuild the MCP server. " +
    "Run this to get the latest version from GitHub.",
  inputSchema: {},
}, async () => {
  const steps: string[] = [];

  // 1. git pull
  const pull = await runShell("git pull --ff-only");
  steps.push(`[git pull] ${pull.stdout || pull.stderr}`);
  if (!pull.ok) {
    return { content: [{ type: "text", text: steps.join("\n\n") + "\n\nUpdate failed at git pull." }] };
  }

  // 2. cmake configure
  const configure = await runShell(
    "cmake -B build -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -3"
  );
  steps.push(`[cmake configure] ${configure.stdout || configure.stderr}`);

  // 3. cmake build
  const build = await runShell(
    "cmake --build build -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) 2>&1 | tail -5",
    { timeout: 300_000 }
  );
  steps.push(`[cmake build] ${build.stdout || build.stderr}`);
  if (!build.ok) {
    return { content: [{ type: "text", text: steps.join("\n\n") + "\n\nUpdate failed at cmake build." }] };
  }

  // 4. rebuild MCP server
  const mcp = await runShell("npm install && npm run build 2>&1 | tail -3", {
    cwd: `${REPO_ROOT}/mcp`,
  });
  steps.push(`[mcp build] ${mcp.stdout || mcp.stderr}`);

  // 5. show new version
  const ver = await runShell("./build/molterm --version");
  steps.push(`[version] ${ver.stdout || ver.stderr}`);

  const status = build.ok && mcp.ok ? "Update complete." : "Update finished with errors.";
  steps.push(status);

  return { content: [{ type: "text", text: steps.join("\n\n") }] };
});

// ═══════════════════════════════════════════════════════════════════════
//  Start server
// ═══════════════════════════════════════════════════════════════════════

async function main() {
  const transport = new StdioServerTransport();
  await server.connect(transport);
}

main().catch((err) => {
  console.error("Fatal:", err);
  process.exit(1);
});
