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

// tr_post.h -- full screen effects that run on the finished 3D view, before the HUD is drawn, for the single player
// and the multiplayer renderers. The view is copied into textures (colour and depth), the effects run on those through
// shader programs, and the result is drawn back over the view. Nothing here runs unless GLSL and framebuffer objects
// are there and an effect is on.
//
// Included once, by the tr_post.cpp of each renderer, after its tr_local.h (backEnd, glState, GL_State, the r_ssao,
// r_bloom and r_smaa cvars, SetViewportAndScissor).

#pragma once

#include "renderer/tr_smaa_data.h"
#include "renderer/tr_light_shafts.h"
#include "renderer/tr_eye_adaptation.h"
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
static PFNGLGENBUFFERSPROC				qglGenBuffers;		// (pixel buffers, to read the eye adaptation's measurement back without waiting)
static PFNGLBINDBUFFERPROC				qglBindBuffer;
static PFNGLBUFFERDATAPROC				qglBufferData;
static PFNGLMAPBUFFERPROC				qglMapBuffer;
static PFNGLUNMAPBUFFERPROC				qglUnmapBuffer;
static PFNGLDELETEBUFFERSPROC			qglDeleteBuffers;

// a texture an effect can draw into
typedef struct {
	GLuint	texture;
	GLuint	fbo;
	int		width, height;
} postTarget_t;

typedef struct {
	GLuint	id;
	GLint	uniforms[12];
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
	postProgram_t	sharpen;			// contrast adaptive sharpening (r_sharpen)
	postTarget_t	shaftsA, shaftsB;	// light shafts (r_lightShafts): the sky near the sun, then blurred towards it
	postProgram_t	shafts;
	postTarget_t	lum, lumAverage;	// eye adaptation (r_eyeAdaptation): the brightness of the view shrunk, then one pixel
	postProgram_t	eye;
	GLuint			eyeBuffers[EYE_READ_FRAMES];	// the measurements on their way back
	int				eyeMeasured;		// how many were made
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
	"uniform float uExposure;\n"	// eye adaptation (tr_eye_adaptation.h), 1 for none
	"void main() {\n"
	"	vec2 uv = ( gl_FragCoord.xy - uRect.xy ) / uRect.zw;\n"
	"	vec3 colour = texture2D( uScene, uv ).rgb;\n"
	"	if ( uMode.x > 0.5 ) {\n"
	"		float ao = texture2D( uAO, uv ).r;\n"
	"		colour = uMode.y > 0.5 ? vec3( ao ) : colour * ao;\n"
	"	}\n"
	"	colour += texture2D( uBloom, uv ).rgb * uMode.z * ( 1.0 - colour );\n"	// like a screen blend, it does not clip what is bright already
	"	if ( uExposure != 1.0 ) {\n"
	"		colour = 1.0 - pow( clamp( 1.0 - colour, 0.0, 1.0 ), vec3( uExposure ) );\n"	// (black stays black, white white)
	"	}\n"
	// Stored in 8 bits, a smooth gradient worked out here (a bloom halo, a dark room the eye adaptation brightens)
	// would show its steps: a noise of up to one step on either side (two patterns, a triangle-shaped spread) turns
	// them into an even grain too fine to see. Black is left black.
	"	vec2 at = gl_FragCoord.xy;\n"
	"	float noise = fract( 52.9829189 * fract( dot( at, vec2( 0.06711056, 0.00583715 ) ) ) ) +\n"
	"		fract( 52.9829189 * fract( dot( at + vec2( 37.0, 17.0 ), vec2( 0.06711056, 0.00583715 ) ) ) ) - 1.0;\n"
	"	colour += noise / 255.0 * clamp( colour * 255.0, 0.0, 1.0 );\n"
	"	gl_FragColor = vec4( colour, 1.0 );\n"
	"}\n";

static void Post_SetAvailable( const char *name, qboolean available ) {
	ri.Cvar_Set( name, available ? "1" : "0" );	// (made if it is not there yet)	// what the menus look at to show the rows for what needs it
}

static void Gamma_Reset( void );
static void ShadowMaps_Reset( void );

void R_Post_Init( void ) {
	memset( &post, 0, sizeof( post ) );
	Gamma_Reset();
	ShadowMaps_Reset();
	Post_SetAvailable( "r_postAvail", qfalse );
	Post_SetAvailable( "r_smaaAvail", qfalse );
	Post_SetAvailable( "r_rtAvail", qfalse );	// (ray tracing is the Vulkan renderer's)
	Post_SetAvailable( "r_softAvail", qfalse );	// (and so are the soft particles)

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

	qglGenBuffers		= (PFNGLGENBUFFERSPROC)		ri.GL_GetProcAddress( "glGenBuffers" );
	qglBindBuffer		= (PFNGLBINDBUFFERPROC)		ri.GL_GetProcAddress( "glBindBuffer" );
	qglBufferData		= (PFNGLBUFFERDATAPROC)		ri.GL_GetProcAddress( "glBufferData" );
	qglMapBuffer		= (PFNGLMAPBUFFERPROC)		ri.GL_GetProcAddress( "glMapBuffer" );
	qglUnmapBuffer		= (PFNGLUNMAPBUFFERPROC)	ri.GL_GetProcAddress( "glUnmapBuffer" );
	qglDeleteBuffers	= (PFNGLDELETEBUFFERSPROC)	ri.GL_GetProcAddress( "glDeleteBuffers" );

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
	Post_FreeTarget( &post.shaftsA );
	Post_FreeTarget( &post.shaftsB );
	Post_FreeTarget( &post.lum );
	Post_FreeTarget( &post.lumAverage );
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
	Post_NewTarget( &post.shaftsA, Q_max( width / LIGHT_SHAFTS_SCALE, 2 ), Q_max( height / LIGHT_SHAFTS_SCALE, 2 ) );
	Post_NewTarget( &post.shaftsB, Q_max( width / LIGHT_SHAFTS_SCALE, 2 ), Q_max( height / LIGHT_SHAFTS_SCALE, 2 ) );
	Post_NewTarget( &post.lum, EYE_LUM_SIZE, EYE_LUM_SIZE );
	Post_NewTarget( &post.lumAverage, 1, 1 );
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
	return (qboolean)( r_ssao->integer || r_bloom->integer || r_smaa->integer || r_postDebug->integer || r_sharpen->value > 0.0f ||
		r_lightShafts->value > 0.0f || r_eyeAdaptation->value > 0.0f );
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
Light shafts (r_lightShafts, see tr_light_shafts.h)

One program for the three passes (uMode): 0 the sky near the sun (where the depth is that of the sky, brighter parts
counting more), 1 that blurred along the line to the sun, fading with the distance, 2 the result as it is, to be added
over the view.
================
*/
static const char *postShaftsFragmentSource =
	"#version 120\n"
	"uniform sampler2D uTex;\n"	// the view (0), the sky near the sun (1), the rays (2)
	"uniform sampler2D uDepth;\n"
	"uniform vec4 uSun;\n"		// xy: the sun on the view, z: the width of the view over its height, w: strength
	"uniform float uMode;\n"
	"void main() {\n"
	"	vec2 uv = gl_TexCoord[0].xy;\n"
	"	if ( uMode < 0.5 ) {\n"
	"		float sky = texture2D( uDepth, uv ).r >= 0.99999 ? 1.0 : 0.0;\n"
	"		vec2 d = ( uv - uSun.xy ) * vec2( uSun.z, 1.0 );\n"
	"		float nearSun = clamp( 1.0 - length( d ) / 0.5, 0.0, 1.0 );\n"
	"		vec3 c = max( texture2D( uTex, uv ).rgb - 0.4, 0.0 ) * 1.6667;\n"	// (only the brighter parts of the sky)
	"		gl_FragColor = vec4( c * c * ( sky * nearSun * nearSun ), 1.0 );\n"
	"	} else if ( uMode < 1.5 ) {\n"
	"		vec2 stepUv = ( uSun.xy - uv ) * ( 0.85 / 48.0 );\n"
	"		float noise = fract( 52.9829189 * fract( dot( gl_FragCoord.xy, vec2( 0.06711056, 0.00583715 ) ) ) );\n"
	"		vec2 at = uv + stepUv * noise;\n"	// (each pixel starts a little further along, so the steps don't show)
	"		vec3 sum = vec3( 0.0 );\n"
	"		float weight = 1.0;\n"
	"		for ( int i = 0; i < 48; i++ ) {\n"
	"			if ( at.x < 0.0 || at.y < 0.0 || at.x > 1.0 || at.y > 1.0 ) break;\n"	// (the sun is off the view)
	"			sum += texture2D( uTex, at ).rgb * weight;\n"
	"			weight *= 0.97;\n"
	"			at += stepUv;\n"
	"		}\n"
	"		gl_FragColor = vec4( sum * ( uSun.w * 0.035 ), 1.0 );\n"
	"	} else {\n"
	"		gl_FragColor = vec4( texture2D( uTex, uv ).rgb, 1.0 );\n"
	"	}\n"
	"}\n";

// Leaves the rays added over the view (framebuffer 0). depthCopied: the depth of the view is in post.depthTexture
// already (SSAO copied it).
static void Post_LightShafts( int x, int y, int width, int height, qboolean depthCopied ) {
	static const char *const uniforms[] = { "uTex", "uDepth", "uSun", "uMode", NULL };
	float sun[3];

	if ( !R_LightShaftsSun( sun ) ) {
		return;
	}
	if ( !post.shafts.id && !Post_BuildProgram( &post.shafts, "post light shafts", postShaftsFragmentSource, uniforms ) ) {
		post.failed = qtrue;
		return;
	}
	if ( !depthCopied ) {
		Post_BindTexture( 1, post.depthTexture );
		qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, x, y, width, height );
	}

	qglUseProgram( post.shafts.id );
	qglUniform1i( post.shafts.uniforms[0], 0 );
	qglUniform1i( post.shafts.uniforms[1], 1 );
	qglUniform4f( post.shafts.uniforms[2], sun[0], sun[1], (float)width / height, sun[2] );

	// the sky near the sun, from the view as it was before the effects
	Post_BindTexture( 0, post.sceneTexture );
	Post_BindTexture( 1, post.depthTexture );
	qglUniform1f( post.shafts.uniforms[3], 0.0f );
	Post_DrawInto( &post.shaftsA );

	// blurred towards the sun
	Post_BindTexture( 0, post.shaftsA.texture );
	qglUniform1f( post.shafts.uniforms[3], 1.0f );
	Post_DrawInto( &post.shaftsB );

	// added over the view
	qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	qglViewport( x, y, width, height );
	Post_BindTexture( 0, post.shaftsB.texture );
	qglUniform1f( post.shafts.uniforms[3], 2.0f );
	GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE );
	Post_Quad();
	GL_State( GLS_DEPTHTEST_DISABLE );
}

/*
================
Eye adaptation (r_eyeAdaptation, see tr_eye_adaptation.h)

The program measures the view in two passes (uMode): 0 the view shrunk to EYE_LUM_SIZE square, each texel the average
of the log2 of the brightness of four pixels, as 0 to 1 over the range the measurement holds; 1 that averaged into one
pixel (32 x 32 bilinear reads, each the average of four texels).
================
*/
static const char *postEyeFragmentSource =
	"#version 120\n"
	"uniform sampler2D uTex;\n"
	"uniform float uMode;\n"
	"float encoded( vec2 uv ) {\n"
	"	float lum = dot( texture2D( uTex, uv ).rgb, vec3( 0.2126, 0.7152, 0.0722 ) );\n"
	"	return clamp( ( log2( lum + 0.001 ) - ( -10.0 ) ) / 12.0, 0.0, 1.0 );\n"	// (EYE_LOG_MIN, EYE_LOG_RANGE)
	"}\n"
	"void main() {\n"
	"	vec2 uv = gl_TexCoord[0].xy;\n"
	"	float sum = 0.0;\n"
	"	if ( uMode < 0.5 ) {\n"
	"		float d = 0.25 / 64.0;\n"		// (EYE_LUM_SIZE)
	"		sum = 0.25 * ( encoded( uv + vec2( -d, -d ) ) + encoded( uv + vec2( d, -d ) ) + encoded( uv + vec2( -d, d ) ) + encoded( uv + vec2( d, d ) ) );\n"
	"	} else {\n"
	"		for ( int y = 0; y < 32; y++ ) {\n"
	"			for ( int x = 0; x < 32; x++ ) {\n"
	"				sum += texture2D( uTex, ( vec2( float( x ), float( y ) ) * 2.0 + 1.0 ) / 64.0 ).r;\n"
	"			}\n"
	"		}\n"
	"		sum /= 1024.0;\n"
	"	}\n"
	"	gl_FragColor = vec4( sum, sum, sum, 1.0 );\n"
	"}\n";

#if EYE_LUM_SIZE != 64 || EYE_LOG_MIN != -10 || EYE_LOG_RANGE != 12
#error "the eye adaptation shaders have its sizes written in"
#endif

// Measures the view (in post.sceneTexture), reads back the measurement of EYE_READ_FRAMES frames ago, and returns the
// exposure for the view, 1 when there is none.
static float Post_EyeAdaptation( void ) {
	static const char *const uniforms[] = { "uTex", "uMode", NULL };
	int measured = -1;
	int i;

	if ( r_eyeAdaptation->value <= 0.0f || !qglGenBuffers || !qglBindBuffer || !qglBufferData || !qglMapBuffer ||
		 !qglUnmapBuffer ) {
		post.eyeMeasured = 0;
		return R_EyeAdaptation_Exposure( -1 );
	}
	if ( !post.eye.id && !Post_BuildProgram( &post.eye, "post eye adaptation", postEyeFragmentSource, uniforms ) ) {
		post.failed = qtrue;
		return 1.0f;
	}
	if ( !post.eyeBuffers[0] ) {
		qglGenBuffers( EYE_READ_FRAMES, post.eyeBuffers );
		for ( i = 0; i < EYE_READ_FRAMES; i++ ) {
			qglBindBuffer( GL_PIXEL_PACK_BUFFER, post.eyeBuffers[i] );
			qglBufferData( GL_PIXEL_PACK_BUFFER, 4, NULL, GL_STREAM_READ );
		}
		post.eyeMeasured = 0;
	}

	// the oldest measurement, before its buffer takes the new one
	const int slot = post.eyeMeasured % EYE_READ_FRAMES;
	qglBindBuffer( GL_PIXEL_PACK_BUFFER, post.eyeBuffers[slot] );
	if ( post.eyeMeasured >= EYE_READ_FRAMES ) {
		const byte *pixel = (const byte *)qglMapBuffer( GL_PIXEL_PACK_BUFFER, GL_READ_ONLY );
		if ( pixel ) {
			measured = pixel[0];
			qglUnmapBuffer( GL_PIXEL_PACK_BUFFER );
		}
	}
	qglBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );

	// the new one
	qglUseProgram( post.eye.id );
	qglUniform1i( post.eye.uniforms[0], 0 );
	Post_BindTexture( 0, post.sceneTexture );
	qglUniform1f( post.eye.uniforms[1], 0.0f );
	Post_DrawInto( &post.lum );
	Post_BindTexture( 0, post.lum.texture );
	qglUniform1f( post.eye.uniforms[1], 1.0f );
	Post_DrawInto( &post.lumAverage );
	qglBindBuffer( GL_PIXEL_PACK_BUFFER, post.eyeBuffers[slot] );
	qglReadPixels( 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, 0 );
	qglBindBuffer( GL_PIXEL_PACK_BUFFER, 0 );
	qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	qglUseProgram( 0 );
	post.eyeMeasured++;

	return R_EyeAdaptation_Exposure( measured );
}

/*
================
RB_PostProcess

Called when the 3D view is finished. Leaves the GL state as it found it, so the 2D drawing after it does not
notice.
================
*/
/*
================
Contrast adaptive sharpening (r_sharpen)

The method of AMD's FidelityFX CAS (GPUOpen, MIT license), written here from its description: each pixel is pulled
away from its four neighbours by an amount that depends on how much room the 3 x 3 pixels around it leave before they
would clip, so flat and busy areas get the most and edges that are already sharp the least, and nothing is pushed past
black or white. r_sharpen is its sharpness, 0 (off) to 1. It runs on the finished 3D view (after SMAA), not the HUD.
================
*/
static const char *postSharpenFragmentSource =
	"#version 120\n"
	"uniform sampler2D uScene;\n"
	"uniform vec3 uParams;\n"		// the size of a texel, the weight of the neighbours at full amount (negative)
	"vec3 at( float x, float y ) { return texture2D( uScene, gl_TexCoord[0].xy + vec2( x, y ) * uParams.xy ).rgb; }\n"
	"void main() {\n"
	"	vec3 a = at( -1.0, -1.0 ), b = at( 0.0, -1.0 ), c = at( 1.0, -1.0 );\n"
	"	vec3 d = at( -1.0, 0.0 ), e = at( 0.0, 0.0 ), f = at( 1.0, 0.0 );\n"
	"	vec3 g = at( -1.0, 1.0 ), h = at( 0.0, 1.0 ), i = at( 1.0, 1.0 );\n"
	"	vec3 mn = min( min( min( d, e ), min( f, b ) ), h );\n"
	"	vec3 mx = max( max( max( d, e ), max( f, b ) ), h );\n"
	"	mn += min( min( min( mn, a ), min( g, c ) ), i );\n"		// (the cross and the whole square, soft)
	"	mx += max( max( max( mx, a ), max( g, c ) ), i );\n"
	"	vec3 amount = sqrt( clamp( min( mn, 2.0 - mx ) / max( mx, vec3( 0.0001 ) ), 0.0, 1.0 ) );\n"
	"	vec3 w = amount * uParams.z;\n"
	"	gl_FragColor = vec4( clamp( ( ( b + d + f + h ) * w + e ) / ( 1.0 + 4.0 * w ), 0.0, 1.0 ), 1.0 );\n"
	"}\n";

static void Post_Sharpen( int x, int y, int width, int height ) {
	static const char *const uniforms[] = { "uScene", "uParams", NULL };
	const float sharpness = ( r_sharpen->value > 1.0f ) ? 1.0f : r_sharpen->value;

	if ( !post.sharpen.id && !Post_BuildProgram( &post.sharpen, "post sharpen", postSharpenFragmentSource, uniforms ) ) {
		post.failed = qtrue;
		return;
	}

	// the view as it is now (with the other effects on it), then drawn over itself sharpened
	Post_BindTexture( 0, post.sceneTexture );
	qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, x, y, width, height );
	qglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	qglViewport( x, y, width, height );
	qglUseProgram( post.sharpen.id );
	qglUniform1i( post.sharpen.uniforms[0], 0 );
	qglUniform3f( post.sharpen.uniforms[1], 1.0f / width, 1.0f / height, -1.0f / ( 8.0f - 3.0f * sharpness ) );
	Post_Quad();
}

void RB_PostProcess( void ) {
	static const char *const compositeUniforms[] = { "uScene", "uAO", "uBloom", "uRect", "uMode", "uExposure", NULL };
	const int	x = backEnd.viewParms.viewportX;
	const int	y = backEnd.viewParms.viewportY;
	const int	width = backEnd.viewParms.viewportWidth;
	const int	height = backEnd.viewParms.viewportHeight;
	const float	rect[4] = { (float)x, (float)y, (float)width, (float)height };
	const float	smaaRect[4] = { 0.0f, 0.0f, (float)width, (float)height };
	const GLboolean	scissored = qglIsEnabled( GL_SCISSOR_TEST );
	qboolean	ssao, bloom, smaa;
	float		exposure;

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

	exposure = Post_EyeAdaptation();

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

	// draw the result over the view, or into the picture SMAA works on (when only sharpening is on there is nothing to
	// draw: the view is as it was)
	if ( ssao || bloom || smaa || r_postDebug->integer || exposure != 1.0f ) {
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
		qglUniform1f( post.composite.uniforms[5], exposure );
		Post_Quad();
		if ( smaa ) {
			Post_Smaa( x, y, width, height );
		}
	}
	if ( r_lightShafts->value > 0.0f ) {
		Post_LightShafts( x, y, width, height, ssao );
	}
	if ( r_sharpen->value > 0.0f ) {
		Post_Sharpen( x, y, width, height );
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

/*
=============================================================================

BRIGHTNESS AND OVERBRIGHT (r_gamma, r_overBrightBits)

The original games set the brightness with the gamma ramps of the graphics card, which applied to everything on the
screen, and they used the same ramps for overbright lighting: with r_overBrightBits 1 the game draws at half the
brightness and the ramps double it, so that lighting can go up to twice as bright as white. SDL3 has no gamma ramps,
so without this the brightness was baked into the textures when they were loaded (only the textures, and only after a
restart) and overbright was off. This does what the ramps did, in a pass over the finished picture before it is shown,
with the same table (R_SetColorMappings), rounding included; the engine then treats it as hardware gamma
(glConfig.deviceSupportsGamma): textures are loaded as they are, overbright works (in a window too, since it changes
only the game's picture) and screenshots get the table applied as they are saved. At brightness 1 without overbright
(the defaults) it does nothing.
=============================================================================
*/

static struct {
	qboolean		failed;
	postProgram_t	program;
	GLuint			texture;
	int				width, height;
} gammaPass;

static void Gamma_Reset( void ) {
	memset( &gammaPass, 0, sizeof( gammaPass ) );	// (the textures and programs went with the context)
}

static const char *gammaFragmentSource =
	"#version 120\n"
	"uniform sampler2D uScene;\n"
	"uniform vec2 uParams;\n"		// 1 / gamma, 2 ^ overbright
	"void main() {\n"
	"	vec3 c = floor( texture2D( uScene, gl_TexCoord[0].xy ).rgb * 255.0 + 0.5 );\n"
	"	c = floor( 255.0 * pow( c / 255.0, vec3( uParams.x ) ) + 0.5 ) * uParams.y;\n"
	"	gl_FragColor = vec4( min( c, vec3( 255.0 ) ) / 255.0, 1.0 );\n"
	"}\n";

qboolean R_Post_GammaAvailable( void ) {
	return (qboolean)( R_GLSL_Available() && qglActiveTextureARB && qglMultiTexCoord2fARB );
}

void RB_GammaPass( void ) {
	static const char *const uniforms[] = { "uScene", "uParams", NULL };
	const float	gamma = r_gamma->value;
	const int	overbright = tr.overbrightBits;
	const int	width = glConfig.vidWidth;
	const int	height = glConfig.vidHeight;

	if ( gammaPass.failed || !glConfig.deviceSupportsGamma || ( gamma == 1.0f && overbright <= 0 ) ) {
		return;
	}
	if ( !gammaPass.program.id && !Post_BuildProgram( &gammaPass.program, "gamma", gammaFragmentSource, uniforms ) ) {
		gammaPass.failed = qtrue;
		return;
	}
	if ( !gammaPass.texture || gammaPass.width != width || gammaPass.height != height ) {
		if ( gammaPass.texture ) {
			qglDeleteTextures( 1, &gammaPass.texture );
		}
		gammaPass.texture = Post_NewTexture( GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, width, height );
		gammaPass.width = width;
		gammaPass.height = height;
	}

	// the picture as it is, then drawn over itself through the table
	Post_BindTexture( 0, gammaPass.texture );
	qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height );

	qglMatrixMode( GL_PROJECTION );
	qglPushMatrix();
	qglLoadIdentity();
	qglMatrixMode( GL_MODELVIEW );
	qglPushMatrix();
	qglLoadIdentity();
	qglViewport( 0, 0, width, height );
	GL_State( GLS_DEPTHTEST_DISABLE );
	GL_Cull( CT_TWO_SIDED );
	qglDisable( GL_CLIP_PLANE0 );
	qglDisable( GL_SCISSOR_TEST );

	qglUseProgram( gammaPass.program.id );
	qglUniform1i( gammaPass.program.uniforms[0], 0 );
	qglUniform2f( gammaPass.program.uniforms[1], 1.0f / gamma, (float)( 1 << overbright ) );
	Post_Quad();
	qglUseProgram( 0 );

	Post_BindTexture( 0, 0 );
	glState.currenttextures[0] = glState.currenttextures[1] = 0;	// (the engine caches what is bound)
	qglMatrixMode( GL_PROJECTION );
	qglPopMatrix();
	qglMatrixMode( GL_MODELVIEW );
	qglPopMatrix();
}

/*
=============================================================================

SHADOW MAPS OF THE CHARACTERS (cg_shadows 4, see tr_shadow_volumes.h)

For every caster, a pass over the 3D view: the position of each pixel from the depth, where it is in the caster's tile,
and how many of a few samples around that place have the caster above the point; the pixel is darkened by that share
of the caster's darkness (multiplied into the picture, so where shadows overlap they darken more). The samples spread
the wider the further the point is below the caster, which makes the soft edge. Pixels of the characters are left
alone through the stencil.

=============================================================================
*/

static struct {
	qboolean		failed;
	postProgram_t	program;
	GLuint			atlas, depth;
	int				width, height;
} shadowPass;

static void ShadowMaps_Reset( void ) {
	memset( &shadowPass, 0, sizeof( shadowPass ) );
	ri.Cvar_Set( "r_shadowMapsAvail", R_ShadowMaps_Available() ? "1" : "0" );	// (what the menus look at)
}

// (ray traced shadows are the Vulkan renderer's, vk_post.cpp)
qboolean R_RayTracedShadows_Available( void ) {
	return qfalse;
}

int R_RayTracing( void ) {
	return 0;
}

qboolean R_DlightShadows( void ) {
	return qfalse;
}

void RB_RayTracedShadowsApply( const shadowMapCaster_t *casters, int numCasters, const float *xyz, int numPoints,
							   const int *indexes, int numTriangles, int stencilSkip ) {
}

// (the soft particles are the Vulkan renderer's only, so far)
void RB_SoftParticlesBegin( void ) {
}

qboolean RB_SoftParticles( float units ) {
	return qfalse;
}

qboolean R_ShadowMaps_Available( void ) {
	return (qboolean)( R_GLSL_Available() && qglActiveTextureARB && qglMultiTexCoord2fARB && !shadowPass.failed );
}

#if SHADOWMAP_ATLAS != 512 || SHADOWMAP_TILES != 4 || SHADOWMAP_TILE != 128
#error "the shadow map shaders have the sizes of the atlas written in"
#endif
static const char *shadowMapFragmentSource =
	"#version 120\n"
	POST_VIEWPOS_SOURCE
	"uniform sampler2D uAtlas;\n"
	"uniform vec4 uRowS, uRowT, uRowN, uRowZ;\n"
	"uniform vec4 uParams;\n"		// the floor, darkness, bias, softness (texels per normalized height)
	"uniform vec2 uTile;\n"		// the corner of the caster's tile
	"const float texel = 1.0 / 512.0;\n"		// (SHADOWMAP_ATLAS, SHADOWMAP_TILES, SHADOWMAP_TILE)
	"float casterAt( vec2 st, vec2 lo, vec2 hi ) {\n"		// the normalized height of the caster there, -1 if none
	"	float r = texture2D( uAtlas, clamp( st, lo, hi ) ).r * 255.0;\n"
	"	return r < 0.5 ? -1.0 : ( r - 1.0 ) / 254.0;\n"
	"}\n"
	"void main() {\n"
	"	vec2 uv = gl_TexCoord[0].xy;\n"
	"	float depth = texture2D( uDepth, uv ).r;\n"
	"	if ( depth >= 1.0 || depth < 0.3 ) { gl_FragColor = vec4( 1.0 ); return; }\n"	// the sky, the weapon
	"	vec4 e = vec4( viewPos( uv ), 1.0 );\n"
	"	if ( dot( uRowZ, e ) < uParams.x ) { gl_FragColor = vec4( 1.0 ); return; }\n"
	"	vec2 st = vec2( dot( uRowS, e ), dot( uRowT, e ) );\n"
	"	vec2 lo = uTile + 0.5 * texel;\n"
	"	vec2 hi = uTile + ( 128.0 - 0.5 ) * texel;\n"
	"	if ( st.x < lo.x || st.y < lo.y || st.x > hi.x || st.y > hi.y ) { gl_FragColor = vec4( 1.0 ); return; }\n"
	"	float n = dot( uRowN, e );\n"
	// how far below the caster the point is, from a few samples of what is above it
	"	float blocker = 0.0, found = 0.0;\n"
	"	for ( int i = 0; i < 5; i++ ) {\n"
	"		vec2 o = vec2( float( i == 1 ) - float( i == 2 ), float( i == 3 ) - float( i == 4 ) ) * 6.0;\n"
	"		float c = casterAt( st + o * texel, lo, hi );\n"
	"		if ( c >= 0.0 && n < c - uParams.z ) { blocker += c; found += 1.0; }\n"
	"	}\n"
	"	if ( found < 0.5 ) { gl_FragColor = vec4( 1.0 ); return; }\n"
	"	float radius = clamp( 0.75 + ( blocker / found - n ) * uParams.w, 0.75, 10.0 );\n"
	"	float shadow = 0.0;\n"
	"	for ( int i = 0; i < 12; i++ ) {\n"		// a spiral of samples
	"		float a = float( i ) * 2.39996323, r = sqrt( ( float( i ) + 0.5 ) / 12.0 ) * radius;\n"
	"		float c = casterAt( st + vec2( cos( a ), sin( a ) ) * r * texel, lo, hi );\n"
	"		if ( c >= 0.0 && n < c - uParams.z ) shadow += 1.0;\n"
	"	}\n"
	"	float f = 1.0 - uParams.y * shadow / 12.0;\n"
	"	gl_FragColor = vec4( f, f, f, 1.0 );\n"
	"}\n";

void RB_ShadowMapsApply( const shadowMapCaster_t *casters, int numCasters, const byte *atlas, int stencilSkip ) {
	static const char *const uniforms[] = { "uDepth", "uProjA", "uProjB", "uAtlas", "uRowS", "uRowT", "uRowN", "uRowZ", "uParams", "uTile", NULL };
	postProgram_t	*program = &shadowPass.program;
	const float		*proj = backEnd.viewParms.projectionMatrix;
	const int		x = backEnd.viewParms.viewportX;
	const int		y = backEnd.viewParms.viewportY;
	const int		width = backEnd.viewParms.viewportWidth;
	const int		height = backEnd.viewParms.viewportHeight;
	int				i;

	if ( shadowPass.failed || numCasters <= 0 ) {
		return;
	}
	if ( !program->id && !Post_BuildProgram( program, "shadow maps", shadowMapFragmentSource, uniforms ) ) {
		shadowPass.failed = qtrue;
		return;
	}

	// the atlas, and the depth of the view
	if ( !shadowPass.atlas ) {
		shadowPass.atlas = Post_NewTexture( GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, SHADOWMAP_ATLAS, SHADOWMAP_ATLAS );
	}
	Post_BindTexture( 1, shadowPass.atlas );
	qglTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, SHADOWMAP_ATLAS, SHADOWMAP_ATLAS, GL_RGBA, GL_UNSIGNED_BYTE, atlas );
	if ( !shadowPass.depth || shadowPass.width != width || shadowPass.height != height ) {
		if ( shadowPass.depth ) {
			qglDeleteTextures( 1, &shadowPass.depth );
		}
		shadowPass.depth = Post_NewTexture( GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, width, height );
		shadowPass.width = width;
		shadowPass.height = height;
	}
	Post_BindTexture( 0, shadowPass.depth );
	qglCopyTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, x, y, width, height );

	qglMatrixMode( GL_PROJECTION );
	qglPushMatrix();
	qglLoadIdentity();
	qglMatrixMode( GL_MODELVIEW );
	qglPushMatrix();
	qglLoadIdentity();
	qglViewport( x, y, width, height );
	const GLboolean scissored = qglIsEnabled( GL_SCISSOR_TEST );
	const GLboolean clipped = qglIsEnabled( GL_CLIP_PLANE0 );
	qglEnable( GL_SCISSOR_TEST );	// (each caster only where its shadow can be)
	qglDisable( GL_CLIP_PLANE0 );
	GL_Cull( CT_TWO_SIDED );
	GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ZERO | GLS_DSTBLEND_SRC_COLOR );	// (multiplied into the picture)
	if ( stencilSkip ) {
		qglEnable( GL_STENCIL_TEST );
		qglStencilMask( 0 );
		qglStencilFunc( GL_EQUAL, 0, stencilSkip );
		qglStencilOp( GL_KEEP, GL_KEEP, GL_KEEP );
	} else {
		qglDisable( GL_STENCIL_TEST );
	}

	qglUseProgram( program->id );
	qglUniform1i( program->uniforms[0], 0 );
	qglUniform4f( program->uniforms[1], proj[0], proj[5], proj[8], proj[9] );
	qglUniform2f( program->uniforms[2], proj[10], proj[14] );
	qglUniform1i( program->uniforms[3], 1 );
	for ( i = 0; i < numCasters; i++ ) {
		const shadowMapCaster_t *c = &casters[i];
		const int left = (int)floorf( c->rect[0] * width ), bottom = (int)floorf( c->rect[1] * height );
		const int right = (int)ceilf( c->rect[2] * width ), top = (int)ceilf( c->rect[3] * height );
		if ( right <= left || top <= bottom ) {
			continue;
		}
		qglScissor( x + left, y + bottom, right - left, top - bottom );
		qglUniform4fv( program->uniforms[4], 1, c->rowS );
		qglUniform4fv( program->uniforms[5], 1, c->rowT );
		qglUniform4fv( program->uniforms[6], 1, c->rowN );
		qglUniform4fv( program->uniforms[7], 1, c->rowZ );
		qglUniform4f( program->uniforms[8], c->floorZ, c->darkness, c->bias, c->softness );
		qglUniform2f( program->uniforms[9], c->tile[0], c->tile[1] );
		Post_Quad();
	}
	qglUseProgram( 0 );

	Post_BindTexture( 1, 0 );
	Post_BindTexture( 0, 0 );
	glState.currenttextures[0] = glState.currenttextures[1] = 0;	// (the engine caches what is bound)
	if ( !scissored ) {
		qglDisable( GL_SCISSOR_TEST );
	}
	if ( clipped ) {
		qglEnable( GL_CLIP_PLANE0 );
	}
	qglMatrixMode( GL_PROJECTION );
	qglPopMatrix();
	qglMatrixMode( GL_MODELVIEW );
	qglPopMatrix();
	SetViewportAndScissor();
}
