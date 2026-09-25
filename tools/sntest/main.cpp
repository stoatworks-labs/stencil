/**
	sntest -- render Stencil offline, and read the stencil back out of it.

	Whether any island of sheet is left floating, whether each bridge is the
	shortest gap there was, how wide the bridges are, what shape the
	overspray has, how many tones n layers give, whether the bridges hold
	still on a moving clip, and whether they survive a resize are facts with
	one right answer each. Every GL check renders a picture through the REAL
	plugin class in a headless GL context and measures it with the harness's
	own code (Reference.h: flood-fill labelling, an exact EDT, brute-force
	closest pairs) rather than eyeballing.

		sntest --out /tmp/frame.png     a picture (--source letters|nested|blobs|...)
		sntest --list                   every parameter, its kind, default and range
		sntest --islands                no island of sheet floats, labelled from the output
		sntest --shortest               each bridge is the shortest gap, against brute force
		sntest --width                  bridges have the stated width, and are 4-connected
		sntest --overspray              the edge profile is the cone's chord fraction
		sntest --layers                 n layers give n + 1 plateaus, dark over light
		sntest --stability              bridges translate with the picture, ties and all
		sntest --churn                  how often bridges change on a slow sub-pixel pan
		sntest --resize                 last frame's bridges survive a resize
		sntest --negative               every GL check above can FAIL
		sntest --offline                the checks that need no GL (what CI runs)
		sntest --bench                  the render cost, and the bridging's share
		sntest --dump-shaders DIR       the exact GLSL the plugin compiles
		sntest --pipe                   raw frames in, raw frames out

	Run each at two rasters at least -- the one you develop at and 320x180,
	which is what CI uses -- and on Apple's software renderer
	(SNTEST_RENDERER=software). AGENTS.md has one line per check on where
	each tolerance comes from.

	**Orientation.** The checks work in GL's orientation throughout: row 0 is
	the BOTTOM. Only `--out` and `--pipe`, which exchange pictures with the
	outside world, flip.

	`--pipe` takes the fleet's frame format:

		ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
		  | sntest --pipe --size 1920x1080 [--script cues.txt] \
		  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov
*/

#include "Fixtures.h"
#include "Harness.h"
#include "Reference.h"

#include "Bridge.h"
#include "Controls.h"
#include "Palette.h"
#include "Shaders.h"
#include "Spray.h"
#include "Stencil.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace harness;
using namespace stencil;

namespace
{
constexpr double kPi = 3.14159265358979323846;

//---------------------------------------------------------------------------
// The baseline every geometry check starts from: one layer cut at 0.5, no
// smoothing, bridges 1.5% of the frame high, nothing dropped; no spray blur,
// no lift, no drips, Pressure 1; the Mono cans on the Plain wall, so the
// output's red channel says paint or wall at every pixel.
//---------------------------------------------------------------------------
void baseline( Stencil& p )
{
	set( p, "Layers", 1.0f );
	set( p, "Threshold", 0.5f );
	set( p, "Smooth", 0.0f );
	set( p, "Bridge Width", controls::BridgeWidthParam( 0.015f ) );
	set( p, "Min Island", 0.0f );
	set( p, "Distance", 0.0f );
	set( p, "Pressure", 0.5f );
	set( p, "Lift", 0.0f );
	set( p, "Drips", 0.0f );
	set( p, "Palette", static_cast< float >( controls::kMono ) );
	set( p, "Layer Colours", static_cast< float >( controls::kFromPalette ) );
	set( p, "Wall", static_cast< float >( controls::kWallPlain ) );
	set( p, "Mix", 1.0f );
}

/// The output's sheet (1: no paint) from its red channel, against the wall
/// and the one ink.
std::vector< uint8_t > sheetOfOutput( const Image& out, const float* ink )
{
	const float cut = 0.5f * ( palette::kPlainWall.r + ink[ 0 ] );
	std::vector< uint8_t > sheet( static_cast< size_t >( out.width ) * out.height );
	for( int y = 0; y < out.height; ++y )
		for( int x = 0; x < out.width; ++x )
			sheet[ static_cast< size_t >( y ) * out.width + x ] = out.at( x, y )[ 0 ] > cut ? 1 : 0;
	return sheet;
}

/// The sheet of a bridge::Grid (margin included).
std::vector< uint8_t > sheetOfGrid( const bridge::Grid& g )
{
	std::vector< uint8_t > sheet( g.cells.size() );
	for( size_t i = 0; i < g.cells.size(); ++i )
		sheet[ i ] = bridge::IsSheet( g.cells[ i ] ) ? 1 : 0;
	return sheet;
}

std::vector< uint8_t > sheetOfCells( const std::vector< uint8_t >& cells )
{
	std::vector< uint8_t > sheet( cells.size() );
	for( size_t i = 0; i < cells.size(); ++i )
		sheet[ i ] = bridge::IsSheet( cells[ i ] ) ? 1 : 0;
	return sheet;
}

/// A lattice mask with the margin round it, as the plugin's grid has.
std::vector< uint8_t > withMargin( const std::vector< uint8_t >& mask, int w, int h )
{
	std::vector< uint8_t > out( static_cast< size_t >( w + 2 ) * ( h + 2 ), 1 );
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
			out[ static_cast< size_t >( y + 1 ) * ( w + 2 ) + x + 1 ] = mask[ static_cast< size_t >( y ) * w + x ];
	return out;
}

int passBound( int islands )
{
	if( islands <= 0 )
		return 0;
	int bound = 1;
	while( ( 1 << bound ) <= islands )
		++bound;
	return bound;//floor( log2 F ) + 1
}

/// Save the tally, run, and say how many failed: for the negative controls.
int failuresOf( const std::function< void() >& run )
{
	const int checks = g_checks, failures = g_failures;
	run();
	const int failed = g_failures - failures;
	g_checks         = checks;
	g_failures       = failures;
	return failed;
}

//---------------------------------------------------------------------------
// --islands. After bridging, NO piece of sheet is disconnected from the
// frame's edge, labelled by flood fill from the OUTPUT picture. On each
// fixture the harness also counts the islands the input had, on its own,
// and requires the plugin to have found the same number, and holds the
// passes to floor( log2 F ) + 1.
//---------------------------------------------------------------------------
void runIslands( int width, int height, int perturb, bool quiet )
{
	const int shapes[] = { fixtures::kLetters, fixtures::kNested, fixtures::kIslandsInIslands, fixtures::kBlobs };
	for( int which : shapes )
	{
		Session s;
		baseline( s.plugin );
		s.plugin.SetPerturbForTest( perturb );
		if( !s.begin( width, height ) )
		{
			report( false, quiet, "islands: InitGL failed" );
			return;
		}
		const fixtures::Lattice l( width, height );
		const std::vector< uint8_t > mask = fixtures::latticeMask( which, width, height );
		s.render( fixtures::expand( mask, width, height ) );

		const std::vector< uint8_t > input = withMargin( mask, l.width, l.height );
		const int before                   = reference::label( input, l.width + 2, l.height + 2 ).floating();
		const int depth                    = reference::nestingDepth( input, l.width + 2, l.height + 2 );

		const Image out                  = s.readBack();
		const std::vector< uint8_t > sheet = sheetOfOutput( out, s.plugin.InkForTest( 1 ) );
		const int after                  = reference::label( sheet, width, height ).floating();
		const bridge::Result& r          = s.plugin.CutForTest( 1 );

		const bool ok = before > 0 && after == 0 && r.islands == before && r.passes <= passBound( before );
		report( ok, quiet,
		        "islands: %-18s %3d islands in the input (the plugin found %d), nested %d deep; %zu bridges in %d "
		        "pass%s (bound floor(log2 F)+1 = %d); floating in the OUTPUT at %dx%d: %d",
		        fixtures::Name( which ), before, r.islands, depth, r.bridges.size(), r.passes, r.passes == 1 ? "" : "es",
		        passBound( before ), width, height, after );
		s.end();
	}

	//Several layers at once, on the blobs smoothed: every layer's grid,
	//labelled by the harness.
	{
		Session s;
		baseline( s.plugin );
		set( s.plugin, "Layers", 3.0f );
		set( s.plugin, "Threshold", 0.8f );
		s.plugin.SetPerturbForTest( perturb );
		if( !s.begin( width, height ) )
			return;
		s.render( fixtures::smoothImage( fixtures::kBlobs, width, height ) );
		int floating = 0, islands = 0, bridges = 0;
		for( int layer = 1; layer <= 3; ++layer )
		{
			const bridge::Grid& g = s.plugin.GridForTest( layer );
			floating += reference::label( sheetOfGrid( g ), g.width, g.height ).floating();
			islands += s.plugin.CutForTest( layer ).islands;
			bridges += static_cast< int >( s.plugin.CutForTest( layer ).bridges.size() );
		}
		report( islands > 0 && floating == 0, quiet,
		        "islands: three layers of smoothed blobs: %d islands, %d bridges; floating in the three cut grids: %d",
		        islands, bridges, floating );
		s.end();
	}
}

//---------------------------------------------------------------------------
// --shortest. Every bridge's length against the shortest gap from its
// island to any other piece of sheet in the grid as it stood at the start
// of that pass, by brute force over boundary cells (or the exact EDT on the
// biggest islands). A bridge is a real pair of cells, so it can never be
// SHORTER than the gap; the spec allows it one lattice texel longer.
//---------------------------------------------------------------------------
void runShortest( int width, int height, int perturb, bool quiet )
{
	const int shapes[] = { fixtures::kLetters, fixtures::kNested, fixtures::kIslandsInIslands, fixtures::kBlobs,
		                   fixtures::kOffsetRing };
	for( int which : shapes )
	{
		Session s;
		baseline( s.plugin );
		s.plugin.SetPerturbForTest( perturb );
		s.plugin.SetRecordForTest( true );
		if( !s.begin( width, height ) )
			return;
		s.render( fixtures::latticeImage( which, width, height ) );
		const bridge::Result& r = s.plugin.CutForTest( 1 );
		const int gw            = s.plugin.LatticeWidthForTest() + 2;
		const int gh            = s.plugin.LatticeHeightForTest() + 2;

		int checked = 0, exact = 0, brute = 0, bad = 0;
		double worst = 0.0, sumExcess = 0.0;
		std::vector< reference::Pieces > pieces( r.snapshots.size() );
		std::vector< std::vector< uint8_t > > sheets( r.snapshots.size() );
		for( size_t p = 0; p < r.snapshots.size(); ++p )
		{
			sheets[ p ] = sheetOfCells( r.snapshots[ p ] );
			pieces[ p ] = reference::label( sheets[ p ], gw, gh );
		}
		for( const bridge::Bridge& b : r.bridges )
		{
			const size_t p = static_cast< size_t >( b.pass - 1 );
			if( p >= pieces.size() )
			{
				++bad;
				continue;
			}
			const int ia = pieces[ p ].piece[ static_cast< size_t >( b.ay ) * gw + b.ax ];
			const int ib = pieces[ p ].piece[ static_cast< size_t >( b.by ) * gw + b.bx ];
			if( ia < 0 || ib < 0 || ia == ib || pieces[ p ].held[ static_cast< size_t >( ia ) ] )
			{
				++bad;
				continue;
			}
			bool usedBrute     = false;
			const double gap   = reference::shortestGap( pieces[ p ], sheets[ p ], ia, usedBrute );
			const double excess = b.length - gap;
			brute += usedBrute ? 1 : 0;
			++checked;
			exact += std::fabs( excess ) < 1e-9 ? 1 : 0;
			worst = std::max( worst, excess );
			sumExcess += excess;
			if( excess < -1e-9 || excess > 1.0 + 1e-9 )
				++bad;
		}
		report( checked > 0 && bad == 0, quiet,
		        "shortest: %-18s %3d bridges against the brute-force gap (%d by brute force, %d by exact EDT): %d exactly "
		        "the shortest, worst %.3f texels longer (allowed 1), mean %.4f",
		        fixtures::Name( which ), checked, brute, checked - brute, exact, worst, checked ? sumExcess / checked : 0.0 );
		s.end();
	}
}

//---------------------------------------------------------------------------
// --width. A band of width W centred on a line through cell centres covers,
// across it, exactly the cells within W/2 of the line: 2 floor( W / 2 ) + 1
// of them square to it, and 2 floor( W_eff / sqrt 2 ) + 1 along a row
// through a 45-degree band, W_eff = max( W, sqrt 2 ) (the floor that keeps a
// diagonal band 4-connected). The axis-aligned one is read off the OUTPUT;
// the diagonal one off the cut grid, where it is exact.
//---------------------------------------------------------------------------
void runWidth( int width, int height, int perturb, bool quiet )
{
	const fixtures::Lattice l( width, height );
	const int cx = l.width / 2, cy = l.height / 2;

	//The square ring: an island of half-side a in a hole of half-side b,
	//built on the lattice itself so the four gaps are exactly equal.
	for( float widthHeights : { 0.01f, 0.025f } )
	{
		const int a = static_cast< int >( std::lround( 0.16 * l.height ) );
		const int b = static_cast< int >( std::lround( 0.30 * l.height ) );
		std::vector< uint8_t > mask( static_cast< size_t >( l.width ) * l.height, 1 );
		for( int y = 0; y < l.height; ++y )
			for( int x = 0; x < l.width; ++x )
			{
				const int r = std::max( std::abs( x - cx ), std::abs( y - cy ) );
				if( r <= b && r > a )
					mask[ static_cast< size_t >( y ) * l.width + x ] = 0;
			}
		Session s;
		baseline( s.plugin );
		set( s.plugin, "Bridge Width", controls::BridgeWidthParam( widthHeights ) );
		s.plugin.SetPerturbForTest( perturb );
		if( !s.begin( width, height ) )
			return;
		s.render( fixtures::expand( mask, width, height ) );
		const bridge::Result& r = s.plugin.CutForTest( 1 );
		const double w          = controls::BridgeWidthHeights( controls::BridgeWidthParam( widthHeights ) ) * height / l.k;
		const int want          = 2 * static_cast< int >( std::floor( w / 2.0 + 1e-9 ) ) + 1;
		bool ok                 = r.bridges.size() == 1;
		int run = 0, dx = 0, dy = 0;
		if( ok )
		{
			const bridge::Bridge& br = r.bridges[ 0 ];
			dx                       = br.bx - br.ax;
			dy                       = br.by - br.ay;
			ok                       = dx == 0 && dy != 0;
			//The output row through the middle of the gap, and the run of
			//sheet in it that holds the bridge's column.
			const int cell = ( br.ay + br.by ) / 2 - 1;
			const int row  = cell * l.k + l.k / 2;
			const int col  = ( br.ax - 1 ) * l.k + l.k / 2;
			const Image out = s.readBack();
			const std::vector< uint8_t > sheet = sheetOfOutput( out, s.plugin.InkForTest( 1 ) );
			if( ok && sheet[ static_cast< size_t >( row ) * width + col ] )
			{
				int left = col, right = col;
				while( left > 0 && sheet[ static_cast< size_t >( row ) * width + left - 1 ] )
					--left;
				while( right < width - 1 && sheet[ static_cast< size_t >( row ) * width + right + 1 ] )
					++right;
				run = right - left + 1;
			}
			ok = ok && run == want * l.k;
		}
		report( ok, quiet,
		        "width: square ring, Bridge Width %.1f%% = %.3f texels: the bridge runs (%d, %d); across it in the OUTPUT "
		        "%d px = %d texels, want 2 floor(W/2) + 1 = %d texels (%d px)",
		        100.0 * widthHeights, w, dx, dy, run, run / l.k, want, want * l.k );
		s.end();
	}

	//The diamond ring, L1 radii r1 and r2 = r1 + m with m a multiple of 4,
	//so the shortest gap is exactly (m/2, m/2) and its middle is a cell.
	for( float widthHeights : { 0.001f, 0.02f } )
	{
		const int r1 = static_cast< int >( std::lround( 0.20 * l.height ) );
		const int m  = 4 * std::max( 1, static_cast< int >( std::lround( 0.05 * l.height ) ) );
		const int r2 = r1 + m;
		std::vector< uint8_t > mask( static_cast< size_t >( l.width ) * l.height, 1 );
		for( int y = 0; y < l.height; ++y )
			for( int x = 0; x < l.width; ++x )
			{
				const int r = std::abs( x - cx ) + std::abs( y - cy );
				if( r < r2 && r > r1 )
					mask[ static_cast< size_t >( y ) * l.width + x ] = 0;
			}
		Session s;
		baseline( s.plugin );
		set( s.plugin, "Bridge Width", controls::BridgeWidthParam( widthHeights ) );
		s.plugin.SetPerturbForTest( perturb );
		if( !s.begin( width, height ) )
			return;
		s.render( fixtures::expand( mask, width, height ) );
		const bridge::Result& r = s.plugin.CutForTest( 1 );
		const bridge::Grid& g   = s.plugin.GridForTest( 1 );
		const double w     = controls::BridgeWidthHeights( controls::BridgeWidthParam( widthHeights ) ) * height / l.k;
		const double wEff  = std::max( w, std::sqrt( 2.0 ) );
		const int want     = 2 * static_cast< int >( std::floor( wEff / std::sqrt( 2.0 ) + 1e-9 ) ) + 1;
		bool ok            = r.bridges.size() == 1;
		int count = 0, dx = 0, dy = 0;
		if( ok )
		{
			const bridge::Bridge& br = r.bridges[ 0 ];
			dx                       = br.bx - br.ax;
			dy                       = br.by - br.ay;
			ok                       = std::abs( dx ) == m / 2 && std::abs( dy ) == m / 2;
			const int my             = br.ay + dy / 2;
			for( int x = 0; x < g.width; ++x )
				count += g.at( x, my ) == bridge::kBridge ? 1 : 0;
			ok = ok && count == want;
		}
		const int floating = reference::label( sheetOfGrid( g ), g.width, g.height ).floating();
		ok                 = ok && floating == 0;
		report( ok, quiet,
		        "width: diamond ring, Bridge Width %.1f%% = %.3f texels: the bridge runs (%d, %d), %d cells across a row "
		        "through its middle, want 2 floor(W_eff/sqrt2) + 1 = %d (W_eff %.3f); pieces floating 4-connected: %d",
		        100.0 * widthHeights, w, dx, dy, count, want, wEff, floating );
		s.end();
	}
}

//---------------------------------------------------------------------------
// --overspray. A straight edge (the left half a hole), sprayed from
// Distance: the paint across it, read out of the output, against the chord
// fraction f(u) = ( R^2 acos( -u/R ) + u sqrt( R^2 - u^2 ) ) / ( pi R^2 ) at
// every texel centre, bilinearly interpolated to each output pixel as the
// composite samples it. And the 25-75% width, 0.80794 R.
//---------------------------------------------------------------------------
double chord( double radius, double u )
{
	if( u <= -radius )
		return 0.0;
	if( u >= radius )
		return 1.0;
	return ( radius * radius * std::acos( -u / radius ) + u * std::sqrt( radius * radius - u * u ) ) / ( kPi * radius * radius );
}

/// Where a sampled, increasing-into-the-hole profile crosses `level`, by
/// linear interpolation, in texels. Samples at positions `at`.
double crossing( const std::vector< double >& at, const std::vector< double >& value, double level )
{
	for( size_t i = 1; i < value.size(); ++i )
	{
		const double a = value[ i - 1 ], b = value[ i ];
		if( ( a - level ) * ( b - level ) <= 0.0 && a != b )
			return at[ i - 1 ] + ( level - a ) / ( b - a ) * ( at[ i ] - at[ i - 1 ] );
	}
	return std::nan( "" );
}

void runOverspray( int width, int height, int perturb, bool quiet )
{
	const fixtures::Lattice l( width, height );
	double widths[ 2 ] = { 0, 0 }, radii[ 2 ] = { 0, 0 };
	int n = 0;
	for( float distance : { 0.01f, 0.02f } )
	{
		Session s;
		baseline( s.plugin );
		set( s.plugin, "Distance", controls::DistanceParam( distance ) );
		s.plugin.SetPerturbForTest( perturb );
		if( !s.begin( width, height ) )
			return;
		const std::vector< uint8_t > mask = fixtures::latticeMask( fixtures::kEdge, width, height );
		s.render( fixtures::expand( mask, width, height ) );
		const Image out = s.readBack();

		//The edge, in texels: the first sheet cell of the lattice's middle row.
		int edge = 0;
		while( edge < l.width && !mask[ static_cast< size_t >( l.height / 2 ) * l.width + edge ] )
			++edge;
		const double R = controls::FootprintRadiusHeights( controls::DistanceParam( distance ) ) * height / l.k;
		const size_t taps = spray::Footprint( R ).size();
		//One conversion into RGBA16F, which GL does not require to round to
		//nearest -- this renderer truncates -- so one whole ulp of [0.5, 1),
		//2^-11; a float sum of `taps` terms each under 1 (taps x 2^-24); and
		//the composite and its inversion here (2^-20).
		const double tolerance = std::ldexp( 1.0, -11 ) + static_cast< double >( taps ) * std::ldexp( 1.0, -24 ) + std::ldexp( 1.0, -20 );

		const float* ink = s.plugin.InkForTest( 1 );
		const int row    = height / 2;
		double worst     = 0.0;
		int samples      = 0;
		std::vector< double > at, measured, expected;
		for( int x = 0; x < width; ++x )
		{
			const double X = ( x + 0.5 ) / l.k - 0.5;//texel-centre coordinates
			if( std::fabs( X + 0.5 - edge ) > R + 2.0 )
				continue;
			const int i0    = static_cast< int >( std::floor( X ) );
			const double fx = X - i0;
			auto f          = [ & ]( int i ) { return chord( R, edge - ( i + 0.5 ) ); };
			const double expect = ( 1.0 - fx ) * f( i0 ) + fx * f( i0 + 1 );
			const double got    = ( palette::kPlainWall.r - out.at( x, row )[ 0 ] ) / ( palette::kPlainWall.r - ink[ 0 ] );
			worst               = std::max( worst, std::fabs( got - expect ) );
			++samples;
			//Into the hole is increasing u, so the profile runs from x high to low.
			at.insert( at.begin(), edge - ( X + 0.5 ) );
			measured.insert( measured.begin(), got );
			expected.insert( expected.begin(), expect );
		}
		//The 25-75% width, by linear interpolation between the samples, of
		//the picture and of the chord fraction sampled the same way: the
		//density's tolerance moves each crossing by at most tolerance / f'
		//at the quartile, f' = 2 sqrt( 1 - 0.40397^2 ) / ( pi R ). The
		//sampled width is not 0.80794 R exactly (a profile known at texel
		//centres, linear between, bends the crossings at small R); that it
		//is the chord fraction's is the pointwise check above.
		const double w      = crossing( at, measured, 0.75 ) - crossing( at, measured, 0.25 );
		const double wWant  = crossing( at, expected, 0.75 ) - crossing( at, expected, 0.25 );
		const double fp     = 2.0 / ( kPi * R ) * std::sqrt( 1.0 - 0.40397 * 0.40397 );
		const double wBound = 2.0 * tolerance / fp;
		const bool ok       = samples > 4 && worst <= tolerance && std::fabs( w - wWant ) <= wBound;
		report( ok, quiet,
		        "overspray: Distance %.0f%% -> footprint R = %.3f texels (%zu taps): %d pixels across the edge, worst %.2e "
		        "from the chord fraction (allowed %.2e); 25-75%% width %.4f texels, the sampled chord's %.4f (+- %.4f), "
		        "%.4f R (the continuous chord's is 0.80794 R)",
		        100.0 * distance, R, taps, samples, worst, tolerance, w, wWant, wBound, w / R );
		widths[ n ] = w;
		radii[ n ]  = R;
		++n;
		s.end();
	}
	if( n == 2 && !quiet )
		std::printf( "          the width grows with Distance: %.4f / %.4f = %.4f for R %.3f / %.3f = %.4f\n", widths[ 1 ],
		             widths[ 0 ], widths[ 1 ] / widths[ 0 ], radii[ 1 ], radii[ 0 ], radii[ 1 ] / radii[ 0 ] );
}

//---------------------------------------------------------------------------
// --layers. A ramp of tone, left dark to right light, cut at n layers: the
// output has n + 1 plateaus, the darkest ink on the left, each exactly its
// ink, the wall on the right past Threshold. A pixel is judged where the
// answer is decidable: its texel and both neighbours (which the composite's
// bilinear filter reaches) in the same band, each with its tone more than
// 2^-10 from every cut (the tone is stored in RGBA16F, which is off by under
// an ulp, 2^-11 below 1, whichever way the conversion rounds).
//---------------------------------------------------------------------------
void runLayers( int width, int height, int perturb, bool quiet )
{
	const fixtures::Lattice l( width, height );
	const float threshold = 0.8f;
	for( int n = 1; n <= 4; ++n )
	{
		Session s;
		baseline( s.plugin );
		set( s.plugin, "Layers", static_cast< float >( n ) );
		set( s.plugin, "Threshold", threshold );
		set( s.plugin, "Palette", static_cast< float >( controls::kPop ) );
		s.plugin.SetPerturbForTest( perturb );
		if( !s.begin( width, height ) )
			return;
		s.render( fixtures::latticeImage( fixtures::kRamp, width, height ) );
		const Image out = s.readBack();

		//The expected band of texel i: 0 the wall, else the darkest layer
		//whose cut its tone is under.
		auto toneOf = [ & ]( int i ) {
			double sum = 0.0;
			int count  = 0;
			for( int x = i * l.k; x < i * l.k + l.k; ++x )
			{
				sum += ( std::min( x, width - 1 ) + 0.5 ) / width;
				++count;
			}
			return sum / count;
		};
		auto bandOf = [ & ]( double tone ) {
			for( int layer = n; layer >= 1; --layer )
				if( tone < controls::LayerThreshold( threshold, layer, n ) )
					return layer;
			return 0;
		};
		auto colourOf = [ & ]( int band, float* rgb ) {
			if( band == 0 )
			{
				rgb[ 0 ] = palette::kPlainWall.r;
				rgb[ 1 ] = palette::kPlainWall.g;
				rgb[ 2 ] = palette::kPlainWall.b;
				return;
			}
			const palette::Colour c = palette::Ink( controls::kPop, band, n );
			rgb[ 0 ]                = c.r;
			rgb[ 1 ]                = c.g;
			rgb[ 2 ]                = c.b;
		};

		const int row = height / 2;
		int judged = 0, wrong = 0, runs = 0, lastBand = -1;
		std::vector< int > order;
		double worst = 0.0;
		for( int x = 0; x < width; ++x )
		{
			const int i       = x / l.k;
			const double tone = toneOf( i );
			const int band    = bandOf( tone );
			//A texel's band is certain when its tone is further than 2^-10
			//from every cut: the tone is stored in RGBA16F, off by under an
			//ulp (2^-11 below 1) whichever way the conversion rounds.
			auto certain = [ & ]( int j ) {
				if( j < 0 || j >= l.width )
					return true;
				for( int layer = 1; layer <= n; ++layer )
					if( std::fabs( toneOf( j ) - controls::LayerThreshold( threshold, layer, n ) ) <= std::ldexp( 1.0, -10 ) )
						return false;
				return j == i || bandOf( toneOf( j ) ) == band;
			};
			const bool decidable = certain( i ) && certain( i - 1 ) && certain( i + 1 );
			if( !decidable )
				continue;
			float want[ 3 ];
			colourOf( band, want );
			const float* got = out.at( x, row );
			double d         = 0.0;
			for( int c = 0; c < 3; ++c )
				d = std::max( d, static_cast< double >( std::fabs( got[ c ] - want[ c ] ) ) );
			worst = std::max( worst, d );
			++judged;
			if( d > 1e-6 )
				++wrong;
			if( band != lastBand )
			{
				++runs;
				order.push_back( band );
				lastBand = band;
			}
		}
		bool ordered = static_cast< int >( order.size() ) == n + 1;
		for( size_t j = 0; ordered && j < order.size(); ++j )
			ordered = order[ j ] == n - static_cast< int >( j );
		const bool ok = judged > width * 3 / 4 && wrong == 0 && ordered;
		report( ok, quiet,
		        "layers: %d layer%s at Threshold %.1f: %d plateaus across the ramp, darkest ink first (%s); %d of %d "
		        "decidable pixels exactly their plateau's colour, worst %.1e (allowed 1e-6)",
		        n, n == 1 ? "" : "s", threshold, runs, ordered ? "in order" : "OUT OF ORDER", judged - wrong, judged, worst );
		s.end();
	}
}

//---------------------------------------------------------------------------
// --stability.
//
// (a) A picture panned by whole lattice cells: every frame's bridges must be
// the first frame's translated, exactly. The ties (four equal sides of a
// square, the symmetric counters) are broken by a total order a translation
// preserves, so there is nothing for them to flicker between.
//
// (b) A slow sub-pixel pan, antialiased, as footage is: churn is the share
// of bridges from one frame to the next that are not the same bridge moved
// with the picture (midpoints within 1.5 texels once the pan is taken out,
// lengths within 1.5 texels). Measured with last frame's bridges kept, as
// the plugin runs, and without, for the record; the check requires keeping
// them to churn less, and a fixture that churns at all without.
//---------------------------------------------------------------------------
struct Seen
{
	double mx, my, length;//frame heights of the CONTENT
};

std::vector< Seen > bridgesOf( const Stencil& p, double pan, int height )
{
	std::vector< Seen > out;
	const int k = p.LatticeScaleForTest();
	for( const bridge::Bridge& b : p.CutForTest( 1 ).bridges )
	{
		const double mx = ( 0.5 * ( b.ax + b.bx ) - 0.5 ) * k / height - pan;
		const double my = ( 0.5 * ( b.ay + b.by ) - 0.5 ) * k / height;
		out.push_back( { mx, my, b.length * k / height } );
	}
	return out;
}

int unmatched( const std::vector< Seen >& a, const std::vector< Seen >& b, double tolerance )
{
	int n = 0;
	for( const Seen& s : a )
	{
		bool found = false;
		for( const Seen& t : b )
			if( std::hypot( s.mx - t.mx, s.my - t.my ) <= tolerance && std::fabs( s.length - t.length ) <= tolerance )
			{
				found = true;
				break;
			}
		n += found ? 0 : 1;
	}
	return n;
}

/// The worst any bridge of the last frame is longer than the brute-force
/// shortest gap of its pass (texels).
double worstExcess( const Stencil& p )
{
	const bridge::Result& r = p.CutForTest( 1 );
	const int gw            = p.LatticeWidthForTest() + 2;
	const int gh            = p.LatticeHeightForTest() + 2;
	double worst            = 0.0;
	for( size_t pass = 0; pass < r.snapshots.size(); ++pass )
	{
		const std::vector< uint8_t > sheet = sheetOfCells( r.snapshots[ pass ] );
		const reference::Pieces pieces     = reference::label( sheet, gw, gh );
		for( const bridge::Bridge& b : r.bridges )
		{
			if( b.pass != static_cast< int >( pass ) + 1 )
				continue;
			const int ia = pieces.piece[ static_cast< size_t >( b.ay ) * gw + b.ax ];
			if( ia < 0 )
				return 1e9;
			bool brute = false;
			worst      = std::max( worst, b.length - reference::shortestGap( pieces, sheet, ia, brute ) );
		}
	}
	return worst;
}

double churnOf( int width, int height, fixtures::PanCache& pictures, int perturb, int frames, double pxPerFrame,
                int& changedFrames, int& total, double& excess )
{
	Session s;
	baseline( s.plugin );
	s.plugin.SetPerturbForTest( perturb );
	s.plugin.SetRecordForTest( true );
	excess = 0.0;
	if( !s.begin( width, height ) )
		return -1.0;
	const double texel = static_cast< double >( s.plugin.LatticeScale( height ) ) / height;
	std::vector< Seen > previous;
	int moved = 0;
	total = 0;
	changedFrames = 0;
	for( int f = 0; f < frames; ++f )
	{
		const double pan = f * pxPerFrame / height;
		s.render( pictures.at( f * pxPerFrame ) );
		const std::vector< Seen > now = bridgesOf( s.plugin, pan, height );
		//Every tenth frame, the kept bridges against brute force: keeping
		//one is allowed to cost at most Stencil::kKeepSlack.
		if( f % 10 == 9 )
			excess = std::max( excess, worstExcess( s.plugin ) );
		if( f > 0 )
		{
			const int m = unmatched( now, previous, 1.5 * texel ) + unmatched( previous, now, 1.5 * texel );
			moved += m;
			total += static_cast< int >( now.size() + previous.size() );
			changedFrames += m > 0 ? 1 : 0;
		}
		previous = now;
	}
	s.end();
	return total > 0 ? static_cast< double >( moved ) / total : 0.0;
}

void runStability( int width, int height, int perturb, bool quiet )
{
	const fixtures::Lattice l( width, height );
	//(a) whole cells, and the tie-break alone: last frame's bridges are
	//kept where they were in the FRAME while they are good enough, which is
	//not a translation, so they are switched off here.
	for( int which : { fixtures::kSquareRing, fixtures::kLetters, fixtures::kNested } )
	{
		Session s;
		baseline( s.plugin );
		s.plugin.SetPerturbForTest( perturb | bridge::kPerturbNoHistory );
		if( !s.begin( width, height ) )
			return;
		//The fixture drawn once, eight cells left of centre, and shifted by
		//whole cells, so the lattice mask translates exactly.
		const std::vector< uint8_t > base = fixtures::latticeMask( which, width, height, -8.0 * l.k / height );
		std::vector< bridge::Bridge > first;
		int frames = 0, differ = 0;
		size_t count = 0;
		for( int f = 0; f < 16; ++f )
		{
			std::vector< uint8_t > mask( base.size(), 1 );
			for( int y = 0; y < l.height; ++y )
				for( int x = f; x < l.width; ++x )
					mask[ static_cast< size_t >( y ) * l.width + x ] = base[ static_cast< size_t >( y ) * l.width + x - f ];
			s.render( fixtures::expand( mask, width, height ) );
			const std::vector< bridge::Bridge >& now = s.plugin.CutForTest( 1 ).bridges;
			if( f == 0 )
			{
				first = now;
				count = now.size();
			}
			else
			{
				bool same = now.size() == first.size();
				for( size_t i = 0; same && i < now.size(); ++i )
				{
					bool found = false;
					for( const bridge::Bridge& b : first )
						found = found
						        || ( now[ i ].ax == b.ax + f && now[ i ].ay == b.ay && now[ i ].bx == b.bx + f && now[ i ].by == b.by );
					same = found;
				}
				differ += same ? 0 : 1;
			}
			++frames;
		}
		report( count > 0 && differ == 0, quiet,
		        "stability: %-18s panned a whole texel a frame for %d frames, the tie-break alone: %zu bridges, %d "
		        "frame%s NOT the first translated (must be 0)",
		        fixtures::Name( which ), frames, count, differ, differ == 1 ? "" : "s" );
		s.end();
	}

}

/// --churn: (b) above, split out so the negative controls need not run it.
void runChurn( int width, int height, int perturb, bool quiet )
{
	//(b) a slow sub-pixel pan.
	const double speed = 0.2;//pixels a frame
	const int frames   = 60;
	for( int which : { fixtures::kLetters, fixtures::kNested, fixtures::kBlobs } )
	{
		int changedKept = 0, totalKept = 0, changedFree = 0, totalFree = 0;
		double excessKept = 0.0, excessFree = 0.0;
		fixtures::PanCache pictures( width, height, which, static_cast< int >( std::ceil( speed * frames ) ) + 2 );
		const double kept = churnOf( width, height, pictures, perturb, frames, speed, changedKept, totalKept, excessKept );
		const double free = churnOf( width, height, pictures, perturb | bridge::kPerturbNoHistory, frames, speed, changedFree,
		                             totalFree, excessFree );
		//A kept bridge may be kKeepSlack longer than the shortest by design;
		//a fresh one, by the spec's allowance, one texel.
		const bool ok = kept >= 0.0 && free > 0.0 && kept < free && excessKept <= Stencil::kKeepSlack + 1.0 + 1e-9
		                && excessFree <= 1.0 + 1e-9;
		report( ok, quiet,
		        "churn: %-18s panned %.1f px a frame for %d frames: churn %.2f%% of bridge-frames (%d frames changed) "
		        "keeping last frame's bridges, %.2f%% (%d frames) without; worst bridge over the brute-force shortest "
		        "%.3f texels kept (allowed %.0f), %.3f not (allowed 1)",
		        fixtures::Name( which ), speed, frames, 100.0 * kept, changedKept, 100.0 * free, changedFree, excessKept,
		        Stencil::kKeepSlack + 1.0, excessFree );
	}
}

//---------------------------------------------------------------------------
// --resize. Last frame's bridges are state, so they must survive a resize.
// A square island whose left and right gaps are equal (a tie a fresh
// instance breaks to the LEFT, the lower x) after a frame whose right gap
// was shorter (so the bridge was on the right, and is kept). Resized to a
// raster whose lattice is 1x or 2x the first, where the tie is still exact,
// the kept bridge must still be on the right.
//---------------------------------------------------------------------------
std::vector< uint8_t > resizeFixture( int latticeW, int latticeH, int leftGap )
{
	//Base cells of 1/180 of the lattice's height, so every length scales
	//exactly between lattices of 180, 360 and 540 rows.
	const int u  = std::max( 1, latticeH / 180 );
	const int cx = latticeW / 2, cy = latticeH / 2;
	std::vector< uint8_t > mask( static_cast< size_t >( latticeW ) * latticeH, 1 );
	for( int y = 0; y < latticeH; ++y )
		for( int x = 0; x < latticeW; ++x )
		{
			const bool hole   = x >= cx - ( 20 + leftGap ) * u && x < cx + 30 * u && y >= cy - 40 * u && y < cy + 40 * u;
			const bool island = x >= cx - 20 * u && x < cx + 20 * u && y >= cy - 20 * u && y < cy + 20 * u;
			if( hole && !island )
				mask[ static_cast< size_t >( y ) * latticeW + x ] = 0;
		}
	return mask;
}

/// -1 left, +1 right, 0 anything else (the one bridge's direction).
int sideOf( const Stencil& p )
{
	const std::vector< bridge::Bridge >& b = p.CutForTest( 1 ).bridges;
	if( b.size() != 1 || b[ 0 ].by != b[ 0 ].ay )
		return 0;
	return b[ 0 ].bx < b[ 0 ].ax ? -1 : 1;
}

void runResize( int width, int height, int perturb, bool quiet )
{
	const char* names[] = { "left", "?", "right" };
	auto picture        = [ & ]( int w, int h, int leftGap ) {
        const fixtures::Lattice l( w, h );
        return fixtures::expand( resizeFixture( l.width, l.height, leftGap ), w, h );
	};
	//The raster to resize to: twice the size if its lattice doubles, else
	//half, whose lattice is the same size.
	int w2 = 2 * width, h2 = 2 * height;
	if( 2 * height > Stencil::kLatticeLines )
	{
		w2 = width / 2;
		h2 = height / 2;
	}

	int fresh = 0, kept = 0, resized = 0;
	{
		Session s;
		baseline( s.plugin );
		s.plugin.SetPerturbForTest( perturb );
		if( !s.begin( width, height ) )
			return;
		s.render( picture( width, height, 10 ) );
		fresh = sideOf( s.plugin );
		s.end();
	}
	{
		Session s;
		baseline( s.plugin );
		s.plugin.SetPerturbForTest( perturb );
		if( !s.begin( width, height ) )
			return;
		for( int f = 0; f < 3; ++f )
			s.render( picture( width, height, 12 ) );
		s.render( picture( width, height, 10 ) );
		kept = sideOf( s.plugin );
		s.resize( w2, h2 );
		s.render( picture( w2, h2, 10 ) );
		resized = sideOf( s.plugin );
		s.end();
	}
	report( fresh == -1, quiet, "resize: a fresh instance breaks the tie to the %s (want left)", names[ fresh + 1 ] );
	report( kept == 1, quiet, "resize: after three frames with the right gap shorter, the tie keeps the %s (want right)",
	        names[ kept + 1 ] );
	report( resized == 1, quiet, "resize: %dx%d -> %dx%d mid-run, the first frame after keeps the %s (want right)", width,
	        height, w2, h2, names[ resized + 1 ] );
}

//---------------------------------------------------------------------------
// --negative. Each perturbation breaks one piece of the model; its check
// must FAIL.
//---------------------------------------------------------------------------
void runNegative( int width, int height )
{
	struct Control
	{
		const char* what;
		int bits;
		std::function< void( int ) > check;
	};
	const std::vector< Control > controls = {
		{ "no bridges: --islands", bridge::kPerturbNoBridges, [ & ]( int b ) { runIslands( width, height, b, true ); } },
		{ "bridge straight up, not the shortest: --shortest", bridge::kPerturbFixedDirection,
		  [ & ]( int b ) { runShortest( width, height, b, true ); } },
		{ "a band W wide each side: --width", bridge::kPerturbWideBand, [ & ]( int b ) { runWidth( width, height, b, true ); } },
		{ "no 4-connectivity floor on a thin diagonal: --width", bridge::kPerturbThinBand,
		  [ & ]( int b ) { runWidth( width, height, b, true ); } },
		{ "a Gaussian footprint of the same quartile width: --overspray", Stencil::kPerturbGaussianCone,
		  [ & ]( int b ) { runOverspray( width, height, b, true ); } },
		{ "layers painted darkest first: --layers", Stencil::kPerturbLightOverDark,
		  [ & ]( int b ) { runLayers( width, height, b, true ); } },
		{ "ties broken toward the frame's centre: --stability", bridge::kPerturbTieCentre,
		  [ & ]( int b ) { runStability( width, height, b, true ); } },
		{ "last frame's bridges ignored: --resize", bridge::kPerturbNoHistory,
		  [ & ]( int b ) { runResize( width, height, b, true ); } },
		{ "a resize forgets last frame's bridges: --resize", Stencil::kPerturbResizeClears,
		  [ & ]( int b ) { runResize( width, height, b, true ); } },
	};
	for( const Control& c : controls )
	{
		const int failed = failuresOf( [ & ]() { c.check( c.bits ); } );
		report( failed > 0, false, "negative: %-62s %d check%s failed (must be > 0)", c.what, failed, failed == 1 ? "" : "s" );
	}
}

//---------------------------------------------------------------------------
// The checks that need no GL.
//---------------------------------------------------------------------------
void runNames()
{
	Stencil plugin;
	int bad = 0;
	std::map< std::string, int > seen;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		if( p.name.size() > 16 )
		{
			std::printf( "   names: '%s' is %zu characters, over 16\n", p.name.c_str(), p.name.size() );
			++bad;
		}
		if( seen[ p.name ]++ > 0 )
		{
			std::printf( "   names: '%s' is not unique\n", p.name.c_str() );
			++bad;
		}
	}
	const char* display = "SW Stencil";
	report( bad == 0 && std::strlen( display ) <= 16, false,
	        "names: %zu parameters, all within 16 characters and unique; display name '%s' (%zu)", seen.size(), display,
	        std::strlen( display ) );
}

/// --exact: the reference EDT against brute force, on random masks.
void runExact( bool cityBlock, bool quiet )
{
	int bad = 0, masks = 0, cells = 0;
	uint32_t state = 12345u;
	auto next      = [ & ]() {
		state = state * 1664525u + 1013904223u;
		return state >> 8;
	};
	for( int trial = 0; trial < 40; ++trial )
	{
		const int w = 8 + static_cast< int >( next() % 57 ), h = 6 + static_cast< int >( next() % 41 );
		const int density = 1 + static_cast< int >( next() % 60 );
		std::vector< uint8_t > seed( static_cast< size_t >( w ) * h );
		for( auto& m : seed )
			m = ( next() % 100 ) < static_cast< uint32_t >( density ) ? 1 : 0;
		const std::vector< double > fast = reference::squaredEdt( seed, w, h, cityBlock );
		for( int y = 0; y < h; ++y )
			for( int x = 0; x < w; ++x )
			{
				double best = reference::kInf;
				for( int yy = 0; yy < h; ++yy )
					for( int xx = 0; xx < w; ++xx )
						if( seed[ static_cast< size_t >( yy ) * w + xx ] )
							best = std::min( best, double( xx - x ) * ( xx - x ) + double( yy - y ) * ( yy - y ) );
				++cells;
				if( fast[ static_cast< size_t >( y ) * w + x ] != best )
					++bad;
			}
		++masks;
	}
	report( bad == 0, quiet, "exact: the reference EDT against brute force, %d random masks, %d cells: %d disagree (must be 0)",
	        masks, cells, bad );
}

/// The flood, exactly, by brute force: for every cell the nearest sheet
/// cell, ties by (y, x), and the nearest sheet cell of another piece than
/// that one's, ties by (y, x). `ignoreLabels` is the negative control: the
/// nearest sheet cell other than the cell itself, whatever its piece.
bridge::Flood bruteFlood( bool ignoreLabels )
{
	return [ ignoreLabels ]( const std::vector< uint32_t >& labels, int w, int h, const bridge::Region&,
	                         std::vector< uint32_t >& second ) {
		second.assign( static_cast< size_t >( w ) * h, bridge::kNone );
		std::vector< int > sheet;
		for( int i = 0; i < w * h; ++i )
			if( labels[ static_cast< size_t >( i ) ] )
				sheet.push_back( i );
		for( int i = 0; i < w * h; ++i )
		{
			const int x = i % w, y = i / w;
			auto better = [ & ]( long d, int j, long bestD, int best ) {
				if( d != bestD )
					return d < bestD;
				return j / w != best / w ? j / w < best / w : j % w < best % w;
			};
			long d1 = -1;
			int s1  = -1;
			for( int j : sheet )
			{
				const long d = long( j % w - x ) * ( j % w - x ) + long( j / w - y ) * ( j / w - y );
				if( s1 < 0 || better( d, j, d1, s1 ) )
				{
					d1 = d;
					s1 = j;
				}
			}
			if( s1 < 0 )
				continue;
			long d2 = -1;
			int s2  = -1;
			for( int j : sheet )
			{
				if( ignoreLabels ? j == i : labels[ static_cast< size_t >( j ) ] == labels[ static_cast< size_t >( s1 ) ] )
					continue;
				const long d = long( j % w - x ) * ( j % w - x ) + long( j / w - y ) * ( j / w - y );
				if( s2 < 0 || better( d, j, d2, s2 ) )
				{
					d2 = d;
					s2 = j;
				}
			}
			if( s2 >= 0 )
				second[ static_cast< size_t >( i ) ] = bridge::Pack( s2 % w, s2 / w );
		}
		return true;
	};
}

/// --cutter: Bridge.cpp on random masks with the exact flood above. No
/// floating piece after, every bridge EXACTLY the shortest (the flood is
/// exact, so there is no JFA slack to allow), the passes within the bound,
/// and the union-find labels the same partition as the flood fill.
void runCutter( bool ignoreLabels, bool quiet )
{
	uint32_t state = 777u;
	auto next      = [ & ]() {
		state = state * 1664525u + 1013904223u;
		return state >> 8;
	};
	int trials = 0, floating = 0, notShortest = 0, overBound = 0, partitions = 0, bridges = 0, islands = 0, deepest = 0;
	for( int trial = 0; trial < 30; ++trial )
	{
		const int w = 40 + static_cast< int >( next() % 30 ), h = 30 + static_cast< int >( next() % 20 );
		bridge::Grid g;
		g.width  = w + 2;
		g.height = h + 2;
		g.cells.assign( static_cast< size_t >( g.width ) * g.height, bridge::kSheet );
		const double cell = 3.0 + ( next() % 5 );
		const uint64_t salt = next();
		if( trial % 2 == 0 )
		{
			for( int y = 0; y < h; ++y )
				for( int x = 0; x < w; ++x )
					if( fixtures::valueNoise( x, y, cell, salt ) < 0.5 )
						g.at( x + 1, y + 1 ) = bridge::kHole;
		}
		else
		{
			//Rings in rings, each off the last one's centre, dark and white
			//in turn, with specks of noise: islands several deep.
			double cx = 0.5 * w + ( next() % 5 ) - 2.0, cy = 0.5 * h + ( next() % 5 ) - 2.0;
			const int rings = 2 + static_cast< int >( next() % 3 );
			const double step = 0.5 * std::min( w, h ) / ( 2 * rings + 1 );
			for( int y = 0; y < h; ++y )
				for( int x = 0; x < w; ++x )
				{
					double ox = cx, oy = cy;
					int band = 0;
					for( int ring = 0; ring < 2 * rings; ++ring )
					{
						const double r = std::hypot( x - ox, y - oy );
						if( r < ( 2 * rings - ring ) * step )
							band = ring + 1;
						ox += ( ring % 2 ? 0.4 : -0.3 );
						oy += 0.25;
					}
					const bool dark = band % 2 == 1 || fixtures::valueNoise( x, y, 2.0, salt ) < 0.12;
					if( dark )
						g.at( x + 1, y + 1 ) = bridge::kHole;
				}
		}
		const std::vector< uint8_t > before = sheetOfGrid( g );
		const int f = reference::label( before, g.width, g.height ).floating();
		islands += f;
		deepest = std::max( deepest, reference::nestingDepth( before, g.width, g.height ) );

		//Union-find against flood fill, as partitions.
		{
			std::vector< uint32_t > labels;
			uint32_t anchor = 0;
			bridge::Label( g, labels, anchor );
			const reference::Pieces p = reference::label( before, g.width, g.height );
			std::map< uint32_t, int > aToB;
			std::map< int, uint32_t > bToA;
			for( size_t i = 0; i < labels.size(); ++i )
			{
				if( ( labels[ i ] == 0 ) != ( p.piece[ i ] < 0 ) )
				{
					++partitions;
					break;
				}
				if( labels[ i ] == 0 )
					continue;
				auto a = aToB.emplace( labels[ i ], p.piece[ i ] );
				auto b = bToA.emplace( p.piece[ i ], labels[ i ] );
				if( a.first->second != p.piece[ i ] || b.first->second != labels[ i ] )
				{
					++partitions;
					break;
				}
			}
		}

		bridge::Settings settings;
		settings.width = 1.0 + ( next() % 3 );
		const bridge::Result r = bridge::Cut( g, settings, {}, bruteFlood( ignoreLabels ), true );
		floating += reference::label( sheetOfGrid( g ), g.width, g.height ).floating();
		overBound += r.passes > passBound( f ) ? 1 : 0;
		for( const bridge::Bridge& b : r.bridges )
		{
			const size_t p = static_cast< size_t >( b.pass - 1 );
			const std::vector< uint8_t > sheet = sheetOfCells( r.snapshots[ p ] );
			const reference::Pieces pieces     = reference::label( sheet, g.width, g.height );
			bool brute                         = false;
			const int ia                       = pieces.piece[ static_cast< size_t >( b.ay ) * g.width + b.ax ];
			const double gap                   = ia >= 0 ? reference::shortestGap( pieces, sheet, ia, brute ) : -1.0;
			notShortest += std::fabs( b.length - gap ) > 1e-9 ? 1 : 0;
			++bridges;
		}
		++trials;
	}
	report( floating == 0 && notShortest == 0 && overBound == 0 && partitions == 0 && islands > 0, quiet,
	        "cutter: %d random grids, %d islands (nested up to %d deep), %d bridges with an exact flood: %d floating after, "
	        "%d not exactly the shortest, %d over the pass bound, %d union-find partitions unlike the flood fill's (all "
	        "must be 0)",
	        trials, islands, deepest, bridges, floating, notShortest, overBound, partitions );
}

/// --band: every band is 4-connected and holds both its ends; square and
/// 45-degree bands have the exact cross-section.
void runBand( int perturb, bool quiet )
{
	uint32_t state = 4242u;
	auto next      = [ & ]() {
		state = state * 1664525u + 1013904223u;
		return state >> 8;
	};
	int bands = 0, broken = 0, missing = 0, wrongWidth = 0;
	std::vector< std::pair< int, int > > cells;
	for( int trial = 0; trial < 2000; ++trial )
	{
		const int n  = 64;
		const int ax = 8 + static_cast< int >( next() % 48 ), ay = 8 + static_cast< int >( next() % 48 );
		int bx = 8 + static_cast< int >( next() % 48 ), by = 8 + static_cast< int >( next() % 48 );
		if( bx == ax && by == ay )
			++bx;
		const double width = 0.05 * ( next() % 120 );
		bridge::Band( ax, ay, bx, by, width, perturb, n, n, cells );
		std::vector< uint8_t > on( static_cast< size_t >( n ) * n, 0 );
		for( const auto& c : cells )
			on[ static_cast< size_t >( c.second ) * n + c.first ] = 1;
		if( !on[ static_cast< size_t >( ay ) * n + ax ] || !on[ static_cast< size_t >( by ) * n + bx ] )
			++missing;
		//4-connected: one piece, found by the harness's flood fill (margin
		//ignored: a band never reaches the edge here).
		std::vector< uint8_t > inner( on );
		const reference::Pieces p = reference::label( inner, n, n );
		if( p.size.size() != 1 )
			++broken;
		++bands;

		//An axis-aligned or 45-degree band: the cross-section in the middle.
		const int dx = bx - ax, dy = by - ay;
		if( dx == 0 && std::abs( dy ) >= 4 )
		{
			const int my = ay + dy / 2;
			int count    = 0;
			for( int x = 0; x < n; ++x )
				count += on[ static_cast< size_t >( my ) * n + x ];
			const int want = 2 * static_cast< int >( std::floor( std::max( width, 1.0 ) / 2.0 + 1e-9 ) ) + 1;
			wrongWidth += count != want ? 1 : 0;
		}
	}
	report( broken == 0 && missing == 0 && wrongWidth == 0, quiet,
	        "band: %d random bridges, widths 0 to 6 cells: %d not 4-connected, %d missing an end, %d vertical ones not 2 "
	        "floor(W/2) + 1 across (all must be 0)",
	        bands, broken, missing, wrongWidth );
}

/// --footprint: the taps sum to 1, and over a half-plane of whole texels
/// they sum to the chord fraction, computed here from its own formula.
void runFootprint( bool gaussian, bool quiet )
{
	double worst = 0.0, worstSum = 0.0;
	int cases    = 0;
	for( double R : { 0.7, 1.8, 3.6, 7.2, 10.8 } )
	{
		const std::vector< spray::Tap > taps = spray::Footprint( gaussian ? 2.0 * 0.5989 * R : R, gaussian );
		double sum                           = 0.0;
		for( const spray::Tap& t : taps )
			sum += t.weight;
		worstSum = std::max( worstSum, std::fabs( sum - 1.0 ) );
		//The edge between texel columns, at every offset of the centre that
		//the lattice can have from it: a texel centre is u = e - (i + 0.5).
		for( int i = -static_cast< int >( R ) - 2; i <= static_cast< int >( R ) + 2; ++i )
		{
			const double u = i + 0.5;//the centre's distance into the hole, which is x < 0 .. here x < u
			double inside  = 0.0;
			for( const spray::Tap& t : taps )
				if( t.dx + 0.5 <= u + 1e-9 )//the tap's whole texel lies left of the edge
					inside += t.weight;
			worst = std::max( worst, std::fabs( inside - chord( R, u ) ) );
			++cases;
		}
	}
	//The weights are floats, so 2^-24 each and a sum of up to 625.
	const double tolerance = 625.0 * std::ldexp( 1.0, -24 );
	report( worst <= tolerance && worstSum <= tolerance, quiet,
	        "footprint: %d half-planes, R 0.7 to 10.8 texels: the taps inside against the chord fraction, worst %.2e; the "
	        "taps' sum off 1 by %.2e (allowed %.1e, float weights)",
	        cases, worst, worstSum, tolerance );
}

void runOffline()
{
	runNames();
	runExact( false, false );
	runCutter( false, false );
	runBand( 0, false );
	runFootprint( false, false );
	//Their negative controls.
	struct Control
	{
		const char* what;
		std::function< void() > check;
	};
	const std::vector< Control > controls = {
		{ "a city-block reference distance: --exact", [] { runExact( true, true ); } },
		{ "a flood blind to the pieces: --cutter", [] { runCutter( true, true ); } },
		{ "no 4-connectivity floor: --band", [] { runBand( bridge::kPerturbThinBand, true ); } },
		{ "a band W wide each side: --band", [] { runBand( bridge::kPerturbWideBand, true ); } },
		{ "a Gaussian footprint: --footprint", [] { runFootprint( true, true ); } },
	};
	for( const Control& c : controls )
	{
		const int failed = failuresOf( c.check );
		report( failed > 0, false, "negative: %-50s %d check%s failed (must be > 0)", c.what, failed, failed == 1 ? "" : "s" );
	}
}

//---------------------------------------------------------------------------
// --bench: the defaults on the blobs and letters, the cost per frame, and
// the bridging's share.
//---------------------------------------------------------------------------
Image benchScene( int width, int height )
{
	Image blobs   = fixtures::smoothImage( fixtures::kBlobs, width, height );
	Image letters = fixtures::smoothImage( fixtures::kLetters, width, height );
	//Dark wherever either is: tone 0.1 there, 0.95 elsewhere.
	for( size_t i = 0; i < blobs.rgba.size(); i += 4 )
		for( int c = 0; c < 3; ++c )
			blobs.rgba[ i + c ] = 0.1f + 0.85f * blobs.rgba[ i + c ] * letters.rgba[ i + c ];
	return blobs;
}

int runBench( const std::vector< std::string >& settings, int frames, bool fourK )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	std::vector< Size > sizes = { { "1280x720 ", 1280, 720 }, { "1920x1080", 1920, 1080 } };
	if( fourK )
		sizes.push_back( { "3840x2160", 3840, 2160 } );
	std::printf( "the defaults on the bench scene (blobs through letters), %d frames, best of three runs, after a warm-up, "
	             "glFinish both sides\n\n",
	             frames );
	std::printf( "resolution  lattice    ms/frame   detect  flood   cutter  paint   passes  islands  bridges  label ms  grids flooded\n" );
	for( const Size& size : sizes )
	{
		Session s;
		s.floatOutput = false;
		for( const std::string& setting : settings )
		{
			std::string error;
			applySetting( s.plugin, setting, error );
		}
		if( !s.begin( size.width, size.height ) )
			return 1;
		s.upload( benchScene( size.width, size.height ) );
		for( int f = 0; f < 10; ++f )
			s.render();
		glFinish();
		double best = 1e9;
		for( int run = 0; run < 3; ++run )
		{
			const auto start = std::chrono::steady_clock::now();
			for( int f = 0; f < frames; ++f )
				s.render();
			glFinish();
			best = std::min( best, std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() / frames );
		}
		s.plugin.SetTimingForTest( true );
		Stencil::Timing t{ 1e9, 1e9, 1e9, 1e9, 0 };
		for( int f = 0; f < 10; ++f )
		{
			s.render();
			const Stencil::Timing& now = s.plugin.TimingForTest();
			t.detect = std::min( t.detect, now.detect );
			t.flood  = std::min( t.flood, now.flood );
			t.cutter = std::min( t.cutter, now.cutter );
			t.paint  = std::min( t.paint, now.paint );
			t.passes = now.passes;
		}
		int islands = 0, bridges = 0;
		double flooded = 0.0, grids = 0.0;
		for( int layer = 1; layer <= s.plugin.LayersForTest(); ++layer )
		{
			const bridge::Result& r = s.plugin.CutForTest( layer );
			islands += r.islands;
			bridges += static_cast< int >( r.bridges.size() );
			flooded += static_cast< double >( r.regionCells );
			grids += static_cast< double >( s.plugin.GridForTest( layer ).cells.size() );
		}
		//The labelling alone: union-find over the first layer's grid, best
		//of twenty.
		double labelBest = 1e9;
		{
			const bridge::Grid& g = s.plugin.GridForTest( 1 );
			std::vector< uint32_t > labels;
			uint32_t anchor = 0;
			for( int run = 0; run < 20; ++run )
			{
				const auto start = std::chrono::steady_clock::now();
				bridge::Label( g, labels, anchor );
				labelBest = std::min( labelBest, std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count() );
			}
		}
		std::printf( "%s   %4dx%-4d  %7.3f   %6.3f  %6.3f  %6.3f  %6.3f  %4d    %5d    %5d     %6.3f    %5.2f\n", size.name,
		             s.plugin.LatticeWidthForTest(), s.plugin.LatticeHeightForTest(), best, t.detect, t.flood, t.cutter,
		             t.paint, t.passes, islands, bridges, labelBest, grids > 0.0 ? flooded / grids : 0.0 );
		s.end();
	}
	std::printf( "\ndetect = detect + smooth + the tone's readback; flood = every flood pass of every layer, with its label\n"
	             "upload and seed readback (GPU, and the stall); cutter = labelling, choosing and cutting on the CPU;\n"
	             "paint = spray, creep, settle and the composite; label = one union-find labelling of one layer's grid\n"
	             "on the CPU (part of cutter); grids flooded = cells flooded over every pass, in whole grids a layer\n"
	             "(1 would be one full flood per layer). The stage times are the best of ten frames each.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	const std::pair< const char*, std::string > files[] = {
		{ "vertex.vert", shaders::Vertex() },   { "detect.frag", shaders::Detect() },
		{ "blur.frag", shaders::Blur() },       { "seed.frag", shaders::Seed() },
		{ "flood.frag", shaders::Flood() },     { "spray.frag", shaders::Spray() },
		{ "settle.frag", shaders::Settle() },   { "composite.frag", shaders::Composite() },
	};
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Parameter Name value' per line, the fleet's
// format. A STANDARD parameter ramps linearly between cues. An option, a
// boolean and an integer STEP: they hold the last cue at or before the
// frame, because there is nothing between Brick and Concrete to ramp
// through. An event fires on its cue frame only.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame, unsigned int type )
{
	if( track.empty() )
		return 0.0f;
	if( type == FF_TYPE_EVENT )
	{
		for( const auto& cue : track )
			if( cue.first == frame )
				return cue.second;
		return 0.0f;
	}
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			if( type != FF_TYPE_STANDARD )
				return frame == b.first ? b.second : a.second;
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"sntest -- render and measure SW Stencil\n"
		"\n"
		"  --out PATH          render --source through the plugin (default /tmp/stencil.png)\n"
		"  --source NAME       letters, nested, islands, blobs, offset, square, diamond, edge, ramp,\n"
		"                      bench (the default: blobs through letters)\n"
		"  --size WxH          raster (default 1280x720)\n"
		"  --frames N          frames to render before reading back (default 1)\n"
		"  --set \"Name=V\"      set a parameter by its display name (element index for options,\n"
		"                      the count for Layers). Repeatable.\n"
		"  --list              every parameter, its kind, default and range\n"
		"\n"
		"  checks that render, at --size (each also on SNTEST_RENDERER=software):\n"
		"  --islands           no island of sheet floats, labelled from the output\n"
		"  --shortest          every bridge is the shortest gap, within a texel, against brute force\n"
		"  --width             bridges have the stated width, and are 4-connected\n"
		"  --overspray         the edge profile is the cone's chord fraction; width 0.808 R\n"
		"  --layers            n layers give n + 1 plateaus, darkest ink first\n"
		"  --stability         bridges translate with the picture, ties and all\n"
		"  --churn             how often bridges change on a slow sub-pixel pan, kept and not\n"
		"  --resize            last frame's bridges survive a resize\n"
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks verbosely against a perturbed model (bits in Bridge.h, Stencil.h)\n"
		"\n"
		"  checks that need no GL:\n"
		"  --offline           --names --exact --cutter --band --footprint and their negative controls\n"
		"  --names --exact --cutter --band --footprint   one at a time\n"
		"  --allow-no-gl       a machine with no GL context SKIPS the GL checks, loudly\n"
		"\n"
		"  --bench             ms/frame at 720p and 1080p, and the bridging's share; --bench-4k adds 4K\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Parameter Name value'\n"
		"  --help\n" );
}

Image sourceImage( const std::string& name, int width, int height, bool& ok )
{
	ok = true;
	const std::map< std::string, int > named = {
		{ "letters", fixtures::kLetters }, { "nested", fixtures::kNested },   { "islands", fixtures::kIslandsInIslands },
		{ "blobs", fixtures::kBlobs },     { "offset", fixtures::kOffsetRing }, { "square", fixtures::kSquareRing },
		{ "diamond", fixtures::kDiamond }, { "edge", fixtures::kEdge },         { "ramp", fixtures::kRamp },
	};
	if( name == "bench" )
		return benchScene( width, height );
	const auto found = named.find( name );
	if( found == named.end() )
	{
		ok = false;
		return Image();
	}
	return found->second == fixtures::kRamp ? fixtures::latticeImage( fixtures::kRamp, width, height )
	                                        : fixtures::smoothImage( found->second, width, height );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/stencil.png";
	std::string scriptPath;
	std::string dumpDir;
	std::string source = "bench";
	int width      = 1280;
	int height     = 720;
	int frames     = 1;
	int failRender = -1;
	int perturb    = 0;
	bool wantList  = false;
	bool wantBench = false;
	bool bench4k   = false;
	bool wantPipe  = false;
	bool allowNoGL = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	const std::set< std::string > rendered = { "--islands", "--shortest", "--width", "--overspray", "--layers",
		                                       "--stability", "--churn", "--resize", "--negative" };
	const std::set< std::string > offline  = { "--names", "--exact", "--cutter", "--band", "--footprint", "--offline" };

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--source" && hasNext )
			source = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--fail-render-at" && hasNext )
			failRender = std::atoi( argv[ ++i ] );//test hook: verify.sh proves --pipe exits 1 on a failed render
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--bench-4k" )
			wantBench = bench4k = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--allow-no-gl" )
			allowNoGL = true;
		else if( rendered.count( argument ) || offline.count( argument ) )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 )
	{
		std::fprintf( stderr, "width, height and frames must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed: answered before a context is made, so it works in CI.
		Stencil plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	if( !checks.empty() )
	{
		bool needGL = false;
		for( const std::string& check : checks )
		{
			if( check == "--offline" )
				runOffline();
			else if( check == "--names" )
				runNames();
			else if( check == "--exact" )
				runExact( false, false );
			else if( check == "--cutter" )
				runCutter( false, false );
			else if( check == "--band" )
				runBand( perturb, false );
			else if( check == "--footprint" )
				runFootprint( false, false );
			else
				needGL = true;
		}
		if( needGL )
		{
			CGLContextObj context = createContext();
			if( context == nullptr && allowNoGL )
				std::printf( "   SKIP   could not create an OpenGL 4.1 core context, accelerated or software.\n"
				             "          The rendering checks and their negative controls were NOT run.\n" );
			else if( context == nullptr )
			{
				std::printf( "   FAILED could not create an OpenGL 4.1 core context\n" );
				++g_failures;
			}
			else
			{
				for( const std::string& check : checks )
				{
					if( check == "--islands" )
						runIslands( width, height, perturb, false );
					else if( check == "--shortest" )
						runShortest( width, height, perturb, false );
					else if( check == "--width" )
						runWidth( width, height, perturb, false );
					else if( check == "--overspray" )
						runOverspray( width, height, perturb, false );
					else if( check == "--layers" )
						runLayers( width, height, perturb, false );
					else if( check == "--stability" )
						runStability( width, height, perturb, false );
					else if( check == "--churn" )
						runChurn( width, height, perturb, false );
					else if( check == "--resize" )
						runResize( width, height, perturb, false );
					else if( check == "--negative" )
						runNegative( width, height );
				}
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
			}
		}
		std::printf( "%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	if( wantBench )
		return finish( runBench( settings, frames < 20 ? 30 : frames, bench4k ) );

	Session session;
	session.floatOutput = false;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}
	session.plugin.SetPerturbForTest( perturb );

	if( wantPipe )
	{
		//Everything but the video goes to stderr: one stray byte in stdout is
		//a torn frame for the rest of the reel.
		struct Automation
		{
			unsigned int index;
			unsigned int type;
			Track track;
		};
		std::vector< Automation > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation.push_back( { static_cast< unsigned int >( index ), session.plugin.GetParamType( static_cast< unsigned int >( index ) ), entry.second } );
			}
		}

		//A closed stdout must be a failed write we can see, not a SIGPIPE
		//that kills the process with 141 before it can say so.
		std::signal( SIGPIPE, SIG_IGN );

		if( !session.begin( width, height ) )
			return finish( 1 );

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame is the end of the stream, never a frame.
			if( got < frame.size() )
			{
				if( got > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes, %dx%d): dropped\n", got, frame.size(), width, height );
				break;
			}

			//Through the plugin's own setter, so a cue moves what a slider
			//would, and an event is a press.
			for( const Automation& a : automation )
				session.plugin.SetFloatParameter( a.index, valueAt( a.track, index, a.type ) );

			session.uploadTopFirst( frame );
			const bool ok = index != failRender && session.render();
			if( !ok )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const std::vector< unsigned char > out = session.readBackTopFirst();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}
		session.end();
		return finish( status );
	}

	bool ok = false;
	const Image image = sourceImage( source, width, height, ok );
	if( !ok )
	{
		std::fprintf( stderr, "unknown --source %s\n", source.c_str() );
		return finish( 2 );
	}
	if( !session.begin( width, height ) )
		return finish( 1 );
	for( int frame = 0; frame < frames; ++frame )
		if( !session.render( image ) )
			return finish( 1 );
	const std::vector< unsigned char > pixels = session.readBackTopFirst();
	session.end();
	if( !writePng( outPath, width, height, pixels ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frame%s)\n", outPath.c_str(), width, height, frames, frames == 1 ? "" : "s" );
	return finish( 0 );
}
