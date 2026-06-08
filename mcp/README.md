# molterm-mcp

MCP (Model Context Protocol) server for [MolTerm](https://github.com/vv137/molterm) — exposes molecular structure analysis tools to LLM clients like Claude Code, Claude Desktop, and other MCP-compatible hosts.

## Tools

| Tool | Description |
|------|-------------|
| `fetch` | Download a structure from RCSB PDB (`1ubq`) or AlphaFold DB (`afdb:P00533`) |
| `info` | Load a structure file and return summary (atom count, bonds, chains) |
| `count` | Count atoms matching a selection expression |
| `select` | Run selection algebra and return match count |
| `screenshot` | Render a structure to PNG and return the image |
| `render_pdb` | Fetch by PDB ID and render in one step |
| `measure` | Measure distance between two atoms by selection |
| `align` | Structural alignment (TM-align) with RMSD/TM-score |
| `dssp` | DSSP secondary structure assignment |
| `run_script` | Execute arbitrary molterm command sequences |
| `help` | List molterm commands, or show usage for one |
| `version` | Report the molterm binary version and repo tag |
| `update` | Install the latest released binary (opt-in; see below) |

## Install

Only **Node.js >= 18** is required. On install, the package fetches the matching
prebuilt molterm binary from the GitHub release — no C++ toolchain needed.

> **Supported platforms:** macOS (Apple Silicon), Linux x86_64, Linux aarch64.
> On other platforms (e.g. Intel macOS, Windows) the download is skipped — build
> molterm from source and point `MOLTERM_BIN` at it (see [Other platforms](#other-platforms)).

### Claude Code

```sh
claude mcp add molterm -- npx -y molterm-mcp
```

### Claude Desktop / other MCP hosts

Add an entry to the host's MCP config (Claude Desktop:
`claude_desktop_config.json`):

```json
{
  "mcpServers": {
    "molterm": {
      "command": "npx",
      "args": ["-y", "molterm-mcp"]
    }
  }
}
```

### Global install (optional)

```sh
npm install -g molterm-mcp     # downloads the binary during install
```

then use `"command": "molterm-mcp"` (no `args`) in the config above.

### Other platforms

If no prebuilt binary exists for your platform, build molterm from source and
set `MOLTERM_BIN`:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

```json
{
  "mcpServers": {
    "molterm": {
      "command": "npx",
      "args": ["-y", "molterm-mcp"],
      "env": { "MOLTERM_BIN": "/path/to/molterm/build/molterm" }
    }
  }
}
```

Binary resolution order: `$MOLTERM_BIN` → vendored download → in-repo
`build/molterm` → `molterm` on `PATH`. A binary **v0.47.1 or newer** is required
(earlier headless builds mis-resolve the `<obj>/(...)` qualifier and scoped named
selections, affecting the `select`/`measure`/`align` tools).

### Install knobs (env)

| Variable | Effect |
|----------|--------|
| `MOLTERM_BIN` | Use this binary; skip the download |
| `MOLTERM_BINARY_VERSION` | Pin a different release tag (default tracks the package) |
| `MOLTERM_SKIP_DOWNLOAD=1` | Skip the download (offline/CI) |
| `MOLTERM_FORCE_DOWNLOAD=1` | Download even when an in-repo dev build exists |

#### Optional: enable the `update` tool

The `update` tool downloads and installs the latest released molterm binary
(`scripts/update.sh`). Because it mutates the binary the server then runs, it is
**disabled by default**. To allow it, set `MOLTERM_MCP_ALLOW_UPDATE=1` in the
server's `env`.

## Usage examples

Once configured, an LLM client can call these tools:

**Fetch and inspect a protein:**
> "Fetch PDB 4HHB and tell me how many chains it has"
> → `fetch { id: "4hhb" }` → `count { file: "...", selection: "name CA" }`

**Render a structure:**
> "Show me insulin (PDB 4INS) in cartoon representation colored by chain"
> → `render_pdb { id: "4ins", commands: ["show cartoon", "color chain", "orient"] }`

**Measure a distance:**
> "What's the CA-CA distance between residues 1 and 46 in crambin?"
> → `measure { file: "1crn.cif", sel1: "resi 1 and name CA", sel2: "resi 46 and name CA" }`

**Compare two structures:**
> "Align 1crn and 1ubq and report the TM-score"
> → `align { file1: "1crn.cif", file2: "1ubq.cif" }`

**Advanced scripting:**
> → `run_script { commands: ["fetch 1bna", "show cartoon", "color chain", "orient", "screenshot out.png 1920 1080 300"] }`

## Selection syntax

The `count`, `select`, and `screenshot` tools accept molterm selection expressions:

```
chain A                     # by chain
resi 50-60                  # by residue range
name CA                     # by atom name
helix / sheet / loop        # by secondary structure
chain A and resi 10-20      # boolean AND
chain A or chain B          # boolean OR
not water                   # negation
(chain A and helix) or (chain B and sheet)  # grouping
```

## Development

From the project root, build the C++ core once (`cmake -B build && cmake --build
build`), then:

```sh
cd mcp
npm install      # postinstall detects build/molterm and skips the download
npm run build    # compile TypeScript -> dist/
npm run start    # run the server (stdio transport)
```

The repo ships a project-scoped `.mcp.json` at the root, so opening it in Claude
Code registers the local server automatically (runs `node mcp/dist/index.js`,
which resolves the in-repo `build/molterm`).
