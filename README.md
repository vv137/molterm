<p align="center">
  <strong>MolTerm</strong> — Terminal-based Molecular Viewer
  <br>
  <em>VIM-like interface &bull; Unicode &amp; pixel rendering &bull; PyMOL export</em>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-17-blue?logo=cplusplus" alt="C++17">
  <img src="https://img.shields.io/badge/license-MIT-green" alt="MIT License">
  <img src="https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey" alt="Platform">
  <img src="https://img.shields.io/badge/rendering-Braille%20%7C%20Sixel%20%7C%20Kitty%20%7C%20iTerm2-orange" alt="Renderers">
</p>

---

MolTerm renders 3D molecular structures directly in the terminal. It targets structural biologists and computational chemists who live in the terminal and want quick molecule inspection without launching a full GUI.

## Features

- **Smart defaults** — auto-detects protein/nucleic/ligand content: cartoon for macromolecules, ball-and-stick for ligands, chain coloring (`gd` / `:preset` to re-apply)
- **3-tier bond detection** — standard residue table (20 AA + 8 nucleotides, with bond order) → inter-residue peptide/phosphodiester bonds → distance fallback for ligands
- **Multi-renderer pipeline** — Unicode Braille (8x resolution), half-block, ASCII, and native pixel protocols (Sixel, Kitty, iTerm2) with auto-detection
- **VIM-like modal interface** — Normal, Command, Search modes with trie-based multi-key bindings (`sw`, `dd`, `gt`, etc.)
- **Rich representations** — wireframe, ball-and-stick, spacefill, cartoon (Catmull-Rom spline + cross-section rasterization), flat ribbon, backbone trace — per-object or per-selection visibility
- **Selection algebra** — recursive descent parser: `chain A and helix`, `resi 50-60 or name CA`, boolean `and/or/not` with parentheses
- **Mouse selection** — `gs`/`gS`/`gc` pick modes for atom/residue/chain selection with `$sele` highlight overlay
- **Multi-level inspect** — click to inspect at atom/residue/chain/object level (`I` cycles), pick registers pk1-pk4 for measurements
- **Biological assemblies** — generate quaternary structures from PDB/mmCIF symmetry operators (`:assembly`)
- **Structure alignment** — TM-align and MM-align via USalign integration
- **Online fetch** — download from RCSB PDB (`fetch 1abc`) and AlphaFold DB (`fetch afdb:P12345`)
- **Session management** — auto-save on quit, `--resume` to restore, `:save` for manual save
- **PyMOL session export** — `.pml` scripts with `set_view`, repr, coloring
- **Screenshot from any renderer** — `:screenshot` renders offscreen via PixelCanvas even in braille/ASCII mode
- **Multi-state animation** — NMR ensemble / trajectory state cycling with `[`/`]` keys
- **Measurement tools** — `:measure`, `:angle`, `:dihedral` with pk1-pk4 pick registers or serial numbers
- **Full customization** — keybindings, color themes, and settings via TOML configs in `~/.molterm/`
- **Structured logging** — session log to `~/.molterm/molterm.log`

## Quick Start

```bash
# Build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Run
./molterm protein.pdb
./molterm structure.cif.gz        # gzipped files supported
./molterm --resume                # restore last session (auto-saved on quit)
./molterm -r                      # short form
./molterm --version               # prints version + git hash
```

### Dependencies

| Dependency | Version | Source |
|-----------|---------|--------|
| **gemmi** | v0.7.0 | FetchContent (automatic) |
| **USalign** | latest | FetchContent (automatic) |
| **toml++** | v3.4.0 | FetchContent (automatic) |
| **ncurses** | system | `apt install libncurses-dev` / `brew install ncurses` |
| **zlib** | system | Usually pre-installed |

All C++ dependencies are fetched automatically by CMake. Only ncurses and zlib need to be installed on the system.

---

## Usage

### VIM-like Modes

| Mode | Entry | Exit | Purpose |
|------|-------|------|---------|
| **Normal** | `ESC` / `Ctrl+C` | — | Navigation, object manipulation, mouse inspect/select |
| **Command** | `:` | `ESC`, `Enter` | Typed commands with tab completion |
| **Search** | `/` | `ESC`, `Enter` | Selection expression search, `n`/`N` navigate |

### Keybindings (press `?` for in-app cheat sheet)

<details>
<summary><strong>Navigation</strong></summary>

| Key | Action |
|-----|--------|
| `h`/`j`/`k`/`l` or arrows | Rotate molecule |
| `W`/`A`/`S`/`D` | Pan view |
| `+`/`-` | Zoom in/out |
| `<`/`>` | Z-axis rotation |
| `0` | Reset view |
| `.` | Repeat last action |
| Scroll wheel | Zoom |

</details>

<details>
<summary><strong>Representations</strong> — <code>s</code>=show, <code>x</code>=hide</summary>

| Key | Action |
|-----|--------|
| `sw` / `xw` | Wireframe |
| `sb` / `xb` | Ball-and-stick |
| `ss` / `xs` | Spacefill (CPK) |
| `sc` / `xc` | Cartoon (3D tube) |
| `sr` / `xr` | Ribbon (flat) |
| `sk` / `xk` | Backbone trace |
| `xa` | Hide all |
| `gd` | Apply default preset (cartoon + ballstick ligands) |

</details>

<details>
<summary><strong>Coloring</strong> — <code>c</code> prefix</summary>

| Key | Scheme |
|-----|--------|
| `ce` | Element (CPK) |
| `cc` | Chain |
| `cs` | Secondary structure |
| `cb` | B-factor |
| `cp` | pLDDT (AlphaFold confidence) |
| `cr` | Rainbow (N→C terminus) |
| `ct` | Residue type (nonpolar/polar/acidic/basic) |

</details>

<details>
<summary><strong>Objects, Tabs &amp; Other</strong></summary>

| Key | Action |
|-----|--------|
| `Tab` / `Shift+Tab` | Next/prev object |
| `Space` | Toggle visibility |
| `dd` | Delete object |
| `yy` / `p` | Yank / paste object |
| `gt` / `gT` | Next/prev tab |
| `Ctrl+T` / `Ctrl+W` | New/close tab |
| `o` | Toggle object panel |
| `i` | Inspect info (shows current level) |
| `I` | Cycle inspect level (atom/residue/chain/object) |
| Click | Inspect at current level (stores pk1→pk4 registers) |
| `gs` | Enter atom select mode (click to toggle atoms in `$sele`) |
| `gS` | Enter residue select mode (click to toggle residues) |
| `gc` | Enter chain select mode (click to toggle chains) |
| `ESC` | Exit select mode, keep selection |
| `[` / `]` | Prev/next state (NMR ensembles) |
| `m` | Toggle braille/pixel renderer |
| `P` | Screenshot (PNG, pixel renderer) |
| `q` + `a-z` | Record macro |
| `@` + `a-z` | Play macro |
| `?` | Help overlay |

</details>

### Commands

```vim
:load <file>                    " Load mmCIF/PDB/gzipped file
:fetch <pdb_id>                 " Download from RCSB PDB (e.g. fetch 1abc)
:fetch afdb:<uniprot_id>        " Download from AlphaFold DB (e.g. fetch afdb:P12345)
:show <repr> [selection]         " Show repr (optionally for selection only)
:hide <repr|all> [selection]    " Hide repr (optionally for selection only)
:color <scheme>                 " element, chain, ss, bfactor, plddt, rainbow, restype, clear
:color <name> [selection]       " Per-atom color (red, blue, salmon, etc.) with optional selection
:select <expr>                  " Select atoms (see Selection Algebra below)
:select <name> = <expr>         " Named selection (e.g. :select s1 = $sele)
:select clear                   " Clear mouse selection ($sele)
:count <expr>                   " Count matching atoms
:center [selection]             " Center view
:zoom [selection]               " Center + zoom to fit
:orient [selection]             " Align principal axes + center + zoom
:align <obj> [sel] to <obj>     " TM-align via USalign
:mmalign <obj> [sel] to <obj>   " MM-align for complexes
:assembly [id|list]             " Generate biological assembly (default: 1)
:measure [s1 s2]                " Distance (no args = pk1↔pk2 from last clicks)
:angle [s1 s2 s3]              " Angle at s2 (no args = pk1-pk2-pk3)
:dihedral [s1 s2 s3 s4]        " Dihedral (no args = pk1-pk4)
                                " Args: serial number, pk1-pk4, or $selection
:label <selection>              " Show residue labels on viewport
:label clear                    " Remove all labels (also :unlabel)
:preset                         " Apply smart defaults (cartoon protein, ballstick ligands)
:run <script.mt>                " Execute command script (# comments supported)
:save                           " Save session (auto-saved on quit)
:export <file.pml>              " Export session as PyMOL script
:screenshot [file.png]          " Save viewport as PNG (works in any renderer)
:set renderer <type>            " ascii, braille, block, pixel, sixel, kitty, iterm2
:set fog <0-1>                  " Depth fog strength
:set bt|wt|br <n>               " Backbone/wireframe thickness, ball radius
:info                           " Show atom/bond count
:q                              " Quit
```

### Selection Algebra

Recursive descent parser with boolean operators. Used by `:select`, `:count`, `:color`, and `/` search.

```vim
:select chain A and helix               " helix residues in chain A
:select resi 50-60 or name CA           " residue range or all Cα atoms
:select not water and not hydro          " heavy atoms, no water
:select backbone and chain B             " backbone of chain B
:select active = resi 100-120 and chain A
:select site = $sele                     " save mouse selection to named selection
:color red $active                       " use named selection with $
:show cartoon chain A                    " show cartoon only for chain A
/helix and chain A                       " search with n/N navigation
```

**Mouse selection workflow:**

1. `gs` (atom), `gS` (residue), or `gc` (chain) to enter select mode
2. Click to toggle atoms/residues/chains in `$sele` (status bar shows count)
3. `ESC` to exit select mode
4. `:select mysite = $sele` to save, `:color red $mysite`, `:show cartoon $mysite`
5. `:select clear` to reset

**Pick registers for measurement:**

1. Click atoms in inspect mode — each click stores pk1→pk2→pk3→pk4 (rotating)
2. `:measure` (distance pk1↔pk2), `:angle` (pk1-pk2-pk3), `:dihedral` (pk1-pk4)

**Keywords:** `all`, `chain`, `resn`, `resi` (range), `name`, `element`, `helix`, `sheet`, `loop`, `backbone`/`bb`, `sidechain`/`sc`, `hydro`, `water`, `obj`, `$name`

**Operators:** `and`, `or`, `not`, `( )`

---

## Rendering

### Canvas Backends

| Backend | Resolution | Characters | Best for |
|---------|-----------|------------|----------|
| **BrailleCanvas** (default) | 8× (2×4 sub-pixels) | Unicode Braille `⠀`–`⣿` | SSH, most terminals |
| **BlockCanvas** | 2× (1×2 sub-pixels) | Half-blocks `▀▄█` | Wide compatibility |
| **AsciiCanvas** | 1× | `* @ - \| /` | Legacy terminals |
| **PixelCanvas** | Native pixels | Sixel / Kitty / iTerm2 | Local terminals with graphics support |

**PixelCanvas features:** sphere shading (Half-Lambert), line shading, depth fog, frame diff, adaptive frame skip, LOD for >10K atoms.

Switch at runtime: `:set renderer braille|block|ascii|pixel` or `m` to toggle.

### Color Schemes

| Scheme | Key | Description |
|--------|-----|-------------|
| Element (CPK) | `ce` | C=green N=blue O=red S=yellow P=magenta |
| Chain | `cc` | Cycled per chain (green, cyan, magenta, yellow, red, blue) |
| Secondary structure | `cs` | Helix=red Sheet=yellow Loop=green |
| B-factor | `cb` | Blue→Green→Red gradient |
| pLDDT | `cp` | AlphaFold confidence (>90 blue, 70-90 light blue, 50-70 yellow, <50 orange) |
| Rainbow | `cr` | Per-chain N→C terminus blue→red gradient |
| Residue type | `ct` | VMD-like: nonpolar (white), polar (green), acidic (red), basic (blue) |

**Per-atom coloring:** `:color <name> [selection]` — 15 named colors: `red green blue yellow magenta cyan white orange pink lime teal purple salmon slate gray`

---

## Customization

Configuration files in `~/.molterm/`:

```
~/.molterm/
├── config.toml          # general settings (default renderer, auto-center, etc.)
├── keymap.toml          # custom keybindings (overrides defaults)
├── colors.toml          # custom color schemes
└── molterm.log          # session log (auto-created)
```

<details>
<summary><strong>keymap.toml example</strong></summary>

```toml
[normal]
"h"         = "rotate_left"
"j"         = "rotate_down"
"k"         = "rotate_up"
"l"         = "rotate_right"
"H"         = "pan_left"
"J"         = "pan_down"
"K"         = "pan_up"
"L"         = "pan_right"
"+"         = "zoom_in"
"-"         = "zoom_out"
"0"         = "reset_view"
"gt"        = "next_tab"
"gT"        = "prev_tab"
"<C-t>"     = "new_tab"
"<C-w>"     = "close_tab"
"sw"        = "show_wireframe"
"sb"        = "show_ballstick"
"sc"        = "show_cartoon"
"sr"        = "show_ribbon"
"ce"        = "color_by_element"
"cc"        = "color_by_chain"
"/"         = "enter_search"
"?"         = "show_help"
"["         = "prev_state"
"]"         = "next_state"

[command]
"<CR>"    = "execute"
"<Esc>"   = "exit_to_normal"
"<Tab>"   = "autocomplete"
```

</details>

<details>
<summary><strong>colors.toml example</strong></summary>

```toml
[schemes.element]
C  = "green"
N  = "blue"
O  = "red"
S  = "yellow"
P  = "magenta"
H  = "white"
_default = "white"

[schemes.chain]
_cycle = ["green", "cyan", "magenta", "yellow", "red", "blue"]

[schemes.ss]
helix = "red"
sheet = "yellow"
loop  = "green"

[schemes.bfactor]
gradient = ["blue", "green", "red"]
min = 0.0
max = 100.0
```

</details>

---

## Architecture

```
molterm/
├── CMakeLists.txt
├── include/molterm/
│   ├── app/         Application, TabManager, Tab
│   ├── core/        MolObject, AtomData, BondData, Selection, ObjectStore, Logger
│   ├── io/          CifLoader, Aligner, SessionExporter
│   ├── render/      Canvas (Braille/Block/Ascii/Pixel), Camera, ColorMapper, DepthBuffer
│   │                GraphicsEncoder (Sixel/Kitty/iTerm2), ProtocolPicker
│   ├── repr/        Representation (Wireframe/BallStick/Backbone/Spacefill/Cartoon/Ribbon)
│   ├── tui/         Screen, Window, Layout, StatusBar, CommandLine, TabBar, ObjectPanel
│   ├── input/       InputHandler, Keymap (trie), KeymapManager, Action, Mode
│   ├── cmd/         CommandParser, CommandRegistry, UndoStack
│   └── config/      ConfigParser (TOML)
└── src/             .cpp implementations mirror include/ structure
```

### Rendering Pipeline

```
MolObject → Representation → Canvas → Window (ncurses)
                 ↑                ↑
   ColorMapper (scheme)     Camera (3×3 rot + pan + zoom)
```

### Coding Conventions

- **C++17** strict — no exceptions in hot paths
- `std::unique_ptr` / `std::shared_ptr` with clear ownership
- `enum class` over raw enums
- `#pragma once` for header guards
- **Naming:** `PascalCase` types, `camelCase` methods/variables, `UPPER_SNAKE` constants
- All ncurses calls through `Screen`/`Window` wrappers
- Separate concerns: parsing (io/), model (core/), rendering (render/ + repr/), TUI (tui/), input (input/), commands (cmd/)

---

## PyMOL Export

Export the current session as a `.pml` script that reconstructs the view in PyMOL:

```
:export session.pml
```

Generates `load`, `show`, `color`, `select`, and `set_view` commands with the current camera matrix.

---

## Implementation Status

### Phase 1: Foundation — DONE

- [x] CMakeLists.txt with gemmi (FetchContent) + ncurses
- [x] Screen/Window — ncurses RAII wrappers
- [x] MolObject + CifLoader — gemmi mmCIF/PDB parsing, spatial hash bond detection + `_struct_conn`
- [x] Camera — 3×3 rotation matrix, orthographic projection, zoom, pan
- [x] AsciiRenderer — basic wireframe rendering (legacy, replaced by Canvas in Phase 2)
- [x] InputHandler — trie-based multi-key sequences, 4-mode state machine
- [x] Layout — TabBar, Viewport, ObjectPanel, StatusBar, CommandLine
- [x] Commands — :load, :q, :show, :hide, :color, :zoom, :tabnew, :tabclose, :objects, :delete, :rename, :info, :help, :set
- [x] Tab system — multiple tabs, copy/move objects between tabs
- [x] SIGWINCH resize handling

### Phase 2: Rich Rendering — DONE

- [x] Canvas abstraction — abstract sub-pixel drawing (drawDot, drawLine, drawCircle)
- [x] BrailleCanvas — 2×4 sub-pixel Unicode Braille, 8× resolution
- [x] BlockCanvas — 1×2 sub-pixel Unicode half-blocks, 2× resolution
- [x] AsciiCanvas — 1×1 fallback with directional line chars
- [x] DepthBuffer — Z-sorting at sub-pixel resolution
- [x] ColorMapper — element/chain/SS/B-factor color schemes
- [x] WireframeRepr — half-bond coloring, atom dots
- [x] BallStickRepr — filled circles with adaptive radius
- [x] BackboneRepr — Cα–Cα chain trace
- [x] Mouse support — scroll wheel zoom, tab bar click
- [x] Runtime renderer switching — `:set renderer ascii|braille|block`

### Phase 3: VIM Features + Selection — DONE

- [x] Selection algebra — recursive descent parser: chain, resn, resi (range), name, element, helix/sheet/loop, backbone/sidechain, hydro, water, and/or/not/parens
- [x] Search — `/` parses selection expression, `n`/`N` navigate matches with atom details
- [x] Undo/Redo — UndoStack with push/undo/redo, 100-entry limit, `u`/`Ctrl+R` bindings
- [x] SpacefillRepr — VDW spheres, back-to-front sorted, scale adjustable
- [x] CartoonRepr — SS-aware Cα trace (thick helix, wide sheet, thin loop)
- [x] Commands — `:select <expr>`, `:select name = expr`, `:count <expr>`, `:sele`
- [x] Named selections — stored in map, `:sele` lists with atom counts
- [x] Per-atom coloring — 15-color palette, `:color <name> <selection>`, overrides scheme
- [x] `$name` references — `$sele`, `$ala` etc. in selection expressions
- [x] Auto-sele — every selection result auto-saved as `sele`
- [x] `obj` keyword — `obj myprotein` selects all atoms if object name matches
- [x] Inspect mode — mouse click picks atoms at configurable level (atom/residue/chain/object), `I` cycles level
- [x] Command history — `:` shows last 5 commands overlay, `↑`/`↓` cycle, 200 limit
- [x] USalign integration — `:align`, `:mmalign`, `:super` with per-side selection and `-ter 0`

### Phase 4: Customization + Export — DONE

- [x] ConfigParser — TOML config loading from `~/.molterm/` via toml++ v3.4.0
- [x] KeymapManager TOML — fully customizable keybindings from `keymap.toml`
- [x] Color schemes — user-defined color themes from `colors.toml`
- [x] SessionExporter — `.pml` script generation with `set_view`, repr, coloring
- [x] `:fetch` — download structures from RCSB PDB (`fetch 1abc`) and AlphaFold DB (`fetch afdb:P12345`)
- [x] pLDDT — AlphaFold confidence color scheme, `cp` keybinding, `:color plddt`
- [x] Macro recording — `q` + register (a-z) to record, `@` + register to play
- [x] Tab completion — context-aware for commands, filenames, object names, repr names, color names, settings
- [x] `$` selection prefix — `$sele`, `$ala` etc. (changed from `@`)

### Phase 4.5: PixelCanvas + Graphics — DONE

- [x] PixelCanvas — RGB framebuffer with pluggable GraphicsEncoder (Sixel/Kitty/iTerm2)
- [x] ProtocolPicker — auto-detect terminal graphics protocol via env vars
- [x] KittyEncoder — zlib compression + chunked base64 + atomic image replacement
- [x] ITermEncoder — OSC 1337 inline image protocol with BMP encoding
- [x] SixelEncoder — 6×6×6 color cube quantization + RLE + transparent background
- [x] Depth fog — post-pass atmospheric perspective, configurable via `:set fog`
- [x] Sphere shading — Half-Lambert lighting on filled circles (Spacefill/BallStick)
- [x] Line shading — depth-based intensity on wireframe/backbone/cartoon
- [x] Z-axis rotation — `<`/`>` keys for roll
- [x] Projection cache — `prepareProjection()` per frame, `projectCached()` per vertex
- [x] LOD — skip atom dots for >10K atom wireframe
- [x] Adaptive frame skip — skip 1-3 frames when render > 100ms
- [x] Frame diff — skip identical frames via RGB memcmp
- [x] Rainbow color scheme — per-chain N→C blue→red gradient, `cr` keybinding
- [x] Gzipped PDB/CIF — transparent `.gz` support via gemmi
- [x] `-v`/`--version` — git tag or dev+hash

### Phase 5: Polish — DONE

- [x] **Help system** — `?` shows keybinding cheat sheet overlay (press any key to dismiss)
- [x] **Measurement tools** — `:measure`, `:angle`, `:dihedral` with pick registers (pk1-pk4) or serial numbers
- [x] **Multi-state animation** — `[`/`]` state cycling for NMR ensembles; state shown in status bar
- [x] **Ribbon geometry** — Catmull-Rom spline ribbon with C→O guide vectors, sheet arrowheads, cross-fill
- [x] **Logging** — structured logging to `~/.molterm/molterm.log` with timestamped session markers

### Phase 5.5: Smart Defaults + Interaction — DONE

- [x] **3-tier bond detection** — standard residue table (20 AA + 8 NA with bond order) → peptide/phosphodiester inter-residue → distance fallback for ligands
- [x] **Smart default repr** — auto-detect protein/NA/ligand: cartoon for macromolecules, ball-and-stick for ligands, chain coloring
- [x] **`:preset` / `gd`** — re-apply smart defaults on demand
- [x] **VMD-like residue type coloring** — `ct` / `:color restype` (nonpolar/polar/acidic/basic)
- [x] **Mouse-only inspect** — click to inspect at atom/residue/chain/object level, `I` cycles
- [x] **Pick registers** — pk1→pk4 rotating, used by `:measure`/`:angle`/`:dihedral` (no args)
- [x] **Mouse selection modes** — `gs` atom, `gS` residue, `gc` chain, click to toggle in `$sele`
- [x] **Selection highlight** — `$sele` atoms shown as `*` overlay on viewport
- [x] **Per-selection show/hide** — `:show cartoon chain A`, `:hide wireframe helix`
- [x] **Biological assembly** — `:assembly [id|list]` via gemmi `make_assembly()`
- [x] **Session autosave** — auto-save on quit, `--resume` / `-r` to restore, `:save` manual
- [x] **Offscreen screenshot** — `:screenshot` works in any renderer (offscreen PixelCanvas)
- [x] **SSH optimizations** — BrailleCanvas diff-flush, projection dedup, Bresenham depth step

### Phase 6: Annotation + Scripting — DONE

- [x] **Atom/residue labels** — `:label <selection>` rendered on viewport, `:label clear` / `:unlabel` to remove
- [x] **Measurement display** — dashed lines + distance/angle values drawn between measured atoms on viewport
- [x] **`:run` script** — execute `.mt` command script files for automation (`:run setup.mt`)
- [x] **BlockCanvas diff flush** — cell-level dirty tracking (same as BrailleCanvas) for SSH

### Phase 7: Visualization

- [ ] **Solvent-accessible surface** — Shrake-Rupley SAS, rendered as silhouette contour or filled mesh
- [ ] **Stereoscopic view** — side-by-side 3D (split viewport, ±2° rotation offset)
- [ ] **Sequence bar** — bottom row showing 1-letter sequence, click to navigate to residue
- [ ] **Contact map** — residue-residue Cα distance matrix overlay
- [ ] **Electrostatic coloring** — Coulombic surface color from partial charges

### Phase 8: Export + Generation

- [ ] **Animation export** — rotate/state-cycle → GIF or APNG (`:record spin 360 out.gif`)
- [ ] **Crystal packing** — symmetry mates from `UnitCell` (`:symmates [radius]`)
- [ ] **SMILES input** — `:smiles CCO` → simple 3D coordinate generation

### Optimization Backlog

- [x] Frustum culling — BallStick/Backbone skip off-screen atoms (Wireframe, Spacefill already had it)
- [x] Spatial hash for picking — `findNearestAtom` O(N) → O(1) via 2D grid (20px cells, 3×3 query)
- [x] Spacefill depth pre-sort — sort only when camera dirty, reuse sorted order across frames
- [ ] Compile-time bond table — `constexpr` static initialization

---

## License

MIT
