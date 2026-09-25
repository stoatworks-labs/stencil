#pragma once

/**
	Host parameters are 0..1; these are what they mean.

	`CFFGLPluginManager::SetParamInfo` clamps a STANDARD default into 0..1
	*before* returning, and `SetParamRange` can only be called afterwards, so
	a parameter declared in physical units cannot declare a default in them.
	Every slider here is therefore a plain 0..1 float and the conversions live
	in this one file, which the plugin and the harness both use. Every mapping
	a check needs to hit exactly has an inverse. `Layers` is the exception: an
	FF_TYPE_INTEGER holds the real count, 1 to 4, because the SDK's clamp is
	guarded for integers (graticule's finding).

	**Lengths are in frame heights, not pixels.** A bridge 0.5% of the frame
	high is the same bridge at 320x180 and at 3840x2160, so the look does not
	change with the composition's raster and every harness check means the
	same thing at every raster. The stencil itself is cut on a lattice (see
	Stencil.h, `LatticeScale`), and lengths are converted into its texels at
	the last moment.

	Options are mapped by INDEX. An option parameter's range reads back 0..1
	from the SDK whatever its element count, so nothing outside this file
	should reason from the range.
*/
namespace stencil::controls
{

/// Layers: one threshold per layer, 1 to 4 (the lattice mask packs one layer
/// per channel of an RGBA8 texture, which is where the 4 comes from).
constexpr int kMinLayers = 1;
constexpr int kMaxLayers = 4;
int LayerCount( float value );

/// Threshold: the lightest cut, as a tone (the clip's luma over white paper).
/// Layer k of n (1 = the lightest) paints where the tone is under
/// Threshold x (n - k + 1) / n, so the n cuts divide 0..Threshold evenly and
/// everything lighter than Threshold is bare wall.
float LayerThreshold( float threshold, int layer, int layers );

/// Smooth: a Gaussian on the tone before it is cut, sigma 0 to 1% of the
/// frame height, linear.
float SmoothSigmaHeights( float value );

/// Bridge Width: 0.1% to 2% of the frame height, geometric.
float BridgeWidthHeights( float value );
float BridgeWidthParam( float heights );

/// Min Island: an island whose area is under (side x frame height)^2 is not
/// bridged but dropped, as a cutter drops a speck: it falls out and is
/// painted. The side runs 0 to 5% of the frame height, linear.
float MinIslandSideHeights( float value );
float MinIslandParam( float heights );

/// Distance: how far the nozzle is from the wall, as what it does to the
/// paint: the radius of the cone's footprint through a hole's edge, 0 to 2%
/// of the frame height, linear, so the overspray's half width is exactly
/// proportional to it (tools/sntest --overspray).
float FootprintRadiusHeights( float value );
float DistanceParam( float heights );

/// Pressure: paint laid per unit of coverage, 0 to 2, linear. Over the
/// saturation (kSaturation) the excess runs as drips.
float PressureAmount( float value );
constexpr float kSaturation = 0.9f;

/// Lift: how far the sheet stands off the wall. Paint creeps under every
/// edge by a Gaussian of sigma Lift x 0.8% of the frame height, weighted by
/// how lifted the sheet is there (a bridge lifts fully, the rest of the sheet
/// a third as much), and in proportion to Lift.
float LiftSigmaHeights( float value );
float LiftAmount( float value );
constexpr float kBridgeLift = 1.0f;
constexpr float kSheetLift  = 0.35f;

/// Drips: the share of lattice columns under saturated paint that run, and
/// how far (up to 12% of the frame height).
float DripChance( float value );
float DripLengthHeights( float value );

/// Option counts, and names in their menu order.
constexpr int kPaletteCount = 5;
const char* PaletteName( int index );
constexpr int kLayerColoursCount = 2;
const char* LayerColoursName( int index );
constexpr int kWallCount = 4;
const char* WallName( int index );

enum Palette
{
	kMono,
	kStreet,
	kSepia,
	kPop,
	kCool
};
enum LayerColours
{
	kFromPalette,
	kFromClip
};
enum Wall
{
	kWallClip,
	kWallBrick,
	kWallConcrete,
	kWallPlain
};

/// An option's value to its index, rounded and clamped.
int OptionIndex( float value, int count );

} // namespace stencil::controls
