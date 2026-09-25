#pragma once

#include "Bridge.h"
#include "PassBuffer.h"

#include <FFGLSDK.h>

#include "StoatworksAboutParams.h"

#include <string>
#include <vector>

/**
	Stencil -- spray paint through a cut stencil, as an FFGL effect.

	**The one idea.** A stencil is a sheet with holes cut in it, and paint
	goes through the holes. A stencil cannot hold a floating piece: the middle
	of an O would fall out when cut, so the cutter leaves BRIDGES, thin strips
	of sheet that tie every island to the rest. Then the paint is sprayed from
	a nozzle at a distance, and overspray creeps under the stencil's edges by
	an amount set by the cone and the sheet's lift off the wall.

	So the clip's tone is cut into Layers thresholds, each layer's floating
	pieces are found by labelling the sheet and bridged with the shortest
	strip a distance transform finds, and the paint density is the hole
	convolved with the nozzle's footprint. The gap in the O, the split
	letters, the segmented shadows, the soft halo and the drips all fall out.

	The passes are in Shaders.h; the cutter's half (labelling, choosing,
	cutting) in Bridge.h, with no GL in it. See AGENTS.md.
*/
class Stencil : public CFFGLPlugin
{
public:
	Stencil();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Cut
		PT_LAYERS,
		PT_THRESHOLD,
		PT_INVERT,
		PT_SMOOTH,
		PT_BRIDGE_WIDTH,
		PT_MIN_ISLAND,

		//Spray
		PT_DISTANCE,
		PT_PRESSURE,
		PT_LIFT,
		PT_DRIPS,

		//Paint
		PT_PALETTE,
		PT_LAYER_COLOURS,
		PT_WALL,
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// Negative-control hooks. Always 0 in the plugin. The low bits are
	/// bridge::Perturb's and go straight to the cutter.
	enum Perturb : int
	{
		kPerturbResizeClears  = 1 << 8, ///< a resize forgets last frame's bridges
		kPerturbGaussianCone  = 1 << 9, ///< a Gaussian footprint of the same quartile width, not the cone's disc
		kPerturbLightOverDark = 1 << 10,///< paint the layers darkest first
	};

	/// The stencil is cut on a lattice of LatticeScale( height ) job pixels a
	/// texel: ceil( height / kLatticeLines ), so at most kLatticeLines rows
	/// (180 at 320x180, 360 at 720p, 540 at 1080p and 4K). AGENTS.md: why.
	static constexpr int kLatticeLines = 540;
	static int LatticeScale( int height );

	/// A kept bridge may be this many lattice texels longer than the
	/// shortest, and slide this many texels a frame.
	static constexpr double kKeepSlack = 1.0;
	static constexpr int kKeepRadius   = 2;

	//--- test hooks (sntest) ---------------------------------------------------
	void SetPerturbForTest( int bits );
	/// Keep each pass's grid (bridge::Result::snapshots) for the checks.
	void SetRecordForTest( bool on );
	/// Time the stages with glFinish on both sides (for --bench).
	void SetTimingForTest( bool on );

	struct Timing
	{
		double detect = 0.0;///< detect, blur and the tone's readback
		double flood  = 0.0;///< the floods, uploads and readbacks, every pass, every layer
		double cutter = 0.0;///< the CPU: labelling, choosing, cutting
		double paint  = 0.0;///< spray, creep, settle and the composite
		int passes    = 0;  ///< the most passes any layer needed
	};
	const Timing& TimingForTest() const
	{
		return timingNow;
	}

	int LatticeWidthForTest() const
	{
		return latticeWidth;
	}
	int LatticeHeightForTest() const
	{
		return latticeHeight;
	}
	int LatticeScaleForTest() const
	{
		return latticeScale;
	}
	int LayersForTest() const
	{
		return layersNow;
	}
	/// Layer 1..n: what the cutter did this frame, and the grid it left.
	const stencil::bridge::Result& CutForTest( int layer ) const
	{
		return results[ static_cast< size_t >( layer - 1 ) ];
	}
	const stencil::bridge::Grid& GridForTest( int layer ) const
	{
		return grids[ static_cast< size_t >( layer - 1 ) ];
	}
	/// The paint that landed, lattice, RGBA (a layer a channel), rows bottom-up.
	bool ReadCoverageForTest( std::vector< float >& out, int& width, int& height );
	/// The ink layer 1..n was painted with this frame (straight RGB).
	const float* InkForTest( int layer ) const
	{
		return inks[ static_cast< size_t >( layer - 1 ) ];
	}

private:
	bool compileAll();
	bool flood( const std::vector< uint32_t >& labels, int width, int height, std::vector< uint16_t >& second );

	ffglex::FFGLShader detectShader;
	ffglex::FFGLShader blurShader;
	ffglex::FFGLShader seedShader;
	ffglex::FFGLShader floodShader;
	ffglex::FFGLShader sprayShader;
	ffglex::FFGLShader settleShader;
	ffglex::FFGLShader compositeShader;
	ffglex::FFGLScreenQuad quad;

	stencil::PassBuffer tone[ 2 ];  ///< tone and colour, and the blur's scratch; lattice
	stencil::PassBuffer labels;     ///< the pieces' labels, R32UI; grid
	stencil::PassBuffer seeds[ 2 ]; ///< the flood's ping-pong, RGBA16UI; grid
	stencil::PassBuffer cut;        ///< the cut stencil, RGBA8, a layer a channel; lattice
	stencil::PassBuffer taps;       ///< the footprint's taps, RGBA32F, N x 1
	stencil::PassBuffer sprayed;    ///< the spray through the holes, RGBA16F; lattice
	stencil::PassBuffer creep[ 2 ]; ///< paint under lifted edges, and its scratch; lattice
	stencil::PassBuffer coverage;   ///< what landed, drips and all, RGBA16F; lattice

	int latticeWidth  = 0;
	int latticeHeight = 0;
	int latticeScale  = 1;
	int layersNow     = 1;
	int lastWidth     = 0;
	int lastHeight    = 0;

	double tapRadius = -1.0;
	bool tapGaussian = false;
	int tapCount     = 0;

	std::vector< float > toneValues; ///< lattice, the tone (R)
	std::vector< float > colourValues;///< lattice RGBA, for From Clip
	std::vector< uint16_t > seedValues;
	std::vector< uint8_t > cutValues;///< lattice RGBA8

	stencil::bridge::Grid grids[ 4 ];
	stencil::bridge::Result results[ 4 ];
	/// Last frame's bridges per layer, a-ends only matter, in 0..1 of the
	/// lattice so a resize can carry them.
	struct Kept
	{
		double ax, ay, bx, by;
	};
	std::vector< Kept > history[ 4 ];
	float inks[ 4 ][ 3 ] = {};

	int perturb   = 0;
	bool record   = false;
	bool timing   = false;
	Timing timingNow;

	/// Zero-initialised: the About block's ids are never stored to, so
	/// without this GetFloatParameter hands the host whatever was on the
	/// stack for them.
	float params[ PT_COUNT ] = {};

	std::string aboutText;
};
