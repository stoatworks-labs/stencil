#include "Bridge.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <tuple>

namespace stencil::bridge
{
namespace
{
double millisSince( std::chrono::steady_clock::time_point start )
{
	return std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count();
}

/// Union-find over cell indices, path halving, union by index (the smaller
/// root wins, so a piece's root is its lowest cell in raster order and the
/// labels do not depend on the order the unions happened in).
struct Forest
{
	std::vector< int32_t > parent;

	void reset( size_t n )
	{
		parent.resize( n );
		for( size_t i = 0; i < n; ++i )
			parent[ i ] = static_cast< int32_t >( i );
	}
	int32_t find( int32_t i )
	{
		while( parent[ i ] != i )
		{
			parent[ i ] = parent[ parent[ i ] ];
			i           = parent[ i ];
		}
		return i;
	}
	void unite( int32_t a, int32_t b )
	{
		a = find( a );
		b = find( b );
		if( a == b )
			return;
		if( a < b )
			parent[ b ] = a;
		else
			parent[ a ] = b;
	}
};

void build( const Grid& grid, Forest& forest )
{
	forest.reset( grid.cells.size() );
	for( int y = 0; y < grid.height; ++y )
		for( int x = 0; x < grid.width; ++x )
		{
			if( !IsSheet( grid.at( x, y ) ) )
				continue;
			const int32_t i = y * grid.width + x;
			if( x > 0 && IsSheet( grid.at( x - 1, y ) ) )
				forest.unite( i, i - 1 );
			if( y > 0 && IsSheet( grid.at( x, y - 1 ) ) )
				forest.unite( i, i - grid.width );
		}
}

/// A candidate bridge's order: shortest, then the lowest a in raster order,
/// then the lowest b. `tie` is 0 unless a perturbation puts something first.
struct Key
{
	int64_t length2 = std::numeric_limits< int64_t >::max();
	int64_t tie     = 0;
	int ay = 0, ax = 0, by = 0, bx = 0;

	bool operator<( const Key& o ) const
	{
		return std::tie( length2, tie, ay, ax, by, bx ) < std::tie( o.length2, o.tie, o.ay, o.ax, o.by, o.bx );
	}
	bool valid() const
	{
		return length2 != std::numeric_limits< int64_t >::max();
	}
};

Key keyFor( int ax, int ay, int bx, int by, const Grid& grid, int perturb )
{
	Key k;
	const int64_t dx = bx - ax, dy = by - ay;
	k.length2        = dx * dx + dy * dy;
	k.ax             = ax;
	k.ay             = ay;
	k.bx             = bx;
	k.by             = by;
	if( perturb & kPerturbTieCentre )
	{
		//Twice the offset from the centre, so it is an integer on either
		//parity of grid.
		const int64_t cx = 2 * ax - ( grid.width - 1 ), cy = 2 * ay - ( grid.height - 1 );
		k.tie            = cx * cx + cy * cy;
	}
	return k;
}
} // namespace

void Band( int ax, int ay, int bx, int by, double width, int perturb, int gridWidth, int gridHeight,
           std::vector< std::pair< int, int > >& out )
{
	out.clear();
	const double dx = bx - ax, dy = by - ay;
	const double length = std::sqrt( dx * dx + dy * dy );
	if( length <= 0.0 )
		return;
	const double ux = dx / length, uy = dy / length;//along
	const double nx = -uy, ny = ux;                  //across
	//An arithmetic line of thickness |nx| + |ny| is the thinnest that is
	//4-connected (a step along x moves the across-coordinate by nx, along y
	//by ny, so a band that wide always has a next cell to step to).
	double full = std::max( 0.0, width );
	if( !( perturb & kPerturbThinBand ) )
		full = std::max( full, std::fabs( nx ) + std::fabs( ny ) );
	const double half = ( perturb & kPerturbWideBand ) ? full : 0.5 * full;
	const double eps  = 1e-9;

	const int x0 = std::max( 0, static_cast< int >( std::floor( std::min( ax, bx ) - half - 1.0 ) ) );
	const int x1 = std::min( gridWidth - 1, static_cast< int >( std::ceil( std::max( ax, bx ) + half + 1.0 ) ) );
	const int y0 = std::max( 0, static_cast< int >( std::floor( std::min( ay, by ) - half - 1.0 ) ) );
	const int y1 = std::min( gridHeight - 1, static_cast< int >( std::ceil( std::max( ay, by ) + half + 1.0 ) ) );
	for( int y = y0; y <= y1; ++y )
		for( int x = x0; x <= x1; ++x )
		{
			const double px = x - ax, py = y - ay;
			const double t  = px * ux + py * uy;
			const double s  = px * nx + py * ny;
			if( t >= -eps && t <= length + eps && std::fabs( s ) <= half + eps )
				out.emplace_back( x, y );
		}
}

void Label( const Grid& grid, std::vector< uint32_t >& labels, uint32_t& anchor )
{
	Forest forest;
	build( grid, forest );
	labels.assign( grid.cells.size(), 0u );
	for( size_t i = 0; i < grid.cells.size(); ++i )
		if( IsSheet( grid.cells[ i ] ) )
			labels[ i ] = static_cast< uint32_t >( forest.find( static_cast< int32_t >( i ) ) ) + 1u;
	anchor = static_cast< uint32_t >( forest.find( 0 ) ) + 1u;
}

Result Cut( Grid& grid, const Settings& settings, const std::vector< Bridge >& history, const Flood& flood, bool record )
{
	Result result;
	const auto start   = std::chrono::steady_clock::now();
	const int w        = grid.width;
	const int h        = grid.height;
	const size_t cells = grid.cells.size();
	if( w < 3 || h < 3 || cells != static_cast< size_t >( w ) * h )
		return result;

	//The margin is sheet, whatever the caller left there.
	for( int x = 0; x < w; ++x )
		grid.at( x, 0 ) = grid.at( x, h - 1 ) = kSheet;
	for( int y = 0; y < h; ++y )
		grid.at( 0, y ) = grid.at( w - 1, y ) = kSheet;

	Forest forest;
	build( grid, forest );

	//--- drop the specks -----------------------------------------------------
	{
		std::vector< int32_t > size( cells, 0 );
		for( size_t i = 0; i < cells; ++i )
			if( IsSheet( grid.cells[ i ] ) )
				++size[ static_cast< size_t >( forest.find( static_cast< int32_t >( i ) ) ) ];
		const int32_t anchor = forest.find( 0 );
		bool any             = false;
		for( size_t i = 0; i < cells; ++i )
		{
			if( static_cast< int32_t >( i ) != forest.find( static_cast< int32_t >( i ) ) || size[ i ] == 0
			    || static_cast< int32_t >( i ) == anchor )
				continue;
			if( static_cast< double >( size[ i ] ) < settings.minIslandArea )
			{
				++result.dropped;
				size[ i ] = -1;//marked
				any       = true;
			}
			else
				++result.islands;
		}
		if( any )
		{
			for( size_t i = 0; i < cells; ++i )
				if( IsSheet( grid.cells[ i ] ) && size[ static_cast< size_t >( forest.find( static_cast< int32_t >( i ) ) ) ] < 0 )
					grid.cells[ i ] = kHole;
			build( grid, forest );
		}
	}

	std::vector< uint32_t > labels( cells );
	std::vector< uint16_t > second;
	std::vector< std::pair< int, int > > band;
	//Floating pieces are numbered densely each pass, so the per-piece arrays
	//are as long as there are pieces, not cells.
	std::vector< int32_t > dense( cells, -1 );
	std::vector< int32_t > roots;
	std::vector< Key > best, kept;
	std::vector< int64_t > keptMoved;

	for( int pass = 1; pass <= settings.maxPasses; ++pass )
	{
		const int32_t anchor = forest.find( 0 );
		for( int32_t r : roots )
			dense[ static_cast< size_t >( r ) ] = -1;
		roots.clear();
		for( size_t i = 0; i < cells; ++i )
		{
			if( IsSheet( grid.cells[ i ] ) )
			{
				const int32_t r = forest.find( static_cast< int32_t >( i ) );
				labels[ i ]     = static_cast< uint32_t >( r ) + 1u;
				if( r != anchor && dense[ static_cast< size_t >( r ) ] < 0 )
				{
					dense[ static_cast< size_t >( r ) ] = static_cast< int32_t >( roots.size() );
					roots.push_back( r );
				}
			}
			else
				labels[ i ] = 0u;
		}
		if( roots.empty() || ( settings.perturb & kPerturbNoBridges ) )
			break;

		if( record )
			result.snapshots.push_back( grid.cells );
		result.passes = pass;

		//The floating piece a cell belongs to, or -1.
		auto pieceOf = [ & ]( size_t i ) -> int32_t {
			return labels[ i ] ? dense[ labels[ i ] - 1u ] : -1;
		};

		//--- the candidates --------------------------------------------------
		best.assign( roots.size(), Key{} );
		kept.assign( roots.size(), Key{} );
		keptMoved.assign( roots.size(), 0 );

		if( settings.perturb & kPerturbFixedDirection )
		{
			//The negative control: straight up from each island's top cell
			//(highest y, then lowest x) to the first sheet of another piece.
			std::vector< int > topY( roots.size(), -1 ), topX( roots.size(), 0 );
			for( int y = 0; y < h; ++y )
				for( int x = 0; x < w; ++x )
				{
					const int32_t d = pieceOf( static_cast< size_t >( y ) * w + x );
					if( d >= 0 && y > topY[ d ] )
					{
						topY[ d ] = y;
						topX[ d ] = x;
					}
				}
			for( size_t d = 0; d < roots.size(); ++d )
			{
				const int x = topX[ d ];
				for( int y = topY[ d ] + 1; y < h; ++y )
				{
					const uint32_t l = labels[ static_cast< size_t >( y ) * w + x ];
					if( l != 0 && l != static_cast< uint32_t >( roots[ d ] ) + 1u )
					{
						best[ d ] = keyFor( x, topY[ d ], x, y, grid, 0 );
						break;
					}
				}
			}
		}
		else
		{
			const auto floodStart = std::chrono::steady_clock::now();
			const bool ok         = flood( labels, w, h, second );
			result.floodMillis += millisSince( floodStart );
			if( !ok || second.size() != cells * 2 )
			{
				result.floatingLeft = -1;
				break;
			}

			for( int y = 1; y < h - 1; ++y )
				for( int x = 1; x < w - 1; ++x )
				{
					const size_t i  = static_cast< size_t >( y ) * w + x;
					const int32_t d = pieceOf( i );
					if( d < 0 )
						continue;
					const uint16_t sx = second[ 2 * i ], sy = second[ 2 * i + 1 ];
					if( sx == kNone || sy == kNone )
						continue;
					const Key k = keyFor( x, y, sx, sy, grid, settings.perturb );
					if( k < best[ static_cast< size_t >( d ) ] )
						best[ static_cast< size_t >( d ) ] = k;
				}

			//Last frame's bridges. Within keepRadius of where each started,
			//the cell of the same piece nearest to that start whose bridge
			//is no more than keepSlack longer than the shortest: a kept
			//bridge stays where it was, cell for cell, for as long as it is
			//good enough, rather than sliding to whichever near-tie is
			//shortest this frame.
			//Both ends are looked at, because either piece may be the one
			//that bridges this frame.
			if( !( settings.perturb & kPerturbNoHistory ) )
				for( const Bridge& old : history )
					for( int end = 0; end < 2; ++end )
					{
						const int ox = end == 0 ? old.ax : old.bx, oy = end == 0 ? old.ay : old.by;
						const int rad = settings.keepRadius;
						for( int y = std::max( 1, oy - rad ); y <= std::min( h - 2, oy + rad ); ++y )
							for( int x = std::max( 1, ox - rad ); x <= std::min( w - 2, ox + rad ); ++x )
							{
								const size_t i  = static_cast< size_t >( y ) * w + x;
								const int32_t d = pieceOf( i );
								if( d < 0 || !best[ static_cast< size_t >( d ) ].valid() )
									continue;
								const uint16_t sx = second[ 2 * i ], sy = second[ 2 * i + 1 ];
								if( sx == kNone || sy == kNone )
									continue;
								const Key k = keyFor( x, y, sx, sy, grid, settings.perturb );
								if( std::sqrt( static_cast< double >( k.length2 ) )
								    > std::sqrt( static_cast< double >( best[ static_cast< size_t >( d ) ].length2 ) ) + settings.keepSlack + 1e-9 )
									continue;
								//Nearest the old end first; then the usual order.
								const int64_t mx = x - ox, my = y - oy;
								const int64_t moved = mx * mx + my * my;
								Key& incumbent      = kept[ static_cast< size_t >( d ) ];
								if( !incumbent.valid() || moved < keptMoved[ static_cast< size_t >( d ) ]
								    || ( moved == keptMoved[ static_cast< size_t >( d ) ] && k < incumbent ) )
								{
									incumbent                               = k;
									keptMoved[ static_cast< size_t >( d ) ] = moved;
								}
							}
					}
		}

		//--- choose, in order, and cut -----------------------------------------
		struct Choice
		{
			Key key;
			uint32_t island;
			bool kept;
		};
		std::vector< Choice > choices;
		for( size_t d = 0; d < roots.size(); ++d )
		{
			if( !best[ d ].valid() )
				continue;
			Choice c{ best[ d ], static_cast< uint32_t >( roots[ d ] ) + 1u, false };
			if( kept[ d ].valid() )
			{
				c.key  = kept[ d ];
				c.kept = true;
			}
			choices.push_back( c );
		}
		//Kept bridges first, so the piece on the far side of a kept bridge
		//does not get in first with a bridge of its own; then shortest first.
		std::sort( choices.begin(), choices.end(), []( const Choice& a, const Choice& b ) {
			if( a.kept != b.kept )
				return a.kept;
			return a.key < b.key;
		} );

		for( const Choice& c : choices )
		{
			const int32_t ia = c.key.ay * w + c.key.ax;
			const int32_t ib = c.key.by * w + c.key.bx;
			//Already joined this pass: the other side's bridge did it.
			if( forest.find( ia ) == forest.find( ib ) )
				continue;
			Band( c.key.ax, c.key.ay, c.key.bx, c.key.by, settings.width, settings.perturb, w, h, band );
			for( const auto& cell : band )
			{
				uint8_t& v = grid.at( cell.first, cell.second );
				if( v == kHole )
					v = kBridge;
			}
			for( const auto& cell : band )
			{
				const int x     = cell.first, y = cell.second;
				const int32_t i = y * w + x;
				if( x > 0 && IsSheet( grid.at( x - 1, y ) ) )
					forest.unite( i, i - 1 );
				if( x < w - 1 && IsSheet( grid.at( x + 1, y ) ) )
					forest.unite( i, i + 1 );
				if( y > 0 && IsSheet( grid.at( x, y - 1 ) ) )
					forest.unite( i, i - w );
				if( y < h - 1 && IsSheet( grid.at( x, y + 1 ) ) )
					forest.unite( i, i + w );
			}
			Bridge b;
			b.ax     = c.key.ax;
			b.ay     = c.key.ay;
			b.bx     = c.key.bx;
			b.by     = c.key.by;
			b.pass   = pass;
			b.island = c.island;
			b.length = std::sqrt( static_cast< double >( c.key.length2 ) );
			b.kept   = c.kept;
			result.bridges.push_back( b );
		}
	}

	//--- what is left floating (nothing, unless the passes ran out) ------------
	{
		const int32_t anchor = forest.find( 0 );
		std::vector< uint8_t > seen( cells, 0 );
		for( size_t i = 0; i < cells; ++i )
			if( IsSheet( grid.cells[ i ] ) )
			{
				const int32_t r = forest.find( static_cast< int32_t >( i ) );
				if( r != anchor && !seen[ static_cast< size_t >( r ) ] )
				{
					seen[ static_cast< size_t >( r ) ] = 1;
					if( result.floatingLeft >= 0 )
						++result.floatingLeft;
				}
			}
	}
	result.cpuMillis = millisSince( start ) - result.floodMillis;
	return result;
}

} // namespace stencil::bridge
