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

## Setup

### Prerequisites

- **molterm** binary — **v0.47.1 or newer** (earlier headless builds resolve
  the `<obj>/(...)` object qualifier and scoped named selections incorrectly,
  which affects the `select`/`measure`/`align` tools). Build from the project
  root:
  ```sh
  cmake -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j$(nproc)
  ```

- **Node.js** >= 18

### Install

```sh
cd mcp
npm install
npm run build
```

### Configure

This repo ships a project-scoped `.mcp.json` at the root, so opening the repo in
Claude Code registers the server automatically (it runs `node mcp/dist/index.js`
from the project root). For Claude Desktop or another host, add an entry with
absolute paths:

```json
{
  "mcpServers": {
    "molterm": {
      "command": "node",
      "args": ["/path/to/molterm/mcp/dist/index.js"],
      "env": {
        "MOLTERM_BIN": "/path/to/molterm/build/molterm"
      }
    }
  }
}
```

If `MOLTERM_BIN` is not set, it defaults to `../../build/molterm` relative to the
`dist/` directory.

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

```sh
npm run build    # compile TypeScript
npm run start    # run the server (stdio transport)
```
