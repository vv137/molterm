# Command Reference

[← Back to README](../README.md) · See also: [Selections](SELECTIONS.md) · [Scripting](SCRIPTING.md)

### Commands

```vim
:help                           " Command index overlay (grouped by category)
:help <cmd>                     " Per-command overlay: usage, description, examples
:load <pattern>...              " Load mmCIF/PDB/gzipped file(s); supports shell globs and brace ranges:
                                "   :load *.pdb
                                "   :load model_{1..5}.cif
                                "   :load relaxed_*.pdb confident_*.cif
                                "   Idempotent on canonical source path: re-running
                                "   `:load same.cif` after an earlier load returns
                                "   "Loaded <name> (cached, same path)" instead of
                                "   creating a `_1`-suffixed duplicate. Re-runnable
                                "   batch-render scripts no longer stack overlapping
                                "   copies of the assembly. To force a refresh of an
                                "   already-loaded object, `:delete <name>` first.
:fetch <pdb_id>                 " Download from RCSB PDB (e.g. fetch 1abc)
:fetch afdb:<uniprot_id>        " Download from AlphaFold DB (e.g. fetch afdb:P12345)
:show <repr> [selection]         " Show repr (optionally for selection only); applies across scope (see Multi-object scope)
:hide [repr|all] [selection]    " Hide repr (optionally for selection only); applies across scope
:color <scheme>                 " element/cpk, chain, ss, bfactor, plddt, rainbow, restype, sasa, heteroatom, clear
:color <name> [selection]       " Per-atom color (red, blue, salmon, etc.) with optional selection
                                "   Multi-object: a bare selection covers every loaded object;
                                "   narrow with `obj <name>` or `/objname/...`. Append `!`
                                "   to flip scope for one call (e.g. `:color! red, chain A`).
:color "#RRGGBB" [selection]    " 24-bit hex literal — also `#RGB` short form and
:color "rgb(R,G,B)" [selection] "   `rgb(0..255, 0..255, 0..255)`. Pixel/screenshot output
                                "   honours the full 24-bit value; the 8-colour terminal
                                "   maps to the nearest named pair. Round-trips through
                                "   `:export *.pml` as PyMOL `0xRRGGBB` so figure scripts
                                "   keep the authored shade.
:select <expr>                  " Select atoms (see Selection Algebra below)
:select <name> = <expr>         " Named selection (e.g. :select s1 = $sele)
:select clear                   " Clear $sele and pk1-pk4 (also bound to gx)
:count <expr>                   " Count matching atoms
:cmp <expr-A> vs <expr-B>       " Compare two selections (Venn breakdown + verdict).
                                "   Prints |A| |B| |A∩B| |A\B| |B\A| and ends with
                                "   one verdict word — equal / A⊆B / B⊆A / disjoint /
                                "   overlap — which is greppable from scripts.
                                "   `vs` is the separator (= / , would be ambiguous).
                                "   Issue #53. Examples:
                                "     :cmp chain A vs chain B
                                "     :cmp $old_paratope vs $new_paratope
                                "     :cmp $paratope vs byres within 5 of $antigen
:center [selection]             " Center view
:zoom [selection]               " Center + zoom to fit
:orient [selection]             " Align principal axes + center + zoom
:orient view <vx> <vy> <vz>     " View along direction in PCA frame (also reruns PCA)
:turn x|y|z <deg>               " Rotate camera around screen axis (no PCA, cheap)
:align <obj> [sel] to <obj>     " TM-align via USalign
:mmalign <obj> [sel] to <obj>   " MM-align for complexes
:alignto <ref> [sel]            " Align every other object in tab onto <ref>
:alignto <sel> to <ref> [sel]   " Align current object onto <ref> (intra-object selection alignment allowed)
:loadalign <pattern> [sel] [tm|mm] " Glob/brace-load files; align all to the first
                                "   sel applies to both sides — e.g. confident domain only:
                                "   :loadalign model_?.cif chain A+B
:assembly [id|list]             " Generate biological assembly (default: 1)
:measure [s1 s2] [= "caption"]  " Distance (no args = pk1↔pk2 from last clicks).
                                "   Endpoints may also be two parenthesized
                                "   selections:
                                "   :measure (resi 2 and name SG) (resi 30 and
                                "   name SG) = "C28-C56 SS"  — the headless way
                                "   to pin an atom by selection (no picking).
                                "   When a selection matches >1 atom, reports the
                                "   closest approach (min heavy-atom distance)
                                "   between the two groups.
:angle [s1 s2 s3] [= "caption"] " Angle at s2 (no args = pk1-pk2-pk3).
:dihedral [s1 s2 s3 s4] [= "..."] " Dihedral (no args = pk1-pk4).
                                "   Args: serial number, pk1-pk4, or $selection.
                                "   Persisted as a dashed line + value label
                                "   (rendered into screenshots and exported as
                                "   PyMOL distance/angle/dihedral with `label`
                                "   carrying the optional caption).
:rmsd <selA> vs <selB>          " RMSD (+ N) of the optimal superposition of two
                                "   equal-count, in-correspondence selections —
                                "   WITHOUT moving anything (unlike :align/:super,
                                "   which report RMSD only as a side effect of
                                "   superposing). Current object.
:hbonds [selection]             " Auto-detect hydrogen bonds (N/O↔N/O ≤ 3.5 Å)
                                "   and draw them as dashed contact lines + labels
                                "   (rendered into screenshots, exported as PyMOL
                                "   distance objects). Whole structure by default.
:saltbridge [selection]         " Auto-detect salt bridges (Asp/Glu carboxylate ↔
                                "   Lys/Arg ≤ 4.0 Å) and draw them as dashed
                                "   contacts. Whole structure by default.
:bond [s1 s2 | (selA) (selB)]   " Draw a bond between two atoms (real topology
                                "   edge → renders as a stick wherever both
                                "   atoms are shown in wireframe/ballstick).
:unbond [s1 s2 | (selA)(selB)]  " Remove a bond between two atoms.
:disulfide [selection]          " Auto-detect cysteine SG–SG pairs (1.6–2.5 Å)
                                "   and draw them as bonds. Whole structure by
                                "   default; pass a selection to limit scope.
                                "   Fills in disulfides that prediction models
                                "   (no struct_conn) omit from connectivity.
:label <selection>              " Show residue labels on viewport (text from
                                "   :set label_format, default <resname><resseq>)
:label corner <pos> = "text"     " Free-position label pinned to a viewport corner.
                                "   <pos>: topleft|topright|bottomleft|bottomright
                                "   (or short tl/tr/bl/br). Inset by ~half the label
                                "   font height from the edge.
:label screen <fx> <fy> = "text" " Free label at normalised viewport coords
                                "   (0,0) = top-left, (1,1) = bottom-right.
:label world <x> <y> <z> = "text" " Free label at an explicit 3D position; tracks
                                "   the camera so the label rotates with the model.
                                "   Useful for annotating an active site location
                                "   without picking a specific atom.
                                "   All free-label forms honor :set label_color and
                                "   :set label_font_size; supported in pixel mode.

" ── Persistent solid arrows / axes (issue #38) ───────────────────
" Distinct from :measure (dashed + auto distance) — a solid arrow with
" a triangular head reads as "this is an axis vector", which is what
" you want for the principal axis of a domain, the V-V axis of a TCR,
" or any directional annotation. Endpoints persist as world coords;
" atom-anchored arrows resolve once and don't re-track if atoms move
" (re-issue after :align).
:arrow <s1> <s2> [= "text"]      " Arrow from atom serial s1 to s2.
:arrow $regA $regB [= "text"]    " Arrow between two vec3-typed registers
                                "   (set via :let — see #32, #35).
:axis $pcaReg [= "text"]         " Major axis (axis1) of a pca-result
                                "   register, centered on its centroid,
                                "   length = ±1σ (√eigval) along that axis.
                                "   Composes naturally with `:let G = pca(<sel>)`.
:set arrow_color <c>             " Same color spec as label_color. Default: yellow.
:set arrow_thickness|at <n>      " Shaft pixel thickness, 1..10 (default: 2).
:set arrow_head_size|ahs <n>     " Arrowhead length in pixels, 2..32 (default: 8).
                                "   All three knobs scale by :set overlay_scale.
:label <selection> = "<text>"   " Override the label text for matched atoms
                                "   (e.g. :label name CA and resi 1 = "P1")
:label clear                    " Remove all labels and overrides (atom + corner/screen/world)
:unlabel [<selection>|corner [<which>]|screen|world]
                                " Remove labels (issue #58):
                                "   no arg               every atom label AND every free label
                                "   <selection>          atom labels matching the selection only
                                "   corner               every corner-anchored free label
                                "   corner topleft|tl|topright|tr|bottomleft|bl|bottomright|br
                                "                         that one corner only
                                "   screen | world       every screen-anchored / world-anchored free label
:overlay                        " Toggle overlay visibility (labels, measurements, sele)
:overlay clear                  " Clear all measurements, atom labels, free labels, arrows
:preset                         " Apply smart defaults (cartoon protein, ballstick ligands)
:run [--strict] [--fresh] <script.mt>
                                " Execute a command script (# comments supported).
                                "   --strict  Abort on the first failing command (CI-style).
                                "   --fresh   Clear overlay annotations (labels,
                                "             :measure / :angle / :dihedral) before
                                "             running, so a batch-render driver
                                "             (`:run --fresh fig1.mt; :run --fresh fig2.mt`)
                                "             does not leak fig1's labels into fig2.
                                "             Without --fresh, overlays accumulate
                                "             across :run calls — useful for layered
                                "             setup scripts, intentional caption stacks.
                                " Failures (issue #80) are reported as
                                "   `path:line: \`cmd\`: reason` to stderr (headless mode)
                                "   or `~/.molterm/molterm.log` (TUI mode); the cmdline
                                "   summary cites the first failure's file:line so the
                                "   first jump-to is one click away.
:save                           " Save session (auto-saved on quit). Persists
                                "   loaded objects, per-tab camera state, and
                                "   typed registers (`:let`) so `:resume` recovers
                                "   the exact register values without
                                "   re-evaluating their original expressions —
                                "   important when the source structure has
                                "   changed since the autosave.
:export <file.pml>              " Export session as PyMOL script
:screenshot [file.png] [W H [DPI]] " Save PNG; W H force off-screen size, optional DPI stamps pHYs metadata
:interface [cutoff]             " Toggle inter-chain contact overlay (closest heavy atom, default: 4.5Å)
:interface legend               " Overlay with interaction-color legend + per-type stats (focus-aware scope)
:focus <selection>              " Click-to-focus: zoom + isolate sidechains
                                "   Whole interacting residues are auto-promoted into the neighborhood
:focus off                      " Exit focus session, restore camera + reprs
:dssp                           " Recompute DSSP secondary structure for current state (per-state cached)
:sasa                           " Compute solvent accessible surface area (PDB-REDO/dssp); reports total + per-chain Å² and mean relative accessibility
:set renderer <type>            " ascii, braille, block, pixel, sixel, kitty, iterm2
:set stereo off|walleye|crosseye " Side-by-side stereoscopic split. Walleye =
                                 "   parallel viewing (left image to left eye);
                                 "   crosseye = the user crosses their eyes.
:set stereo_angle <deg>         " Parallax angle (total, eyes ±half), default 6
:set fog <0-1>                  " Depth fog strength (default: 0.35)
:set bg|background_color <m>    " transparent (default) | white | black |
                                "   "#RRGGBB" | "#RGB" | "rgb(R,G,B)"
                                "   Named modes (transparent/white/black) and
                                "   arbitrary opaque RGB triples are both accepted —
                                "   a slate-grey publication background is
                                "   `:set bg "#202020"` or `:set bg "rgb(32,32,32)"`.
                                "   Honored in --no-tui and stable across sizes;
                                "   transparent uses a touched-pixel mask, not a
                                "   color heuristic, so labels and outline-darkened
                                "   atoms stay opaque. Custom RGB is always opaque;
                                "   for a transparent custom color, use the
                                "   transparent mode and post-process the PNG.
:set v|verbose on|off           " Stream diagnostic lines to stderr after each
                                "   command (off by default):
                                "     [view]   :center/:zoom/:orient/:turn/:focus
                                "              -> camera center, zoom, pan, atom count
                                "     [sel]    :select <name> = <expr>  ->  N atoms /
                                "              M residues [across chains A,B,...]
                                "     [align]  per (mobile,target) pair attempted by
                                "              :align / :alignto, with TM1/TM2/RMSD/Aligned
                                "     [render] :screenshot — PNG dims, DPI, bg, outline,
                                "              fog, elapsed seconds, visible_atoms
                                "   `[warn] :screenshot — 0 visible atoms; PNG will be
                                "   empty` is also printed to stderr regardless of
                                "   verbose, since silent empty PNGs are the most
                                "   common headless-script footgun.
:set transp|transparency <0..1> [selection]
                                " Per-atom transparency (0 = opaque, 1 = invisible);
                                "   selection narrows the application; no selection =
                                "   whole object. Pixel-mode only; transparent pixels
                                "   are skipped by the outline pass so glassy helices
                                "   don't carry a black silhouette.
:set outline on|off             " Silhouette outlines
:set ot|outline_threshold <n>   " Outline depth sensitivity (default: 0.3)
:set od|outline_darken <n>      " Outline darkness (default: 0.15, 0=black)
:set cm|cartoon_mode default|pymol " Cartoon style preset: default (molterm ribbon, tuned by the cartoon_* sizes below) or pymol (fixed PyMOL proportions: oval helix, barbed-arrow strand, round loop)
:set ch|cartoon_helix <n>       " Cartoon helix half-width Å (default: 1.30)
:set csh|cartoon_sheet <n>      " Cartoon sheet half-width Å (default: 1.50)
:set cl|cartoon_loop <n>        " Cartoon loop radius Å (default: 0.20)
:set csd|cartoon_subdiv <n>     " Cartoon spline subdivisions (default: 14)
:set csa|cartoon_aspect <n>     " Cartoon helix W:H aspect ratio (default: 5.0)
:set chr|cartoon_helix_radial <n> " Cartoon helix elliptical cross-section vertices (4-64, default: 16)
:set cth|cartoon_tubular_helix on|off " Tubular helix mode (circular tube vs elliptical ribbon, default: off)
:set ctr|cartoon_tubular_radius <n> " Tubular helix tube radius Å (default: 0.7)
:set css|cartoon_sheet_smooth <n> " Flatten β-strands: sheet Cα smoothing passes (0-10, default: 2; 0=raw Cα)
:set cshh|cartoon_sheet_height <n> " Cartoon sheet slab half-height Å (0.01-2.0, default: 0.20)
:set cstn|cartoon_tension <n>   " Loop/sheet spline tension (0.0-1.0, default: 0.5)
:set chtn|cartoon_helix_tension <n> " Helix spline tension (0.0-1.0, default: 0.9)
:set csf|cartoon_sheet_flat <n> " Strand flatten via C→O hint blend (0.0-1.0, default: 0.65)
:set caw|cartoon_arrow_width <n> " Sheet arrowhead tip width scale (1.0-4.0, default: ~1.47)
:set cfs|cartoon_frame_smooth <n> " Ribbon frame (twist) smoothing passes (0-10, default: 1)
:set cws|cartoon_width_smooth <n> " Cross-section width/height smoothing passes (0-10, default: 2)
:set cnw|cartoon_nucleic_width <n> " Nucleic ribbon half-width Å (0.05-2.0, default: 0.60)
:set cnh|cartoon_nucleic_height <n> " Nucleic ribbon half-height Å (0.05-2.0, default: 0.30)
:set bs_units vdw|cell          " BallStick sizing: vdw (Å×factor) or cell (legacy sub-pixel)
:set bsf|bs_factor <n>          " BallStick sizeFactor × vdW when bs_units=vdw (default: 0.15)
:set sfs|spacefill_scale <n>    " Spacefill ×vdW (default: 1.0, full vdW = CPK)
:set surface_mode|surf_mode <m>        " Surface type: ses (default, solvent-excluded) | sas | vdw | gaussian
:set surface_probe|surf_probe <n>      " SES/SAS probe sphere radius Å (0.0-3.0, default: 1.4 ≈ water)
:set surface_resolution|surf_res <n>   " Surface grid spacing Å (0.2-3.0, default: 0.7; smaller = finer/slower)
:set surface_scale|surf_scale <n>      " Surface atom radius ×vdW (0.2-3.0, default: 1.0)
:set surface_smoothness|surf_smooth <n> " Gaussian-mode kernel sharpness k (0.5-8.0, default: 2.0)
:set surface_iso|surf_iso <n>          " Gaussian-mode iso-level for mesh extraction (0.05-5.0, default: 1.0)
:set scope all|current           " Multi-object dispatch (default: all). With `all`, per-object
                                "   commands (color/show/hide, hotkey repr toggles, zoom/center)
                                "   apply to every object in the tab. Narrow with `obj <name>`
                                "   or `/objname/...` in the selection. `:cmd!` flips scope once.
:set panel on|off                " Object panel visibility
:set auto_center on|off          " Auto-center camera on load
:set seqbar on|off               " Sequence bar visibility
:set seqwrap on|off              " Sequence bar wrap mode
:set ic|interface_color <name>   " Interface overlay color (default: yellow)
:set it|interface_thickness <n>  " Interface dashed-line thickness, pixel mode (1-6, default: 4)
:set is|interface_show <spec>    " Which interaction types draw dashes:
                                "   all | specific | none | <list of hbond,salt,hydrophobic,other>
                                "   Lists may use ',' or '+' as separator: :set is hbond,salt
                                "   Examples: :set is hbond,salt          :set is hbond+salt
                                "             :set is hbond,salt,hydrophobic
                                "   Default 'specific' = hbond + salt only.
                                "   The legend stats remain complete regardless.
:set bt <n>                     " Backbone trace thickness, cells (default: 0.5)
:set wt|wireframe_thickness <n> " Wireframe line radius Å, scales with zoom (0.01-1.0, default: 0.10)
:set br <int>                   " BallStick legacy sub-pixel radius (default: 1, only when bs_units=cell)
:set ff|focus_fill <0.05-1.0>    " Focus fill fraction — fraction of screen the subject occupies (default: 0.6)
:set fe|focus_extra <Å>          " Focus extra radius padding around the subject (default: 4.0)
:set fmr|focus_min_radius <Å>    " Focus minimum radius clamp (default: 2.0)
:set fr|focus_radius <Å>         " Focus neighborhood cutoff (default: 5.0)
:set fd|focus_dim <0-1>          " Focus dim strength for non-subject atoms (default: 0.55)
:set fg|focus_granularity <g>    " residue|chain|sidechain — what gf+click expands to (default: residue)
:set lf|label_format <fmt>       " Default :label text template (empty = <resname><resseq>)
                                "   Tokens: {resname} {resseq}/{seqid} {chain} {name} {element} {restype}
                                "   Example: :set lf "P{resseq}"   then  :label name CA and resi 1-10
                                "   gives P1..P10. Per-atom :label sel = "text" overrides the template.
:set lfs|label_font_size <px>   " :label font size in pixels (default: 14, range: 8..72).
                                "   Independent of cell size — keeps labels legible on
                                "   2400x1800 / 4800-DPI screenshots where the cell-derived
                                "   default would shrink them to ~6 pt.
:set anf|annotation_font_size <px>
                                " :measure / :angle / :dihedral caption font size in
                                "   pixels (default: 14, range: 8..72).
:set anlw|annotation_linewidth <px>
                                " :measure dashed-line thickness (default: 2 sub-pixels,
                                "   range: 1..8).
:set scale|overlay_scale <x>     " Global multiplier on label_font_size, annotation_font_size,
                                "   annotation_linewidth, and the $sele/pk yellow rings
                                "   (default: 1.0, range: 0.5..4.0). Quick toggle between
                                "   rough (1.0) and hi-DPI (2.0) renders.
:set sm|size_mode <mode>         " How label / annotation / arrow sizes scale across
                                "   different render resolutions:
                                "     relative  — (default) interpret sizes as "pixels at
                                "                 reference_canvas_height tall canvas".
                                "                 :screenshot W H rescales by
                                "                 canvasH/refH so labels stay the same
                                "                 fraction of the figure across resolutions
                                "                 — a 1200x900 rough render and a 2400x1800
                                "                 final render show the same figure.
                                "     pixels    — raw screen pixels. lfs 22 is always
                                "                 22 px, so a hi-DPI render shows labels
                                "                 tiny relative to the canvas. Pre-0.45
                                "                 behavior; set this for byte-stable
                                "                 small-canvas output.
                                "     physical  — interpret sizes as point sizes (1 pt =
                                "                 live_dpi/72 px). :screenshot W H DPI
                                "                 then auto-rescales by DPI/live_dpi so
                                "                 lfs 22 prints at ~22 pt regardless of
                                "                 the screenshot's DPI metadata. Closer to
                                "                 how scientific journals expect figure
                                "                 sizing to work.
                                "   With the default `relative`, keep the same lfs / anf
                                "   for both rough and final :screenshot — they round-trip.
:set reference_canvas_height <h> " Canvas height (px) at which `lfs N`/`anf N` mean N
                                "   pixels under size_mode = relative. Default: 1080
                                "   (range: 240..8192). Larger value = labels shrink
                                "   relative to the figure at the same lfs.
:set live_dpi <d>                " DPI assumed for the live render under
                                "   size_mode = physical. Default: 96 (range: 36..600).
                                "   Affects only the auto-rescale ratio; live label
                                "   pixel size is still lfs * overlay_scale.
:set label_color <c>            " Color for :label text. Accepts named (red, white,
                                "   black, salmon, slate, …), hex (#RRGGBB / #RGB),
                                "   or rgb(R,G,B). Default: white. Set to `default`
                                "   (or `clear` / `auto` / `off`) to revert.
                                "   Pixel mode only — ncurses fallback uses palette.
:set annotation_color <c>       " Color for :measure / :angle / :dihedral captions.
                                "   Same color spec as label_color. Default: yellow.
:set measurement_line_color <c>  " Color for :measure / :angle / :dihedral dashed lines.
                                "   Same color spec. Default: yellow. Independent of
                                "   the caption color so a dim line + bright caption
                                "   pairing (or vice versa) is one knob away.
:set outline_color <c>          " Color used by silhouette / both outline modes.
                                "   Same color spec. Default: black (which is what
                                "   `darken` happens to converge to). Honored by
                                "   silhouette + both modes; ignored by edge mode.
:set label_outline on|off        " Halo (text outline) for :label glyphs (issue #49,
                                "   default ON). When on, drawTextOutlinedRGB
                                "   paints a contrasting rim around each glyph
                                "   before the body color, so labels stay legible
                                "   against any local pixel — coloured atoms,
                                "   ribbon interior, dark/light bg. Pixel mode
                                "   only — ncurses fallback ignores it. Turn it
                                "   off with `:set label_outline off`.
:set label_outline_color <c>    " Halo color. Same spec as label_color (named,
                                "   #RRGGBB, rgb(R,G,B), or `default` to clear).
                                "   When unset (default), molterm picks
                                "   white-on-dark / black-on-light against the
                                "   body color so the toggle alone is usually
                                "   enough.
:set label_outline_thickness <px>
                                " Halo radius in pixels (default: 2, range: 1..6).
                                "   Computed as a Chebyshev (square) dilation of
                                "   the glyph alpha mask, so thickness 1 reads
                                "   as a one-pixel ring even on small text.
:set annotation_outline on|off   " Halo for :measure / :angle / :dihedral
                                "   captions and arrow captions (default off).
                                "   Same auto-color logic as label_outline.
:set annotation_outline_color <c>
                                " Halo color for annotation glyphs. Same spec.
:set annotation_outline_thickness <px>
                                " Halo radius for annotation glyphs (default: 2,
                                "   range: 1..6).
:set outline_mode edge|silhouette|both
                                " Outline post-pass behavior:
                                "     edge        — legacy behavior; darken edge pixels
                                "                   by `outline_darken`. Works on light
                                "                   bg; invisible on dark bg (darken-
                                "                   of-black is still black).
                                "     silhouette  — paint silhouette pixels a fixed
                                "                   color (outline_color). Closes the
                                "                   dark-bg gap — pair with a light
                                "                   outline_color for a Mariuzza-style
                                "                   light rim on a dark hero figure.
                                "     both        — silhouette paint + edge darken;
                                "                   colored rim with subtle interior
                                "                   depth-edge darkening on top.
:get <option>                    " Query current value of any :set option (for scripting)
:let <name> = <expr>             " Bind a typed register (scalar / vec3 / pca-result)
                                "   for reuse in later commands. Closes #32, #33, #35.
                                "   Expression supports:
                                "     - Scalars: 1.5, -3
                                "     - Vec3 literals: [1, 0, 0]
                                "     - Atom positions: pos(A:1:CA)         (chain:resi:atom)
                                "                      pos(1ubq/A:1:CA)    (obj qualifier, issue #66)
                                "     - Group centers: centroid(<sel>)      -> geometric mean
                                "                      com(<sel>)           -> mass-weighted
                                "     - Register refs: $G.axis1, $v.length
                                "                      (bare names also work: G.axis1)
                                "     - Vector algebra: + - * /, dot(), cross(),
                                "                      length(), distance()/dist(),
                                "                      normalize(), midpoint(),
                                "                      angle(), dihedral()   (degrees)
                                "     - Scalar math:   abs, sqrt, exp, log, log10, log2,
                                "                      sin, cos, tan, asin, acos, atan,
                                "                      floor, ceil, round
                                "                      (2-arg) min, max, pow, atan2
                                "     - PCA primitive: pca(<selection>)     -> { axis1,
                                "                      axis2, axis3, eigvals, center }
                                "     - Axis fits:     helix_axis(<sel>)     -> helical axis
                                "                      superpose_axis(A vs B) -> screw axis,
                                "                      angle = rotation (degrees),
                                "                      rmsd = post-fit residual (Å)
                                "     - RMSD query:    rmsd(A vs B)          -> Å, no superpose
                                "   Type rules: vec±vec, scalar±scalar, scalar*vec,
                                "   vec/scalar, dot/cross/length/angle on vec3.
                                "   Examples:
                                "     :let v_axis = pos(A:43:CA) - pos(B:23:CA)
                                "     :let drift  = length(pos(model/A:50:CA) -
                                "                          pos(ref/A:50:CA))
                                "     :let G      = pca(chain A and helix)
                                "     :let theta  = abs(angle(v_axis, G.axis1))
:unlet <name> | :unlet *         " Drop one named register, or all of them.
:registers                       " List every register and its current value.
:expose <name> [<name>...]       " Mark registers for export from a scope=local
                                "   script frame to the caller (issue #67). Names
                                "   starting with `_` are auto-private. Outside
                                "   a scope=local frame, no-op.
:echo <text>                     " Print to stdout after ${var} / ${reg:fmt} expansion.
                                "   LLM-agent-friendly: machine-readable output without
                                "   relying on the status bar. Useful for scripted
                                "   analysis pipelines:
                                "     :let crossing = angle($v_proj, $p_proj)
                                "     :echo crossing_deg=${crossing:.2f}

" ── Script scope & call args (issue #67) ──────────────────────────────
" By default (back-compat), a `:run` script reads/writes the same
" register and env namespace as the caller — convenient for in-place
" recipes, but it means a library script's temporaries can leak into
" the caller. The first line of a script can opt into a local frame:
"
"   #!molterm scope=local export=crossing,incident
"   let _scratch = 99      # `_`-prefix never escapes the script
"   let crossing = 28.0    # only listed names flow back to caller
"   let incident = 12.5
"
" The shebang grammar is `#!molterm key=value ...`. Recognised keys:
"   scope=local|inherit (default: inherit)
"   export=name1,name2,...
" In-script `:expose` adds names dynamically; `_`-prefixed names are
" silently dropped to enforce privacy.
"
" Call-site arguments (KEY=VALUE) implicitly trigger scope=local:
"
"   :run @lib/tcr_angles TCR_A=D TCR_B=E MHC=A PEP=C MHC_HELIX1=50-85
"
" The KEY=VALUE pairs land in the script's env (visible as `${TCR_A}`
" etc.) and are popped when the script exits. The caller's env is
" untouched.

" ── Control flow (issue #68) ──────────────────────────────────────────
" Scripts support :if / :elseif / :else / :endif and numeric :foreach.
" Conditions go through the :let expression evaluator on both sides
" of a comparison (==, !=, <, >, <=, >=).
"
"   :let crossing = angle($v_proj, $p_proj)
"   :if $crossing > 60
"   :  label corner topleft = "warning: atypical crossing"
"   :elseif $crossing > 30
"   :  label corner topleft = "canonical crossing"
"   :else
"   :  label corner topleft = "shallow crossing (8YIV-like)"
"   :endif
"
" Numeric range iteration:
"
"   :foreach i in 1..5
"   :  run @lib/render_one i=${i}
"   :end
"
" Nested :if and :foreach work; loop variable is set as a scalar
" register on each iteration. Iteration over selections / lists is
" not yet supported — use a :let-driven calculation if you need
" per-residue logic for now.

" ── ${name.field[:fmt]} interpolation ───────────────────────────────
" Inside any string-typed argument (`:setenv`, `:label`, `:measure ...= caption`,
" etc.), references to registers + a printf-style format spec are expanded
" before the command sees them. Lookup order: registers, scriptEnv (set by
" :setenv), process getenv. Examples:
"     ${theta:.2f}            -> 12.34
"     ${v.length:.1f}         -> 8.7
"     ${V.x:.4f}              -> 1.0000
"     ${G.center}             -> (1.230, 4.560, 7.890)   (vec3 default fmt)
"     ${G.center:.2f}         -> (1.23, 4.56, 7.89)      (per-component fmt)
:set / :set all                  " (no value) Print every queryable option's
                                "   current value, one per line — Vim parity for
                                "   `:set all`. The list is the canonical option
                                "   table, so a freshly-added knob shows up here
                                "   automatically. Use this to discover option
                                "   names before scripting `:get <opt>`.
:camera                          " Print the current camera state (rotation 3x3,
                                "   center XYZ, zoom, pan XY) as a key=value blob
                                "   suitable for pasting into a script.
:camera save <file>              " Write the camera state to <file>. The file
                                "   is forward-compatible: a `# molterm camera v1`
                                "   header version-tags it, and unknown keys are
                                "   silently ignored on load.
:camera load <file>              " Restore camera state from a file written by
                                "   `:camera save`. Makes figure scripts
                                "   bit-reproducible — without this, every render
                                "   starts from a freshly-PCA'd pose and tiny
                                "   structural changes silently shift the camera.
:camera reset                    " Reset to the default identity-rotation /
                                "   zoom-1.0 / pan-0,0 pose.
:info                           " Show atom/bond count
:q                              " Quit
```

