#pragma once

#include <vector>

/**
	The spray's footprint through a hole's edge, as a gather kernel.

	A nozzle at a distance from the wall sprays a cone. Through the edge of a
	hole, a point on the wall receives the part of the cone the hole lets
	through, so the paint density is the hole mask convolved with the cone's
	footprint: a uniform disc whose radius grows in proportion to the
	distance. Across a straight edge that is, exactly, the fraction of a disc
	on one side of a chord,

	    f(u) = ( R^2 acos( -u / R ) + u sqrt( R^2 - u^2 ) ) / ( pi R^2 )

	for u the distance into the hole, and its 25-75% width is 0.8079 R: the
	overspray's half width is proportional to the distance.

	The kernel is gathered on the stencil's lattice, where the mask is a union
	of whole texels, so each tap's weight is the EXACT area of the disc inside
	that texel's square (integrated in closed form below), and the sum over a
	half-plane of texels is exactly the chord fraction above -- not a sampled
	approximation of it. `tools/sntest --overspray` measures that out of the
	plugin's picture against f(u), which it computes on its own.
*/
namespace stencil::spray
{

struct Tap
{
	float dx, dy;///< texel offset
	float weight;///< share of the footprint, the taps sum to 1
};

/// The footprint of `radius` texels (0 is a single tap of weight 1).
std::vector< Tap > Footprint( double radius, bool gaussian = false );

/// Area of the disc of `radius` about the origin inside [x0, x1] x [y0, y1].
double DiscRectArea( double radius, double x0, double x1, double y0, double y1 );


} // namespace stencil::spray
