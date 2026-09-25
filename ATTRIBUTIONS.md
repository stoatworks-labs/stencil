# Attributions

Stencil is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

**Provisional hand copy (2026-09-25).** In a registered repo this file is generated —
the master lists live in the `stoatworks-backend` repo and are pushed out by
`scripts/sync-attributions.py`. Stencil is not registered yet, so this was written by
hand in that file's shape; register the project and re-run the sync before the first
release.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### The jump flood and PassBuffer — Stoatworks toolpath

<https://github.com/stoatworks-labs/toolpath>  
Licence: MIT  
Copyright: Stoatworks Labs

The flood's schedule (1+JFA, then a finishing run of halving steps from 1/128 of the longer side), its squared-integer distances and total-order ties, the working lattice and the integer-format PassBuffer are toolpath's. Stencil's flood carries a different pair of seeds: the nearest sheet texel, and the nearest of another piece.

### Harness shape, --pipe contract and verify — Stoatworks tinsel, rebate, gate and repousse

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

The offline harness's shape, the --pipe frame contract with SIGPIPE ignored and options stepping between cues, tools/verify.sh, sweep.py, the negative-control pattern, the software-renderer switch (repousse's) and the provisional About headers follow the fleet's siblings.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Stencil cutting and spray painting

Bridges (the strips a stencil cutter leaves so that the counter of an O does not fall out), multi-layer stencils sprayed light to dark, overspray under a lifted edge and drips are the street artist's and the sign-writer's craft. Implemented from that description; no artist's work or stencil software was used.

## Standards and published specifications

What the implementation is measured against.

- **Guodong Rong and Tiow-Seng Tan, "Jump Flooding in GPU with Applications to Voronoi Diagram and Distance Transform" (I3D 2006)** — Jump flooding and its 1+JFA variant, implemented from the paper.
- **Pedro Felzenszwalb and Daniel Huttenlocher, "Distance Transforms of Sampled Functions" (Theory of Computing 8, 2012)** — The exact Euclidean distance transform the harness measures the bridges against.
- **Robert Tarjan, "Efficiency of a Good But Not Linear Set Union Algorithm" (JACM 22, 1975)** — Union-find with path halving, which labels the sheet.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
