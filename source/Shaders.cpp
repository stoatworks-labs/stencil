#include "Shaders.h"

namespace stencil::shaders
{
namespace
{
const char* const kVersion = "#version 410 core\n";

//---------------------------------------------------------------------------
// The vertex shader every full-screen pass shares.
//---------------------------------------------------------------------------
const char* const kVertexBody = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// Integer hashing (PCG output mix): exact in 32 bits, the same on every
// driver. Never fract( sin( x ) ).
//---------------------------------------------------------------------------
const char* const kHashBody = R"(
uint pcg( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

float hash01( uint v )
{
	return float( pcg( v ) >> 8u ) * ( 1.0 / 16777216.0 );
}

float hash2( ivec2 p, uint salt )
{
	return hash01( uint( p.x ) + pcg( uint( p.y ) + pcg( salt ) ) );
}
)";

//---------------------------------------------------------------------------
// 1. detect. The tone (r) and the straight colour (gba), each the MEAN over
// the Scale x Scale block of input pixels a texel stands for, clamped at the
// frame's edge. The tone is in r so a GL_RED readback is all the cutter needs.
//---------------------------------------------------------------------------
const char* const kDetectBody = R"(
uniform sampler2D InputTexture;
uniform ivec2 InputSize;//the input's pixels (the content, not the hardware size)
uniform int Scale;      //input pixels to a texel side
uniform int Invert;     //1: paint the light, not the dark

out vec4 fragColor;

void main()
{
	ivec2 base = ivec2( gl_FragCoord.xy ) * Scale;
	ivec2 last = InputSize - ivec2( 1 );
	vec4 sum   = vec4( 0.0 );
	for( int j = 0; j < Scale; ++j )
	{
		for( int i = 0; i < Scale; ++i )
		{
			vec4 c = texelFetch( InputTexture, min( base + ivec2( i, j ), last ), 0 );
			//Un-premultiply for the colour, or a soft alpha edge reads as a
			//darker colour.
			vec3 straight = c.a > 0.0031 ? c.rgb / c.a : c.rgb;
			float luma    = dot( straight, vec3( 0.2126, 0.7152, 0.0722 ) );
			if( Invert != 0 )
				luma = 1.0 - luma;
			//The tone: the clip laid over white paper. A transparent pixel
			//is bare wall, never a hole to paint through, either way round.
			sum += vec4( c.a * luma + ( 1.0 - c.a ), straight );
		}
	}
	fragColor = sum / float( Scale * Scale );
}
)";

//---------------------------------------------------------------------------
// 2 and 6. blur. One axis of a Gaussian, texel for texel, clamped at the
// lattice's edge. FromCut reads the cut mask and blurs its holes instead.
//---------------------------------------------------------------------------
const char* const kBlurBody = R"(
uniform sampler2D Value;
uniform ivec2 Axis;  //(1, 0) or (0, 1)
uniform float Sigma; //texels
uniform int Taps;    //each side
uniform int FromCut; //1: blur step( 0.75, value ), the holes of the cut

out vec4 fragColor;

vec4 valueAt( ivec2 q )
{
	vec4 v = texelFetch( Value, q, 0 );
	return FromCut != 0 ? step( vec4( 0.75 ), v ) : v;
}

void main()
{
	ivec2 p    = ivec2( gl_FragCoord.xy );
	ivec2 last = textureSize( Value, 0 ) - ivec2( 1 );
	vec4 sum   = vec4( 0.0 );
	float weights = 0.0;
	for( int k = -Taps; k <= Taps; ++k )
	{
		float w = exp( -0.5 * float( k * k ) / ( Sigma * Sigma ) );
		ivec2 q = p + Axis * k;
		//Past the lattice is the margin: sheet, no hole, for the cut; the
		//edge texel again for the tone.
		if( FromCut != 0 && ( q.x < 0 || q.y < 0 || q.x > last.x || q.y > last.y ) )
		{
			weights += w;
			continue;
		}
		sum += w * valueAt( clamp( q, ivec2( 0 ), last ) );
		weights += w;
	}
	fragColor = sum / weights;
}
)";

//---------------------------------------------------------------------------
// 3. seed. Every sheet texel of the grid is its own nearest sheet; nothing
// knows yet of another piece. A seed is (position, piece): the position
// packed x | y << 16, NONE for none, and the piece's label beside it, so the
// flood never has to look a label up.
//---------------------------------------------------------------------------
const char* const kSeedBody = R"(
uniform usampler2D Labels;//0 hole, else the piece's label

out uvec4 fragSeeds;

const uint NONE = 0xffffffffu;

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );
	uint l  = texelFetch( Labels, p, 0 ).r;
	fragSeeds = l != 0u ? uvec4( uint( p.x ) | ( uint( p.y ) << 16 ), l, NONE, 0u ) : uvec4( NONE, 0u, NONE, 0u );
}
)";

//---------------------------------------------------------------------------
// 4. flood. One jump-flooding pass at Step, carrying two seeds: the nearest
// sheet texel (first), and the nearest sheet texel whose piece is not the
// first's (second), each as (position, piece).
//
// Distances are squared integers and ties are broken by the seed's (y, x),
// a total order, so the result is a pure function of the labels whatever
// order a driver visits fragments or neighbours in, and a translation of
// the picture translates it.
//---------------------------------------------------------------------------
const char* const kFloodBody = R"(
uniform usampler2D Seeds;//RGBA32UI: first (position, piece), second (position, piece)
uniform int Step;
uniform ivec4 Region;    //the cells flooded, [x, z) x [y, w): nothing outside is read

out uvec4 fragSeeds;

const uint NONE = 0xffffffffu;
const int FAR   = 0x7fffffff;

ivec2 p;
uint firstSeed;
int firstDist;
uint firstLabel;
uint secondSeed;
int secondDist;
uint secondLabel;

ivec2 unpack( uint s )
{
	return ivec2( int( s & 0xffffu ), int( s >> 16 ) );
}

int distanceTo( uint s )
{
	if( s == NONE )
		return FAR;
	ivec2 v = unpack( s ) - p;
	return v.x * v.x + v.y * v.y;
}

//Is (d, s) nearer than (dRef, sRef)? Ties by y, then x: the packed position
//IS (y, x) in order, y in the high half.
bool nearer( int d, uint s, int dRef, uint sRef )
{
	if( d != dRef )
		return d < dRef;
	return s < sRef;
}

void consider( uint s, uint l )
{
	if( s == NONE || l == 0u )
		return;
	int d = distanceTo( s );
	if( nearer( d, s, firstDist, firstSeed ) )
	{
		//The old first becomes the second if it is another piece's: it is
		//then nearer than any second of that piece's own could be.
		if( l != firstLabel && firstLabel != 0u )
		{
			secondSeed  = firstSeed;
			secondDist  = firstDist;
			secondLabel = firstLabel;
		}
		firstSeed  = s;
		firstDist  = d;
		firstLabel = l;
	}
	else if( l != firstLabel && nearer( d, s, secondDist, secondSeed ) )
	{
		secondSeed  = s;
		secondDist  = d;
		secondLabel = l;
	}
}

void main()
{
	p           = ivec2( gl_FragCoord.xy );
	uvec4 own   = texelFetch( Seeds, p, 0 );
	firstSeed   = own.x;
	firstLabel  = own.y;
	firstDist   = distanceTo( own.x );
	secondSeed  = own.z;
	secondLabel = own.w;
	secondDist  = distanceTo( own.z );

	for( int dy = -1; dy <= 1; ++dy )
	{
		for( int dx = -1; dx <= 1; ++dx )
		{
			if( dx == 0 && dy == 0 )
				continue;
			ivec2 q = p + ivec2( dx, dy ) * Step;
			if( q.x < Region.x || q.y < Region.y || q.x >= Region.z || q.y >= Region.w )
				continue;
			uvec4 c = texelFetch( Seeds, q, 0 );
			consider( c.x, c.y );
			consider( c.z, c.w );
		}
	}
	fragSeeds = uvec4( firstSeed, firstLabel, secondSeed, secondLabel );
}
)";

//---------------------------------------------------------------------------
// 4b. second. Only the second seed's position leaves the GPU: a quarter of
// what the flood holds.
//---------------------------------------------------------------------------
const char* const kSecondBody = R"(
uniform usampler2D Seeds;

out uvec4 fragSecond;

void main()
{
	fragSecond = uvec4( texelFetch( Seeds, ivec2( gl_FragCoord.xy ), 0 ).z, 0u, 0u, 0u );
}
)";

//---------------------------------------------------------------------------
// 5. spray. The hole mask of every layer (one a channel) through the cone's
// footprint: sum of weight x hole over the taps. The weights are the exact
// areas of the disc inside each texel's square (Spray.cpp), so over a
// half-plane of whole texels the sum is the chord fraction exactly. Past the
// lattice is the margin, sheet.
//---------------------------------------------------------------------------
const char* const kSprayBody = R"(
uniform sampler2D Cut; //RGBA8: 0 sheet, 0.5 bridge, 1 hole, a layer a channel
uniform sampler2D Taps;//RGBA32F, TapCount x 1: (dx, dy, weight, 0)
uniform int TapCount;

out vec4 fragColor;

void main()
{
	ivec2 p    = ivec2( gl_FragCoord.xy );
	ivec2 size = textureSize( Cut, 0 );
	vec4 sum   = vec4( 0.0 );
	for( int t = 0; t < TapCount; ++t )
	{
		vec4 tap = texelFetch( Taps, ivec2( t, 0 ), 0 );
		ivec2 q  = p + ivec2( tap.xy );
		if( q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y )
			continue;
		sum += tap.z * step( vec4( 0.75 ), texelFetch( Cut, q, 0 ) );
	}
	fragColor = sum;
}
)";

//---------------------------------------------------------------------------
// 7. settle. The paint that lands: Pressure x (the spray, plus the creep
// under a lifted edge -- a bridge lifts fully, the rest of the sheet by
// SheetLift). Then the drips: in a column the hash picks, paint over
// Saturation runs down, as far as the column's reach times how much over it
// is, thinning to half at its tip.
//---------------------------------------------------------------------------
const char* const kSettleBody = R"(
uniform sampler2D Cut;
uniform sampler2D Sprayed;
uniform sampler2D Creep;
uniform int UseCreep;
uniform float Pressure;
uniform float LiftAmount;
uniform float BridgeLift;
uniform float SheetLift;
uniform float Saturation;
uniform int DripReach;   //texels, the longest a drip can run
uniform float DripChance;//share of columns that run
uniform int Layers;

out vec4 fragColor;

vec4 landed( ivec2 q )
{
	vec4 cut    = texelFetch( Cut, q, 0 );
	vec4 hole   = step( vec4( 0.75 ), cut );
	vec4 bridge = step( vec4( 0.25 ), cut ) * ( vec4( 1.0 ) - hole );
	vec4 lifted = mix( vec4( SheetLift ), vec4( BridgeLift ), bridge ) * ( vec4( 1.0 ) - hole );
	vec4 creep  = UseCreep != 0 ? LiftAmount * lifted * texelFetch( Creep, q, 0 ) : vec4( 0.0 );
	return Pressure * ( texelFetch( Sprayed, q, 0 ) + creep );
}

void main()
{
	ivec2 p    = ivec2( gl_FragCoord.xy );
	ivec2 size = textureSize( Cut, 0 );
	vec4 paint = landed( p );

	vec4 live = vec4( 0.0 );
	for( int c = 0; c < 4; ++c )
		live[ c ] = c < Layers ? 1.0 : 0.0;

	vec4 drip = vec4( 0.0 );
	if( DripReach > 0 && DripChance > 0.0 )
	{
		vec4 runs = vec4( 0.0 ), reach = vec4( 0.0 );
		for( int c = 0; c < 4; ++c )
		{
			runs[ c ]  = hash2( ivec2( p.x, c ), 17u ) < DripChance ? 1.0 : 0.0;
			reach[ c ] = float( DripReach ) * ( 0.35 + 0.65 * hash2( ivec2( p.x, c ), 29u ) );
		}
		runs *= live;
		if( dot( runs, runs ) > 0.0 )
		{
			for( int j = 1; j <= DripReach; ++j )
			{
				ivec2 q = p + ivec2( 0, j );
				if( q.y >= size.y )
					break;
				vec4 over = max( landed( q ) - vec4( Saturation ), vec4( 0.0 ) );
				//How far this source's excess runs: a full run at 0.3 over.
				vec4 run  = reach * min( over / 0.3, vec4( 1.0 ) );
				vec4 t    = vec4( float( j ) ) / max( run, vec4( 1.0e-3 ) );
				vec4 here = step( vec4( float( j ) ), run ) * ( vec4( 1.0 ) - 0.5 * t * t );
				drip      = max( drip, runs * here );
			}
		}
	}
	fragColor = max( min( paint, vec4( 1.0 ) ), drip ) * live;
}
)";

//---------------------------------------------------------------------------
// 8. composite. The wall, then each layer's paint over it in order, lightest
// first, so a darker layer lands over a lighter one.
//---------------------------------------------------------------------------
const char* const kCompositeBody = R"(
uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform sampler2D Coverage; //lattice, a layer a channel
uniform vec2 LatticeUV;     //the job's 0..1 on the lattice's 0..1
uniform int Layers;
uniform vec3 Ink[ 4 ];      //lightest first
uniform int Order;          //0 lightest first (the plugin); 1 the other way (a test hook)
uniform int Wall;           //0 clip, 1 brick, 2 concrete, 3 plain
uniform vec3 PlainWall;
uniform vec2 OutSize;
uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

//Value noise on a lattice of `cell` frame heights: smooth, raster-free.
float valueNoise( vec2 q, float cell, uint salt )
{
	vec2 g  = q / cell;
	ivec2 i = ivec2( floor( g ) );
	vec2 f  = g - floor( g );
	f       = f * f * ( 3.0 - 2.0 * f );
	float a = hash2( i, salt ), b = hash2( i + ivec2( 1, 0 ), salt );
	float c = hash2( i + ivec2( 0, 1 ), salt ), d = hash2( i + ivec2( 1, 1 ), salt );
	return mix( mix( a, b, f.x ), mix( c, d, f.x ), f.y );
}

vec3 brick( vec2 q )
{
	//Courses 5% of the frame high, bricks 2.2 courses long, stretcher bond.
	float course = 0.05;
	float row    = floor( q.y / course );
	float span   = 0.11;
	float shift  = mod( row, 2.0 ) * 0.5 * span;
	float col    = floor( ( q.x + shift ) / span );
	vec2 local   = vec2( q.x + shift - col * span, q.y - row * course );
	float mortar = 0.0045;
	float edge   = min( min( local.x, span - local.x ), min( local.y, course - local.y ) );
	float joint  = 1.0 - smoothstep( 0.5 * mortar, 0.5 * mortar + 0.0015, edge );

	ivec2 id   = ivec2( int( col ), int( row ) );
	float tint = hash2( id, 5u );
	vec3 clay  = mix( vec3( 0.52, 0.22, 0.15 ), vec3( 0.66, 0.36, 0.24 ), tint );
	clay      *= 0.86 + 0.14 * hash2( id, 9u );
	float grain = valueNoise( q, 0.004, 11u ) * 0.6 + valueNoise( q, 0.015, 13u ) * 0.4;
	clay       *= 0.88 + 0.24 * grain;
	vec3 mortarColour = vec3( 0.63, 0.61, 0.57 ) * ( 0.9 + 0.2 * valueNoise( q, 0.003, 15u ) );
	return mix( clay, mortarColour, joint );
}

vec3 concrete( vec2 q )
{
	float broad = valueNoise( q, 0.12, 21u );
	float mid   = valueNoise( q, 0.025, 23u );
	float fine  = valueNoise( q, 0.004, 25u );
	float v     = 0.60 + 0.07 * ( broad - 0.5 ) + 0.06 * ( mid - 0.5 ) + 0.08 * ( fine - 0.5 );
	//Pits: a few cells of a fine lattice are small dark holes.
	vec2 g    = q / 0.006;
	ivec2 id  = ivec2( floor( g ) );
	float pit = hash2( id, 27u ) < 0.04 ? 1.0 - smoothstep( 0.12, 0.28, length( g - floor( g ) - vec2( 0.5 ) ) ) : 0.0;
	v        *= 1.0 - 0.35 * pit;
	return vec3( v, v * 0.99, v * 0.955 );
}

void main()
{
	vec4 clip = texture( InputTexture, uv * MaxUV );
	vec2 q    = gl_FragCoord.xy / OutSize.y;//frame heights

	vec3 rgb;
	float alpha = 1.0;
	if( Wall == 0 )
	{
		rgb   = clip.rgb;//premultiplied, as it came
		alpha = clip.a;
	}
	else if( Wall == 1 )
		rgb = brick( q );
	else if( Wall == 2 )
		rgb = concrete( q );
	else
		rgb = PlainWall;

	vec4 cover = texture( Coverage, uv * LatticeUV );
	for( int n = 0; n < 4; ++n )
	{
		if( n >= Layers )
			break;
		int layer = Order == 0 ? n : Layers - 1 - n;
		float c   = clamp( cover[ layer ], 0.0, 1.0 );
		//Paint over, premultiplied: opaque where it lands.
		rgb   = rgb * ( 1.0 - c ) + Ink[ layer ] * c;
		alpha = alpha * ( 1.0 - c ) + c;
	}

	vec4 effect = vec4( rgb, alpha );
	fragColor   = MixAmount >= 1.0 ? effect : mix( clip, effect, MixAmount );
}
)";

std::string assemble( const char* body )
{
	return std::string( kVersion ) + body;
}

std::string assembleHashed( const char* body )
{
	return std::string( kVersion ) + kHashBody + body;
}
} // namespace

std::string Vertex()
{
	return assemble( kVertexBody );
}
std::string Detect()
{
	return assemble( kDetectBody );
}
std::string Blur()
{
	return assemble( kBlurBody );
}
std::string Seed()
{
	return assemble( kSeedBody );
}
std::string Flood()
{
	return assemble( kFloodBody );
}
std::string Second()
{
	return assemble( kSecondBody );
}
std::string Spray()
{
	return assemble( kSprayBody );
}
std::string Settle()
{
	return assembleHashed( kSettleBody );
}
std::string Composite()
{
	return assembleHashed( kCompositeBody );
}

} // namespace stencil::shaders
