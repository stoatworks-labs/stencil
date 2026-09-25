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

/// Union-find with path halving, union by index (the smaller root wins).
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

/**
	Label the sheet by RUNS: each row's maximal stretches of sheet are the
	nodes, and a run is united with every run of the row below whose x range
	overlaps its own (4-connectivity: sharing a column, not a corner). There
	are tens of times fewer runs than cells, which is what makes this cheap
	enough to redo every pass. Runs are numbered in raster order and the
	smaller root wins, so a piece's root is its first run, and its label is
	the index of that run's first cell plus one: the lowest cell of the piece
	in raster order, whatever order the unions happened in.
*/
struct Runs
{
	struct Run
	{
		int32_t row, x0, x1;//x1 inclusive
	};
	std::vector< Run > runs;
	std::vector< int32_t > rowStart;//runs of row y are [rowStart[y], rowStart[y + 1])
	Forest forest;

	void build( const Grid& grid )
	{
		runs.clear();
		rowStart.assign( static_cast< size_t >( grid.height ) + 1, 0 );
		for( int y = 0; y < grid.height; ++y )
		{
			rowStart[ static_cast< size_t >( y ) ] = static_cast< int32_t >( runs.size() );
			const uint8_t* row = grid.cells.data() + static_cast< size_t >( y ) * grid.width;
			int x              = 0;
			while( x < grid.width )
			{
				while( x < grid.width && !IsSheet( row[ x ] ) )
					++x;
				if( x >= grid.width )
					break;
				const int x0 = x;
				while( x < grid.width && IsSheet( row[ x ] ) )
					++x;
				runs.push_back( { y, x0, x - 1 } );
			}
		}
		rowStart[ static_cast< size_t >( grid.height ) ] = static_cast< int32_t >( runs.size() );

		forest.reset( runs.size() );
		for( int y = 1; y < grid.height; ++y )
		{
			int32_t below     = rowStart[ static_cast< size_t >( y - 1 ) ];
			const int32_t end = rowStart[ static_cast< size_t >( y ) ];
			for( int32_t r = rowStart[ static_cast< size_t >( y ) ]; r < rowStart[ static_cast< size_t >( y ) + 1 ]; ++r )
			{
				//Skip the runs below that end before this one starts; unite
				//with every one that overlaps it.
				while( below < end && runs[ static_cast< size_t >( below ) ].x1 < runs[ static_cast< size_t >( r ) ].x0 )
					++below;
				for( int32_t b = below; b < end && runs[ static_cast< size_t >( b ) ].x0 <= runs[ static_cast< size_t >( r ) ].x1; ++b )
					forest.unite( r, b );
			}
		}
	}

	/// Every cell's label (0 for hole), and the margin's.
	void fill( const Grid& grid, std::vector< uint32_t >& labels, uint32_t& anchor )
	{
		labels.assign( grid.cells.size(), 0u );
		for( size_t r = 0; r < runs.size(); ++r )
		{
			const Run& root   = runs[ static_cast< size_t >( forest.find( static_cast< int32_t >( r ) ) ) ];
			const uint32_t id = static_cast< uint32_t >( root.row ) * static_cast< uint32_t >( grid.width ) + static_cast< uint32_t >( root.x0 ) + 1u;
			const Run& run    = runs[ r ];
			std::fill( labels.begin() + static_cast< long >( static_cast< size_t >( run.row ) * grid.width + run.x0 ),
			           labels.begin() + static_cast< long >( static_cast< size_t >( run.row ) * grid.width + run.x1 + 1 ), id );
		}
		//Cell 0 is a corner of the margin, always sheet, always the first run.
		anchor = labels.empty() ? 0u : labels[ 0 ];
	}
};

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
	Runs runs;
	runs.build( grid );
	runs.fill( grid, labels, anchor );
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

	std::vector< uint32_t > labels;
	uint32_t anchor = 0;
	Runs runs;
	auto relabel = [ & ]() {
		runs.build( grid );
		runs.fill( grid, labels, anchor );
	};
	relabel();

	//--- drop the specks -----------------------------------------------------
	{
		std::vector< int64_t > size( runs.runs.size(), 0 );
		for( size_t r = 0; r < runs.runs.size(); ++r )
			size[ static_cast< size_t >( runs.forest.find( static_cast< int32_t >( r ) ) ) ] += runs.runs[ r ].x1 - runs.runs[ r ].x0 + 1;
		const int32_t held = runs.forest.find( 0 );
		std::vector< uint8_t > drop( runs.runs.size(), 0 );
		bool any = false;
		for( size_t r = 0; r < runs.runs.size(); ++r )
		{
			if( runs.forest.find( static_cast< int32_t >( r ) ) != static_cast< int32_t >( r ) || static_cast< int32_t >( r ) == held )
				continue;
			if( static_cast< double >( size[ r ] ) < settings.minIslandArea )
			{
				++result.dropped;
				drop[ r ] = 1;
				any       = true;
			}
			else
				++result.islands;
		}
		if( any )
		{
			for( size_t r = 0; r < runs.runs.size(); ++r )
				if( drop[ static_cast< size_t >( runs.forest.find( static_cast< int32_t >( r ) ) ) ] )
				{
					const Runs::Run& run = runs.runs[ r ];
					std::fill( grid.cells.begin() + static_cast< long >( static_cast< size_t >( run.row ) * w + run.x0 ),
					           grid.cells.begin() + static_cast< long >( static_cast< size_t >( run.row ) * w + run.x1 + 1 ), kHole );
				}
			relabel();
		}
	}

	std::vector< uint32_t > second;
	std::vector< std::pair< int, int > > band;
	//Floating pieces are numbered densely each pass (by label - 1), so the
	//per-piece arrays are as long as there are pieces, not cells.
	std::vector< int32_t > dense( cells, -1 );
	std::vector< uint32_t > roots;//the floating pieces' labels
	std::vector< Key > best, kept;
	std::vector< int64_t > keptMoved;
	Forest joined;//this pass's merges, over dense ids and the margin

	for( int pass = 1; pass <= settings.maxPasses; ++pass )
	{
		if( pass > 1 )
			relabel();
		for( uint32_t l : roots )
			dense[ l - 1u ] = -1;
		roots.clear();
		for( size_t r = 0; r < runs.runs.size(); ++r )
		{
			if( runs.forest.find( static_cast< int32_t >( r ) ) != static_cast< int32_t >( r ) )
				continue;
			const Runs::Run& run = runs.runs[ r ];
			const uint32_t l     = static_cast< uint32_t >( run.row ) * static_cast< uint32_t >( w ) + static_cast< uint32_t >( run.x0 ) + 1u;
			if( l == anchor )
				continue;
			dense[ l - 1u ] = static_cast< int32_t >( roots.size() );
			roots.push_back( l );
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
					if( l != 0 && l != roots[ d ] )
					{
						best[ d ] = keyFor( x, topY[ d ], x, y, grid, 0 );
						break;
					}
				}
			}
		}
		else
		{
			//The region: the whole grid on the first pass; after it, the box
			//round every floating piece grown by the shortest bridge the last
			//flood already offers it to a piece it is not part of now. If any
			//piece has none, the whole grid again.
			Region region{ 0, 0, w, h };
			if( pass > 1 && second.size() == cells )
			{
				const size_t n = roots.size();
				std::vector< int > x0( n, w ), y0( n, h ), x1( n, -1 ), y1( n, -1 );
				std::vector< int64_t > reach( n, std::numeric_limits< int64_t >::max() );
				for( int y = 1; y < h - 1; ++y )
					for( int x = 1; x < w - 1; ++x )
					{
						const size_t i  = static_cast< size_t >( y ) * w + x;
						const int32_t d = pieceOf( i );
						if( d < 0 )
							continue;
						x0[ d ] = std::min( x0[ d ], x );
						y0[ d ] = std::min( y0[ d ], y );
						x1[ d ] = std::max( x1[ d ], x );
						y1[ d ] = std::max( y1[ d ], y );
						const uint32_t packed = second[ i ];
						if( packed == kNone )
							continue;
						const int sx = static_cast< int >( packed & 0xffffu ), sy = static_cast< int >( packed >> 16 );
						if( sx >= w || sy >= h )
							continue;
						const uint32_t other = labels[ static_cast< size_t >( sy ) * w + sx ];
						if( other == 0 || other == labels[ i ] )
							continue;
						const int64_t dx = sx - x, dy = sy - y;
						reach[ d ]       = std::min( reach[ d ], dx * dx + dy * dy );
					}
				bool bounded = n > 0;
				Region box{ w, h, 0, 0 };
				for( size_t d = 0; d < n && bounded; ++d )
				{
					if( reach[ d ] == std::numeric_limits< int64_t >::max() )
					{
						bounded = false;
						break;
					}
					const int grow = static_cast< int >( std::ceil( std::sqrt( static_cast< double >( reach[ d ] ) ) ) ) + 1;
					box.x0         = std::min( box.x0, std::max( 0, x0[ d ] - grow ) );
					box.y0         = std::min( box.y0, std::max( 0, y0[ d ] - grow ) );
					box.x1         = std::max( box.x1, std::min( w, x1[ d ] + grow + 1 ) );
					box.y1         = std::max( box.y1, std::min( h, y1[ d ] + grow + 1 ) );
				}
				if( bounded )
					region = box;
			}
			result.regionCells += static_cast< int64_t >( region.x1 - region.x0 ) * ( region.y1 - region.y0 );

			const auto floodStart = std::chrono::steady_clock::now();
			const bool ok         = flood( labels, w, h, region, second );
			result.floodMillis += millisSince( floodStart );
			if( !ok || second.size() != cells )
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
					const uint32_t packed = second[ i ];
					if( packed == kNone )
						continue;
					const int sx = static_cast< int >( packed & 0xffffu ), sy = static_cast< int >( packed >> 16 );
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
								if( x < region.x0 || y < region.y0 || x >= region.x1 || y >= region.y1 )
									continue;
								const uint32_t packed = second[ i ];
								if( packed == kNone )
									continue;
								const int sx = static_cast< int >( packed & 0xffffu ), sy = static_cast< int >( packed >> 16 );
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
			Choice c{ best[ d ], roots[ d ], false };
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

		const int32_t margin = static_cast< int32_t >( roots.size() );
		joined.reset( roots.size() + 1 );
		auto idOf = [ & ]( uint32_t l ) -> int32_t { return l == anchor ? margin : dense[ l - 1u ]; };
		for( const Choice& c : choices )
		{
			const uint32_t la = labels[ static_cast< size_t >( c.key.ay ) * w + c.key.ax ];
			const uint32_t lb = labels[ static_cast< size_t >( c.key.by ) * w + c.key.bx ];
			if( la == 0 || lb == 0 )
				continue;
			const int32_t ia = idOf( la ), ib = idOf( lb );
			//Already joined this pass: the other side's bridge did it.
			if( joined.find( ia ) == joined.find( ib ) )
				continue;
			joined.unite( ia, ib );
			Band( c.key.ax, c.key.ay, c.key.bx, c.key.by, settings.width, settings.perturb, w, h, band );
			for( const auto& cell : band )
			{
				const int x = cell.first, y = cell.second;
				uint8_t& v  = grid.at( x, y );
				if( v == kHole )
					v = kBridge;
				//Whatever piece the band touches, it joins.
				const int nx[ 5 ] = { x, x - 1, x + 1, x, x };
				const int ny[ 5 ] = { y, y, y, y - 1, y + 1 };
				for( int n = 0; n < 5; ++n )
				{
					if( nx[ n ] < 0 || ny[ n ] < 0 || nx[ n ] >= w || ny[ n ] >= h )
						continue;
					const uint32_t l = labels[ static_cast< size_t >( ny[ n ] ) * w + nx[ n ] ];
					if( l != 0 )
						joined.unite( ia, idOf( l ) );
				}
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
	relabel();
	if( result.floatingLeft >= 0 )
		for( size_t r = 0; r < runs.runs.size(); ++r )
			if( runs.forest.find( static_cast< int32_t >( r ) ) == static_cast< int32_t >( r ) && runs.forest.find( 0 ) != static_cast< int32_t >( r ) )
				++result.floatingLeft;
	result.cpuMillis = millisSince( start ) - result.floodMillis;
	return result;
}

} // namespace stencil::bridge
