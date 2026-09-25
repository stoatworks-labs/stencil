#include "Spray.h"

#include <algorithm>
#include <cmath>

namespace stencil::spray
{
namespace
{
/// The antiderivative of sqrt( R^2 - x^2 ), for |x| <= R.
double chordIntegral( double radius, double x )
{
	x              = std::clamp( x, -radius, radius );
	const double s = std::sqrt( std::max( 0.0, radius * radius - x * x ) );
	return 0.5 * ( x * s + radius * radius * std::asin( x / radius ) );
}

/// Area of the disc inside { x <= a, y <= b }, in closed form: the integral
/// over x of the length of the chord at x below b, split where the chord's
/// end crosses b.
double quadrant( double radius, double a, double b )
{
	const double hi = std::min( a, radius );
	if( hi <= -radius )
		return 0.0;
	auto S = [ radius ]( double x ) { return chordIntegral( radius, x ); };
	//Integral of (2 s) over [lo, hi], of (b + s) over [lo, hi].
	auto full    = [ & ]( double lo, double up ) { return up > lo ? 2.0 * ( S( up ) - S( lo ) ) : 0.0; };
	auto partial = [ & ]( double lo, double up ) { return up > lo ? b * ( up - lo ) + ( S( up ) - S( lo ) ) : 0.0; };

	if( b >= radius )
		return full( -radius, hi );
	if( b <= -radius )
		return 0.0;
	const double c = std::sqrt( radius * radius - b * b );
	if( b >= 0.0 )
	{
		//|x| > c: the whole chord is under b. |x| <= c: from -s up to b.
		return full( -radius, std::min( hi, -c ) ) + partial( -c, std::min( hi, c ) ) + full( c, hi );
	}
	//b < 0: only where the chord reaches below b, |x| < c, from -s up to b.
	return partial( -c, std::min( hi, c ) );
}
} // namespace

double DiscRectArea( double radius, double x0, double x1, double y0, double y1 )
{
	if( radius <= 0.0 || x1 <= x0 || y1 <= y0 )
		return 0.0;
	return quadrant( radius, x1, y1 ) - quadrant( radius, x0, y1 ) - quadrant( radius, x1, y0 ) + quadrant( radius, x0, y0 );
}

std::vector< Tap > Footprint( double radius, bool gaussian )
{
	std::vector< Tap > taps;
	if( radius < 1e-6 )
	{
		taps.push_back( { 0.0f, 0.0f, 1.0f } );
		return taps;
	}
	//Every texel whose square the disc reaches: |offset| - 0.5 < R on each
	//axis. For the Gaussian (the negative control) the same window.
	const int reach = static_cast< int >( std::ceil( radius + 0.5 ) );
	const double sigma = 0.5 * radius;
	std::vector< double > weights;
	double total = 0.0;
	for( int j = -reach; j <= reach; ++j )
		for( int i = -reach; i <= reach; ++i )
		{
			double w = 0.0;
			if( gaussian )
				w = std::exp( -0.5 * ( i * i + j * j ) / ( sigma * sigma ) );
			else
				w = DiscRectArea( radius, i - 0.5, i + 0.5, j - 0.5, j + 0.5 );
			if( w <= 0.0 )
				continue;
			taps.push_back( { static_cast< float >( i ), static_cast< float >( j ), 0.0f } );
			weights.push_back( w );
			total += w;
		}
	//Divided by the sum rather than by pi R^2: they agree to the closed
	//form's rounding, and the sum is what makes the taps add to 1 exactly
	//where the whole footprint is in the hole.
	for( size_t t = 0; t < taps.size(); ++t )
		taps[ t ].weight = static_cast< float >( weights[ t ] / total );
	return taps;
}

} // namespace stencil::spray
