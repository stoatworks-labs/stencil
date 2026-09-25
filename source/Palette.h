#pragma once

/**
	The paints, and the plain wall.

	A palette is four cans, lightest first. A stencil of n layers takes the
	DARKEST n of them, so one layer is always the darkest can (the classic
	one-colour stencil is black), two layers are the two darkest, and so on:
	adding a layer adds a lighter can underneath and never changes what the
	darkest layer is sprayed with. Colours are straight (not premultiplied)
	linear-ish RGB in 0..1, chosen by eye on Resolume's demo clips.

	This is data, and the harness reads it from here to know which colour a
	plateau should be; it is not a formula typed twice.
*/
namespace stencil::palette
{

struct Colour
{
	float r, g, b;
};

constexpr int kCans = 4;

/// The four cans of a palette, lightest first.
const Colour* Cans( int palette );

/// Layer `layer` (1 = the lightest, sprayed first) of `layers`.
Colour Ink( int palette, int layer, int layers );

/// The Plain wall: a flat, slightly warm off-white.
constexpr Colour kPlainWall = { 0.93f, 0.92f, 0.89f };

} // namespace stencil::palette
