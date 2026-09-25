#pragma once
/**
	The harness's own geometry, written separately from the plugin's so that
	a check compares two different computations and not one formula typed
	twice:

	  - labelling by breadth-first flood fill, not union-find;
	  - the exact Euclidean distance transform of Felzenszwalb and
	    Huttenlocher, not a jump flood -- and `--exact` checks it against
	    brute force;
	  - the closest pair between two pieces by brute force over their
	    boundary cells (the nearest cell of a set to any point outside it is
	    always a boundary cell: from any other, a step toward the point in its
	    larger coordinate is strictly nearer).

	A "mask" here is a grid of sheet (1) and hole (0), row 0 at the bottom,
	with the plugin's convention that everything past the edge is sheet: the
	margin. Sheet is 4-connected.
*/

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

namespace reference
{

constexpr double kInf = 1e20;

/// Pieces of sheet, 4-connected, by flood fill. `piece[i]` is -1 for hole;
/// `held[p]` is true when piece p touches the edge of the mask (and so the
/// margin beyond it).
struct Pieces
{
	int width = 0, height = 0;
	std::vector< int > piece;
	std::vector< uint8_t > held;
	std::vector< int > size;

	int floating() const
	{
		int n = 0;
		for( uint8_t h : held )
			n += h ? 0 : 1;
		return n;
	}
};

inline Pieces label( const std::vector< uint8_t >& sheet, int width, int height )
{
	Pieces out;
	out.width  = width;
	out.height = height;
	out.piece.assign( sheet.size(), -1 );
	std::deque< int > queue;
	for( int start = 0; start < static_cast< int >( sheet.size() ); ++start )
	{
		if( !sheet[ static_cast< size_t >( start ) ] || out.piece[ static_cast< size_t >( start ) ] >= 0 )
			continue;
		const int id = static_cast< int >( out.held.size() );
		out.held.push_back( 0 );
		out.size.push_back( 0 );
		out.piece[ static_cast< size_t >( start ) ] = id;
		queue.push_back( start );
		while( !queue.empty() )
		{
			const int i = queue.front();
			queue.pop_front();
			++out.size[ static_cast< size_t >( id ) ];
			const int x = i % width, y = i / width;
			if( x == 0 || y == 0 || x == width - 1 || y == height - 1 )
				out.held[ static_cast< size_t >( id ) ] = 1;
			const int nx[ 4 ] = { x - 1, x + 1, x, x };
			const int ny[ 4 ] = { y, y, y - 1, y + 1 };
			for( int n = 0; n < 4; ++n )
			{
				if( nx[ n ] < 0 || ny[ n ] < 0 || nx[ n ] >= width || ny[ n ] >= height )
					continue;
				const int j = ny[ n ] * width + nx[ n ];
				if( sheet[ static_cast< size_t >( j ) ] && out.piece[ static_cast< size_t >( j ) ] < 0 )
				{
					out.piece[ static_cast< size_t >( j ) ] = id;
					queue.push_back( j );
				}
			}
		}
	}
	return out;
}

/// The deepest island: pieces of sheet and hole alternate outward to the
/// margin (sheet 4-, hole 8-connected); an island's depth is how many
/// pieces of sheet, itself included, lie between it and the margin. The
/// margin's own piece and everything held by it is depth 0.
inline int nestingDepth( const std::vector< uint8_t >& sheet, int width, int height )
{
	//Label holes 8-connected too, then walk outward from the margin through
	//the adjacency of pieces, sheet and hole alternating.
	const Pieces s = label( sheet, width, height );
	std::vector< int > hole( sheet.size(), -1 );
	int holes = 0;
	std::deque< int > queue;
	for( int start = 0; start < static_cast< int >( sheet.size() ); ++start )
	{
		if( sheet[ static_cast< size_t >( start ) ] || hole[ static_cast< size_t >( start ) ] >= 0 )
			continue;
		hole[ static_cast< size_t >( start ) ] = holes;
		queue.push_back( start );
		while( !queue.empty() )
		{
			const int i = queue.front();
			queue.pop_front();
			const int x = i % width, y = i / width;
			for( int dy = -1; dy <= 1; ++dy )
				for( int dx = -1; dx <= 1; ++dx )
				{
					const int xx = x + dx, yy = y + dy;
					if( xx < 0 || yy < 0 || xx >= width || yy >= height )
						continue;
					const int j = yy * width + xx;
					if( !sheet[ static_cast< size_t >( j ) ] && hole[ static_cast< size_t >( j ) ] < 0 )
					{
						hole[ static_cast< size_t >( j ) ] = holes;
						queue.push_back( j );
					}
				}
		}
		++holes;
	}
	//Nodes: sheet pieces 0..S-1, holes S..S+H-1, and the margin S+H.
	const int S = static_cast< int >( s.held.size() ), H = holes, margin = S + H;
	std::vector< std::vector< int > > adjacent( static_cast< size_t >( margin + 1 ) );
	auto link = [ & ]( int a, int b ) {
		adjacent[ static_cast< size_t >( a ) ].push_back( b );
		adjacent[ static_cast< size_t >( b ) ].push_back( a );
	};
	for( int p = 0; p < S; ++p )
		if( s.held[ static_cast< size_t >( p ) ] )
			link( p, margin );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const int i = y * width + x;
			if( !sheet[ static_cast< size_t >( i ) ] )
				continue;
			const int nx[ 4 ] = { x - 1, x + 1, x, x };
			const int ny[ 4 ] = { y, y, y - 1, y + 1 };
			for( int n = 0; n < 4; ++n )
			{
				if( nx[ n ] < 0 || ny[ n ] < 0 || nx[ n ] >= width || ny[ n ] >= height )
					continue;
				const int j = ny[ n ] * width + nx[ n ];
				if( !sheet[ static_cast< size_t >( j ) ] )
					link( s.piece[ static_cast< size_t >( i ) ], S + hole[ static_cast< size_t >( j ) ] );
			}
		}
	std::vector< int > steps( static_cast< size_t >( margin + 1 ), -1 );
	steps[ static_cast< size_t >( margin ) ] = 0;
	queue.clear();
	queue.push_back( margin );
	while( !queue.empty() )
	{
		const int n = queue.front();
		queue.pop_front();
		for( int m : adjacent[ static_cast< size_t >( n ) ] )
			if( steps[ static_cast< size_t >( m ) ] < 0 )
			{
				steps[ static_cast< size_t >( m ) ] = steps[ static_cast< size_t >( n ) ] + 1;
				queue.push_back( m );
			}
	}
	int deepest = 0;
	for( int p = 0; p < S; ++p )
		deepest = std::max( deepest, steps[ static_cast< size_t >( p ) ] / 2 );
	return deepest;
}

//---------------------------------------------------------------------------
// The exact Euclidean distance transform: Felzenszwalb & Huttenlocher's
// lower envelope of parabolas, one dimension at a time. Exact in integers --
// squared distances between cell centres. `--exact` checks it against brute
// force.
//---------------------------------------------------------------------------
inline void edt1d( const std::vector< double >& f, std::vector< double >& d, int n )
{
	std::vector< int > v( static_cast< size_t >( n ) );
	std::vector< double > z( static_cast< size_t >( n ) + 1 );
	int k     = 0;
	int first = -1;
	for( int q = 0; q < n; ++q )
		if( f[ static_cast< size_t >( q ) ] < kInf )
		{
			first = q;
			break;
		}
	if( first < 0 )
	{
		for( int q = 0; q < n; ++q )
			d[ static_cast< size_t >( q ) ] = kInf;
		return;
	}
	v[ 0 ] = first;
	z[ 0 ] = -kInf;
	z[ 1 ] = kInf;
	for( int q = first + 1; q < n; ++q )
	{
		if( !( f[ static_cast< size_t >( q ) ] < kInf ) )
			continue;
		double s;
		while( true )
		{
			const int p = v[ static_cast< size_t >( k ) ];
			s = ( ( f[ static_cast< size_t >( q ) ] + double( q ) * q ) - ( f[ static_cast< size_t >( p ) ] + double( p ) * p ) )
			    / ( 2.0 * q - 2.0 * p );
			if( s <= z[ static_cast< size_t >( k ) ] && k > 0 )
				--k;
			else
				break;
		}
		++k;
		v[ static_cast< size_t >( k ) ]     = q;
		z[ static_cast< size_t >( k ) ]     = s;
		z[ static_cast< size_t >( k ) + 1 ] = kInf;
	}
	k = 0;
	for( int q = 0; q < n; ++q )
	{
		while( z[ static_cast< size_t >( k ) + 1 ] < q )
			++k;
		const int p                     = v[ static_cast< size_t >( k ) ];
		d[ static_cast< size_t >( q ) ] = double( q - p ) * ( q - p ) + f[ static_cast< size_t >( p ) ];
	}
}

/// Squared distance from every cell centre to the nearest cell centre where
/// `seed` is true; kInf where there is none. `cityBlock` is the negative
/// control: a two-pass chamfer, not Euclidean.
inline std::vector< double > squaredEdt( const std::vector< uint8_t >& seed, int width, int height, bool cityBlock = false )
{
	std::vector< double > grid( static_cast< size_t >( width ) * height );
	for( size_t i = 0; i < grid.size(); ++i )
		grid[ i ] = seed[ i ] ? 0.0 : kInf;
	if( cityBlock )
	{
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width; ++x )
			{
				double& g = grid[ static_cast< size_t >( y ) * width + x ];
				if( x > 0 )
					g = std::min( g, grid[ static_cast< size_t >( y ) * width + x - 1 ] + 1.0 );
				if( y > 0 )
					g = std::min( g, grid[ static_cast< size_t >( y - 1 ) * width + x ] + 1.0 );
			}
		for( int y = height - 1; y >= 0; --y )
			for( int x = width - 1; x >= 0; --x )
			{
				double& g = grid[ static_cast< size_t >( y ) * width + x ];
				if( x + 1 < width )
					g = std::min( g, grid[ static_cast< size_t >( y ) * width + x + 1 ] + 1.0 );
				if( y + 1 < height )
					g = std::min( g, grid[ static_cast< size_t >( y + 1 ) * width + x ] + 1.0 );
			}
		for( double& g : grid )
			g = g < kInf ? g * g : kInf;
		return grid;
	}
	std::vector< double > f( static_cast< size_t >( std::max( width, height ) ) ), d( f.size() );
	for( int x = 0; x < width; ++x )
	{
		for( int y = 0; y < height; ++y )
			f[ static_cast< size_t >( y ) ] = grid[ static_cast< size_t >( y ) * width + x ];
		edt1d( f, d, height );
		for( int y = 0; y < height; ++y )
			grid[ static_cast< size_t >( y ) * width + x ] = d[ static_cast< size_t >( y ) ];
	}
	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
			f[ static_cast< size_t >( x ) ] = grid[ static_cast< size_t >( y ) * width + x ];
		edt1d( f, d, width );
		for( int x = 0; x < width; ++x )
			grid[ static_cast< size_t >( y ) * width + x ] = d[ static_cast< size_t >( x ) ] >= kInf * 0.5 ? kInf : d[ static_cast< size_t >( x ) ];
	}
	return grid;
}

/// The cells of piece `p` with a 4-neighbour outside it (or on the mask's
/// edge), and the same for every OTHER sheet cell.
inline void boundaries( const Pieces& pieces, const std::vector< uint8_t >& sheet, int p, std::vector< int >& own,
                        std::vector< int >& others )
{
	own.clear();
	others.clear();
	const int w = pieces.width, h = pieces.height;
	for( int y = 0; y < h; ++y )
		for( int x = 0; x < w; ++x )
		{
			const int i = y * w + x;
			if( !sheet[ static_cast< size_t >( i ) ] )
				continue;
			const int mine = pieces.piece[ static_cast< size_t >( i ) ];
			bool edge      = x == 0 || y == 0 || x == w - 1 || y == h - 1;
			const int nx[ 4 ] = { x - 1, x + 1, x, x };
			const int ny[ 4 ] = { y, y, y - 1, y + 1 };
			for( int n = 0; n < 4 && !edge; ++n )
				edge = pieces.piece[ static_cast< size_t >( ny[ n ] * w + nx[ n ] ) ] != mine;
			if( !edge )
				continue;
			( mine == p ? own : others ).push_back( i );
		}
}

/// The shortest distance between cell centres of piece p and of any other
/// sheet, the margin included: brute force over boundary cells when that is
/// under `budget` pairs, else the exact EDT of the other sheet read at p's
/// cells. `brute` says which it used.
inline double shortestGap( const Pieces& pieces, const std::vector< uint8_t >& sheet, int p, bool& brute,
                           double budget = 6e7 )
{
	std::vector< int > own, others;
	boundaries( pieces, sheet, p, own, others );
	const int w = pieces.width;
	if( static_cast< double >( own.size() ) * static_cast< double >( others.size() ) <= budget )
	{
		brute       = true;
		double best = kInf;
		for( int a : own )
		{
			const int ax = a % w, ay = a / w;
			for( int b : others )
			{
				const double dx = b % w - ax, dy = b / w - ay;
				best            = std::min( best, dx * dx + dy * dy );
			}
		}
		return std::sqrt( best );
	}
	brute = false;
	std::vector< uint8_t > other( sheet.size(), 0 );
	for( size_t i = 0; i < sheet.size(); ++i )
		other[ i ] = sheet[ i ] && pieces.piece[ i ] != p ? 1 : 0;
	const std::vector< double > d = squaredEdt( other, pieces.width, pieces.height );
	double best                   = kInf;
	for( int a : own )
		best = std::min( best, d[ static_cast< size_t >( a ) ] );
	return std::sqrt( best );
}

} // namespace reference
