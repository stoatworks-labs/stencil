# AGENTS.md — Stencil

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you
tell anybody this works.

---

## What the plugin is

Spray paint through a cut stencil, as an FFGL 2.1 effect (`SN01`, shown as `SW Stencil`)
for Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake, universal macOS `.bundle` and a
Windows `.dll`. MIT, intended home `github.com/stoatworks-labs/stencil`. **Local only at
v0.1.0** — no remote, no tag, not registered anywhere, never loaded into Resolume.

Built 2026-09-25 in one session (tranche five; Allan picked the idea) from
`specs/SPEC-stencil.md` with `BRIEF.md` and `BRIEF-ADDENDUM.md`: toolpath for the jump
flood, its schedule and the integer-format `PassBuffer`; tinsel, gate and toolpath for the
harness, `--pipe`, verify, the sweep and the negative controls; repousse for the software
renderer switch; graticule for the integer parameter and the provisional About headers.

---

## The one idea

**A stencil cannot hold a floating piece.** Cut an O out of a sheet and its middle falls
out. So a cutter leaves bridges — thin strips of sheet tying every island to the rest —
and then the paint is sprayed through the holes from a nozzle at a distance, and creeps
under the edges.

| the mechanism | what falls out |
| --- | --- |
| each layer's sheet labelled into pieces; a piece is held when it reaches the margin | **islands** — the counters of letters, light inside shadow |
| every island tied by the shortest strip from it to any other piece | **the gap in the O**, where its ring is thinnest; split letters |
| an island tied to another island still floats, so again | **islands inside islands** all end up hung off the frame |
| one threshold per layer, sprayed lightest first | **n + 1 tones**, dark over light, each layer with its own bridges |
| the hole convolved with the nozzle's disc of footprint | **the halo**, its width in proportion to Distance |
| paint under a lifted sheet, a bridge lifting most | **ghostly bridges** when Lift is up |
| paint over saturation runs down | **drips** from lower edges |

### The pipeline

1. **Detect** (GPU, lattice): each texel the mean of its k × k block of input pixels;
   the tone is the clip's luma laid over white paper, `a·luma + (1 − a)` (Invert flips
   the luma, not the paper), so a transparent pixel is bare wall. Optional Gaussian
   (`Smooth`). The tone is read back.
2. **Cut** (CPU, `Bridge.cpp`), per layer: threshold at `Threshold × (n − k + 1) / n`;
   the grid is the lattice with a one-cell margin of sheet all round; drop floating pieces
   smaller than `Min Island`; then passes of: label the sheet (runs + union-find),
   **flood** (GPU) for every cell's nearest sheet cell of another piece, choose each
   floating piece's bridge, cut the bands. Until nothing floats.
3. **Spray** (GPU, lattice): the holes (a layer a channel of an RGBA8 cut: 0 sheet,
   0.5 bridge, 1 hole) gathered through the footprint's taps, exact disc-in-square areas.
4. **Creep** (GPU, lattice): a Gaussian of the holes, when Lift is up.
5. **Settle** (GPU, lattice): Pressure × (spray + Lift × lifted × creep), lifted 1 on a
   bridge and 0.35 on the rest of the sheet; drips down hashed columns from where that is
   over 0.9.
6. **Composite** (GPU, output raster): the wall, then each layer's ink over it, lightest
   first, bilinear from the lattice; Mix.

### Why the shortest bridge to ANY piece, and what that does to "passes = depth"

The spec says bridge each island "to the border-connected sheet with the shortest
bridge… Repeat until no island floats. The number of passes needed is the depth of island
nesting." Those two sentences disagree. The shortest strip from an island often goes to
another island — two counters of a B side by side, a ring's inner island — and then the
pair still floats. Bridging only to held sheet would make one pass enough, but the
bridges long (the inner island of a target would cross every ring). So each island takes
the shortest bridge to any other piece, as the spec's `--shortest` asks, and the passes
are bounded differently: every floating piece either commits a bridge to another piece
or has been joined by one this pass, so no floating piece survives a pass alone and their
number at least halves — **at most ⌊log₂ F⌋ + 1 passes** for F islands. `--islands`
reports the nesting depth (the harness computes it) beside the passes: rings nested four
deep took 3 passes, squares in squares three deep took 1, 22 random blobs 1 deep took 1.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Controls.{h,cpp}` | What a 0..1 slider means, with inverses. Lengths in frame heights. |
| `source/Bridge.{h,cpp}` | The cutter, no GL: run labelling, the drop, the passes, choosing (shortest, kept, tie-break), the band. `Perturb` hooks. The flood is a callback. |
| `source/Spray.{h,cpp}` | The footprint's taps: exact areas of a disc inside each texel square, in closed form. |
| `source/Palette.{h,cpp}` | Five palettes of four cans, the plain wall. Data. |
| `source/Shaders.{h,cpp}` | The nine shaders: vertex, detect, blur, seed, flood, second, spray, settle, composite. |
| `source/PassBuffer.*` | toolpath's: any format including RGBA32UI and R32UI, no depth, `Upload()`. |
| `source/Stencil.{h,cpp}` | The plugin: parameters, the lattice, the GPU flood the cutter calls, the kept bridges, the passes, the test hooks. |
| `source/Diag.{h,cpp}` | A log file, for the shader that will not compile. |
| `tools/sntest/Harness.h` | PNG, CGL (with `SNTEST_RENDERER=software`), parameters by name, the Session. |
| `tools/sntest/Reference.h` | The harness's own geometry: flood-fill labelling, nesting depth, Felzenszwalb–Huttenlocher EDT, brute-force closest pairs. |
| `tools/sntest/Fixtures.h` | Letters, nested rings, squares in squares, blobs, rings, diamond, edge, ramp; on the lattice or antialiased; `PanCache`. |
| `tools/sntest/main.cpp` | The checks, `--bench`, `--pipe`, `--dump-shaders`. |
| `tools/sweep.py` | No control is silently dead. |
| `tools/verify.sh` | All of it, at two rasters and on the software renderer, plus the bundle. |

---

## Traps

Roughly in the order they will bite.

### ☠️ A half float is not rounded to nearest, and the first tolerance assumed it was

`--overspray` read the paint to within 4.88e-4 of the chord fraction against an allowance
of 2.46e-4 built on half an ulp of [0.5, 1) (2⁻¹²). The worst error was 4.88e-4, a hair
under a whole ulp (2⁻¹¹ = 4.883e-4) — what a truncating conversion into RGBA16F gives and
round to nearest cannot — and GL does not promise round to nearest for it. The tolerance is now one whole ulp, and says why; it was not widened to
the number printed. The same fact bit `--layers`, below.

### ☠️ A texel's neighbour is in the picture too

`--layers` judged a pixel "decidable" when its texel's tone was 2⁻¹⁰ clear of every cut.
One pixel at 720p, 3 layers, came out between two inks: its own texel was clear, but the
NEIGHBOUR the composite's bilinear filter reaches was 2.6e-4 from a cut, and the half
float put it on the other side. A pixel is decidable now only when its texel and both
neighbours are clear of every cut and in the same band.

### ☠️ Kept bridges slid, and then the other side got in first

The first slow-pan churn: 33% of bridges changed frame to frame on the letters with last
frame's bridges kept, 74% on the nested rings. Two causes. A kept bridge re-snapped to
the *shortest* cell within two texels of where it was, and on a curve every frame's
resampling moves that among near-ties; it now snaps to the cell *nearest where it was*
among those within the slack. And on the nested rings the kept bridge belonged to one
piece while the piece at its other end, choosing first because its own bridge sorted
shorter, bridged elsewhere and joined them — so the kept one was skipped as "already
joined". Both ends of an old bridge are searched now, and kept bridges are cut first.
6.8% and 5.1% after.

### ☠️ Keeping a bridge is not a translation

A kept bridge stays where it was **in the frame** while it is within the slack. So on a
picture panned by whole texels it lags and then steps, and the check that bridges
translate exactly with the picture failed 15 frames of 16 with keeping on. That check is
about the tie-break, so it runs with keeping off (`kPerturbNoHistory`); the lag is what
`--churn` measures. Tracking the content would need motion estimation; that is an open
question, not a fix.

### ☠️ A scissor on Apple's software renderer left rings floating for 32 passes

The later passes flood only a box round the floating pieces. Confined by `glScissor`, the
accelerated renderer was right and the software renderer produced nothing usable: the
nested rings kept two islands through all 32 passes. Confined by the viewport instead
(`gl_FragCoord` stays in the framebuffer's pixels either way) both are right. The cause
under the scissor was not found. Only the software-renderer pass in verify.sh saw it.

### ☠️ The first cutter took 33 ms a frame at 1080p

Measured with `sample` and the bench's stage times: the flood fetched a label for every
candidate (27 fetches a texel, 4.5 ms a flood), and a cell-by-cell union-find labelled
960 × 540 in 1.6 ms, several times a frame. Seeds are now RGBA32UI (position, piece) —
labels travel with them — and only the second's position is read back; the sheet is
labelled by runs (0.27 ms); later passes flood only a box (a quarter of the grid on the
bench scene). The flood's box is sound: the last flood already offers each floating piece
a bridge of some length L to a piece it is not part of, so its shortest is within L, and
a jump flood's relays between a cell and its seed lie inside the box the two span.

### ☠️ The spec's "passes = nesting depth" is false for shortest bridges

See "The one idea". Recorded as a bound instead, and reported.

### ☠️ Resolume's demo clips carry alpha, and dark-is-paint painted nothing

Four of the six clips looked at are mostly transparent. With the tone as luma over white,
a bright subject on transparency — the fire of Ethnik2, the rings of Trinity — painted
nothing at all. `Invert` (paint the light) was added, which the spec's control list does
not have; a transparent pixel is bare wall either way round. See the decisions.

### ☠️ At a small footprint the halo's width is not 0.808 R

The chord fraction's 25–75% width is 0.80794 R, but the picture holds the profile at
texel centres and a crossing read between them bends: at R = 1.8 texels it reads
0.8495 R. The width check compares against the chord fraction sampled the same way; that
the samples ARE the chord fraction is the pointwise check.

### ☠️ The worktree guard judges by the session's directory

This session started in `~/Projects/infrastructure/stoatworks-backend`, a shared checkout,
so the guard blocked `git add` in `~/dev/stencil` by a bare `cd`. `git -C
/Users/allansargeant/dev/stencil` is what the rules ask for anyway, and passes.

### ☠️ A negative control can loop to the pass cap

Without its 4-connectivity floor (`kPerturbThinBand`) a thin diagonal band joins only at
corners, so the island never connects and the cutter bridges it again every pass, 32
times. The cap (`maxPasses`) is what stops it; the check fails as it should.

### ☠️ The first mutation proved too little

Changing the flood's `l != firstLabel` to `==` failed 20 of 26 checks — everything, since
nothing got bridged, which shows the harness runs the flood but not much else. The
mutation recorded below is subtler and was caught only by the checks that read the
painted output.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and put
back before the composite); every `ffglex::Scoped*` clears to 0 on exit, so every
allocation happens before this frame binds anything (the taps and every lattice and grid
buffer are `Ensure`d at the top of `ProcessOpenGL`; nothing here uses a Scoped binding);
`FFGLFBO` cannot hold an integer texture, so `PassBuffer` is toolpath's; `SetParamInfo`
clamps a STANDARD default into 0..1 (Layers is an integer, which is exempt); the core is
an **OBJECT** library; `SetTextParameter` must return `FF_SUCCESS` for the About block;
`nm | grep -q` fails under pipefail when grep succeeds (verify.sh captures and matches);
an option's range reads back 0..1; `packed`, `far` and `near` are checked for, and there
is no `M_PI` (verify.sh greps for all four); randomness is integer hashing (PCG in the
shaders, splitmix in the harness); SIGPIPE is ignored in `--pipe`. **Resolume's clock
overflows a float** does not apply: nothing reads the clock (`SetTimeSupported( false )`).
**A resize must not clear the state across frames**: the only state is last frame's
bridges, kept in 0..1 of the lattice, and `--resize` checks it.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every check ran at 320×180 (lattice 180 rows, k = 1) and 1280×720
(360 rows, k = 2) in `verify.sh`, and at 320×180 on Apple's software renderer; the
geometry checks also at 1920×1080 (540 rows, k = 2) and 333×187 (odd, the last texel
overhanging) by hand. None was fitted to a number this Mac printed.

| check | what it measures | tolerance and where it comes from | raster dependence |
| --- | --- | --- | --- |
| `--islands` | pieces of sheet cut off from the frame's edge, by the harness's own flood fill of the OUTPUT thresholded half way between wall and ink | **exactly 0**; and the plugin's island count equals the harness's count of the input lattice, exactly; passes ≤ ⌊log₂ F⌋ + 1 (derived above) | the output is the lattice bilinearly sampled; thresholded at 0.5 it is the lattice itself at k = 1 and 2 (a texel's own weight at every pixel is ≥ 0.5625), and at larger k a 4-connected chain of sheet texels stays 4-connected and a corner-only contact stays corner-only (worked at k = 4: 0.47 and 0.53 either side). Fixtures are drawn on the lattice so the cut is exactly the harness's |
| `--shortest` | each bridge's length against the brute-force shortest gap from its island to any other sheet of the grid at the start of its pass (boundary cells against boundary cells; Felzenszwalb–Huttenlocher's exact EDT when that is over 6·10⁷ pairs; neither was needed) | **≥ 0 exactly** (a bridge is a real pair of cells) and **≤ 1 texel**, the spec's allowance. Not derived: a jump flood has no proven error bound (toolpath's trap). Measured 0.000 on every bridge at every raster | none: lengths are between cell centres of the lattice |
| `--width` square | cells of sheet across a vertical bridge, in the OUTPUT, on the lattice's own square ring | **exactly k·(2⌊W/2⌋ + 1)** pixels: the band's line runs through cell centres, so the cells within W/2 of it are 2⌊W/2⌋ + 1 | along a straight row through a band's interior a pixel's own texel always weighs over half, at any k |
| `--width` diagonal | cells of bridge across a row through the middle of a 45° bridge, on the cut grid, and 4-connectivity of the result | **exactly 2⌊W_eff/√2⌋ + 1**, W_eff = max(W, √2): `|j − Δx| ≤ W_eff/√2` along a row; 0 floating pieces | on the grid; 1e-9 inside every ⌊⌋ for float |
| `--overspray` | the paint across a straight edge, out of the output's red channel inverted through the wall and ink, against the chord fraction at texel centres bilinear to each pixel | **2⁻¹¹ + taps·2⁻²⁴ + 2⁻²⁰**: one conversion into RGBA16F, which GL lets truncate (a whole ulp of [0.5, 1)); a float sum of ≤ 425 weights under 1; the composite and its inversion. The width against the sampled chord's ± 2·tol/f′ at the quartile | the composite's bilinear weights are multiples of 1/8 at k = 1, 2, 4, exact in 8-bit sub-texel weights; at k = 3 (1081–1620 rows) they are thirds, which that argument does not cover — not run there |
| `--layers` | the colour of every decidable pixel on a ramp, the number and order of plateaus | **1e-6** on the colour (an ink over coverage 1 is written exactly); a pixel is judged only when its texel and both neighbours are > 2⁻¹⁰ from every cut (under an ulp of the tone's half float either way) and in one band; plateaus exactly n + 1, darkest first | the ramp is drawn per pixel; the tone is the block mean, computed by the harness the same way |
| `--stability` | every frame's bridges against the first frame's translated, a whole texel a frame | **exactly equal** | a total order on cells breaks every tie, and the flood's is a total order too; the fixture stays clear of the frame's edge, where a jump flood's clipping is not translation-equivariant |
| `--churn` | bridges from one frame to the next that are not the same bridge moved (midpoints and lengths within 1.5 texels once the pan is taken out) | kept < not kept, and not kept > 0 (the fixture has near-ties); every tenth frame each bridge ≤ the brute-force shortest + 1 texel fresh, + 2 kept (the keep slack is 1). 1.5 texels: a kept bridge re-snaps each end by at most a cell, plus 0.2 px of pan | measured, not asserted, on the churn itself |
| `--resize` | which side a tie is broken to: fresh, kept, and kept across a resize to a raster whose lattice is 1× or 2× the first | **exact** sides | the fixture is in base cells of 1/180 of the lattice's height, so the tie is exact on both lattices |
| `--exact` (no GL) | the reference EDT against brute force on 40 random masks | **exact** | none |
| `--cutter` (no GL) | `Bridge.cpp` with a brute-force flood on 30 random grids (noise, rings in rings) | **exact**: 0 floating, every bridge the shortest to 1e-9, passes within the bound, union-find the same partition as flood fill | none |
| `--band` (no GL) | 2000 random bands: 4-connected, both ends, vertical cross-sections | **exact** | none |
| `--footprint` (no GL) | taps inside a half-plane against the chord fraction (the harness's own formula), and the taps' sum | **625·2⁻²⁴** (float weights, at most 625 taps) | none |

Deliberately NOT relied on: exact cancellation; the order a driver visits fragments (the
flood's ties are a total order); a scissor (see the traps); round-to-nearest into a half
float.

What might still differ on another rasteriser: bilinear sub-texel weights coarser than
1/8 would move `--overspray` at k = 2 and 4 and the output's thresholded topology at odd
k; a jump flood's results if a driver did integer arithmetic differently (it may not);
the Gaussian taps of Smooth and Lift, which no check is tight on.

---

## Negative controls and the mutation

`sntest --negative` runs nine, and `--perturb BITS` runs any check verbosely against one.
Each perturbs the *plugin* — a `Perturb` bit the shipped plugin carries at zero — never
the harness's expectation. At 320×180:

| perturbation | bits | what fails |
| --- | --- | --- |
| no bridges | 1 | `--islands`: 6 floating on the letters, 4, 6, 22, and 62 in the three layers |
| bridge straight up from the island's top | 2 | `--shortest`: the offset ring's bridge 14 texels over the shortest, the blobs' worst 38 |
| a band W wide each side | 16 | `--width`: 3 and 7 cells across for 1 and 3 |
| no 4-connectivity floor | 32 | `--width`: the thin diagonal joins at corners only; 18 pieces float after 32 passes |
| a Gaussian footprint of the same quartile width | 512 | `--overspray`: 3.4e-2 from the chord fraction, allowed 4.9e-4 (its width passes, which is why the check is pointwise) |
| layers painted darkest first | 1024 | `--layers`: 189 of 316 decidable pixels right at 2 layers |
| ties broken toward the frame's centre | 4 | `--stability`: the square ring's bridge moves on 8 frames of 16 |
| last frame's bridges ignored | 8 | `--resize`: the tie goes left after the right was shorter |
| a resize forgets last frame's bridges | 256 | `--resize`: left after the resize |

And in `--offline`: a city-block reference distance fails `--exact`; a flood blind to the
pieces fails `--cutter`; no floor, and a band twice as wide, fail `--band`; a Gaussian
fails `--footprint`.

### The mutation

One character of the shipped GLSL, on a clean committed tree (f7724e0, and before that on
48f6ecc's with the same result): in the spray shader, `sum += tap.z * step( vec4( 0.75 ), … )` → `0.25`, so a
bridge (0.5 in the cut) is sprayed as if it were a hole. Caught by `--islands` (every
bridged island floats again in the OUTPUT: 6, 4, 6, 22 at 320×180; 6, 4, 6, 23 at 720p)
and `--width` (0 pixels of sheet across the square ring's bridge, want 1 and 3; 6 and 14
px at 720p) at both rasters: 6 of 29 checks at each. **Not** caught by `--shortest`, `--stability`, `--churn` or
`--resize`, which read the cut grid, not the picture, nor by `--overspray` and `--layers`,
whose fixtures have no bridges — correctly. It is the check that labels the painted
output that sees paint where a bridge is. Reverted with `git checkout
source/Shaders.cpp`; the tree was clean before and after. (A first, blunter mutation —
the flood's `l != firstLabel` to `==` — failed 20 of 26 checks at each raster: nothing was
bridged at all.)

---

## Decisions taken without asking

- **Invert, a control the spec does not list.** Four of six demo clips are mostly
  transparent and bright-on-transparent painted nothing; "paint the light" is one
  boolean. Default off: dark is paint, the stencil reproduces the picture's own polarity.
- **A transparent pixel is bare wall**, never a hole, with Invert on or off: there is no
  picture there to spray.
- **Output alpha.** Brick, Concrete and Plain walls cover the frame: opaque at Mix 1.
  Wall = Clip keeps the clip's alpha where no paint landed; paint is opaque where it did
  (premultiplied "over"). Mix blends RGBA with the clip.
- **The lattice: k = ceil(H / 540)**, so at most 540 rows: 180 at 320×180, 360 at 720p,
  540 at 1080p and 4K. The cutter's cost is in cells, so it is capped; toolpath's lesson
  (the verify rasters must run the shipping code) is kept in that k = 1 and k = 2 both run
  in verify.sh and the code is the same for every k. At 4K a texel is 4 px, so with no
  spray the edges step — the honest limit.
- **Everything in frame heights**; converted to lattice texels at the last moment.
- **The margin is sheet.** The sheet is bigger than the frame, so a piece touching the
  edge is held, and a bridge may end on the margin.
- **Sheet 4-connected, hole 8-connected**; a band never thinner than |nₓ| + |n_y|.
- **Shortest bridge to any other piece**, ties by (a.y, a.x, b.y, b.x) lowest first — a
  total order a translation preserves. One bridge per island per pass; islands whose
  partner already joined them this pass are skipped.
- **Kept bridges**: a bridge from last frame is kept — at the cell nearest where it was,
  within 2 texels of either end — while it is no more than 1 texel longer than the
  shortest. Kept ones are cut first. Stored in 0..1 of the lattice so a resize carries
  them. Measured cost: a kept bridge was at most 1.000 texel over the shortest.
- **Min Island drops** (fills with paint) a floating piece whose area is under the square
  of `Min Island`; they are never bridged. Default 1.2% of the frame height.
- **Layers are 1–4** (one a channel of the RGBA8 cut), an FF_TYPE_INTEGER. The cuts are
  `Threshold × (n − k + 1) / n`: the tones under Threshold divided evenly.
- **Palettes of four cans; n layers take the darkest n.** One layer is always the darkest
  can. From Clip: each layer's ink is the mean straight colour of the clip over its band
  of tone, on the CPU from the tone's readback (falls back to the palette if the band is
  empty).
- **The footprint is a uniform disc** (a cone's section at small angles), radius 0–2% of
  the frame height; weights exact disc-in-square areas.
- **Lift** is a Gaussian creep of σ = Lift × 0.8% under the sheet, weighted 1 on a
  bridge and 0.35 elsewhere, times Lift. A model chosen to look right, not measured.
- **Drips are static**: in hashed lattice columns (Drips × 45% of them), paint over 0.9
  runs down up to Drips × 12% of the frame height, scaled by how far over, thinning to
  half at the tip. Stepping them over frames would need state and a clock; not done.
- **The walls** are procedural in frame heights with integer-hash value noise: brick in
  stretcher bond, concrete with pits, a plain off-white.
- **No clock**: `SetTimeSupported( false )`.
- **Test hooks live in the shipped plugin** (the `Perturb` bits, `...ForTest`), inert.
- **`StoatworksAbout.h`, `StoatworksAboutLinks.h`, `StoatworksAboutParams.h` and
  `ATTRIBUTIONS.md` are provisional hand copies**, adapted from toolpath's, with
  `guide=""` (no guide exists, so no guide button): register the project and re-run the
  syncs before the first release.
- **The commit trailer names the model that did the work** (`Claude Opus 5.5`), as the
  session's instructions asked, over the brief's.
- **Defaults** (two layers, Mono on Concrete, Threshold 0.5, Smooth 0.3, bridges 0.5%,
  Min Island 1.2%, Distance 0.3%, Pressure 1.12, Lift 0.3, Drips 0.4), chosen by looking
  at Beat 001, Metalive 01, Trinity_09, BattleWeapon_Tank_09, Ethnik2_23 and
  IntoTheGlow_02 through `--pipe`. On dark opaque clips (Beat 001, IntoTheGlow) the
  defaults paint most of the frame black; that is the picture's polarity, and Invert or a
  lower Threshold is the answer.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4 (2026-09-25)

Every number is `tools/verify.sh` on this machine against a fresh universal Release
build, at 320×180 and 1280×720 and on the software renderer at 320×180, unless it says
otherwise.

- **Islands.** Letters 6 islands, nested rings 4 (4 deep), squares in squares 6 (3 deep),
  blobs 22 / 23, three layers of smoothed blobs 62 / 67 (71 at 1080p): 0 floating in the
  output; the plugin's count equal to the harness's; passes 1, 3, 1, 1 within ⌊log₂ F⌋ + 1
  (3, 3, 3, 5).
- **Shortest.** 39 bridges at 320×180, 40 at 720p and 1080p: every one exactly the
  brute-force shortest (all by brute force; none needed the EDT).
- **Width.** 1 and 3 cells (320×180), 3 and 7 (720p), 5 and 11 (1080p) across square
  bridges of W = 1.8/3.6, 3.6/7.2, 5.4/10.8 texels, exact in the output; 3, 5 / 3, 11 /
  3, 15 across 45° ones, exact; 0 floating.
- **Overspray.** Worst 4.88e-4 from the chord fraction at 320×180 and 720p (allowed 4.9e-4
  to 5.0e-4), 3.8e-4 at 1080p; width 1.5291 / 2.9128 texels for R 1.8 / 3.6 at 320×180
  (sampled chord 1.5283 / 2.9129), ratio 1.905 for 2; 1.983 at 720p, 1.993 at 1080p.
- **Layers.** 2 to 5 plateaus in order for 1 to 4 layers, every decidable pixel (312–318
  of 320; 1248–1276 of 1280) exactly its ink.
- **Stability.** 0 frames of 15 differ from the first translated, on the square ring (1
  bridge), letters (6), nested rings (4).
- **Churn** (0.2 px a frame, 60 frames): kept 6.78% / 5.08% / 2.77% of bridge-frames on
  letters / nested / blobs at 320×180 against 73.7% / 89.8% / 9.50% not kept; 7.06% /
  1.27% / 0.92% against 48.3% / 89.8% / 2.17% at 720p. A kept bridge at most 1.000 texel
  over the brute-force shortest; a fresh one 0.
- **Resize.** Fresh: left; kept: right; after 320×180 → 640×360 and 1280×720 → 640×360:
  right.
- **Offline.** 17 names within 16 characters and unique; the reference EDT equal to brute
  force on 41,795 cells; the cutter on 30 random grids (83 islands, nested up to 3 deep)
  exact; 2000 bands; the footprint within 3.0e-8.
- **Negative controls.** All nine fail their check; all five offline ones too.
- **Mutation.** Caught by `--islands` and `--width` at both rasters (above).
- **No dead controls**, all 14, with the four About entries skipped.
- **Every shader compiles** through `glslc`, all eight fragment and vertex texts, with no
  reserved word as an identifier.
- **`--pipe`** returns exactly two frames for two and a half, refuses an unknown cue (2),
  exits 1 on a failed render and on a closed stdout (`| head -c 1`, bash `PIPESTATUS`, not
  141); an option (Wall), a boolean (Invert) and an integer (Layers) step between cues, a
  slider (Mix) ramps.
- **The bundle** is universal (`x86_64 arm64`), exports `_plugMain`, carries
  `com.stoatworks.ffgl.stencil` and 0.1.0, ad-hoc signs, and `oxbow` reports
  `SW Stencil` / `SN01` / `effect` and renders 120 frames through `plugMain`.
- **Render cost**, `sntest --bench` (the defaults on blobs through letters, 2 layers, ~47
  islands, best of three runs of 30 frames, `glFinish` both sides), on a Mac busy with an
  ffmpeg encode on twelve cores, a VM and other harnesses on the same GPU, so the same
  bench moved by a factor of two between runs. Every run of the final build, best first:
  7.9–20 ms at 720p, 13–26 ms at 1080p, 22 ms at 4K (one run); of which the floods
  3.3–8.6 / 5.7–8.4 / 8.4 ms and the cutter 1.9–4.4 / 3.8–6.5 / 6.0 ms; one labelling
  0.13 ms at 640×360, 0.27 at 960×540; the later passes flood 0.24 of a grid. The first
  cutter was 33 ms at 1080p (the trap above).

### Assumed, or not done

- ☠️ **Never loaded into Resolume**, on either platform. Everything was compiled,
  rendered and measured offline against the real plugin class in a headless CGL context,
  plus an `oxbow` load.
- **Never built on Windows.** The CI workflow is written, not run.
- **Seen on six demo clips only**, one frame each and a few seconds of Metalive; never on
  camera footage, never over minutes.
- **The render cost is heavy**: at 1080p most of a 60 fps frame here, more with more
  layers or islands. Whether Resolume leaves that much is unmeasured.
- **The jump flood has no proven bound**; the spec's 1-texel allowance was never needed,
  but it is an allowance, not a derivation.
- **Lift, drips, the walls and the palettes** are chosen to look right, not measured
  against anything.
- **At k = 3** (1081–1620-row compositions) the output's bilinear weights are thirds, not
  covered by the `--overspray` or `--islands` arguments above, and never run.
- **No OpenFX port, no browser demo, no user guide, no factory presets.**
- **The provisional About headers and ATTRIBUTIONS** are hand copies (above).

---

## Open questions

- **Should kept bridges follow the content?** They hold still in the frame, so on a pan
  they lag and step (`--churn` measures it). Estimating each island's motion from its
  centroid frame to frame would let a kept bridge ride with it.
- **Two bridges for a big island?** A cutter bridges for strength as well as for
  connection; one bridge per pass leaves a large island hanging by a thread.
- **Bridge to held sheet when nearly as short?** It would cut the passes (and the flood
  cost) at the price of the spec's "shortest".
- **The lattice cap.** 540 rows keeps 4K at the cost of 1080p; 360 would roughly halve
  the cost and make 4K edges six pixels a step.
- **Drips that run over time**, which would need state and a clock.
- **Auto levels.** Dark opaque clips paint almost everything at the defaults; a
  threshold relative to the clip's own tones would flicker unless smoothed over frames.

---

## Siblings

- **toolpath** — the jump flood, its schedule, the integer `PassBuffer`, the lattice.
- **tinsel** — `sweep.py`, the trap list, the harness shape.
- **gate** — this tranche's `verify.sh` shape: the software-renderer pass, the `--pipe`
  steps for options and events.
- **repousse** — `*_RENDERER=software`.
- **graticule** — the integer parameter; the provisional About.
- **oxbow** — `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
