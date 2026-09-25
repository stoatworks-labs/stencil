# stencil

Spray paint through a cut stencil, bridges and all, as an FFGL **effect** for Resolume
Arena/Avenue. C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the cutter, the flood, the kept bridges, the spray's
footprint or any check's tolerance.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/sntest --out /tmp/f.png --size 1920x1080 --source letters`
  (sources: `letters nested islands blobs offset square diamond edge ramp bench`)
- Set anything by name: `--set "Layers=3" --set "Wall=1" --set "Distance=0.5"`
  (0..1 for sliders, the element index for options, the count for Layers)
- List parameters, kinds, defaults and ranges: `./build/sntest --list`
- The exact GLSL the plugin compiles: `./build/sntest --dump-shaders DIR`
- Footage through the real shaders — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH` and an optional `--script` of `frame Parameter Name value`
  cues: sliders ramp between cues, options, booleans and integers step (they hold the
  last cue at or before the frame); a cue naming no parameter is refused with exit 2, a
  partial frame at the end of stdin ends the stream cleanly, a failed render or a closed
  stdout (a reader that leaves) exits 1 — SIGPIPE is ignored, so never 141:
  `ffmpeg -i clip.mov -f rawvideo -pix_fmt rgba - | ./build/sntest --pipe --size 1280x720 | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -i - out.mov`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + glslc + reserved words +
  `--offline` + every GL check at 320x180 AND 1280x720 AND on the software renderer +
  `--pipe` + the sweep + a bench + the bundle; several minutes, most of it the software
  renderer)
- No island floats, labelled from the output: `./build/sntest --islands`
- Every bridge the shortest gap, against brute force: `./build/sntest --shortest`
- Bridges the stated width, 4-connected: `./build/sntest --width`
- The edge profile is the cone's chord fraction: `./build/sntest --overspray`
- n layers, n + 1 plateaus, dark over light: `./build/sntest --layers`
- Bridges translate with the picture: `./build/sntest --stability`; slow-pan churn:
  `./build/sntest --churn`
- Last frame's bridges survive a resize: `./build/sntest --resize`
- The checks can fail: `./build/sntest --negative`; one perturbation verbosely:
  `./build/sntest --perturb BITS --shortest` (bits in `Bridge.h` and `Stencil.h`)
- Everything with no GL (what CI can always run): `./build/sntest --offline`
- Apple's software renderer (what a GPU-less runner has): prefix `SNTEST_RENDERER=software`
- On a machine with no GL: add `--allow-no-gl` to the GL checks for a loud SKIP
- Every check takes `--size WxH`; CI runs them at 320x180
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/sntest --bench` (720p, 1080p); `--bench-4k` (all three) once, by hand
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Stencil.bundle`

## Notes
- **The cutter is on the CPU, the flood on the GPU.** Detect → smooth → read the tone
  back; per layer, `Bridge.cpp` thresholds, drops specks, labels (union-find), and asks
  the GPU for a two-seed jump flood per pass; the cut goes back up as RGBA8 (0 sheet,
  0.5 bridge, 1 hole, a layer a channel). A wrong bridge is a `Bridge.cpp` fix or a
  flood fix; a wrong halo is `Spray.cpp` or the settle shader.
- **The stencil is cut on a lattice** of k = ceil(H / 540) job pixels a texel: 180 rows
  at 320x180, 360 at 720p, 540 at 1080p and 4K. Every check reasons on it.
- **Lengths are frame heights**, converted in `Controls.cpp`.
- **The margin is sheet.** A piece is held when it is 4-connected to the ring of sheet
  round the frame; a bridge may end on it.
- **Sheet is 4-connected, hole 8-connected.** A band is never thinner than
  |n.x| + |n.y| cells, or a diagonal bridge would join at corners only.
- **Last frame's bridges are state.** They are kept in 0..1 of the lattice so a resize
  carries them; `--resize` checks it.
- **Nothing reads the host's clock.** `SetTimeSupported( false )`; the drips are static.
- **`Perturb` bits are test hooks**, always 0 in the plugin.
- **Parameter names must be unique and ≤ 16 characters** — `--names` checks.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can widen
  it, so every slider is 0..1 and `Controls.cpp` holds the units. `Layers` is an
  FF_TYPE_INTEGER, 1..4. Options are mapped by index.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `stencil_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `SN01`, display name `SW Stencil`.

## Browser demo
- `demo/` is stencil-demo.stoatworks-labs.com: the kit from `stoatworks-backend/resolume-demo`
  (vendored by its `sync.sh`; never edit `demo/vendor/`), the ten shader bodies spliced in
  by `python3 demo/tools/sync_shaders.py`, held to `Shaders.cpp` by
  `demo/tools/check_shaders.py` (verify.sh runs it). **A shader or constant change means
  re-running sync_shaders.py.**
- `demo/cutter.js` is a hand PORT of `Bridge.cpp`, `demo/controls.js` of Controls.cpp,
  Palette.cpp's Ink and Spray.cpp: a change to any of those needs the same change there.
- Deploy: `cf-run npx wrangler deploy` (a Worker **route** + a proxied AAAA 100:: DNS record,
  not a custom domain); `.github/workflows/deploy.yml` redeploys on a push to main.

## Not done yet
- **Never loaded into Resolume on macOS.** Everything numeric is measured offline on macOS,
  plus an `oxbow` load. On Windows it passes the fleet's Arena gate (Arena 7.27.1, llvmpipe):
  9/9, all 15 controls live (`plugin-bench/arena/expect/stencil.json`).
- No OpenFX port, no factory presets. The user guide is `docs/USER-GUIDE.md` (the site page
  and the PDF are built from it by the website's `build_guides.py stencil`).
- Released at v0.1.0 (github.com/stoatworks-labs/stencil); `StoatworksAbout.h` and
  `ATTRIBUTIONS.md` are generated by stoatworks-backend's sync scripts.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/stencil/stencil.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\stencil\logs\stencil.YYYY-MM-DD.log   (Windows)
