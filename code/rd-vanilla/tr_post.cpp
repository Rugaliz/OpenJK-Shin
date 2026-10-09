/*
===========================================================================
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2013 - 2015, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// tr_post.cpp -- full screen effects that run on the finished 3D view, before the HUD is drawn. The view is
// copied into textures (colour and depth), the effects run on those through shader programs, and the result is
// drawn back over the view. Nothing here runs unless GLSL and framebuffer objects are there and an effect is on.

#include "tr_local.h"
#include "tr_smaa_data.h"
#include <string>
#include <zlib.h>

#ifndef GL_RG8
#define GL_RG8						0x822B
#define GL_RG						0x8227
#endif
#ifndef GL_R8
#define GL_R8						0x8229
#define GL_RED						0x1903
#endif

static PFNGLGENFRAMEBUFFERSPROC			qglGenFramebuffers;
static PFNGLBINDFRAMEBUFFERPROC			qglBindFramebuffer;
static PFNGLFRAMEBUFFERTEXTURE2DPROC	qglFramebufferTexture2D;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC	qglCheckFramebufferStatus;
static PFNGLDELETEFRAMEBUFFERSPROC		qglDeleteFramebuffers;

// a texture an effect can draw into
typedef struct {
	GLuint	texture;
	GLuint	fbo;
	int		width, height;
} postTarget_t;

typedef struct {
	GLuint	id;
	GLint	uniforms[8];
} postProgram_t;

#define POST_BLOOM_LEVELS	5

static struct {
	qboolean		ready;				// entry points are there
	int				width, height;		// size of the view the textures below are made for
	GLuint			sceneTexture;		// copy of the colour of the view
	GLuint			depthTexture;		// copy of its depth
	postTarget_t	aoA, aoB;			// the occlusion as it is made, and while it is blurred
	postTarget_t	bloom[POST_BLOOM_LEVELS];	// the bright parts of the view, each level half the size of the one before
	postProgram_t	composite;			// puts the effects together and draws them over the view
	postProgram_t	ssao, ssaoBlur;
	postProgram_t	bloomDown, bloomUp;
	postTarget_t	final, edges, weights;	// for SMAA: the view with the other effects on, the edges found in it, how to blend them
	postProgram_t	smaaEdges, smaaWeights, smaaBlend;
	GLuint			smaaAreaTexture, smaaSearchTexture;
	qboolean		smaaOk;				// GLSL 3.30 is there, which SMAA is written for
	qboolean		smaaFailed;
	qboolean		failed;				// a program did not build, stop trying
} post;

static const char *postVertexSource =
	"#version 120\n"
	"void main() {\n"
	"	gl_Position = gl_Vertex;\n"	// the quad is given in clip space
	"	gl_TexCoord[0] = gl_MultiTexCoord0;\n"
	"}\n";

// the view space position of a pixel, from its depth and the projection matrix of the view
#define POST_VIEWPOS_SOURCE \
	"uniform sampler2D uDepth;\n" \
	"uniform vec4 uProjA;\n"		/* the matrix: [0] [5] [8] [9] */ \
	"uniform vec2 uProjB;\n"		/* [10] [14] */ \
	"float viewZ( float depth ) {\n" \
	"	return -uProjB.y / ( depth * 2.0 - 1.0 + uProjB.x );\n" \
	"}\n" \
	"vec3 viewPos( vec2 uv ) {\n" \
	"	float z = viewZ( texture2D( uDepth, uv ).r );\n" \
	"	vec2 ndc = uv * 2.0 - 1.0;\n" \
	"	return vec3( -( ndc.x + uProjA.z ) * z / uProjA.x, -( ndc.y + uProjA.w ) * z / uProjA.y, z );\n" \
	"}\n"

// Depth below 0.3 is the weapon in front of the player, which the engine draws squeezed into the nearest part of
// the depth range, so it is left out of the occlusion; a depth of 1 is the sky.
static const char *postSsaoFragmentSource =
	"#version 120\n"
	POST_VIEWPOS_SOURCE
	"uniform vec2 uSize;\n"
	"uniform vec4 uParams;\n"		// radius in units, strength, bias
	"void main() {\n"
	"	vec2 uv = gl_FragCoord.xy / uSize;\n"
	"	float depth = texture2D( uDepth, uv ).r;\n"
	"	if ( depth >= 1.0 || depth < 0.3 ) {\n"
	"		gl_FragColor = vec4( 1.0 );\n"
	"		return;\n"
	"	}\n"
	"	vec3 p = viewPos( uv );\n"
	"	vec2 px = 1.0 / uSize;\n"
	"	vec3 pr = viewPos( uv + vec2( px.x, 0.0 ) );\n"
	"	vec3 pl = viewPos( uv - vec2( px.x, 0.0 ) );\n"
	"	vec3 pu = viewPos( uv + vec2( 0.0, px.y ) );\n"
	"	vec3 pd = viewPos( uv - vec2( 0.0, px.y ) );\n"
	// the normal from the neighbours on the side of the smaller step in depth, so that it holds at edges
	"	vec3 dx = abs( pr.z - p.z ) < abs( p.z - pl.z ) ? pr - p : p - pl;\n"
	"	vec3 dy = abs( pu.z - p.z ) < abs( p.z - pd.z ) ? pu - p : p - pd;\n"
	"	vec3 n = normalize( cross( dx, dy ) );\n"
	"	if ( dot( n, p ) > 0.0 ) {\n"
	"		n = -n;\n"
	"	}\n"
	"	float radius = uParams.x;\n"
	"	float radiusPx = clamp( radius * uProjA.x * 0.5 * uSize.x / -p.z, 3.0, 0.25 * uSize.y );\n"
	"	float noise = fract( 52.9829189 * fract( dot( gl_FragCoord.xy, vec2( 0.06711056, 0.00583715 ) ) ) );\n"
	"	float occlusion = 0.0;\n"
	"	for ( int i = 0; i < 16; i++ ) {\n"
	"		float angle = noise * 6.2831853 + float( i ) * 2.3999632;\n"
	"		float dist = sqrt( ( float( i ) + 0.5 ) / 16.0 ) * radiusPx;\n"
	"		vec2 suv = uv + vec2( cos( angle ), sin( angle ) ) * dist / uSize;\n"
	"		if ( texture2D( uDepth, suv ).r < 0.3 ) {\n"
	"			continue;\n"
	"		}\n"
	"		vec3 v = viewPos( suv ) - p;\n"
	"		float vv = dot( v, v );\n"
	"		float falloff = clamp( 1.0 - vv / ( radius * radius ), 0.0, 1.0 );\n"
	"		occlusion += falloff * max( dot( v, n ) * inversesqrt( vv + 0.0001 ) - uParams.z, 0.0 );\n"
	"	}\n"
	"	gl_FragColor = vec4( vec3( clamp( 1.0 - uParams.y * occlusion / 16.0, 0.0, 1.0 ) ), 1.0 );\n"
	"}\n";

// blurs the occlusion along one axis, across pixels of about the same depth only so that it does not run over edges
static const char *postSsaoBlurFragmentSource =
	"#version 120\n"
	POST_VIEWPOS_SOURCE
	"uniform sampler2D uAO;\n"
	"uniform vec2 uSize;\n"
	"uniform vec2 uDir;\n"
	"void main() {\n"
	"	vec2 uv = gl_FragCoord.xy / uSize;\n"
	"	float centreZ = viewZ( texture2D( uDepth, uv ).r );\n"
	"	float sum = 0.0;\n"
	"	float weights = 0.0;\n"
	"	for ( int i = -4; i <= 4; i++ ) {\n"
	"		vec2 suv = uv + uDir * float( i ) / uSize;\n"
	"		float spatial = exp( -float( i * i ) / 8.0 );\n"
	"		float dz = abs( viewZ( texture2D( uDepth, suv ).r ) - centreZ );\n"
	"		float w = spatial * max( 1.0 - dz / ( 4.0 + 0.02 * abs( centreZ ) ), 0.0 );\n"
	"		sum += w * texture2D( uAO, suv ).r;\n"
	"		weights += w;\n"
	"	}\n"
	"	gl_FragColor = vec4( vec3( sum / max( weights, 0.0001 ) ), 1.0 );\n"
	"}\n";

// Shrinks the source to half size with 13 taps, which holds the bright spots together where a plain average of
// four would make them flicker. On the first level only the part of every tap that is brighter than the
// threshold goes on, with a soft knee.
static const char *postBloomDownFragmentSource =
	"#version 120\n"
	"uniform sampler2D uSource;\n"
	"uniform vec2 uTexel;\n"		// size of a texel of the source
	"uniform vec3 uParams;\n"		// threshold, knee, 1 on the first level
	"vec3 tap( vec2 uv, vec2 offset ) {\n"
	"	vec3 c = texture2D( uSource, uv + offset * uTexel ).rgb;\n"
	"	if ( uParams.z > 0.5 ) {\n"
	"		float brightness = max( c.r, max( c.g, c.b ) );\n"
	"		float soft = clamp( brightness - uParams.x + uParams.y, 0.0, 2.0 * uParams.y );\n"
	"		soft = soft * soft / ( 4.0 * uParams.y + 0.0001 );\n"
	"		c *= max( soft, brightness - uParams.x ) / max( brightness, 0.0001 );\n"
	"	}\n"
	"	return c;\n"
	"}\n"
	"void main() {\n"
	"	vec2 uv = gl_TexCoord[0].st;\n"
	"	vec3 a = tap( uv, vec2( -2.0, -2.0 ) ), b = tap( uv, vec2( 0.0, -2.0 ) ), c = tap( uv, vec2( 2.0, -2.0 ) );\n"
	"	vec3 d = tap( uv, vec2( -2.0, 0.0 ) ), e = tap( uv, vec2( 0.0, 0.0 ) ), f = tap( uv, vec2( 2.0, 0.0 ) );\n"
	"	vec3 g = tap( uv, vec2( -2.0, 2.0 ) ), h = tap( uv, vec2( 0.0, 2.0 ) ), i = tap( uv, vec2( 2.0, 2.0 ) );\n"
	"	vec3 j = tap( uv, vec2( -1.0, -1.0 ) ), k = tap( uv, vec2( 1.0, -1.0 ) );\n"
	"	vec3 l = tap( uv, vec2( -1.0, 1.0 ) ), m = tap( uv, vec2( 1.0, 1.0 ) );\n"
	"	vec3 colour = e * 0.125 + ( a + c + g + i ) * 0.03125 + ( b + d + f + h ) * 0.0625 + ( j + k + l + m ) * 0.125;\n"
	"	gl_FragColor = vec4( colour, 1.0 );\n"
	"}\n";

// grows the source to twice the size with a tent filter; added on top of what is in the target
static const char *postBloomUpFragmentSource =
	"#version 120\n"
	"uniform sampler2D uSource;\n"
	"uniform vec2 uTexel;\n"		// size of a texel of the source
	"uniform float uScale;\n"
	"void main() {\n"
	"	vec2 uv = gl_TexCoord[0].st;\n"
	"	vec3 colour = texture2D( uSource, uv ).rgb * 4.0;\n"
	"	colour += ( texture2D( uSource, uv + vec2( uTexel.x, 0.0 ) ).rgb + texture2D( uSource, uv - vec2( uTexel.x, 0.0 ) ).rgb +\n"
	"				texture2D( uSource, uv + vec2( 0.0, uTexel.y ) ).rgb + texture2D( uSource, uv - vec2( 0.0, uTexel.y ) ).rgb ) * 2.0;\n"
	"	colour += texture2D( uSource, uv + uTexel ).rgb + texture2D( uSource, uv - uTexel ).rgb +\n"
	"			  texture2D( uSource, uv + vec2( uTexel.x, -uTexel.y ) ).rgb + texture2D( uSource, uv + vec2( -uTexel.x, uTexel.y ) ).rgb;\n"
	"	gl_FragColor = vec4( colour * ( uScale / 16.0 ), 1.0 );\n"
	"}\n";

// the view with the effects on it
static const char *postCompositeFragmentSource =
	"#version 120\n"
	"uniform sampler2D uScene;\n"
	"uniform sampler2D uAO;\n"
	"uniform sampler2D uBloom;\n"
	"uniform vec4 uRect;\n"			// x, y, width, height of the view in the window
	"uniform vec3 uMode;\n"			// 1 if the occlusion is on, 1 to show it alone, how much bloom is added
	"void main() {\n"
	"	vec2 uv = ( gl_FragCoord.xy - uRect.xy ) / uRect.zw;\n"
	"	vec3 colour = texture2D( uScene, uv ).rgb;\n"
	"	if ( uMode.x > 0.5 ) {\n"
	"		float ao = texture2D( uAO, uv ).r;\n"
	"		colour = uMode.y > 0.5 ? vec3( ao ) : colour * ao;\n"
	"	}\n"
	"	colour += texture2D( uBloom, uv ).rgb * uMode.z * ( 1.0 - colour );\n"	// like a screen blend, it does not clip what is bright already
	"	gl_FragColor = vec4( colour, 1.0 );\n"
	"}\n";

static void Post_SetAvailable( const char *name, qboolean available ) {
	ri.Cvar_Get( name, "0", 0 );
	ri.Cvar_Set( name, available ? "1" : "0" );	// what the menus look at to show the rows for what needs it
}

void R_Post_Init( void ) {
	memset( &post, 0, sizeof( post ) );
	Post_SetAvailable( "r_postAvail", qfalse );
	Post_SetAvailable( "r_smaaAvail", qfalse );

	if ( !R_GLSL_Available() || !qglActiveTextureARB || !qglMultiTexCoord2fARB ) {
		return;
	}
	if ( !ri.GL_ExtensionSupported( "GL_ARB_framebuffer_object" ) ) {
		Com_Printf( "...GL_ARB_framebuffer_object not found, no post processing\n" );
		return;
	}

	qglGenFramebuffers			= (PFNGLGENFRAMEBUFFERSPROC)		ri.GL_GetProcAddress( "glGenFramebuffers" );
	qglBindFramebuffer			= (PFNGLBINDFRAMEBUFFERPROC)		ri.GL_GetProcAddress( "glBindFramebuffer" );
	qglFramebufferTexture2D		= (PFNGLFRAMEBUFFERTEXTURE2DPROC)	ri.GL_GetProcAddress( "glFramebufferTexture2D" );
	qglCheckFramebufferStatus	= (PFNGLCHECKFRAMEBUFFERSTATUSPROC)	ri.GL_GetProcAddress( "glCheckFramebufferStatus" );
	qglDeleteFramebuffers		= (PFNGLDELETEFRAMEBUFFERSPROC)		ri.GL_GetProcAddress( "glDeleteFramebuffers" );

	if ( !qglGenFramebuffers || !qglBindFramebuffer || !qglFramebufferTexture2D || !qglCheckFramebufferStatus ||
		 !qglDeleteFramebuffers ) {
		Com_Printf( "...GL_ARB_framebuffer_object found, but an entry point is missing\n" );
		return;
	}

	post.ready = qtrue;
	Com_Printf( "...using post processing\n" );

	{
		int major = 0, minor = 0;
		if ( sscanf( glConfig.version_string, "%d.%d", &major, &minor ) >= 2 && major * 10 + minor >= 33 ) {
			post.smaaOk = qtrue;
		} else {
			Com_Printf( "...SMAA needs OpenGL 3.3\n" );
		}
	}
	Post_SetAvailable( "r_postAvail", qtrue );
	Post_SetAvailable( "r_smaaAvail", post.smaaOk );
}

static GLuint Post_NewTexture( GLint internalFormat, GLenum format, GLenum type, int width, int height ) {
	GLuint texture;

	qglGenTextures( 1, &texture );
	qglBindTexture( GL_TEXTURE_2D, texture );
	qglTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, type, NULL );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	return texture;
}

static void Post_FreeTarget( postTarget_t *target ) {
	if ( target->fbo ) {
		qglDeleteFramebuffers( 1, &target->fbo );
	}
	if ( target->texture ) {
		qglDeleteTextures( 1, &target->texture );
	}
	memset( target, 0, sizeof( *target ) );
}

static void Post_NewTarget( postTarget_t *target, int width, int height ) {
	target->texture = Post_NewTexture( GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, width, height );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	qglGenFramebuffers( 1, &target->fbo );
	qglBindFramebuffer( GL_FRAMEBUFFER, target->fbo );
	qglFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target->texture, 0 );
	qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	target->width = width;
	target->height = height;
}

static void Post_FreeTargets( void ) {
	int i;

	if ( post.sceneTexture ) {
		qglDeleteTextures( 1, &post.sceneTexture );
	}
	if ( post.depthTexture ) {
		qglDeleteTextures( 1, &post.depthTexture );
	}
	post.sceneTexture = post.depthTexture = 0;
	Post_FreeTarget( &post.aoA );
	Post_FreeTarget( &post.aoB );
	Post_FreeTarget( &post.final );
	Post_FreeTarget( &post.edges );
	Post_FreeTarget( &post.weights );
	for ( i = 0; i < POST_BLOOM_LEVELS; i++ ) {
		Post_FreeTarget( &post.bloom[i] );
	}
	post.width = post.height = 0;
}

// makes the textures for a view of this size, if they are not there yet
static void Post_EnsureTargets( int width, int height ) {
	int i;

	if ( post.width == width && post.height == height && post.sceneTexture ) {
		return;
	}
	Post_FreeTargets();
	post.sceneTexture = Post_NewTexture( GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, width, height );
	post.depthTexture = Post_NewTexture( GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, width, height );
	Post_NewTarget( &post.aoA, width, height );
	Post_NewTarget( &post.aoB, width, height );
	for ( i = 0; i < POST_BLOOM_LEVELS; i++ ) {
		Post_NewTarget( &post.bloom[i], Q_max( width >> ( i + 1 ), 2 ), Q_max( height >> ( i + 1 ), 2 ) );
	}
	if ( post.smaaOk ) {
		Post_NewTarget( &post.final, width, height );
		Post_NewTarget( &post.edges, width, height );
		Post_NewTarget( &post.weights, width, height );
	}
	post.width = width;
	post.height = height;
}

static qboolean Post_BuildProgramWith( postProgram_t *program, const char *name, const char *vertexSource,
									   const char *fragmentSource, const char *const *uniformNames ) {
	int i;

	program->id = R_GLSL_BuildProgram( name, vertexSource, fragmentSource );
	if ( !program->id ) {
		return qfalse;
	}
	for ( i = 0; uniformNames[i]; i++ ) {
		program->uniforms[i] = qglGetUniformLocation( program->id, uniformNames[i] );
	}
	return qtrue;
}

static qboolean Post_BuildProgram( postProgram_t *program, const char *name, const char *fragmentSource,
								   const char *const *uniformNames ) {
	return Post_BuildProgramWith( program, name, postVertexSource, fragmentSource, uniformNames );
}

static void Post_Quad( void ) {
	qglBegin( GL_QUADS );
	qglMultiTexCoord2fARB( GL_TEXTURE0_ARB, 0.0f, 0.0f );
	qglVertex2f( -1.0f, -1.0f );
	qglMultiTexCoord2fARB( GL_TEXTURE0_ARB, 1.0f, 0.0f );
	qglVertex2f( 1.0f, -1.0f );
	qglMultiTexCoord2fARB( GL_TEXTURE0_ARB, 1.0f, 1.0f );
	qglVertex2f( 1.0f, 1.0f );
	qglMultiTexCoord2fARB( GL_TEXTURE0_ARB, 0.0f, 1.0f );
	qglVertex2f( -1.0f, 1.0f );
	qglEnd();
}

static void Post_BindTexture( int unit, GLuint texture ) {
	qglActiveTextureARB( GL_TEXTURE0_ARB + unit );
	qglBindTexture( GL_TEXTURE_2D, texture );
}

/*
================
SMAA

Subpixel morphological antialiasing, in the three passes of its authors: find the edges (from the brightness of
the pixels), work out from the shape of every edge how much of the pixels on both sides to blend, and blend. It
only moves pixels along the edges it finds, so unlike FXAA it does not soften the rest of the picture. The code
and the lookup textures are theirs (tr_smaa_data.h); this is the glue.
================
*/
static const char *postVertex330Source =
	"#version 330 compatibility\n"
	"void main() {\n"
	"	gl_Position = gl_Vertex;\n"
	"	gl_TexCoord[0] = gl_MultiTexCoord0;\n"
	"}\n";

static const char *smaaEdgesMain =
	"uniform sampler2D uColor;\n"
	"void main() {\n"
	"	vec2 uv = gl_TexCoord[0].st;\n"
	"	vec4 offset[3];\n"
	"	SMAAEdgeDetectionVS( uv, offset );\n"
	"	gl_FragColor = vec4( SMAALumaEdgeDetectionPS( uv, offset, uColor ), 0.0, 0.0 );\n"
	"}\n";

static const char *smaaWeightsMain =
	"uniform sampler2D uEdges;\n"
	"uniform sampler2D uArea;\n"
	"uniform sampler2D uSearch;\n"
	"void main() {\n"
	"	vec2 uv = gl_TexCoord[0].st;\n"
	"	vec2 pixcoord;\n"
	"	vec4 offset[3];\n"
	"	SMAABlendingWeightCalculationVS( uv, pixcoord, offset );\n"
	"	gl_FragColor = SMAABlendingWeightCalculationPS( uv, pixcoord, offset, uEdges, uArea, uSearch, vec4( 0.0 ) );\n"
	"}\n";

static const char *smaaBlendMain =
	"uniform sampler2D uColor;\n"
	"uniform sampler2D uBlend;\n"
	"void main() {\n"
	"	vec2 uv = gl_TexCoord[0].st;\n"
	"	vec4 offset;\n"
	"	SMAANeighborhoodBlendingVS( uv, offset );\n"
	"	gl_FragColor = SMAANeighborhoodBlendingPS( uv, offset, uColor, uBlend );\n"
	"}\n";

static qboolean Post_BuildSmaaProgram( postProgram_t *program, const char *name, const char *main,
									   const char *const *uniformNames ) {
	std::string source = "#version 330 compatibility\n#define SMAA_GLSL_3 1\n#define SMAA_PRESET_HIGH 1\n"
		"uniform vec4 uMetrics;\n#define SMAA_RT_METRICS uMetrics\n";

	for ( size_t i = 0; i < ARRAY_LEN( smaaShaderSource ); i++ ) {
		source += smaaShaderSource[i];
	}
	source += main;
	return Post_BuildProgramWith( program, name, postVertex330Source, source.c_str(), uniformNames );
}

static int Post_Base64Value( char c ) {
	if ( c >= 'A' && c <= 'Z' ) return c - 'A';
	if ( c >= 'a' && c <= 'z' ) return c - 'a' + 26;
	if ( c >= '0' && c <= '9' ) return c - '0' + 52;
	return c == '+' ? 62 : 63;
}

// Makes a texture out of one of the compressed pieces of data in tr_smaa_data.h. Returns 0 if that does not work.
static GLuint Post_DataTexture( const char *const *pieces, size_t numPieces, int width, int height, GLint internalFormat,
								GLenum format, int bytesPerPixel, GLint filter ) {
	std::string text;
	std::string packed;
	std::string pixels;
	GLuint		texture;
	uLongf		pixelsLength = (uLongf)( width * height * bytesPerPixel );
	unsigned int bits = 0;
	int			numBits = 0;

	for ( size_t i = 0; i < numPieces; i++ ) {
		text += pieces[i];
	}
	for ( size_t i = 0; i < text.size() && text[i] != '='; i++ ) {
		bits = ( bits << 6 ) | Post_Base64Value( text[i] );
		numBits += 6;
		if ( numBits >= 8 ) {
			numBits -= 8;
			packed += (char)( ( bits >> numBits ) & 0xFF );
		}
	}

	pixels.resize( pixelsLength );
	if ( uncompress( (Bytef *)&pixels[0], &pixelsLength, (const Bytef *)packed.data(), (uLong)packed.size() ) != Z_OK ||
		 pixelsLength != (uLongf)( width * height * bytesPerPixel ) ) {
		return 0;
	}

	qglGenTextures( 1, &texture );
	qglBindTexture( GL_TEXTURE_2D, texture );
	qglPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	qglTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, pixels.data() );
	qglPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	return texture;
}

// builds what SMAA needs the first time it is used
static qboolean Post_SmaaReady( void ) {
	static const char *const edgesUniforms[] = { "uMetrics", "uColor", NULL };
	static const char *const weightsUniforms[] = { "uMetrics", "uEdges", "uArea", "uSearch", NULL };
	static const char *const blendUniforms[] = { "uMetrics", "uColor", "uBlend", NULL };

	if ( post.smaaFailed ) {
		return qfalse;
	}
	if ( post.smaaEdges.id && post.smaaAreaTexture ) {
		return qtrue;
	}

	if ( !Post_BuildSmaaProgram( &post.smaaEdges, "smaa edges", smaaEdgesMain, edgesUniforms ) ||
		 !Post_BuildSmaaProgram( &post.smaaWeights, "smaa weights", smaaWeightsMain, weightsUniforms ) ||
		 !Post_BuildSmaaProgram( &post.smaaBlend, "smaa blend", smaaBlendMain, blendUniforms ) ) {
		post.smaaFailed = qtrue;
		return qfalse;
	}
	post.smaaAreaTexture = Post_DataTexture( smaaAreaTexZ, ARRAY_LEN( smaaAreaTexZ ), SMAA_AREATEX_WIDTH,
		SMAA_AREATEX_HEIGHT, GL_RG8, GL_RG, 2, GL_LINEAR );
	post.smaaSearchTexture = Post_DataTexture( smaaSearchTexZ, ARRAY_LEN( smaaSearchTexZ ), SMAA_SEARCHTEX_WIDTH,
		SMAA_SEARCHTEX_HEIGHT, GL_R8, GL_RED, 1, GL_NEAREST );
	if ( !post.smaaAreaTexture || !post.smaaSearchTexture ) {
		Com_Printf( S_COLOR_YELLOW "SMAA: the lookup textures do not unpack\n" );
		post.smaaFailed = qtrue;
		return qfalse;
	}
	return qtrue;
}

static qboolean Post_Wanted( void ) {
	if ( !post.ready ) {
		return qfalse;
	}
	if ( backEnd.refdef.rdflags & ( RDF_NOWORLDMODEL | RDF_SKYBOXPORTAL ) ) {
		return qfalse;	// the views that are not the one the player looks through
	}
	if ( backEnd.viewParms.isPortal || backEnd.viewParms.viewportWidth < 64 || backEnd.viewParms.viewportHeight < 64 ) {
		return qfalse;
	}
	return (qboolean)( r_ssao->integer || r_bloom->integer || r_smaa->integer || r_postDebug->integer );
}

// draws the quad into a target (or the window, with 0), with the viewport set for it
static void Post_DrawInto( const postTarget_t *target ) {
	qglBindFramebuffer( GL_FRAMEBUFFER, target->fbo );
	qglViewport( 0, 0, target->width, target->height );
	Post_Quad();
}

/*
================
Post_Ssao

Ambient occlusion from the depth of the view: how much of the space around a pixel, in a sphere of r_ssaoRadius
units, is taken by surfaces in front of it. Leaves it, blurred, in post.aoA.
================
*/
static qboolean Post_Ssao( void ) {
	static const char *const ssaoUniforms[] = { "uDepth", "uProjA", "uProjB", "uSize", "uParams", NULL };
	static const char *const blurUniforms[] = { "uDepth", "uProjA", "uProjB", "uAO", "uSize", "uDir", NULL };
	const float	*proj = backEnd.viewParms.projectionMatrix;
	const float	size[2] = { (float)post.width, (float)post.height };
	const float	params[4] = { r_ssaoRadius->value, r_ssaoStrength->value, 0.1f, 0.0f };
	const float	horizontal[2] = { 1.0f, 0.0f };
	const float	vertical[2] = { 0.0f, 1.0f };

	if ( ( !post.ssao.id && !Post_BuildProgram( &post.ssao, "ssao", postSsaoFragmentSource, ssaoUniforms ) ) ||
		 ( !post.ssaoBlur.id && !Post_BuildProgram( &post.ssaoBlur, "ssao blur", postSsaoBlurFragmentSource, blurUniforms ) ) ) {
		return qfalse;
	}

	// the depth of the view
	Post_BindTexture( 0, post.depthTexture );
	qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, backEnd.viewParms.viewportX, backEnd.viewParms.viewportY, post.width, post.height );

	qglUseProgram( post.ssao.id );
	qglUniform1i( post.ssao.uniforms[0], 0 );
	qglUniform4f( post.ssao.uniforms[1], proj[0], proj[5], proj[8], proj[9] );
	qglUniform2f( post.ssao.uniforms[2], proj[10], proj[14] );
	qglUniform2fv( post.ssao.uniforms[3], 1, size );
	qglUniform4fv( post.ssao.uniforms[4], 1, params );
	Post_DrawInto( &post.aoA );

	qglUseProgram( post.ssaoBlur.id );
	qglUniform1i( post.ssaoBlur.uniforms[0], 0 );
	qglUniform4f( post.ssaoBlur.uniforms[1], proj[0], proj[5], proj[8], proj[9] );
	qglUniform2f( post.ssaoBlur.uniforms[2], proj[10], proj[14] );
	qglUniform1i( post.ssaoBlur.uniforms[3], 1 );
	qglUniform2fv( post.ssaoBlur.uniforms[4], 1, size );

	Post_BindTexture( 1, post.aoA.texture );
	qglUniform2fv( post.ssaoBlur.uniforms[5], 1, horizontal );
	Post_DrawInto( &post.aoB );

	Post_BindTexture( 1, post.aoB.texture );
	qglUniform2fv( post.ssaoBlur.uniforms[5], 1, vertical );
	Post_DrawInto( &post.aoA );

	return qtrue;
}

static void Post_ClearTarget( const postTarget_t *target ) {
	qglBindFramebuffer( GL_FRAMEBUFFER, target->fbo );
	qglClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
	qglClear( GL_COLOR_BUFFER_BIT );
}

// the three passes, from post.final to the view in the window
static void Post_Smaa( int x, int y, int width, int height ) {
	const float	metrics[4] = { 1.0f / width, 1.0f / height, (float)width, (float)height };

	// the edges
	Post_ClearTarget( &post.edges );
	Post_BindTexture( 0, post.final.texture );
	qglUseProgram( post.smaaEdges.id );
	qglUniform4fv( post.smaaEdges.uniforms[0], 1, metrics );
	qglUniform1i( post.smaaEdges.uniforms[1], 0 );
	Post_DrawInto( &post.edges );

	// how to blend them
	Post_ClearTarget( &post.weights );
	Post_BindTexture( 0, post.edges.texture );
	Post_BindTexture( 1, post.smaaAreaTexture );
	Post_BindTexture( 2, post.smaaSearchTexture );
	qglUseProgram( post.smaaWeights.id );
	qglUniform4fv( post.smaaWeights.uniforms[0], 1, metrics );
	qglUniform1i( post.smaaWeights.uniforms[1], 0 );
	qglUniform1i( post.smaaWeights.uniforms[2], 1 );
	qglUniform1i( post.smaaWeights.uniforms[3], 2 );
	Post_DrawInto( &post.weights );

	// the blend, into the window
	Post_BindTexture( 0, post.final.texture );
	Post_BindTexture( 1, post.weights.texture );
	qglUseProgram( post.smaaBlend.id );
	qglUniform4fv( post.smaaBlend.uniforms[0], 1, metrics );
	qglUniform1i( post.smaaBlend.uniforms[1], 0 );
	qglUniform1i( post.smaaBlend.uniforms[2], 1 );
	qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	qglViewport( x, y, width, height );
	Post_Quad();
}

/*
================
Post_Bloom

The parts of the scene that are brighter than r_bloomThreshold, shrunk through POST_BLOOM_LEVELS levels and
grown back, every level added to the next larger one, which makes a glow that is wide and smooth. Leaves it in
post.bloom[0].
================
*/
static qboolean Post_Bloom( void ) {
	static const char *const downUniforms[] = { "uSource", "uTexel", "uParams", NULL };
	static const char *const upUniforms[] = { "uSource", "uTexel", "uScale", NULL };
	int i;

	if ( ( !post.bloomDown.id && !Post_BuildProgram( &post.bloomDown, "bloom down", postBloomDownFragmentSource, downUniforms ) ) ||
		 ( !post.bloomUp.id && !Post_BuildProgram( &post.bloomUp, "bloom up", postBloomUpFragmentSource, upUniforms ) ) ) {
		return qfalse;
	}

	// down: the view, then every level from the one before
	qglUseProgram( post.bloomDown.id );
	qglUniform1i( post.bloomDown.uniforms[0], 0 );
	for ( i = 0; i < POST_BLOOM_LEVELS; i++ ) {
		const GLuint	source = i ? post.bloom[i - 1].texture : post.sceneTexture;
		const int		sourceWidth = i ? post.bloom[i - 1].width : post.width;
		const int		sourceHeight = i ? post.bloom[i - 1].height : post.height;

		Post_BindTexture( 0, source );
		qglUniform2f( post.bloomDown.uniforms[1], 1.0f / sourceWidth, 1.0f / sourceHeight );
		qglUniform3f( post.bloomDown.uniforms[2], r_bloomThreshold->value, 0.5f * r_bloomThreshold->value, i ? 0.0f : 1.0f );
		Post_DrawInto( &post.bloom[i] );
	}

	// up: every level added to the one above it
	GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
	qglUseProgram( post.bloomUp.id );
	qglUniform1i( post.bloomUp.uniforms[0], 0 );
	qglUniform1f( post.bloomUp.uniforms[2], 0.8f );
	for ( i = POST_BLOOM_LEVELS - 1; i > 0; i-- ) {
		Post_BindTexture( 0, post.bloom[i].texture );
		qglUniform2f( post.bloomUp.uniforms[1], 1.0f / post.bloom[i].width, 1.0f / post.bloom[i].height );
		Post_DrawInto( &post.bloom[i - 1] );
	}
	GL_State( GLS_DEPTHTEST_DISABLE );

	return qtrue;
}

/*
================
RB_PostProcess

Called when the 3D view is finished. Leaves the GL state as it found it, so the 2D drawing after it does not
notice.
================
*/
void RB_PostProcess( void ) {
	static const char *const compositeUniforms[] = { "uScene", "uAO", "uBloom", "uRect", "uMode", NULL };
	const int	x = backEnd.viewParms.viewportX;
	const int	y = backEnd.viewParms.viewportY;
	const int	width = backEnd.viewParms.viewportWidth;
	const int	height = backEnd.viewParms.viewportHeight;
	const float	rect[4] = { (float)x, (float)y, (float)width, (float)height };
	const float	smaaRect[4] = { 0.0f, 0.0f, (float)width, (float)height };
	const GLboolean	scissored = qglIsEnabled( GL_SCISSOR_TEST );
	qboolean	ssao, bloom, smaa;

	if ( post.failed || !Post_Wanted() ) {
		return;
	}

	if ( !post.composite.id &&
		 !Post_BuildProgram( &post.composite, "post composite", postCompositeFragmentSource, compositeUniforms ) ) {
		post.failed = qtrue;
		return;
	}

	Post_EnsureTargets( width, height );

	// the view, as the effects get it
	Post_BindTexture( 0, post.sceneTexture );
	qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, x, y, width, height );

	qglMatrixMode( GL_PROJECTION );
	qglPushMatrix();
	qglLoadIdentity();
	qglMatrixMode( GL_MODELVIEW );
	qglPushMatrix();
	qglLoadIdentity();

	GL_State( GLS_DEPTHTEST_DISABLE );
	GL_Cull( CT_TWO_SIDED );
	qglDisable( GL_CLIP_PLANE0 );
	qglDisable( GL_SCISSOR_TEST );

	ssao = (qboolean)( r_ssao->integer && Post_Ssao() );
	if ( r_ssao->integer && !ssao ) {
		post.failed = qtrue;
	}

	bloom = (qboolean)( r_bloom->integer && Post_Bloom() );
	if ( r_bloom->integer && !bloom ) {
		post.failed = qtrue;
	}

	smaa = (qboolean)( r_smaa->integer && post.smaaOk && Post_SmaaReady() );
	if ( r_smaa->integer && post.smaaOk && !smaa ) {
		post.failed = qtrue;
	}

	// draw the result over the view, or into the picture SMAA works on
	if ( smaa ) {
		qglBindFramebuffer( GL_FRAMEBUFFER, post.final.fbo );
		qglViewport( 0, 0, width, height );
	} else {
		qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
		qglViewport( x, y, width, height );
	}
	Post_BindTexture( 0, post.sceneTexture );
	Post_BindTexture( 1, post.aoA.texture );
	Post_BindTexture( 2, post.bloom[0].texture );
	qglUseProgram( post.composite.id );
	qglUniform1i( post.composite.uniforms[0], 0 );
	qglUniform1i( post.composite.uniforms[1], 1 );
	qglUniform1i( post.composite.uniforms[2], 2 );
	qglUniform4fv( post.composite.uniforms[3], 1, smaa ? smaaRect : rect );
	qglUniform3f( post.composite.uniforms[4], ssao ? 1.0f : 0.0f, r_postDebug->integer == 2 ? 1.0f : 0.0f,
		bloom ? r_bloomIntensity->value : 0.0f );
	Post_Quad();
	if ( smaa ) {
		Post_Smaa( x, y, width, height );
	}
	qglUseProgram( 0 );

	// the engine caches what is bound to its two texture units, make it look again
	Post_BindTexture( 3, 0 );
	Post_BindTexture( 2, 0 );
	Post_BindTexture( 1, 0 );
	Post_BindTexture( 0, 0 );
	glState.currenttextures[0] = glState.currenttextures[1] = 0;
	qglActiveTextureARB( GL_TEXTURE0_ARB + glState.currenttmu );

	if ( scissored ) {
		qglEnable( GL_SCISSOR_TEST );
	}
	SetViewportAndScissor();	// also sets the projection matrix, which the pops below put back anyway

	qglMatrixMode( GL_PROJECTION );
	qglPopMatrix();
	qglMatrixMode( GL_MODELVIEW );
	qglPopMatrix();
}
