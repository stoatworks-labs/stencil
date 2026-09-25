# Attributions

Stencil is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### The jump flood, PassBuffer and the lattice — Stoatworks toolpath

<https://github.com/stoatworks-labs/toolpath>  
Licence: MIT  
Copyright: Stoatworks Labs

The flood's schedule (1+JFA, then a finishing run of halving steps), its squared-integer distances and total-order ties, the working lattice and the integer-format PassBuffer are toolpath's. Stencil's flood carries a different pair of seeds: the nearest sheet texel, and the nearest of another piece.

### Harness shape, sweep and the trap list — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

The offline harness's shape, tools/sweep.py, the negative-control pattern and the fleet's trap list are tinsel's.

### Stepped cues and the verify shape — Stoatworks gate

<https://github.com/stoatworks-labs/gate>  
Licence: MIT  
Copyright: Stoatworks Labs

The --pipe contract with SIGPIPE ignored and options, booleans and integers stepping between cues, and verify.sh's shape with its software-renderer pass follow gate's; the SNTEST_RENDERER=software switch is repousse's.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

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

- **Guodong Rong and Tiow-Seng Tan, "Jump Flooding in GPU with Applications to Voronoi Diagram and Distance Transform" (I3D 2006)** — jump flooding and its 1+JFA variant, implemented from the paper.
- **Pedro Felzenszwalb and Daniel Huttenlocher, "Distance Transforms of Sampled Functions" (Theory of Computing 8, 2012)** — the exact Euclidean distance transform the harness measures the bridges against.
- **Robert Tarjan, "Efficiency of a Good But Not Linear Set Union Algorithm" (JACM 22, 1975)** — union-find with path halving, which labels the sheet.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
