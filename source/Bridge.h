#pragma once

#include <cstdint>
#include <functional>
#include <vector>

/**
	The cutter's half of the stencil: find the floating pieces and bridge
	them. No GL in here; the one thing it cannot do quickly on a CPU -- the
	distance transform -- is handed in as a callback (`Flood`), which the
	plugin answers with a jump flood on the GPU and the harness can answer
	with anything it likes.

	**The grid** is the stencil on its lattice with a ring of one cell all
	round it, the MARGIN. A real stencil is cut from a sheet bigger than the
	picture, so the sheet goes on past the frame's edge: the margin is sheet,
	and a piece of sheet is held when it is 4-connected to the margin.

	**Connectivity.** Sheet is 4-connected and hole 8-connected, the dual
	pair. Two pieces of sheet that meet only at a corner are held together by
	a point of zero width, which a knife cuts through: they are two pieces.

	**One pass** labels the sheet (union-find over the cells), asks the flood
	for every cell's nearest sheet cell of ANOTHER piece, and gives every
	floating piece A the bridge from the cell a of A that minimises
	|a - second(a)| -- the shortest gap from A to any other piece -- as a band
	`width` wide. Ties go to the lowest (y, x) of a, then of b: a total order
	on cells that a translation of the picture preserves, so the same shape
	gets the same bridge wherever it is on the frame.

	**Passes.** A piece may bridge to another floating piece, and then the two
	still float together. Every floating piece either commits a bridge to a
	different piece or has already been joined by one this pass, so after a
	pass no floating piece is a single one of the pieces it started with and
	the number of floating pieces at least halves: F islands need at most
	floor(log2 F) + 1 passes. That bound is the spec's "depth of nesting" made
	honest -- see AGENTS.md.

	**Keeping last frame's bridges.** On a moving clip the shortest gap along a
	long, even gap moves about from frame to frame by less than a cell, and a
	bridge that jumps along a letter every frame is flicker. A bridge from the
	previous frame is kept (re-snapped to the best cell within `keepRadius` of
	where it started) as long as it is no more than `keepSlack` cells longer
	than the shortest; past that, the shortest wins.
*/
namespace stencil::bridge
{

enum Cell : uint8_t
{
	kHole   = 0,///< cut away: paint goes through
	kSheet  = 1,///< the stencil sheet
	kBridge = 2 ///< sheet the cutter left as a bridge (was hole)
};

inline bool IsSheet( uint8_t c )
{
	return c != kHole;
}

/// The stencil on its lattice plus the margin: (w + 2) x (h + 2) cells, row
/// 0 at the bottom (GL's orientation), the margin cells always sheet.
struct Grid
{
	int width  = 0;///< including the margin
	int height = 0;
	std::vector< uint8_t > cells;

	uint8_t& at( int x, int y )
	{
		return cells[ static_cast< size_t >( y ) * width + x ];
	}
	uint8_t at( int x, int y ) const
	{
		return cells[ static_cast< size_t >( y ) * width + x ];
	}
	bool margin( int x, int y ) const
	{
		return x == 0 || y == 0 || x == width - 1 || y == height - 1;
	}
};

/// A committed bridge, in grid cells (margin included), a on the island.
struct Bridge
{
	int ax = 0, ay = 0, bx = 0, by = 0;
	int pass = 0;          ///< 1-based
	uint32_t island = 0;   ///< the island's label in that pass (its root + 1)
	double length = 0.0;   ///< |a - b|, cells
	bool kept = false;     ///< re-snapped from a previous frame's bridge
};

/// Sentinel for "no seed" in the flood's output.
constexpr uint32_t kNone = 0xffffffffu;

/// A cell's position as the flood reports it: x | y << 16.
inline uint32_t Pack( int x, int y )
{
	return static_cast< uint32_t >( x ) | ( static_cast< uint32_t >( y ) << 16 );
}

/// A rectangle of cells, [x0, x1) x [y0, y1).
struct Region
{
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

/**
	The distance transform the bridging needs. Given `labels` (0 for hole, a
	piece's label otherwise) on a `width` x `height` grid, write for every
	cell the position of the nearest sheet cell whose label differs from
	that cell's own nearest -- for a sheet cell, the nearest cell of another
	piece -- one value a cell, packed x | y << 16 (Pack), or kNone. Returns
	false on failure.

	Only the cells of `region` need be written, and only seeds inside it
	considered: after the first pass the cutter knows, for every piece still
	floating, a bridge it could have (from the last flood), so the shortest
	it will get lies within that length of the piece, and a jump flood's
	relays between a cell and its seed lie inside the box the two span. The
	cutter asks for the box round every floating piece grown by that length,
	and nothing outside it is read.
*/
using Flood = std::function< bool( const std::vector< uint32_t >& labels, int width, int height, const Region& region,
                                   std::vector< uint32_t >& second ) >;

struct Settings
{
	double width         = 2.0;///< bridge width, cells
	double minIslandArea = 0.0;///< islands of fewer cells than this are dropped
	double keepSlack     = 1.0;///< a kept bridge may be this many cells longer
	int keepRadius       = 2;  ///< cells a kept bridge may slide per frame
	int maxPasses        = 32;
	int perturb          = 0;  ///< Perturb bits, test hooks
};

/// Negative-control hooks, always 0 in the plugin.
enum Perturb : int
{
	kPerturbNoBridges      = 1 << 0,///< find the islands, bridge none
	kPerturbFixedDirection = 1 << 1,///< bridge straight up from the island's top, not the shortest
	kPerturbTieCentre      = 1 << 2,///< break ties toward the frame's centre (not translation-stable)
	kPerturbNoHistory      = 1 << 3,///< ignore last frame's bridges
	kPerturbWideBand       = 1 << 4,///< the band's half width is `width`, not width / 2
	kPerturbThinBand       = 1 << 5,///< no 4-connectivity floor on a thin diagonal band
};

struct Result
{
	int passes       = 0;///< flood passes run
	int islands      = 0;///< floating pieces before bridging (after the drop)
	int dropped      = 0;///< islands under minIslandArea, dropped
	int floatingLeft = 0;///< floating pieces after the last pass (0 unless it gave up)
	std::vector< Bridge > bridges;
	double cpuMillis   = 0.0;///< labelling, choosing and cutting
	double floodMillis = 0.0;///< inside the Flood callback
	int64_t regionCells = 0; ///< cells flooded, summed over the passes
	/// When recording: the grid and its labels at the start of every pass.
	std::vector< std::vector< uint8_t > > snapshots;
};

/**
	Drop the small islands and bridge the rest, in place. `history` is the
	previous frame's bridges, in this grid's cells. Returns what it did.
*/
Result Cut( Grid& grid, const Settings& settings, const std::vector< Bridge >& history, const Flood& flood,
            bool record = false );

/// The cells a bridge from a to b of `width` cells covers: those whose centre
/// is within width / 2 of the segment's line and projects onto the segment,
/// with the band never thinner than |n.x| + |n.y| (n its unit normal), which
/// is what makes a digital band 4-connected at any angle.
void Band( int ax, int ay, int bx, int by, double width, int perturb, int gridWidth, int gridHeight,
           std::vector< std::pair< int, int > >& out );

/// Label the sheet 4-connected: every sheet cell's piece as root + 1, 0 for
/// hole; `anchor` gets the margin's label. Union-find with path halving.
void Label( const Grid& grid, std::vector< uint32_t >& labels, uint32_t& anchor );

} // namespace stencil::bridge
