# MolTerm — TUI Molecular Viewer

> A terminal-based molecular structure viewer with VIM-like interface, ncurses TUI, gemmi-powered mmCIF/PDB parsing, and PyMOL session export.

## Project Overview

MolTerm renders 3D molecular structures in the terminal using Unicode Braille characters and ncurses colors. It targets structural biologists and computational chemists who live in the terminal and want quick molecule inspection without launching a full GUI.

**Language:** C++17  
**Build:** CMake ≥ 3.16  
**Dependencies:** ncurses (system), gemmi v0.7.0 (FetchContent)

## Build Instructions

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
./molterm [file.cif] [file.pdb] [file.pdb.gz] ...
./molterm --version    # prints version + git hash
```

### Dependencies

- **gemmi** v0.7.0 — mmCIF/PDB parser with gzip support (fetched by CMake FetchContent)
- **USalign** — structural alignment TM-align/MM-align (fetched + compiled by CMake FetchContent)
- **toml++** v3.4.0 — TOML config parser (fetched by CMake FetchContent)
- **ncurses** — system package (`apt install libncurses-dev` / `brew install ncurses`)

---

## Architecture

```
molterm/
├── CMakeLists.txt
├── README.md
├── .gitignore
├── include/molterm/
│   ├── app/
│   │   ├── Application.h         # top-level lifecycle, main loop
│   │   ├── TabManager.h          # tab container, move/copy between tabs
│   │   └── Tab.h                 # single tab (objects + camera)
│   ├── core/
│   │   ├── MolObject.h           # molecular object (atoms, bonds, repr, per-atom colors)
│   │   ├── AtomData.h            # per-atom struct (pos, name, element, chain, resSeq, B, occ)
│   │   ├── BondData.h            # bond struct (atom indices + order)
│   │   ├── Selection.h           # selection algebra: parse, set ops, named selections
│   │   └── ObjectStore.h         # shared_ptr registry, add/remove/rename/clone
│   ├── io/
│   │   ├── CifLoader.h           # gemmi mmCIF/PDB loader with spatial hash bonding + _struct_conn
│   │   └── Aligner.h             # USalign integration: TM-align, MM-align, transform
│   ├── render/
│   │   ├── Canvas.h              # abstract sub-pixel canvas (drawDot, drawLine, drawCircle)
│   │   ├── AsciiCanvas.h         # 1×1 sub-pixel per cell, ASCII chars
│   │   ├── BrailleCanvas.h       # 2×4 sub-pixel per cell, Unicode Braille (U+2800–U+28FF)
│   │   ├── BlockCanvas.h         # 1×2 sub-pixel per cell, Unicode half-blocks (▀▄█)
│   │   ├── PixelCanvas.h         # RGB framebuffer + pluggable GraphicsEncoder
│   │   ├── GraphicsEncoder.h     # abstract encoder interface (Sixel/Kitty/iTerm2)
│   │   ├── SixelEncoder.h        # Sixel protocol encoder
│   │   ├── KittyEncoder.h        # Kitty graphics protocol (zlib + chunked)
│   │   ├── ITermEncoder.h        # iTerm2 inline image protocol (OSC 1337)
│   │   ├── ProtocolPicker.h      # auto-detect terminal graphics protocol
│   │   ├── Renderer.h            # legacy abstract renderer (Phase 1)
│   │   ├── AsciiRenderer.h       # legacy Phase 1 ASCII renderer
│   │   ├── Camera.h              # 3×3 rotation, pan, zoom, projection cache
│   │   ├── DepthBuffer.h         # Z-buffer for occlusion (header-only)
│   │   └── ColorMapper.h         # color schemes + 15-color named palette + per-atom overrides
│   ├── repr/
│   │   ├── Representation.h      # abstract: render(MolObject, Camera, Canvas)
│   │   ├── WireframeRepr.h       # bond lines (half-bond coloring) + atom dots
│   │   ├── BallStickRepr.h       # filled circles + thin bond lines
│   │   ├── BackboneRepr.h        # Cα trace per chain
│   │   ├── SpacefillRepr.h       # VDW spheres, depth-sorted
│   │   └── CartoonRepr.h         # SS-aware trace (helix/sheet/loop widths)
│   ├── tui/
│   │   ├── Screen.h              # ncurses RAII (initscr/endwin, colors, input, mouse)
│   │   ├── Window.h              # WINDOW* RAII wrapper (print, color, resize)
│   │   ├── Layout.h              # split: TabBar | Viewport [| ObjectPanel] | StatusBar | CmdLine
│   │   ├── StatusBar.h           # mode indicator, object info, renderer name
│   │   ├── CommandLine.h         # ":" input with cursor, history, word delete
│   │   ├── ObjectPanel.h         # right sidebar: object list with visibility indicators
│   │   └── TabBar.h              # top row: clickable tab labels
│   ├── input/
│   │   ├── InputHandler.h        # mode-aware key dispatch, trie sequence handling
│   │   ├── KeymapManager.h       # loads default keybindings (TOML in Phase 4)
│   │   ├── Keymap.h              # trie-based multi-key sequence → Action mapping
│   │   ├── Action.h              # enum of all bindable actions
│   │   └── Mode.h                # enum: Normal, Command, Visual, Search
│   └── cmd/
│       ├── CommandParser.h       # parse ":cmd arg1, arg2" with quotes and ! detection
│       ├── CommandRegistry.h     # name → lambda handler, tab completion
│       └── UndoStack.h           # undo/redo with 100-entry limit
└── src/                          # .cpp implementations mirror include/ structure
    ├── main.cpp
    ├── app/
    ├── core/
    ├── io/
    ├── render/
    ├── repr/
    ├── tui/
    ├── input/
    └── cmd/
```

---

## Rendering Pipeline

```
MolObject ──→ Representation ──→ Canvas ──→ Window
                   ↑                ↑
    ColorMapper (scheme + per-atom) Camera (3×3 rot + pan + zoom)
```

### Canvas Abstraction

All rendering goes through the `Canvas` interface, which provides sub-pixel drawing primitives. Three backends:

| Canvas | Sub-pixels/cell | Resolution | Characters |
|--------|----------------|------------|------------|
| **BrailleCanvas** (default) | 2×4 | 8× | Unicode Braille `⠀`–`⣿` |
| **BlockCanvas** | 1×2 | 2× | Half-blocks `▀` `▄` `█` |
| **AsciiCanvas** | 1×1 | 1× | `*` `@` `o` `-` `\|` `/` `\` |
| **PixelCanvas** | terminal cell pixels | native | Sixel, Kitty, or iTerm2 protocol (auto-detected) |

**PixelCanvas** features: sphere shading (Half-Lambert), depth fog, frame diff (skip unchanged), adaptive frame skip.

Switch at runtime: `:set renderer braille|block|ascii|pixel`

### Representations

Each `Representation` subclass knows what to draw; the `Canvas` knows how.

| Repr | Show/Hide | Default | Description |
|------|-----------|---------|-------------|
| **WireframeRepr** | `sw`/`xw` | ON | Bond lines with half-bond coloring + atom dots. `:set wt <n>` |
| **BallStickRepr** | `sb`/— | off | Filled circles + thin bonds. `:set br <n>` |
| **BackboneRepr** | `sk`/`xk` | ON | Cα–Cα trace. `:set bt <n>` |
| **SpacefillRepr** | `ss`/— | off | VDW spheres (back-to-front sorted). Scale adjustable |
| **CartoonRepr** | `sc`/— | off | 3D cartoon: Catmull-Rom spline + parallel-transport frame, triangle-rasterized cross-sections (flat helix/sheet, circular coil tube), Lambert shading. Nucleic acid P-atom backbone |
| **RibbonRepr** | `sr`/— | off | Flat ribbon: C→O guide vectors, sheet arrowheads, cross-fill between edges |

### Color Schemes

| Scheme | Key | Description |
|--------|-----|-------------|
| Element (CPK) | `ce` | C=green N=blue O=red S=yellow P=magenta |
| Chain | `cc` | A=green B=cyan C=magenta D=yellow E=red F=blue (cycled) |
| Secondary structure | `cs` | Helix=red Sheet=yellow Loop=green |
| B-factor | `cb` | Blue→Green→Red gradient |
| pLDDT | `cp` | AlphaFold confidence: >90 deep blue, 70-90 light blue, 50-70 yellow, <50 orange |
| Rainbow | `cr` | N-terminus (blue) → C-terminus (red), per-chain gradient |

### Named Color Palette

Per-atom coloring via `:color <name> [selection]`. Overrides scheme for matched atoms.

**Available colors:** `red` `green` `blue` `yellow` `magenta` `cyan` `white` `orange` `pink` `lime` `teal` `purple` `salmon` `slate` `gray`

Extended colors (orange, pink, lime, teal, purple, salmon, slate, gray) use 256-color terminal; fallback to 8-color approximations.

```
:color red chain A            # color chain A red
:color salmon helix           # color helices salmon
:color cyan resi 50-60        # color residue range
:color blue                   # color all atoms blue
:color clear                  # remove all per-atom overrides
:color element                # switch back to element scheme
```

### Selection Algebra

Used by `:select`, `:count`, and `/` search. Recursive descent parser with operator precedence.

**Keywords:**

| Keyword | Example | Description |
|---------|---------|-------------|
| `all` | `all` | All atoms |
| `chain` | `chain A` | Chain ID |
| `resn` | `resn ALA` | Residue name |
| `resi` | `resi 42` or `resi 10-50` | Residue number (or range) |
| `name` | `name CA` | Atom name |
| `element` | `element Fe` | Element symbol |
| `helix` | `helix` | SSType::Helix atoms |
| `sheet` | `sheet` | SSType::Sheet atoms |
| `loop` | `loop` | SSType::Loop atoms |
| `backbone` / `bb` | `backbone` | N, CA, C, O atoms |
| `sidechain` / `sc` | `sidechain` | Non-backbone atoms |
| `hydro` | `hydro` | Hydrogen atoms |
| `water` | `water` | HOH/WAT/DOD residues |
| `obj` | `obj myprotein` | All atoms if current object name matches |
| `$name` | `$ala and chain B` | Reference a named selection |

**Operators:** `and`, `or`, `not`, `( )` — precedence: `or` < `and` < `not`

**Auto-selection:** Every selection operation auto-saves the result as `sele`. Use `$sele` to reference the last result.

**Examples:**

```
:select chain A and resn ALA        # → auto-saved as "sele"
:select ala = resn ALA              # named selection
:color red $ala                     # use named selection in color command
:select active = resi 50-60 and chain A
:count $active and helix            # reference named selection
:color salmon $sele                 # color latest selection result
:sele                               # list: sele(25) ala(25) active(42)
/helix and chain B                  # search mode with n/N navigation
```

---

## VIM-like Mode System

### Modes

| Mode | Entry | Exit | Purpose |
|------|-------|------|---------|
| **Normal** | `ESC` / `Ctrl+C` from any mode | — | Navigation, object manipulation |
| **Command** | `:` from Normal | `ESC`, `Enter` (execute) | Typed commands |
| **Visual** | `v` from Normal | `ESC` | Atom/residue selection (Phase 4) |
| **Search** | `/` from Normal | `ESC`, `Enter` (execute) | Search via selection expressions, `n`/`N` navigate |

### Default Keybindings (Fully Customizable)

All keybindings are defined via a trie-based `Keymap`. Defaults in `KeymapManager::loadDefaults()`. Users will override via `~/.molterm/keymap.toml` (Phase 4).

#### Normal Mode — Navigation

| Key | Action | Description |
|-----|--------|-------------|
| `h` / `←` | `rotate_left` | Rotate molecule left (Y-axis) |
| `j` / `↓` | `rotate_down` | Rotate molecule down (X-axis) |
| `k` / `↑` | `rotate_up` | Rotate molecule up (X-axis) |
| `l` / `→` | `rotate_right` | Rotate molecule right (Y-axis) |
| `<` | `rotate_ccw` | Rotate counter-clockwise (Z-axis) |
| `>` | `rotate_cw` | Rotate clockwise (Z-axis) |
| `W` | `pan_up` | Pan view up |
| `A` | `pan_left` | Pan view left |
| `S` | `pan_down` | Pan view down |
| `D` | `pan_right` | Pan view right |
| `+` / `=` | `zoom_in` | Zoom in |
| `-` | `zoom_out` | Zoom out |
| `0` | `reset_view` | Reset camera to default |
| `.` | `repeat_last` | Repeat last action |
| `Ctrl+L` | `redraw` | Force redraw |
| Scroll wheel | zoom | Mouse scroll zooms in/out |

#### Normal Mode — Object & State

| Key | Action | Description |
|-----|--------|-------------|
| `o` | `toggle_panel` | Toggle object panel sidebar |
| `Tab` | `next_object` | Cycle to next object |
| `Shift+Tab` | `prev_object` | Cycle to previous object |
| `Space` | `toggle_visible` | Toggle visibility of current object |
| `d` `d` | `delete_object` | Delete current object |
| `y` `y` | `yank_object` | Yank (copy) object to clipboard |
| `p` | `paste_object` | Paste yanked object into current tab |
| `r` | `rename_object` | Rename current object |

#### Normal Mode — Representations

| Key | Action | Description |
|-----|--------|-------------|
| `s` `w` | `show_wireframe` | Show wireframe |
| `s` `b` | `show_ballstick` | Show ball-and-stick |
| `s` `s` | `show_spacefill` | Show spacefill / CPK |
| `s` `c` | `show_cartoon` | Show cartoon |
| `s` `k` | `show_backbone` | Show backbone trace |
| `x` `w` | `hide_wireframe` | Hide wireframe |
| `x` `k` | `hide_backbone` | Hide backbone trace |
| `x` `a` | `hide_all` | Hide all representations |

Mnemonic: **s**how → `s` prefix, e**x**it/remove → `x` prefix.

#### Normal Mode — Tabs

| Key | Action | Description |
|-----|--------|-------------|
| `g` `t` | `next_tab` | Go to next tab |
| `g` `T` | `prev_tab` | Go to previous tab |
| `Ctrl+T` | `new_tab` | Open new tab |
| `Ctrl+W` | `close_tab` | Close current tab |
| `m` `t` | `move_to_tab` | Move current object to another tab |
| `d` `t` | `copy_to_tab` | Copy current object to another tab |
| Click tab | `goto_tab` | Mouse click on tab bar |

#### Normal Mode — Coloring

| Key | Action | Description |
|-----|--------|-------------|
| `c` `e` | `color_by_element` | Color by element (CPK) |
| `c` `c` | `color_by_chain` | Color by chain |
| `c` `s` | `color_by_ss` | Color by secondary structure |
| `c` `b` | `color_by_bfactor` | Color by B-factor (heat map) |
| `c` `p` | `color_by_plddt` | Color by pLDDT (AlphaFold confidence) |
| `c` `r` | `color_by_rainbow` | Color rainbow N→C terminus |

#### Normal Mode — Quick Actions

| Key | Action | Description |
|-----|--------|-------------|
| `:` | `enter_command` | Enter command mode |
| `/` | `enter_search` | Enter search mode |
| `v` | `enter_visual` | Enter visual mode |
| `n` | `search_next` | Next search match |
| `N` | `search_prev` | Previous search match |
| `i` | `inspect` | Show detailed info for atom under cursor |
| `?` | `show_help` | Show keybinding help |
| `u` | `undo` | Undo last action |
| `Ctrl+R` | `redo` | Redo |
| `m` | `toggle_pixel` | Toggle braille ↔ pixel renderer |
| `P` | `screenshot` | Save viewport as PNG (pixel renderer) |
| `q` | `start_macro` | Start/stop macro recording (then press a-z for register) |
| `@` | `play_macro` | Play macro (then press a-z for register) |

#### Command Mode (`:`)

Input editing: `Backspace`, `Ctrl+W` (delete word), `Ctrl+U` (clear line).
History: `↑` / `↓` arrow keys cycle through previous commands. Pressing `:` shows last 5 commands as overlay (disappears when typing). History is deduped, 200 entry limit.

**Implemented commands:**

| Command | Description |
|---------|-------------|
| `:load <file>` / `:e <file>` | Load mmCIF/PDB file |
| `:q[!]` / `:quit[!]` / `:qa` | Quit |
| `:show <repr>` | Show repr (wireframe/wire, ballstick/sticks/bs, backbone/trace/ca, spacefill/spheres/cpk, cartoon/ribbon) |
| `:hide <repr\|all>` | Hide repr or all |
| `:color <scheme>` | Color by element/cpk, chain, ss/secondary, bfactor/b, plddt, rainbow, clear |
| `:color <name> [selection]` | Per-atom color: `:color red chain A` (see Named Color Palette) |
| `:center [selection]` | Center camera on selection (or all atoms) |
| `:zoom [selection]` | Center and zoom to fit selection |
| `:orient [selection]` | Align principal axes + center + zoom |
| `:screenshot [file.png]` | Save viewport as PNG (pixel renderer, transparent bg) |
| `:set renderer <type>` | Switch renderer: ascii, braille, block, pixel/auto, kitty, sixel, iterm2 |
| `:set backbone_thickness <n>` | Backbone trace thickness, float (alias: `bt`) |
| `:set wireframe_thickness <n>` | Wireframe line thickness, float (alias: `wt`) |
| `:set ball_radius <n>` | Ball-and-stick atom radius (alias: `br`) |
| `:set pan_speed <n>` | Pan speed per keypress (alias: `ps`, default: 5) |
| `:set fog <0.0-1.0>` | Depth fog strength (0=off, default: 0.35) |
| `:set auto_center` | Toggle auto-center on load (default: on) |
| `:set panel` | Toggle object panel |
| `:tabnew [name]` | Create new tab |
| `:tabclose` | Close current tab |
| `:objects` | List loaded objects |
| `:delete [name]` | Delete object (current if no arg) |
| `:rename [old] <new>` | Rename object |
| `:info` | Show atom/bond count for current object |
| `:select <expr>` | Select atoms by expression (see Selection Algebra) |
| `:select <name> = <expr>` | Create named selection |
| `:count <expr>` | Count atoms matching expression |
| `:sele` | List named selections with atom counts |
| `:align <obj> [sel] to <obj> [sel]` | TM-align via USalign (sel filters alignment, transforms whole object) |
| `:mmalign <obj> [sel] to <obj> [sel]` | MM-align for multi-chain complexes (`-mm 1`) |
| `:super <obj> [sel] to <obj> [sel]` | Alias for `:align` |
| `:fetch <pdb_id\|afdb:uniprot_id>` | Download from RCSB PDB or AlphaFold DB |
| `:export <file.pml>` | Export session as PyMOL script |
| `:help` | List available commands |

**Planned commands (Phase 5):**

```
:save <file>                    Save current state
:map <mode> <key> <action>      Map key to action in mode
:unmap <mode> <key>             Remove key mapping
:keybindings                    Show all current bindings
:measure <a1> <a2>              Measure distance
:angle <a1> <a2> <a3>           Measure angle
:dihedral <a1> <a2> <a3> <a4>  Measure dihedral
```

---

## Customization System (Phase 4)

### Configuration Files

```
~/.molterm/
├── config.toml          # general settings
├── keymap.toml          # custom keybindings (overrides defaults)
└── colors.toml          # custom color schemes
```

### keymap.toml Example

```toml
# Leader key (default: space)
leader = "space"

# Key notation: C- (Ctrl), M- (Alt), S- (Shift), <CR>, <Space>, <Tab>, <BS>

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
"<Tab>"     = "next_object"
"<S-Tab>"   = "prev_object"
"<Space>"   = "toggle_visible"
"gt"        = "next_tab"
"gT"        = "prev_tab"
"<C-t>"     = "new_tab"
"<C-w>"     = "close_tab"
"dd"        = "delete_object"
"yy"        = "yank_object"
"p"         = "paste_object"
"sw"        = "show_wireframe"
"sb"        = "show_ballstick"
"ss"        = "show_spacefill"
"sc"        = "show_cartoon"
"sk"        = "show_backbone"
"xw"        = "hide_wireframe"
"xk"        = "hide_backbone"
"xa"        = "hide_all"
"ce"        = "color_by_element"
"cc"        = "color_by_chain"
"cs"        = "color_by_ss"
"cb"        = "color_by_bfactor"
"mt"        = "move_to_tab"
"dt"        = "copy_to_tab"
"/"         = "enter_search"
"n"         = "search_next"
"N"         = "search_prev"
"i"         = "inspect"
"o"         = "toggle_panel"
"?"         = "show_help"
"u"         = "undo"
"<C-r>"     = "redo"
"."         = "repeat_last"
":"         = "enter_command"
"v"         = "enter_visual"

[visual]
"<Esc>" = "exit_to_normal"
"<C-c>" = "exit_to_normal"

[command]
"<CR>"    = "execute"
"<Esc>"   = "exit_to_normal"
"<C-c>"   = "exit_to_normal"
"<Tab>"   = "autocomplete"
"<Up>"    = "history_prev"
"<Down>"  = "history_next"
"<C-w>"   = "delete_word"
"<C-u>"   = "clear_line"

[search]
"<CR>"    = "execute_search"
"<Esc>"   = "exit_to_normal"
"<C-c>"   = "exit_to_normal"
```

### colors.toml Example

```toml
[schemes.element]
C  = "green"
N  = "blue"
O  = "red"
S  = "yellow"
P  = "magenta"
H  = "white"
Fe = "cyan"
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

---

## PyMOL Export (Phase 4)

### .pml Script Export

Generate a PyMOL command script that reconstructs the MolTerm session:

```python
# Generated by MolTerm
load 1abc.cif, obj_1abc
load 2def.pdb, obj_2def
hide everything
show cartoon, obj_1abc
show sticks, obj_1abc and chain A and resn HEM
color marine, obj_1abc and chain A
color salmon, obj_1abc and chain B
select active_site, obj_1abc and byres (resn HEM around 5)
set_view (\
     0.872,   -0.327,    0.364,\
     0.489,    0.582,   -0.650,\
    -0.000,    0.745,    0.667,\
     0.000,    0.000, -150.000,\
    12.345,   23.456,   34.567,\
   100.000,  200.000,  -20.000 )
```

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

- [x] **Selection algebra** — recursive descent parser: chain, resn, resi (range), name, element, helix/sheet/loop, backbone/sidechain, hydro, water, and/or/not/parens
- [x] **Search** — `/` parses selection expression, `n`/`N` navigate matches with atom details
- [x] **Undo/Redo** — UndoStack with push/undo/redo, 100-entry limit, `u`/`Ctrl+R` bindings
- [x] **SpacefillRepr** — VDW spheres, back-to-front sorted, scale adjustable
- [x] **CartoonRepr** — SS-aware Cα trace (thick helix, wide sheet, thin loop)
- [x] **Commands** — `:select <expr>`, `:select name = expr`, `:count <expr>`, `:sele`
- [x] **Named selections** — stored in map, `:sele` lists with atom counts
- [x] **Per-atom coloring** — 15-color palette, `:color <name> <selection>`, overrides scheme
- [x] **$name references** — `$sele`, `$ala` etc. in selection expressions
- [x] **Auto-sele** — every selection result auto-saved as `sele`
- [x] **`obj` keyword** — `obj myprotein` selects all atoms if object name matches
- [x] **Inspect mode** — `i` toggles crosshair cursor, mouse click picks atoms, shows chain/resn/name/element/B/occ/xyz
- [x] **Command history** — `:` shows last 5 commands overlay, `↑`/`↓` cycle, 200 limit
- [x] **USalign integration** — `:align`, `:mmalign`, `:super` with per-side selection and `-ter 0`
- [ ] **Visual mode** — atom/residue selection extension with hjkl (deferred to Phase 4)

### Phase 4: Customization + Export — DONE

- [x] **ConfigParser** — TOML config loading from `~/.molterm/` via toml++ v3.4.0
- [x] **KeymapManager TOML** — fully customizable keybindings from `keymap.toml` with key notation (`<C-t>`, `<S-Tab>`, etc.)
- [x] **Color schemes** — user-defined color themes from `colors.toml`
- [x] **SessionExporter** — `.pml` script generation with `set_view`, repr, coloring
- [x] **`:fetch`** — download structures from RCSB PDB (`fetch 1abc`) and AlphaFold DB (`fetch afdb:P12345`)
- [x] **pLDDT** — AlphaFold confidence color scheme (very high/high/low/very low), `cp` keybinding, `:color plddt`
- [x] **Macro recording** — `q` + register (a-z) to record, `@` + register to play
- [x] **Tab completion** — context-aware for commands, filenames, object names, repr names, color names, settings
- [x] **`$` selection prefix** — `$sele`, `$ala` etc. (changed from `@`)

### Phase 4.5: PixelCanvas + Visual — DONE

- [x] **PixelCanvas** — RGB framebuffer with pluggable GraphicsEncoder (Sixel/Kitty/iTerm2)
- [x] **ProtocolPicker** — auto-detect terminal graphics protocol via env vars
- [x] **KittyEncoder** — zlib compression + chunked base64 + atomic image replacement
- [x] **ITermEncoder** — OSC 1337 inline image protocol with BMP encoding
- [x] **SixelEncoder** — 6×6×6 color cube quantization + RLE + transparent background
- [x] **Depth fog** — post-pass atmospheric perspective, configurable via `:set fog`
- [x] **Sphere shading** — Half-Lambert lighting on filled circles (Spacefill/BallStick)
- [x] **Line shading** — depth-based intensity on wireframe/backbone/cartoon
- [x] **Z-axis rotation** — `<`/`>` keys for roll
- [x] **Projection cache** — `prepareProjection()` per frame, `projectCached()` per vertex
- [x] **LOD** — skip atom dots for >10K atom wireframe
- [x] **Adaptive frame skip** — skip 1-3 frames when render > 100ms
- [x] **Frame diff** — skip identical frames via RGB memcmp
- [x] **Rainbow color scheme** — per-chain N→C blue→red gradient, `cr` keybinding
- [x] **Gzipped PDB/CIF** — transparent `.gz` support via gemmi
- [x] **`-v`/`--version`** — git tag or dev+hash

### Phase 5: Polish

- [ ] **Help system** — `:help` overlay with keybinding cheat sheet
- [ ] **Measurement tools** — distance, angle, dihedral display
- [ ] **Multi-state animation** — `[`/`]` state cycling, play/pause
- [ ] **Ribbon geometry** — Catmull-Rom spline cartoon with Frenet-Serret frames
- [ ] **Logging** — structured logging to `~/.molterm/molterm.log`

---

## Coding Conventions

- **C++17** strict — no exceptions
- `std::unique_ptr` / `std::shared_ptr` with clear ownership semantics
- `enum class` over raw enums
- `#pragma once` for header guards
- **Naming:** `PascalCase` for types, `camelCase` for methods/variables, `UPPER_SNAKE` for constants
- All ncurses calls through `Screen`/`Window` wrappers — never raw ncurses in business logic
- Separate concerns: parsing (io/), data model (core/), rendering (render/ + repr/), TUI (tui/), input (input/), commands (cmd/)
- Each class testable in isolation

---

## Testing Strategy

- **Unit tests** (Google Test): core data structures, selection parser, projection math, config parser
- **Integration tests**: load known mmCIF → verify atom count, bond count, residue names
- **Rendering tests**: snapshot-based — render to string buffer, compare against expected output
- **Fuzzing**: feed malformed CIF files to CifLoader
