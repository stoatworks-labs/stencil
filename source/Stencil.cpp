#include "Stencil.h"

#include "Controls.h"
#include "Diag.h"
#include "Palette.h"
#include "Shaders.h"
#include "Spray.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace stencil;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Stencil >,// Create method
	"SN01",                  // Plugin unique ID of maximum length 4.
	"SW Stencil",            // Plugin name
	2,                       // API major version number
	1,                       // API minor version number
	0,                       // Plugin major version number
	1,                       // Plugin minor version number
	FF_EFFECT,               // Plugin type
	"Spray paint through a cut stencil.\n\nThe clip's tone is cut into one to four layers. A stencil cannot hold a floating piece, so every island of sheet -- the middle of an O, a highlight inside a shadow -- is tied to the rest by the shortest bridge a distance transform finds. Then the paint is sprayed through the holes from a distance, and overspray creeps under the edges.\n\nWhat falls out: the gap in the O, split letters, segmented shadows, a soft halo that grows with the nozzle's distance and under lifted bridges, and drips under heavy paint.",// Plugin description
	"Stencil FFGL effect"    // About
);

namespace
{
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

void bindTexture( int unit, GLuint texture )
{
	glActiveTexture( GL_TEXTURE0 + static_cast< GLenum >( unit ) );
	glBindTexture( GL_TEXTURE_2D, texture );
}

double millisSince( std::chrono::steady_clock::time_point start )
{
	return std::chrono::duration< double, std::milli >( std::chrono::steady_clock::now() - start ).count();
}

/// The Gaussian's taps each side for a sigma, capped.
int tapsFor( float sigma )
{
	return std::min( 48, static_cast< int >( std::ceil( 3.0f * sigma ) ) );
}

/// Sigma of the Gaussian whose edge profile has the disc's 25-75% width:
/// the disc's is 0.80794 R, a Gaussian's 2 x 0.67449 sigma.
constexpr double kGaussianForDisc = 0.80794 / ( 2.0 * 0.67449 );
} // namespace

int Stencil::LatticeScale( int height )
{
	return std::max( 1, ( height + kLatticeLines - 1 ) / kLatticeLines );
}

//---------------------------------------------------------------------------
Stencil::Stencil()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//Nothing here moves with time: the stencil is a function of the picture
	//and of last frame's bridges, and the drips are static.
	SetTimeSupported( false );

	//---------------------------------------------------------------------
	// Defaults, chosen on Resolume's demo clips: two layers of the Mono cans
	// on concrete, cut at a half tone with a little smoothing, bridges half
	// a percent of the frame high, specks under 1.2% dropped, sprayed from a
	// middling distance with a little lift and a few drips.
	//---------------------------------------------------------------------
	params[ PT_LAYERS ]       = 2.0f;
	params[ PT_THRESHOLD ]    = 0.5f;
	params[ PT_INVERT ]       = 0.0f;
	params[ PT_SMOOTH ]       = 0.3f;
	params[ PT_BRIDGE_WIDTH ] = controls::BridgeWidthParam( 0.005f );
	params[ PT_MIN_ISLAND ]   = controls::MinIslandParam( 0.012f );

	params[ PT_DISTANCE ] = controls::DistanceParam( 0.003f );
	params[ PT_PRESSURE ] = 0.56f;
	params[ PT_LIFT ]     = 0.3f;
	params[ PT_DRIPS ]    = 0.4f;

	params[ PT_PALETTE ]       = static_cast< float >( controls::kMono );
	params[ PT_LAYER_COLOURS ] = static_cast< float >( controls::kFromPalette );
	params[ PT_WALL ]          = static_cast< float >( controls::kWallConcrete );
	params[ PT_MIX ]           = 1.0f;

	auto declareOptions = [ this ]( unsigned int id, const char* name, int count, const char* ( *nameAt )( int ) ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), nameAt( i ), static_cast< float >( i ) );
	};

	//An integer holds the real count: the SDK's clamp of a default into
	//0..1 is guarded for FF_TYPE_INTEGER.
	SetParamInfo( PT_LAYERS, "Layers", FF_TYPE_INTEGER, params[ PT_LAYERS ] );
	SetParamRange( PT_LAYERS, static_cast< float >( controls::kMinLayers ), static_cast< float >( controls::kMaxLayers ) );
	SetParamInfof( PT_THRESHOLD, "Threshold", FF_TYPE_STANDARD );
	SetParamInfo( PT_INVERT, "Invert", FF_TYPE_BOOLEAN, params[ PT_INVERT ] > 0.5f );
	SetParamInfof( PT_SMOOTH, "Smooth", FF_TYPE_STANDARD );
	SetParamInfof( PT_BRIDGE_WIDTH, "Bridge Width", FF_TYPE_STANDARD );
	SetParamInfof( PT_MIN_ISLAND, "Min Island", FF_TYPE_STANDARD );

	SetParamInfof( PT_DISTANCE, "Distance", FF_TYPE_STANDARD );
	SetParamInfof( PT_PRESSURE, "Pressure", FF_TYPE_STANDARD );
	SetParamInfof( PT_LIFT, "Lift", FF_TYPE_STANDARD );
	SetParamInfof( PT_DRIPS, "Drips", FF_TYPE_STANDARD );

	declareOptions( PT_PALETTE, "Palette", controls::kPaletteCount, controls::PaletteName );
	declareOptions( PT_LAYER_COLOURS, "Layer Colours", controls::kLayerColoursCount, controls::LayerColoursName );
	declareOptions( PT_WALL, "Wall", controls::kWallCount, controls::WallName );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	for( FFUInt32 i = PT_LAYERS; i <= PT_MIN_ISLAND; ++i )
		SetParamGroup( i, "Cut" );
	for( FFUInt32 i = PT_DISTANCE; i <= PT_DRIPS; ++i )
		SetParamGroup( i, "Spray" );
	for( FFUInt32 i = PT_PALETTE; i <= PT_MIX; ++i )
		SetParamGroup( i, "Paint" );

	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Stencil effect" );

	diag::init();
}

//---------------------------------------------------------------------------
bool Stencil::compileAll()
{
	const std::string vertex = shaders::Vertex();
	struct
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	} const stages[] = {
		{ &detectShader, shaders::Detect(), "detect" },
		{ &blurShader, shaders::Blur(), "blur" },
		{ &seedShader, shaders::Seed(), "seed" },
		{ &floodShader, shaders::Flood(), "flood" },
		{ &sprayShader, shaders::Spray(), "spray" },
		{ &settleShader, shaders::Settle(), "settle" },
		{ &compositeShader, shaders::Composite(), "composite" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( vertex, stage.fragment ) )
			continue;
		//Returning FF_FAIL is invisible to the operator: the effect simply
		//does nothing. These two lines are the only record of which pass.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Stencil: shader failed to compile" );
		return false;
	}
	return true;
}

FFResult Stencil::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer="
	            + glStringOrUnknown( GL_RENDERER ) + " version=" + glStringOrUnknown( GL_VERSION ) );

	if( !compileAll() || !quad.Initialise() )
	{
		diag::error( "InitGL failed" );
		DeInitGL();
		return FF_FAIL;
	}
	for( auto& h : history )
		h.clear();
	lastWidth = lastHeight = 0;
	tapRadius              = -1.0;

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
// The flood the cutter asks for: upload the labels, seed, jump-flood with
// two seeds a texel, read the seconds back.
//---------------------------------------------------------------------------
bool Stencil::flood( const std::vector< uint32_t >& pieceLabels, int width, int height, std::vector< uint16_t >& second )
{
	if( labels.Width() != width || labels.Height() != height || !seeds[ 0 ].IsValid() )
		return false;
	labels.Upload( pieceLabels.data() );

	//1. seed
	seeds[ 0 ].BindForDrawing();
	glUseProgram( seedShader.GetGLID() );
	bindTexture( 0, labels.TextureID() );
	seedShader.Set( "Labels", 0 );
	quad.Draw();

	//2. flood: the step-1 pass first (the "1+" of 1+JFA, Rong & Tan 2006),
	//the halving sequence from under the longer side, then a finishing run
	//of halving steps from 1/128 of it (toolpath's: with only 2, 1 the thin
	//Voronoi wedges of a curved boundary leave seeds further out the larger
	//the raster).
	std::vector< int > steps;
	steps.push_back( 1 );
	int longest = 1;
	while( longest < std::max( width, height ) )
		longest *= 2;
	for( int step = longest / 2; step >= 1; step /= 2 )
		steps.push_back( step );
	for( int step = std::max( 2, longest / 128 ); step >= 1; step /= 2 )
		steps.push_back( step );

	glUseProgram( floodShader.GetGLID() );
	floodShader.Set( "Seeds", 0 );
	floodShader.Set( "Labels", 1 );
	glUniform2i( floodShader.FindUniform( "Size" ), width, height );
	bindTexture( 1, labels.TextureID() );
	int current = 0;
	for( int step : steps )
	{
		seeds[ 1 - current ].BindForDrawing();
		bindTexture( 0, seeds[ current ].TextureID() );
		floodShader.Set( "Step", step );
		quad.Draw();
		current = 1 - current;
	}

	//3. read back
	seedValues.resize( static_cast< size_t >( width ) * height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, seeds[ current ].FramebufferID() );
	glPixelStorei( GL_PACK_ALIGNMENT, 2 );
	glReadPixels( 0, 0, width, height, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, seedValues.data() );
	glPixelStorei( GL_PACK_ALIGNMENT, 4 );
	bindTexture( 1, 0 );
	bindTexture( 0, 0 );

	second.resize( static_cast< size_t >( width ) * height * 2 );
	for( size_t i = 0, n = static_cast< size_t >( width ) * height; i < n; ++i )
	{
		second[ 2 * i ]     = seedValues[ 4 * i + 2 ];
		second[ 2 * i + 1 ] = seedValues[ 4 * i + 3 ];
	}
	return true;
}

//---------------------------------------------------------------------------
FFResult Stencil::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];
	if( input.Width == 0 || input.Height == 0 )
		return FF_FAIL;

	const int width  = static_cast< int >( input.Width );
	const int height = static_cast< int >( input.Height );

	//ScopedFBOBinding restores the framebuffer binding and only that; the
	//host's viewport is read now and put back before the composite.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	const bool resized = lastWidth != 0 && ( lastWidth != width || lastHeight != height );
	lastWidth          = width;
	lastHeight         = height;
	if( resized && ( perturb & kPerturbResizeClears ) )
		for( auto& h : history )
			h.clear();

	//---------------------------------------------------------------------
	// What the controls say, in lattice texels.
	//---------------------------------------------------------------------
	const int k  = LatticeScale( height );
	const int lw = ( width + k - 1 ) / k;
	const int lh = ( height + k - 1 ) / k;
	const int gw = lw + 2, gh = lh + 2;
	const double texelsPerHeight = static_cast< double >( height ) / k;

	const int layers       = controls::LayerCount( params[ PT_LAYERS ] );
	const float threshold  = std::clamp( params[ PT_THRESHOLD ], 0.0f, 1.0f );
	const float smooth     = controls::SmoothSigmaHeights( params[ PT_SMOOTH ] ) * static_cast< float >( texelsPerHeight );
	const double bridgeW   = controls::BridgeWidthHeights( params[ PT_BRIDGE_WIDTH ] ) * texelsPerHeight;
	const double minSide   = controls::MinIslandSideHeights( params[ PT_MIN_ISLAND ] ) * texelsPerHeight;
	const double radius    = controls::FootprintRadiusHeights( params[ PT_DISTANCE ] ) * texelsPerHeight;
	const float pressure   = controls::PressureAmount( params[ PT_PRESSURE ] );
	const float liftAmount = controls::LiftAmount( params[ PT_LIFT ] );
	const float liftSigma  = controls::LiftSigmaHeights( params[ PT_LIFT ] ) * static_cast< float >( texelsPerHeight );
	const int dripReach    = static_cast< int >( std::lround( controls::DripLengthHeights( params[ PT_DRIPS ] ) * texelsPerHeight ) );
	const float dripChance = controls::DripChance( params[ PT_DRIPS ] );
	const int paletteIndex = controls::OptionIndex( params[ PT_PALETTE ], controls::kPaletteCount );
	const bool fromClip    = controls::OptionIndex( params[ PT_LAYER_COLOURS ], controls::kLayerColoursCount ) == controls::kFromClip;
	const int wall         = controls::OptionIndex( params[ PT_WALL ], controls::kWallCount );
	const bool useCreep    = liftAmount > 0.0f && liftSigma >= 0.3f;
	const bool gaussian    = ( perturb & kPerturbGaussianCone ) != 0;

	//---------------------------------------------------------------------
	// Every buffer, before anything is bound for this frame's drawing.
	//---------------------------------------------------------------------
	if( std::fabs( radius - tapRadius ) > 1e-9 || gaussian != tapGaussian || !taps.IsValid() )
	{
		const std::vector< spray::Tap > footprint
			= gaussian ? spray::Footprint( radius * kGaussianForDisc * 2.0, true ) : spray::Footprint( radius );
		std::vector< float > data;
		for( const spray::Tap& t : footprint )
		{
			data.push_back( t.dx );
			data.push_back( t.dy );
			data.push_back( t.weight );
			data.push_back( 0.0f );
		}
		tapCount = static_cast< int >( footprint.size() );
		if( !taps.Ensure( tapCount, 1, GL_RGBA32F, PassBuffer::Sampling::Nearest ) )
		{
			diag::error( "could not allocate the footprint's taps" );
			return FF_FAIL;
		}
		taps.Upload( data.data() );
		tapRadius   = radius;
		tapGaussian = gaussian;
	}

	const bool allocated = tone[ 0 ].Ensure( lw, lh, GL_RGBA16F, PassBuffer::Sampling::Nearest )
	                       && tone[ 1 ].Ensure( lw, lh, GL_RGBA16F, PassBuffer::Sampling::Nearest )
	                       && labels.Ensure( gw, gh, GL_R32UI, PassBuffer::Sampling::Nearest )
	                       && seeds[ 0 ].Ensure( gw, gh, GL_RGBA16UI, PassBuffer::Sampling::Nearest )
	                       && seeds[ 1 ].Ensure( gw, gh, GL_RGBA16UI, PassBuffer::Sampling::Nearest )
	                       && cut.Ensure( lw, lh, GL_RGBA8, PassBuffer::Sampling::Nearest )
	                       && sprayed.Ensure( lw, lh, GL_RGBA16F, PassBuffer::Sampling::Nearest )
	                       && creep[ 0 ].Ensure( lw, lh, GL_RGBA16F, PassBuffer::Sampling::Nearest )
	                       && creep[ 1 ].Ensure( lw, lh, GL_RGBA16F, PassBuffer::Sampling::Nearest )
	                       && coverage.Ensure( lw, lh, GL_RGBA16F, PassBuffer::Sampling::Linear );
	if( !allocated )
	{
		diag::error( "could not allocate the lattice buffers at " + std::to_string( lw ) + "x" + std::to_string( lh ) );
		return FF_FAIL;
	}
	latticeWidth  = lw;
	latticeHeight = lh;
	latticeScale  = k;
	layersNow     = layers;

	Timing t;
	auto stage = std::chrono::steady_clock::now();
	if( timing )
		glFinish();

	//---------------------------------------------------------------------
	// 1, 2. detect and smooth, on the lattice; read the tone back.
	//---------------------------------------------------------------------
	tone[ 0 ].BindForDrawing();
	glUseProgram( detectShader.GetGLID() );
	bindTexture( 0, input.Handle );
	detectShader.Set( "InputTexture", 0 );
	glUniform2i( detectShader.FindUniform( "InputSize" ), width, height );
	detectShader.Set( "Scale", k );
	detectShader.Set( "Invert", params[ PT_INVERT ] > 0.5f ? 1 : 0 );
	quad.Draw();

	if( smooth >= 0.3f )
	{
		glUseProgram( blurShader.GetGLID() );
		blurShader.Set( "Value", 0 );
		blurShader.Set( "Sigma", smooth );
		blurShader.Set( "Taps", tapsFor( smooth ) );
		blurShader.Set( "FromCut", 0 );
		for( int axis = 0; axis < 2; ++axis )
		{
			tone[ 1 - axis ].BindForDrawing();
			bindTexture( 0, tone[ axis ].TextureID() );
			glUniform2i( blurShader.FindUniform( "Axis" ), axis == 0 ? 1 : 0, axis == 0 ? 0 : 1 );
			quad.Draw();
		}
	}

	glBindFramebuffer( GL_FRAMEBUFFER, tone[ 0 ].FramebufferID() );
	toneValues.resize( static_cast< size_t >( lw ) * lh );
	glPixelStorei( GL_PACK_ALIGNMENT, 4 );
	glReadPixels( 0, 0, lw, lh, GL_RED, GL_FLOAT, toneValues.data() );
	if( fromClip )
	{
		colourValues.resize( static_cast< size_t >( lw ) * lh * 4 );
		glReadPixels( 0, 0, lw, lh, GL_RGBA, GL_FLOAT, colourValues.data() );
	}
	t.detect = millisSince( stage );

	//---------------------------------------------------------------------
	// 3, 4. the cutter, a layer at a time: threshold, drop the specks,
	// bridge the islands (the flood on the GPU, the rest on the CPU).
	//---------------------------------------------------------------------
	bridge::Settings settings;
	settings.width         = bridgeW;
	settings.minIslandArea = minSide * minSide;
	settings.keepSlack     = kKeepSlack;
	settings.keepRadius    = kKeepRadius;
	settings.perturb       = perturb & 0xff;
	const bridge::Flood gpu = [ this ]( const std::vector< uint32_t >& l, int w, int h, std::vector< uint16_t >& s ) {
		return flood( l, w, h, s );
	};

	cutValues.assign( static_cast< size_t >( lw ) * lh * 4, 0 );
	for( int layer = 1; layer <= layers; ++layer )
	{
		const float cutAt       = controls::LayerThreshold( threshold, layer, layers );
		bridge::Grid& grid      = grids[ layer - 1 ];
		grid.width              = gw;
		grid.height             = gh;
		grid.cells.assign( static_cast< size_t >( gw ) * gh, bridge::kSheet );
		for( int y = 0; y < lh; ++y )
			for( int x = 0; x < lw; ++x )
				if( toneValues[ static_cast< size_t >( y ) * lw + x ] < cutAt )
					grid.at( x + 1, y + 1 ) = bridge::kHole;

		//Last frame's bridges, from 0..1 of the lattice into this grid.
		std::vector< bridge::Bridge > kept;
		for( const Kept& old : history[ layer - 1 ] )
		{
			bridge::Bridge b;
			b.ax = static_cast< int >( std::floor( old.ax * lw ) ) + 1;
			b.ay = static_cast< int >( std::floor( old.ay * lh ) ) + 1;
			b.bx = static_cast< int >( std::floor( old.bx * lw ) ) + 1;
			b.by = static_cast< int >( std::floor( old.by * lh ) ) + 1;
			kept.push_back( b );
		}

		results[ layer - 1 ] = bridge::Cut( grid, settings, kept, gpu, record );
		const bridge::Result& r = results[ layer - 1 ];
		t.flood += r.floodMillis;
		t.cutter += r.cpuMillis;
		t.passes = std::max( t.passes, r.passes );

		history[ layer - 1 ].clear();
		for( const bridge::Bridge& b : r.bridges )
			history[ layer - 1 ].push_back( { ( b.ax - 1 + 0.5 ) / lw, ( b.ay - 1 + 0.5 ) / lh, ( b.bx - 1 + 0.5 ) / lw,
			                                   ( b.by - 1 + 0.5 ) / lh } );

		for( int y = 0; y < lh; ++y )
			for( int x = 0; x < lw; ++x )
			{
				const uint8_t c = grid.at( x + 1, y + 1 );
				cutValues[ ( static_cast< size_t >( y ) * lw + x ) * 4 + static_cast< size_t >( layer - 1 ) ]
					= c == bridge::kHole ? 255 : ( c == bridge::kBridge ? 128 : 0 );
			}
	}
	for( int layer = layers + 1; layer <= controls::kMaxLayers; ++layer )
	{
		results[ layer - 1 ] = bridge::Result{};
		history[ layer - 1 ].clear();
	}
	cut.Upload( cutValues.data() );

	//---------------------------------------------------------------------
	// The inks: the palette's, or the clip's own mean colour over each
	// layer's band of tone (lightest first).
	//---------------------------------------------------------------------
	for( int layer = 1; layer <= controls::kMaxLayers; ++layer )
	{
		const palette::Colour c = palette::Ink( paletteIndex, std::min( layer, layers ), layers );
		inks[ layer - 1 ][ 0 ]  = c.r;
		inks[ layer - 1 ][ 1 ]  = c.g;
		inks[ layer - 1 ][ 2 ]  = c.b;
	}
	if( fromClip )
	{
		double sum[ 4 ][ 3 ] = {};
		double count[ 4 ]    = {};
		for( size_t i = 0, n = static_cast< size_t >( lw ) * lh; i < n; ++i )
		{
			const float v = toneValues[ i ];
			//The darkest layer whose cut this tone is under.
			int band = 0;
			for( int layer = layers; layer >= 1; --layer )
				if( v < controls::LayerThreshold( threshold, layer, layers ) )
				{
					band = layer;
					break;
				}
			if( band == 0 )
				continue;
			for( int c = 0; c < 3; ++c )
				sum[ band - 1 ][ c ] += colourValues[ 4 * i + 1 + static_cast< size_t >( c ) ];
			count[ band - 1 ] += 1.0;
		}
		for( int layer = 1; layer <= layers; ++layer )
			if( count[ layer - 1 ] > 0.0 )
				for( int c = 0; c < 3; ++c )
					inks[ layer - 1 ][ c ] = static_cast< float >( sum[ layer - 1 ][ c ] / count[ layer - 1 ] );
	}

	stage = std::chrono::steady_clock::now();
	if( timing )
		glFinish();

	//---------------------------------------------------------------------
	// 5. spray through the holes.
	//---------------------------------------------------------------------
	sprayed.BindForDrawing();
	glUseProgram( sprayShader.GetGLID() );
	bindTexture( 0, cut.TextureID() );
	bindTexture( 1, taps.TextureID() );
	sprayShader.Set( "Cut", 0 );
	sprayShader.Set( "Taps", 1 );
	sprayShader.Set( "TapCount", tapCount );
	quad.Draw();

	//---------------------------------------------------------------------
	// 6. creep under lifted edges: a Gaussian of the holes.
	//---------------------------------------------------------------------
	if( useCreep )
	{
		glUseProgram( blurShader.GetGLID() );
		blurShader.Set( "Value", 0 );
		blurShader.Set( "Sigma", liftSigma );
		blurShader.Set( "Taps", tapsFor( liftSigma ) );

		creep[ 1 ].BindForDrawing();
		bindTexture( 0, cut.TextureID() );
		blurShader.Set( "FromCut", 1 );
		glUniform2i( blurShader.FindUniform( "Axis" ), 1, 0 );
		quad.Draw();

		creep[ 0 ].BindForDrawing();
		bindTexture( 0, creep[ 1 ].TextureID() );
		blurShader.Set( "FromCut", 0 );
		glUniform2i( blurShader.FindUniform( "Axis" ), 0, 1 );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 7. settle: what lands, and the drips.
	//---------------------------------------------------------------------
	coverage.BindForDrawing();
	glUseProgram( settleShader.GetGLID() );
	bindTexture( 0, cut.TextureID() );
	bindTexture( 1, sprayed.TextureID() );
	bindTexture( 2, creep[ 0 ].TextureID() );
	settleShader.Set( "Cut", 0 );
	settleShader.Set( "Sprayed", 1 );
	settleShader.Set( "Creep", 2 );
	settleShader.Set( "UseCreep", useCreep ? 1 : 0 );
	settleShader.Set( "Pressure", pressure );
	settleShader.Set( "LiftAmount", liftAmount );
	settleShader.Set( "BridgeLift", controls::kBridgeLift );
	settleShader.Set( "SheetLift", controls::kSheetLift );
	settleShader.Set( "Saturation", controls::kSaturation );
	settleShader.Set( "DripReach", dripReach );
	settleShader.Set( "DripChance", dripChance );
	settleShader.Set( "Layers", layers );
	quad.Draw();

	//---------------------------------------------------------------------
	// 8. the composite, straight to the host.
	//---------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
	glUseProgram( compositeShader.GetGLID() );
	bindTexture( 0, input.Handle );
	bindTexture( 1, coverage.TextureID() );
	bindTexture( 2, 0 );
	compositeShader.Set( "InputTexture", 0 );
	compositeShader.Set( "Coverage", 1 );
	compositeShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
	//The job's 0..1 on the lattice's: an odd raster's last texel overhangs
	//the frame.
	compositeShader.Set( "LatticeUV", static_cast< float >( width ) / static_cast< float >( k * lw ),
	                     static_cast< float >( height ) / static_cast< float >( k * lh ) );
	compositeShader.Set( "Layers", layers );
	glUniform3fv( compositeShader.FindUniform( "Ink" ), 4, &inks[ 0 ][ 0 ] );
	compositeShader.Set( "Order", ( perturb & kPerturbLightOverDark ) ? 1 : 0 );
	compositeShader.Set( "Wall", wall );
	compositeShader.Set( "PlainWall", palette::kPlainWall.r, palette::kPlainWall.g, palette::kPlainWall.b );
	compositeShader.Set( "OutSize", static_cast< float >( width ), static_cast< float >( height ) );
	compositeShader.Set( "MixAmount", std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) );
	quad.Draw();

	for( int unit = 2; unit >= 0; --unit )
		bindTexture( unit, 0 );
	glUseProgram( 0 );

	if( timing )
		glFinish();
	t.paint   = millisSince( stage );
	timingNow = t;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Stencil::DeInitGL()
{
	detectShader.FreeGLResources();
	blurShader.FreeGLResources();
	seedShader.FreeGLResources();
	floodShader.FreeGLResources();
	sprayShader.FreeGLResources();
	settleShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();
	for( PassBuffer* b : { &tone[ 0 ], &tone[ 1 ], &labels, &seeds[ 0 ], &seeds[ 1 ], &cut, &taps, &sprayed, &creep[ 0 ],
	                       &creep[ 1 ], &coverage } )
		b->Destroy();
	tapRadius = -1.0;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Stencil::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Stencil::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;
	return params[ index ];
}

char* Stencil::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Stencil::SetTextParameter( unsigned int index, const char* value )
{
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

//---------------------------------------------------------------------------
void Stencil::SetPerturbForTest( int bits )
{
	perturb = bits;
}

void Stencil::SetRecordForTest( bool on )
{
	record = on;
}

void Stencil::SetTimingForTest( bool on )
{
	timing = on;
}

bool Stencil::ReadCoverageForTest( std::vector< float >& out, int& width, int& height )
{
	width  = coverage.Width();
	height = coverage.Height();
	if( !coverage.IsValid() )
		return false;
	out.resize( static_cast< size_t >( width ) * height * 4 );
	GLint previous = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &previous );
	glBindTexture( GL_TEXTURE_2D, coverage.TextureID() );
	glPixelStorei( GL_PACK_ALIGNMENT, 4 );
	glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, out.data() );
	glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( previous ) );
	return true;
}
