#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace stencil::controls
{
namespace
{
float clamp01( float v )
{
	return std::clamp( v, 0.0f, 1.0f );
}
} // namespace

int LayerCount( float value )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), kMinLayers, kMaxLayers );
}

float LayerThreshold( float threshold, int layer, int layers )
{
	layers = std::clamp( layers, kMinLayers, kMaxLayers );
	layer  = std::clamp( layer, 1, layers );
	return clamp01( threshold ) * static_cast< float >( layers - layer + 1 ) / static_cast< float >( layers );
}

float SmoothSigmaHeights( float value )
{
	return 0.01f * clamp01( value );
}

float BridgeWidthHeights( float value )
{
	return 0.001f * std::pow( 20.0f, clamp01( value ) );
}

float BridgeWidthParam( float heights )
{
	return clamp01( std::log( std::max( heights, 0.001f ) / 0.001f ) / std::log( 20.0f ) );
}

float MinIslandSideHeights( float value )
{
	return 0.05f * clamp01( value );
}

float MinIslandParam( float heights )
{
	return clamp01( heights / 0.05f );
}

float FootprintRadiusHeights( float value )
{
	return 0.02f * clamp01( value );
}

float DistanceParam( float heights )
{
	return clamp01( heights / 0.02f );
}

float PressureAmount( float value )
{
	return 2.0f * clamp01( value );
}

float LiftSigmaHeights( float value )
{
	return 0.008f * clamp01( value );
}

float LiftAmount( float value )
{
	return clamp01( value );
}

float DripChance( float value )
{
	return 0.45f * clamp01( value );
}

float DripLengthHeights( float value )
{
	return 0.12f * clamp01( value );
}

const char* PaletteName( int index )
{
	static const char* const names[ kPaletteCount ] = { "Mono", "Street", "Sepia", "Pop", "Cool" };
	return names[ std::clamp( index, 0, kPaletteCount - 1 ) ];
}

const char* LayerColoursName( int index )
{
	static const char* const names[ kLayerColoursCount ] = { "Palette", "From Clip" };
	return names[ std::clamp( index, 0, kLayerColoursCount - 1 ) ];
}

const char* WallName( int index )
{
	static const char* const names[ kWallCount ] = { "Clip", "Brick", "Concrete", "Plain" };
	return names[ std::clamp( index, 0, kWallCount - 1 ) ];
}

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

} // namespace stencil::controls
