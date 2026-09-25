#pragma once

#include <string>

/**
	The passes.

	Everything but the composite runs on the stencil's LATTICE: an integer
	`LatticeScale` k of job pixels a texel (Stencil.h), ceil( raster / k )
	texels. The flood runs on the lattice plus a one-texel margin all round,
	the GRID of Bridge.h.

	1. **detect** -- lattice, RGBA16F. Each texel is the mean of its k x k
	   block of input pixels (texelFetch, so the host's filtering never
	   enters): r the TONE, gba the straight colour. The tone is the clip's luma laid
	   over white paper, a * luma + (1 - a), so a transparent clip is bare
	   wall and never a hole.
	2. **blur** -- lattice, RGBA16F, x then y, only when Smooth is up.
	   Read back: the CPU cuts each layer from the tone.
	3. **seed** -- grid, RGBA16UI, from the pieces' labels (R32UI, uploaded
	   by the CPU): every sheet texel its own nearest sheet, nothing yet of
	   another piece.
	4. **flood** -- grid, RGBA16UI, ping-ponged: 1+JFA and a finish, as
	   toolpath's. Each texel carries TWO seeds: its nearest sheet texel, and
	   its nearest sheet texel of a DIFFERENT piece. For a sheet texel the
	   second is the nearest texel of another piece, which is what a bridge
	   is. Read back.
	5. **spray** -- lattice, RGBA16F, one layer a channel: the hole mask
	   (the CPU's cut, uploaded as RGBA8: 0 sheet, 0.5 bridge, 1 hole)
	   gathered through the cone's footprint (Spray.h), exact square-disc
	   areas as weights.
	6. **creep** -- lattice, RGBA16F, x then y, only when Lift is up: a
	   Gaussian of the hole mask, the paint that gets under a lifted edge.
	7. **settle** -- lattice, RGBA16F: Pressure x (spray + lift x creep),
	   and the drips, which run down from wherever that is over saturation.
	8. **composite** -- to the host: the wall, each layer's paint over it in
	   order (lightest first, so dark lands over light), then Mix.

	Every shader is assembled at run time from these strings, so
	`sntest --dump-shaders DIR` writes out exactly what the plugin compiles,
	and that is what `tools/verify.sh` hands to glslc.
*/
namespace stencil::shaders
{

std::string Vertex();
std::string Detect();
std::string Blur();
std::string Seed();
std::string Flood();
std::string Spray();
std::string Settle();
std::string Composite();

/// Value the flood stores for "no seed of this kind yet".
constexpr unsigned kNoSeed = 65535u;

} // namespace stencil::shaders
