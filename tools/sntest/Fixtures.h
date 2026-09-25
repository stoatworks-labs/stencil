#pragma once
/**
	What the checks feed the plugin. Every shape is defined in FRAME HEIGHTS
	(u across, 0 to aspect; v up, 0 to 1), so a fixture means the same thing
	at every raster, and is drawn two ways:

	  - on the LATTICE (`latticeImage`): each lattice cell takes the shape's
	    tone at its centre and every pixel of its k x k block is that tone, so
	    the plugin's block mean reproduces the harness's lattice mask exactly
	    and the checks can reason cell for cell;
	  - antialiased at the output raster (`smoothImage`), 4 x 4 supersampled,
	    for the moving fixtures, where a sub-pixel pan has to change the cut a
	    little at a time as footage does.

	Dark is paint (tone 0, a hole in the stencil), white is sheet (tone 1).
*/

#include "Harness.h"

#include <cmath>
#include <cstdint>

namespace fixtures
{

enum Which
{
	kLetters,          ///< O, A, B and 8: counters that float
	kNested,           ///< rings in rings, off-centre, islands four deep
	kIslandsInIslands, ///< squares in squares in squares
	kBlobs,            ///< thresholded value noise: dozens of islands of every shape
	kOffsetRing,       ///< an island well off the middle of a round hole: shortest is sideways
	kSquareRing,       ///< a square island in a square hole, the same gap on four sides
	kDiamond,          ///< a diamond island in a diamond hole: 45-degree gaps
	kEdge,             ///< the left half dark: one straight edge
	kRamp,             ///< tone rising left to right, 0 to 1
	kCount
};

inline const char* Name( int which )
{
	static const char* const names[ kCount ] = { "letters O A B 8", "nested rings", "islands in islands", "random blobs",
	                                             "offset ring", "square ring", "diamond ring", "edge", "ramp" };
	return names[ which ];
}

/// An integer hash of the harness's own (splitmix64's finaliser), NOT the
/// plugin's PCG.
inline double hash01( int64_t a, int64_t b, uint64_t salt )
{
	uint64_t z = static_cast< uint64_t >( a ) * 0x9E3779B97F4A7C15ull ^ ( static_cast< uint64_t >( b ) + 0x632BE59BD9B4E019ull ) * 0xBF58476D1CE4E5B9ull ^ salt;
	z          = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
	z          = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
	z ^= z >> 31;
	return static_cast< double >( z >> 11 ) / 9007199254740992.0;
}

inline double valueNoise( double u, double v, double cell, uint64_t salt )
{
	const double gx = u / cell, gy = v / cell;
	const int64_t ix = static_cast< int64_t >( std::floor( gx ) ), iy = static_cast< int64_t >( std::floor( gy ) );
	double fx = gx - std::floor( gx ), fy = gy - std::floor( gy );
	fx       = fx * fx * ( 3 - 2 * fx );
	fy       = fy * fy * ( 3 - 2 * fy );
	const double a = hash01( ix, iy, salt ), b = hash01( ix + 1, iy, salt );
	const double c = hash01( ix, iy + 1, salt ), d = hash01( ix + 1, iy + 1, salt );
	return ( a + ( b - a ) * fx ) + ( ( c + ( d - c ) * fx ) - ( a + ( b - a ) * fx ) ) * fy;
}

/// Distance from p to the segment ab.
inline double segment( double px, double py, double ax, double ay, double bx, double by )
{
	const double dx = bx - ax, dy = by - ay;
	const double t  = std::clamp( ( ( px - ax ) * dx + ( py - ay ) * dy ) / ( dx * dx + dy * dy ), 0.0, 1.0 );
	return std::hypot( px - ax - t * dx, py - ay - t * dy );
}

/// Is (u, v) dark (paint)? `aspect` is width / height; the shapes sit in
/// the middle of the frame. `pan` shifts the whole picture right.
inline bool Dark( int which, double u, double v, double aspect, double pan = 0.0 )
{
	u -= pan;
	const double mid = 0.5 * aspect;
	switch( which )
	{
	case kLetters:
	{
		const double s = 0.022;//half the stroke
		//O
		{
			const double d = std::hypot( ( u - ( mid - 0.62 ) ) / 0.8, v - 0.5 );
			if( std::fabs( d - 0.13 ) < s )
				return true;
		}
		//A
		{
			const double x = mid - 0.21, top = 0.66, foot = 0.34;
			if( segment( u, v, x - 0.12, foot, x, top ) < s || segment( u, v, x + 0.12, foot, x, top ) < s
			    || segment( u, v, x - 0.075, 0.45, x + 0.075, 0.45 ) < s )
				return true;
		}
		//B
		{
			const double x = mid + 0.14;
			if( segment( u, v, x - 0.07, 0.34, x - 0.07, 0.66 ) < s )
				return true;
			for( double cy : { 0.42, 0.58 } )
			{
				const double d = std::hypot( ( u - ( x - 0.02 ) ) / 1.25, v - cy );
				if( u > x - 0.07 && std::fabs( d - 0.06 ) < s )
					return true;
			}
			if( std::fabs( v - 0.66 ) < s && u > x - 0.07 && u < x - 0.02 )
				return true;
			if( std::fabs( v - 0.34 ) < s && u > x - 0.07 && u < x - 0.02 )
				return true;
			if( std::fabs( v - 0.50 ) < s && u > x - 0.07 && u < x - 0.02 )
				return true;
		}
		//8
		{
			const double x = mid + 0.52;
			for( double cy : { 0.425, 0.575 } )
			{
				const double d = std::hypot( u - x, v - cy );
				if( std::fabs( d - 0.075 ) < s )
					return true;
			}
		}
		return false;
	}
	case kNested:
	{
		//Dark rings round off-centre whites, four deep, so every island's
		//shortest bridge has one place to go.
		const double radii[][ 2 ] = { { 0.40, 0.44 }, { 0.30, 0.33 }, { 0.20, 0.235 }, { 0.10, 0.13 } };
		double cx = mid, cy = 0.5;
		for( int ring = 0; ring < 4; ++ring )
		{
			const double d = std::hypot( u - cx, v - cy );
			if( d > radii[ ring ][ 0 ] && d < radii[ ring ][ 1 ] )
				return true;
			cx += 0.018 * ( ring % 2 ? -1 : 1 );
			cy += 0.012;
		}
		//and a dark spot in the middle of the last white
		return std::hypot( u - cx, v - cy ) < 0.035;
	}
	case kIslandsInIslands:
	{
		//Square frames of dark, 5% wide, in white, in dark frames: three
		//deep, two side by side at the top level.
		for( double cx : { mid - 0.42, mid + 0.42 } )
		{
			const double dx = std::fabs( u - cx ), dy = std::fabs( v - 0.5 );
			const double r  = std::max( dx, dy * 1.0 );
			for( double edge : { 0.38, 0.25, 0.12 } )
				if( r < edge && r > edge - 0.05 )
					return true;
			if( r < 0.03 )
				return true;
		}
		return false;
	}
	case kBlobs:
	{
		const double n = 0.62 * valueNoise( u, v, 0.075, 7 ) + 0.38 * valueNoise( u, v, 0.03, 11 );
		return n < 0.47;
	}
	case kOffsetRing:
	{
		//A round hole of radius 0.3 with a disc of 0.18 well to its right:
		//the gap is 0.04 on the right and 0.2 on the left, 0.12 or more up.
		const double d = std::hypot( u - mid, v - 0.5 );
		const double e = std::hypot( u - ( mid + 0.08 ), v - 0.5 );
		return d < 0.3 && e > 0.18;
	}
	case kSquareRing:
	{
		const double r = std::max( std::fabs( u - mid ), std::fabs( v - 0.5 ) );
		return r < 0.3 && r > 0.18;
	}
	case kDiamond:
	{
		const double r = std::fabs( u - mid ) + std::fabs( v - 0.5 );
		return r < 0.4 && r > 0.22;
	}
	case kEdge:
		return u < mid;
	case kRamp:
		return false;
	default:
		return false;
	}
}

/// The lattice of a raster, as the plugin cuts it.
struct Lattice
{
	int k = 1, width = 0, height = 0;
	Lattice( int w, int h )
	{
		k      = Stencil::LatticeScale( h );
		width  = ( w + k - 1 ) / k;
		height = ( h + k - 1 ) / k;
	}
};

/// The fixture on the lattice: 1 sheet (white), 0 hole (dark), one value a
/// cell, rows bottom-up. Cell (i, j) takes the tone at its centre, in frame
/// heights of the OUTPUT raster.
inline std::vector< uint8_t > latticeMask( int which, int outW, int outH, double pan = 0.0 )
{
	const Lattice l( outW, outH );
	const double aspect = static_cast< double >( outW ) / outH;
	std::vector< uint8_t > mask( static_cast< size_t >( l.width ) * l.height );
	for( int j = 0; j < l.height; ++j )
		for( int i = 0; i < l.width; ++i )
		{
			const double u = ( i + 0.5 ) * l.k / outH, v = ( j + 0.5 ) * l.k / outH;
			mask[ static_cast< size_t >( j ) * l.width + i ] = Dark( which, u, v, aspect, pan ) ? 0 : 1;
		}
	return mask;
}

/// A lattice mask as the picture to feed: every pixel its cell's tone.
inline harness::Image expand( const std::vector< uint8_t >& mask, int outW, int outH )
{
	const Lattice l( outW, outH );
	harness::Image image( outW, outH, 1.0f );
	for( int y = 0; y < outH; ++y )
		for( int x = 0; x < outW; ++x )
			image.grey( x, y, mask[ static_cast< size_t >( y / l.k ) * l.width + x / l.k ] ? 1.0f : 0.0f );
	return image;
}

inline harness::Image latticeImage( int which, int outW, int outH, double pan = 0.0 )
{
	if( which == kRamp )
	{
		harness::Image image( outW, outH, 1.0f );
		for( int y = 0; y < outH; ++y )
			for( int x = 0; x < outW; ++x )
				image.grey( x, y, static_cast< float >( ( x + 0.5 ) / outW ) );
		return image;
	}
	return expand( latticeMask( which, outW, outH, pan ), outW, outH );
}

/// Antialiased at the output raster, 4 x 4 samples a pixel.
inline harness::Image smoothImage( int which, int outW, int outH, double pan = 0.0 )
{
	harness::Image image( outW, outH, 1.0f );
	const double aspect = static_cast< double >( outW ) / outH;
	for( int y = 0; y < outH; ++y )
		for( int x = 0; x < outW; ++x )
		{
			int dark = 0;
			for( int sy = 0; sy < 4; ++sy )
				for( int sx = 0; sx < 4; ++sx )
					dark += Dark( which, ( x + ( sx + 0.5 ) / 4.0 ) / outH, ( y + ( sy + 0.5 ) / 4.0 ) / outH, aspect, pan ) ? 1 : 0;
			image.grey( x, y, 1.0f - static_cast< float >( dark ) / 16.0f );
		}
	return image;
}

/// smoothImage for a picture panned by `pan` PIXELS, for a run of frames:
/// the pan's fractional part picks one of a few pictures drawn once (4 x 4
/// supersampled, `margin` pixels wider on the left), and its whole part
/// crops it. Exactly smoothImage's picture, a great deal faster.
struct PanCache
{
	int which = 0, width = 0, height = 0, margin = 0;
	std::vector< std::pair< double, harness::Image > > drawn;

	PanCache( int w, int h, int shape, int leftMargin ) : which( shape ), width( w ), height( h ), margin( leftMargin )
	{
	}

	harness::Image at( double panPixels )
	{
		const double whole = std::floor( panPixels + 1e-9 );
		const double frac  = panPixels - whole;
		const harness::Image* base = nullptr;
		for( const auto& d : drawn )
			if( std::fabs( d.first - frac ) < 1e-9 )
				base = &d.second;
		if( base == nullptr )
		{
			const int wide      = width + margin;
			const double aspect = static_cast< double >( width ) / height;
			harness::Image image( wide, height, 1.0f );
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < wide; ++x )
				{
					int dark = 0;
					for( int sy = 0; sy < 4; ++sy )
						for( int sx = 0; sx < 4; ++sx )
							dark += Dark( which, ( x - margin + ( sx + 0.5 ) / 4.0 - frac ) / height, ( y + ( sy + 0.5 ) / 4.0 ) / height,
							              aspect )
							            ? 1
							            : 0;
					image.grey( x, y, 1.0f - static_cast< float >( dark ) / 16.0f );
				}
			drawn.emplace_back( frac, std::move( image ) );
			base = &drawn.back().second;
		}
		harness::Image out( width, height, 1.0f );
		const int shift = static_cast< int >( whole );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width; ++x )
			{
				const int from = std::clamp( x - shift + margin, 0, width + margin - 1 );
				std::copy( base->at( from, y ), base->at( from, y ) + 4, out.at( x, y ) );
			}
		return out;
	}
};

} // namespace fixtures
