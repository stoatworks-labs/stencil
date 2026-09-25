#include "Palette.h"

#include "Controls.h"

#include <algorithm>

namespace stencil::palette
{
namespace
{
// Lightest first. Each palette's darkest can is the one a single-layer
// stencil is sprayed with.
const Colour kPalettes[ controls::kPaletteCount ][ kCans ] = {
	// Mono: three greys and a near-black.
	{ { 0.72f, 0.72f, 0.71f }, { 0.48f, 0.48f, 0.47f }, { 0.24f, 0.24f, 0.24f }, { 0.05f, 0.05f, 0.06f } },
	// Street: cream, signal red, charcoal, black.
	{ { 0.86f, 0.80f, 0.64f }, { 0.80f, 0.13f, 0.12f }, { 0.20f, 0.19f, 0.20f }, { 0.04f, 0.04f, 0.05f } },
	// Sepia: a brown ramp.
	{ { 0.78f, 0.64f, 0.45f }, { 0.56f, 0.40f, 0.25f }, { 0.33f, 0.21f, 0.12f }, { 0.12f, 0.07f, 0.04f } },
	// Pop: yellow, magenta, blue, black.
	{ { 0.98f, 0.82f, 0.10f }, { 0.90f, 0.20f, 0.52f }, { 0.13f, 0.30f, 0.78f }, { 0.04f, 0.04f, 0.06f } },
	// Cool: ice, teal, navy, ink.
	{ { 0.66f, 0.82f, 0.86f }, { 0.20f, 0.56f, 0.62f }, { 0.12f, 0.20f, 0.40f }, { 0.04f, 0.05f, 0.10f } },
};
} // namespace

const Colour* Cans( int palette )
{
	return kPalettes[ std::clamp( palette, 0, controls::kPaletteCount - 1 ) ];
}

Colour Ink( int palette, int layer, int layers )
{
	layers = std::clamp( layers, controls::kMinLayers, controls::kMaxLayers );
	layer  = std::clamp( layer, 1, layers );
	//The darkest `layers` cans, lightest first: layer 1 is can kCans - layers.
	return Cans( palette )[ kCans - layers + layer - 1 ];
}

} // namespace stencil::palette
