/*
===========================================================================
Copyright (C) 2013 - 2018, OpenJK contributors

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

// The fixed function OpenGL calls of the original renderer, answered on top of Vulkan (docs/vulkan-renderer.md).
// The calls only record state (blend, depth, stencil, texture units, matrices, arrays...). A draw turns the state
// into a pipeline and the vertex arrays into a buffer (vk_draw.cpp). Calls that are not implemented yet do nothing.

#include "../server/exe_headers.h"
#include "../rd-vanilla/tr_local.h"
#include "vk_priv.h"
#include <vector>

#define MAX_UNITS		4
#define MATRIX_DEPTH	32

typedef struct clientArray_s {
	bool			enabled;
	int				size;
	GLenum			type;
	int				stride;
	const void		*pointer;
} clientArray_t;

static struct vkglState_s {
	// switches
	bool			blend, depthTest, cullFace, alphaTest, stencilTest, scissorTest, polyOffsetFill, polyOffsetLine, fog, clip0;
	bool			texture2D[MAX_UNITS];

	// functions
	GLenum			srcBlend, dstBlend;
	GLenum			depthFunc;
	bool			depthMask;
	GLenum			cullFaceMode;
	GLenum			alphaFunc;
	float			alphaRef;
	bool			wireframe;
	GLenum			stencilFunc;
	GLint			stencilRef;
	GLuint			stencilCompareMask, stencilWriteMask;
	GLenum			stencilFail[2], stencilZFail[2], stencilZPass[2];	// front, back
	unsigned char	colorMask;
	float			polyOffsetFactor, polyOffsetUnits;
	GLenum			texEnv[MAX_UNITS];
	float			depthRange[2];
	int				viewport[4];
	int				scissor[4];

	// fog
	GLenum			fogMode;
	float			fogDensity, fogStart, fogEnd;
	float			fogColor[4];
	float			clipPlaneEye[4];

	// textures
	GLuint			bound[MAX_UNITS];
	int				activeUnit;
	int				clientUnit;

	// arrays
	clientArray_t	vertexArray, colorArray, texCoordArray[MAX_UNITS];

	// current values
	float			color[4];
	float			texCoord[MAX_UNITS][2];

	// matrices
	GLenum			matrixMode;
	float			modelview[16], projection[16];
	float			modelviewStack[MATRIX_DEPTH][16], projectionStack[MATRIX_DEPTH][16];
	int				modelviewDepth, projectionDepth;
} gl;

static GLuint	nextGeneratedTexture = 1u << 24;

static const float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

/*
=============================================================================

MATRICES (OpenGL's, column major)

=============================================================================
*/

static void MatrixMultiply( const float *a, const float *b, float *out )	// out = a * b
{
	float result[16];
	for ( int col = 0; col < 4; col++ )
	{
		for ( int row = 0; row < 4; row++ )
		{
			float sum = 0.0f;
			for ( int k = 0; k < 4; k++ )
			{
				sum += a[k * 4 + row] * b[col * 4 + k];
			}
			result[col * 4 + row] = sum;
		}
	}
	memcpy( out, result, sizeof( result ) );
}

static bool MatrixInvert( const float *m, float *out )
{
	float inv[16];
	inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
	inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
	inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
	inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
	inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
	inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
	inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
	inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
	inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
	inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
	inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
	inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
	inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
	inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
	inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
	inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
	const float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
	if ( det == 0.0f )
	{
		return false;
	}
	for ( int i = 0; i < 16; i++ )
	{
		out[i] = inv[i] / det;
	}
	return true;
}

static float *CurrentMatrix( void )
{
	return gl.matrixMode == GL_PROJECTION ? gl.projection : gl.modelview;
}

void vkglMatrixMode( GLenum mode )
{
	gl.matrixMode = mode;
}

void vkglLoadIdentity( void )
{
	memcpy( CurrentMatrix(), identity, sizeof( identity ) );
}

void vkglLoadMatrixf( const GLfloat *m )
{
	memcpy( CurrentMatrix(), m, 16 * sizeof( float ) );
}

void vkglOrtho( GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble nearVal, GLdouble farVal )
{
	float ortho[16] = {};
	ortho[0] = (float)( 2.0 / ( right - left ) );
	ortho[5] = (float)( 2.0 / ( top - bottom ) );
	ortho[10] = (float)( -2.0 / ( farVal - nearVal ) );
	ortho[12] = (float)( -( right + left ) / ( right - left ) );
	ortho[13] = (float)( -( top + bottom ) / ( top - bottom ) );
	ortho[14] = (float)( -( farVal + nearVal ) / ( farVal - nearVal ) );
	ortho[15] = 1.0f;
	MatrixMultiply( CurrentMatrix(), ortho, CurrentMatrix() );
}

void vkglTranslatef( GLfloat x, GLfloat y, GLfloat z )
{
	float translation[16];
	memcpy( translation, identity, sizeof( identity ) );
	translation[12] = x;
	translation[13] = y;
	translation[14] = z;
	MatrixMultiply( CurrentMatrix(), translation, CurrentMatrix() );
}

void vkglPushMatrix( void )
{
	if ( gl.matrixMode == GL_PROJECTION )
	{
		if ( gl.projectionDepth < MATRIX_DEPTH ) memcpy( gl.projectionStack[gl.projectionDepth++], gl.projection, sizeof( gl.projection ) );
	}
	else
	{
		if ( gl.modelviewDepth < MATRIX_DEPTH ) memcpy( gl.modelviewStack[gl.modelviewDepth++], gl.modelview, sizeof( gl.modelview ) );
	}
}

void vkglPopMatrix( void )
{
	if ( gl.matrixMode == GL_PROJECTION )
	{
		if ( gl.projectionDepth > 0 ) memcpy( gl.projection, gl.projectionStack[--gl.projectionDepth], sizeof( gl.projection ) );
	}
	else
	{
		if ( gl.modelviewDepth > 0 ) memcpy( gl.modelview, gl.modelviewStack[--gl.modelviewDepth], sizeof( gl.modelview ) );
	}
}

/*
=============================================================================

STATE

=============================================================================
*/

static void SetSwitch( GLenum cap, bool on )
{
	switch ( cap )
	{
	case GL_BLEND:				gl.blend = on; break;
	case GL_DEPTH_TEST:			gl.depthTest = on; break;
	case GL_CULL_FACE:			gl.cullFace = on; break;
	case GL_ALPHA_TEST:			gl.alphaTest = on; break;
	case GL_STENCIL_TEST:		gl.stencilTest = on; break;
	case GL_SCISSOR_TEST:		gl.scissorTest = on; break;
	case GL_POLYGON_OFFSET_FILL:	gl.polyOffsetFill = on; break;
	case GL_POLYGON_OFFSET_LINE:	gl.polyOffsetLine = on; break;
	case GL_FOG:				gl.fog = on; break;
	case GL_CLIP_PLANE0:		gl.clip0 = on; break;
	case GL_TEXTURE_2D:			gl.texture2D[gl.activeUnit] = on; break;
	default:					break;	// (multisampling, ARB programs, rectangle textures: not here)
	}
}

void vkglEnable( GLenum cap )
{
	SetSwitch( cap, true );
}

void vkglDisable( GLenum cap )
{
	SetSwitch( cap, false );
}

GLboolean vkglIsEnabled( GLenum cap )
{
	switch ( cap )
	{
	case GL_BLEND:				return gl.blend;
	case GL_DEPTH_TEST:			return gl.depthTest;
	case GL_CULL_FACE:			return gl.cullFace;
	case GL_ALPHA_TEST:			return gl.alphaTest;
	case GL_STENCIL_TEST:		return gl.stencilTest;
	case GL_SCISSOR_TEST:		return gl.scissorTest;
	case GL_FOG:				return gl.fog;
	case GL_CLIP_PLANE0:		return gl.clip0;
	case GL_TEXTURE_2D:			return gl.texture2D[gl.activeUnit];
	}
	return GL_FALSE;
}

void vkglBlendFunc( GLenum sfactor, GLenum dfactor )
{
	gl.srcBlend = sfactor;
	gl.dstBlend = dfactor;
}

void vkglDepthFunc( GLenum func )
{
	gl.depthFunc = func;
}

void vkglDepthMask( GLboolean flag )
{
	gl.depthMask = flag != GL_FALSE;
}

void vkglDepthRange( GLclampd nearVal, GLclampd farVal )
{
	gl.depthRange[0] = (float)nearVal;
	gl.depthRange[1] = (float)farVal;
}

void vkglCullFace( GLenum mode )
{
	gl.cullFaceMode = mode;
}

void vkglAlphaFunc( GLenum func, GLclampf ref )
{
	gl.alphaFunc = func;
	gl.alphaRef = ref;
}

void vkglPolygonMode( GLenum face, GLenum mode )
{
	gl.wireframe = mode == GL_LINE;
}

void vkglPolygonOffset( GLfloat factor, GLfloat units )
{
	gl.polyOffsetFactor = factor;
	gl.polyOffsetUnits = units;
}

void vkglColorMask( GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha )
{
	gl.colorMask = (unsigned char)( ( red ? 1 : 0 ) | ( green ? 2 : 0 ) | ( blue ? 4 : 0 ) | ( alpha ? 8 : 0 ) );
}

void vkglStencilFunc( GLenum func, GLint ref, GLuint mask )
{
	gl.stencilFunc = func;
	gl.stencilRef = ref;
	gl.stencilCompareMask = mask;
}

void vkglStencilMask( GLuint mask )
{
	gl.stencilWriteMask = mask;
}

void vkglStencilOp( GLenum fail, GLenum zfail, GLenum zpass )
{
	for ( int face = 0; face < 2; face++ )
	{
		gl.stencilFail[face] = fail;
		gl.stencilZFail[face] = zfail;
		gl.stencilZPass[face] = zpass;
	}
}

void vkglViewport( GLint x, GLint y, GLsizei width, GLsizei height )
{
	gl.viewport[0] = x;
	gl.viewport[1] = y;
	gl.viewport[2] = width;
	gl.viewport[3] = height;
}

void vkglScissor( GLint x, GLint y, GLsizei width, GLsizei height )
{
	gl.scissor[0] = x;
	gl.scissor[1] = y;
	gl.scissor[2] = width;
	gl.scissor[3] = height;
}

void vkglFogi( GLenum pname, GLint param )
{
	if ( pname == GL_FOG_MODE ) gl.fogMode = (GLenum)param;
}

void vkglFogf( GLenum pname, GLfloat param )
{
	switch ( pname )
	{
	case GL_FOG_MODE:		gl.fogMode = (GLenum)param; break;
	case GL_FOG_DENSITY:	gl.fogDensity = param; break;
	case GL_FOG_START:		gl.fogStart = param; break;
	case GL_FOG_END:		gl.fogEnd = param; break;
	}
}

void vkglFogfv( GLenum pname, const GLfloat *params )
{
	if ( pname == GL_FOG_COLOR )
	{
		memcpy( gl.fogColor, params, 4 * sizeof( float ) );
	}
	else
	{
		vkglFogf( pname, params[0] );
	}
}

// the plane is in eye coordinates from now on, whatever the modelview matrix does later
void vkglClipPlane( GLenum plane, const GLdouble *equation )
{
	if ( plane != GL_CLIP_PLANE0 )
	{
		return;
	}
	float inverse[16];
	if ( !MatrixInvert( gl.modelview, inverse ) )
	{
		return;
	}
	const float given[4] = { (float)equation[0], (float)equation[1], (float)equation[2], (float)equation[3] };
	for ( int c = 0; c < 4; c++ )
	{
		gl.clipPlaneEye[c] = inverse[c * 4 + 0] * given[0] + inverse[c * 4 + 1] * given[1] + inverse[c * 4 + 2] * given[2] + inverse[c * 4 + 3] * given[3];
	}
}

void vkglTexEnvf( GLenum target, GLenum pname, GLfloat param )
{
	if ( target == GL_TEXTURE_ENV && pname == GL_TEXTURE_ENV_MODE )
	{
		gl.texEnv[gl.activeUnit] = (GLenum)param;
	}
}

// (colour, shading, lines, buffers and display lists: nothing to do)
void vkglShadeModel( GLenum mode ) {}
void vkglLineWidth( GLfloat width ) {}
void vkglDrawBuffer( GLenum mode ) {}
void vkglPixelStorei( GLenum pname, GLint param ) {}
void vkglCallList( GLuint list ) {}
void vkglDeleteLists( GLuint list, GLsizei range ) {}
void vkglEndList( void ) {}
GLuint vkglGenLists( GLsizei range ) { return 0; }
void vkglNewList( GLuint list, GLenum mode ) {}
void vkglNormalPointer( GLenum type, GLsizei stride, const GLvoid *ptr ) {}

void APIENTRY vkglActiveTextureARB( GLenum texture )
{
	const int unit = (int)texture - (int)GL_TEXTURE0_ARB;
	if ( unit >= 0 && unit < MAX_UNITS ) gl.activeUnit = unit;
}

void APIENTRY vkglClientActiveTextureARB( GLenum texture )
{
	const int unit = (int)texture - (int)GL_TEXTURE0_ARB;
	if ( unit >= 0 && unit < MAX_UNITS ) gl.clientUnit = unit;
}

void APIENTRY vkglMultiTexCoord2fARB( GLenum target, GLfloat s, GLfloat t )
{
	const int unit = (int)target - (int)GL_TEXTURE0_ARB;
	if ( unit >= 0 && unit < MAX_UNITS )
	{
		gl.texCoord[unit][0] = s;
		gl.texCoord[unit][1] = t;
	}
}

// (the arrays are copied when drawn, there is nothing to lock)
void APIENTRY vkglLockArraysEXT( GLint first, GLsizei count ) {}
void APIENTRY vkglUnlockArraysEXT( void ) {}

void APIENTRY vkglStencilOpSeparate( GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass )
{
	for ( int i = 0; i < 2; i++ )
	{
		if ( ( i == 0 && face == GL_BACK ) || ( i == 1 && face == GL_FRONT ) )
		{
			continue;
		}
		gl.stencilFail[i] = sfail;
		gl.stencilZFail[i] = dpfail;
		gl.stencilZPass[i] = dppass;
	}
}

/*
=============================================================================

CURRENT VALUES, ARRAYS

=============================================================================
*/

void vkglColor3f( GLfloat red, GLfloat green, GLfloat blue )
{
	gl.color[0] = red; gl.color[1] = green; gl.color[2] = blue; gl.color[3] = 1.0f;
}

void vkglColor4f( GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha )
{
	gl.color[0] = red; gl.color[1] = green; gl.color[2] = blue; gl.color[3] = alpha;
}

void vkglColor4ub( GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha )
{
	gl.color[0] = red / 255.0f; gl.color[1] = green / 255.0f; gl.color[2] = blue / 255.0f; gl.color[3] = alpha / 255.0f;
}

void vkglColor4ubv( const GLubyte *v )
{
	vkglColor4ub( v[0], v[1], v[2], v[3] );
}

void vkglTexCoord2f( GLfloat s, GLfloat t )
{
	gl.texCoord[0][0] = s;
	gl.texCoord[0][1] = t;
}

void vkglTexCoord2fv( const GLfloat *v )
{
	gl.texCoord[0][0] = v[0];
	gl.texCoord[0][1] = v[1];
}

static void SetArrayState( GLenum array, bool on )
{
	switch ( array )
	{
	case GL_VERTEX_ARRAY:			gl.vertexArray.enabled = on; break;
	case GL_COLOR_ARRAY:			gl.colorArray.enabled = on; break;
	case GL_TEXTURE_COORD_ARRAY:	gl.texCoordArray[gl.clientUnit].enabled = on; break;
	default:						break;
	}
}

void vkglEnableClientState( GLenum array )
{
	SetArrayState( array, true );
}

void vkglDisableClientState( GLenum array )
{
	SetArrayState( array, false );
}

static void SetPointer( clientArray_t *array, GLint size, GLenum type, GLsizei stride, const GLvoid *ptr )
{
	array->size = size;
	array->type = type;
	array->stride = stride;
	array->pointer = ptr;
}

void vkglVertexPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr )
{
	SetPointer( &gl.vertexArray, size, type, stride, ptr );
}

void vkglColorPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr )
{
	SetPointer( &gl.colorArray, size, type, stride, ptr );
}

void vkglTexCoordPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr )
{
	SetPointer( &gl.texCoordArray[gl.clientUnit], size, type, stride, ptr );
}

void vkglArrayElement( GLint i ) {}

/*
=============================================================================

DRAWING

=============================================================================
*/

static void FloatsToColor( const float *rgba, byte *out )
{
	for ( int i = 0; i < 4; i++ )
	{
		const float v = Q_max( 0.0f, Q_min( 1.0f, rgba[i] ) );
		out[i] = (byte)( v * 255.0f + 0.5f );
	}
}

static int EnvCode( GLenum env )
{
	switch ( env )
	{
	case GL_REPLACE:	return 1;
	case GL_ADD:		return 2;
	case GL_DECAL:		return 3;
	}
	return 0;	// modulate
}

// everything the pipeline and the shaders are told about the state of the moment
static void PrepareDraw( vkPipelineKey_t *key, vkDynamicState_t *dynamic, vkConstants_t *constants,
	bool lines, bool colorConst, bool uv0Const, bool uv1Const )
{
	memset( key, 0, sizeof( *key ) );
	memset( dynamic, 0, sizeof( *dynamic ) );
	memset( constants, 0, sizeof( *constants ) );

	key->blendEnable = gl.blend;
	key->srcBlend = (unsigned short)gl.srcBlend;
	key->dstBlend = (unsigned short)gl.dstBlend;
	key->depthTest = gl.depthTest;
	key->depthFunc = (unsigned short)gl.depthFunc;
	key->depthWrite = gl.depthMask;
	key->cullMode = !gl.cullFace ? 0 : ( gl.cullFaceMode == GL_FRONT ? 1 : 2 );
	key->lines = lines;
	key->wireframe = gl.wireframe;
	key->polyOffset = gl.polyOffsetFill || ( gl.polyOffsetLine && gl.wireframe );
	key->alphaFunc = gl.alphaTest ? (unsigned short)gl.alphaFunc : (unsigned short)GL_ALWAYS;
	key->alphaRef = gl.alphaTest ? gl.alphaRef : 0.0f;
	key->unitMask = (unsigned char)( ( gl.texture2D[0] ? 1 : 0 ) | ( gl.texture2D[1] ? 2 : 0 ) );
	key->env0 = (unsigned char)EnvCode( gl.texEnv[0] );
	key->env1 = (unsigned char)EnvCode( gl.texEnv[1] );
	key->fogMode = !gl.fog ? 0 : ( gl.fogMode == GL_LINEAR ? 1 : 2 );
	key->clip = gl.clip0;
	key->colorMask = gl.colorMask;
	key->colorConst = colorConst;
	key->uv0Const = uv0Const;
	key->uv1Const = uv1Const;
	key->stencilTest = gl.stencilTest;
	if ( gl.stencilTest )
	{
		for ( int face = 0; face < 2; face++ )
		{
			key->stencilFunc[face] = (unsigned short)gl.stencilFunc;
			key->stencilFail[face] = (unsigned short)gl.stencilFail[face];
			key->stencilZFail[face] = (unsigned short)gl.stencilZFail[face];
			key->stencilZPass[face] = (unsigned short)gl.stencilZPass[face];
		}
	}

	for ( int i = 0; i < 4; i++ )
	{
		dynamic->viewport[i] = (float)gl.viewport[i];
	}
	dynamic->depthRange[0] = gl.depthRange[0];
	dynamic->depthRange[1] = gl.depthRange[1];
	if ( gl.scissorTest )
	{
		memcpy( dynamic->scissor, gl.scissor, sizeof( dynamic->scissor ) );
	}
	else
	{
		dynamic->scissor[2] = vk.width;
		dynamic->scissor[3] = vk.height;
	}
	dynamic->polyOffsetFactor = gl.polyOffsetFactor;
	dynamic->polyOffsetUnits = gl.polyOffsetUnits;
	for ( int face = 0; face < 2; face++ )
	{
		dynamic->stencilRef[face] = (unsigned int)gl.stencilRef;
		dynamic->stencilCompareMask[face] = gl.stencilCompareMask;
		dynamic->stencilWriteMask[face] = gl.stencilWriteMask;
	}

	// OpenGL's clip space to Vulkan's: Y down, depth 0 to 1
	static const float clipFix[16] = { 1,0,0,0, 0,-1,0,0, 0,0,0.5f,0, 0,0,0.5f,1 };
	float projection[16];
	MatrixMultiply( clipFix, gl.projection, projection );
	MatrixMultiply( projection, gl.modelview, constants->mvp );

	constants->eyeZ[0] = gl.modelview[2];
	constants->eyeZ[1] = gl.modelview[6];
	constants->eyeZ[2] = gl.modelview[10];
	constants->eyeZ[3] = gl.modelview[14];

	if ( gl.clip0 )
	{
		for ( int c = 0; c < 4; c++ )
		{
			const float *column = gl.modelview + c * 4;
			constants->clipPlane[c] = column[0] * gl.clipPlaneEye[0] + column[1] * gl.clipPlaneEye[1]
				+ column[2] * gl.clipPlaneEye[2] + column[3] * gl.clipPlaneEye[3];
		}
	}
	if ( gl.fog )
	{
		constants->fogColorDensity[0] = gl.fogColor[0];
		constants->fogColorDensity[1] = gl.fogColor[1];
		constants->fogColorDensity[2] = gl.fogColor[2];
		constants->fogColorDensity[3] = gl.fogDensity;
		constants->fogRange[0] = gl.fogStart;
		constants->fogRange[1] = gl.fogEnd;
	}
}

// where one attribute of a draw comes from
typedef struct source_s {
	bool			array;		// else: the current value
	const byte		*base;
	int				stride;
	int				size;
	GLenum			type;
} source_t;

static source_t ArraySource( const clientArray_t &array, int defaultSize )
{
	source_t source = {};
	source.array = array.enabled && array.pointer;
	source.base = (const byte *)array.pointer;
	source.size = array.size ? array.size : defaultSize;
	source.type = array.type;
	int elementSize = array.type == GL_UNSIGNED_BYTE ? 1 : 4;
	source.stride = array.stride ? array.stride : source.size * elementSize;
	return source;
}

static void WritePositions( const source_t &source, int first, int count, float *out )
{
	for ( int i = 0; i < count; i++ )
	{
		const float *in = (const float *)( source.base + (size_t)( first + i ) * source.stride );
		out[i * 3 + 0] = in[0];
		out[i * 3 + 1] = in[1];
		out[i * 3 + 2] = source.size >= 3 ? in[2] : 0.0f;
	}
}

static void WriteColors( const source_t &source, int first, int count, byte *out )
{
	for ( int i = 0; i < count; i++ )
	{
		const byte *in = source.base + (size_t)( first + i ) * source.stride;
		if ( source.type == GL_UNSIGNED_BYTE )
		{
			out[i * 4 + 0] = in[0];
			out[i * 4 + 1] = in[1];
			out[i * 4 + 2] = in[2];
			out[i * 4 + 3] = source.size >= 4 ? in[3] : 255;
		}
		else
		{
			const float *f = (const float *)in;
			const float rgba[4] = { f[0], f[1], f[2], source.size >= 4 ? f[3] : 1.0f };
			FloatsToColor( rgba, out + i * 4 );
		}
	}
}

static void WriteTexCoords( const source_t &source, int first, int count, float *out )
{
	for ( int i = 0; i < count; i++ )
	{
		const float *in = (const float *)( source.base + (size_t)( first + i ) * source.stride );
		out[i * 2 + 0] = in[0];
		out[i * 2 + 1] = in[1];
	}
}

/*
=================
Emit

Draws the vertices [first, first + count) of the given sources (or the indexed ones) as the primitive mode.
indices are of indexType; the vertices they name are [firstVertex...] already counted from the sources' start.
=================
*/
static void Emit( GLenum mode, const source_t &position, const source_t &color, const source_t &uv0, const source_t &uv1,
	int first, int count, const void *indices, GLenum indexType, int indexCount )
{
	// the index list of what the GPU draws: always triangles (or lines)
	std::vector<unsigned int> generated;
	const bool lines = mode == GL_LINES;
	if ( lines && indices )
	{
		return;	// (not used)
	}

	int vertexFirst = first, vertexCount = count;
	int outIndexCount = 0;
	const unsigned int *userIndices = NULL;
	unsigned int minIndex = 0;

	if ( indices )
	{
		// scan: the vertices named by the indices are the ones to copy
		unsigned int lo = 0xFFFFFFFFu, hi = 0;
		for ( int i = 0; i < indexCount; i++ )
		{
			unsigned int index = indexType == GL_UNSIGNED_INT ? ( (const unsigned int *)indices )[i]
				: ( indexType == GL_UNSIGNED_SHORT ? ( (const unsigned short *)indices )[i] : ( (const unsigned char *)indices )[i] );
			if ( index < lo ) lo = index;
			if ( index > hi ) hi = index;
		}
		if ( lo > hi )
		{
			return;
		}
		vertexFirst = (int)lo;
		vertexCount = (int)( hi - lo + 1 );
		minIndex = lo;
		outIndexCount = indexCount;
	}
	else
	{
		switch ( mode )
		{
		case GL_TRIANGLES:
		case GL_LINES:
			break;	// in order, no indices
		case GL_TRIANGLE_STRIP:
			for ( int i = 0; i + 2 < count; i++ )
			{
				if ( i & 1 ) { generated.push_back( i + 1 ); generated.push_back( i ); generated.push_back( i + 2 ); }
				else { generated.push_back( i ); generated.push_back( i + 1 ); generated.push_back( i + 2 ); }
			}
			break;
		case GL_TRIANGLE_FAN:
		case GL_POLYGON:
			for ( int i = 1; i + 1 < count; i++ )
			{
				generated.push_back( 0 ); generated.push_back( i ); generated.push_back( i + 1 );
			}
			break;
		case GL_QUADS:
			for ( int i = 0; i + 3 < count; i += 4 )
			{
				generated.push_back( i ); generated.push_back( i + 1 ); generated.push_back( i + 2 );
				generated.push_back( i ); generated.push_back( i + 2 ); generated.push_back( i + 3 );
			}
			break;
		default:
			return;
		}
		outIndexCount = (int)generated.size();
		if ( ( mode != GL_TRIANGLES && mode != GL_LINES ) && !outIndexCount )
		{
			return;
		}
	}
	if ( vertexCount <= 0 )
	{
		return;
	}

	// which texture coordinates matter: those of the units that are on
	const bool useUv0 = gl.texture2D[0] && uv0.array;
	const bool useUv1 = gl.texture2D[1] && uv1.array;

	vkPipelineKey_t key;
	vkDynamicState_t dynamic;
	vkConstants_t constants;
	PrepareDraw( &key, &dynamic, &constants, lines, !color.array, !useUv0, !useUv1 );

	vkGeometry_t geometry;
	VK_AllocateGeometry( &geometry, vertexCount, outIndexCount, &key );

	WritePositions( position, vertexFirst, vertexCount, geometry.position );
	if ( color.array )
	{
		WriteColors( color, vertexFirst, vertexCount, geometry.color );
	}
	else
	{
		FloatsToColor( gl.color, geometry.color );
	}
	if ( useUv0 )
	{
		WriteTexCoords( uv0, vertexFirst, vertexCount, geometry.texCoord0 );
	}
	else
	{
		geometry.texCoord0[0] = gl.texCoord[0][0];
		geometry.texCoord0[1] = gl.texCoord[0][1];
	}
	if ( useUv1 )
	{
		WriteTexCoords( uv1, vertexFirst, vertexCount, geometry.texCoord1 );
	}
	else
	{
		geometry.texCoord1[0] = gl.texCoord[1][0];
		geometry.texCoord1[1] = gl.texCoord[1][1];
	}

	if ( indices )
	{
		for ( int i = 0; i < indexCount; i++ )
		{
			unsigned int index = indexType == GL_UNSIGNED_INT ? ( (const unsigned int *)indices )[i]
				: ( indexType == GL_UNSIGNED_SHORT ? ( (const unsigned short *)indices )[i] : ( (const unsigned char *)indices )[i] );
			geometry.indices[i] = index - minIndex;
		}
	}
	else if ( !generated.empty() )
	{
		memcpy( geometry.indices, &generated[0], generated.size() * sizeof( unsigned int ) );
	}
	(void)userIndices;

	vkTexture_t *texture0 = gl.texture2D[0] ? VK_TextureForId( gl.bound[0], false ) : NULL;
	vkTexture_t *texture1 = gl.texture2D[1] ? VK_TextureForId( gl.bound[1], false ) : NULL;
	VK_Draw( &key, &dynamic, &constants, texture0, texture1, &geometry );
}

void vkglDrawElements( GLenum mode, GLsizei count, GLenum type, const GLvoid *indices )
{
	source_t texCoord1 = ArraySource( gl.texCoordArray[1], 2 );
	Emit( mode, ArraySource( gl.vertexArray, 3 ), ArraySource( gl.colorArray, 4 ), ArraySource( gl.texCoordArray[0], 2 ), texCoord1,
		0, 0, indices, type, count );
}

void vkglDrawArrays( GLenum mode, GLint first, GLsizei count )
{
	Emit( mode, ArraySource( gl.vertexArray, 3 ), ArraySource( gl.colorArray, 4 ), ArraySource( gl.texCoordArray[0], 2 ),
		ArraySource( gl.texCoordArray[1], 2 ), first, count, NULL, 0, 0 );
}

// immediate mode: what is between glBegin and glEnd is collected, then drawn like arrays
typedef struct immediateVertex_s {
	float	position[3];
	byte	color[4];
	float	texCoord0[2];
	float	texCoord1[2];
} immediateVertex_t;

static std::vector<immediateVertex_t>	immediate;
static GLenum							immediateMode;

void vkglBegin( GLenum mode )
{
	immediate.clear();
	immediateMode = mode;
}

static void AddVertex( float x, float y, float z )
{
	immediateVertex_t vertex;
	vertex.position[0] = x;
	vertex.position[1] = y;
	vertex.position[2] = z;
	FloatsToColor( gl.color, vertex.color );
	vertex.texCoord0[0] = gl.texCoord[0][0];
	vertex.texCoord0[1] = gl.texCoord[0][1];
	vertex.texCoord1[0] = gl.texCoord[1][0];
	vertex.texCoord1[1] = gl.texCoord[1][1];
	immediate.push_back( vertex );
}

void vkglVertex2f( GLfloat x, GLfloat y )
{
	AddVertex( x, y, 0.0f );
}

void vkglVertex3f( GLfloat x, GLfloat y, GLfloat z )
{
	AddVertex( x, y, z );
}

void vkglVertex3fv( const GLfloat *v )
{
	AddVertex( v[0], v[1], v[2] );
}

void vkglEnd( void )
{
	if ( immediate.empty() )
	{
		return;
	}
	const immediateVertex_t *base = &immediate[0];
	const int stride = (int)sizeof( immediateVertex_t );
	source_t position = { true, (const byte *)base->position, stride, 3, GL_FLOAT };
	source_t color = { true, (const byte *)base->color, stride, 4, GL_UNSIGNED_BYTE };
	source_t uv0 = { true, (const byte *)base->texCoord0, stride, 2, GL_FLOAT };
	source_t uv1 = { true, (const byte *)base->texCoord1, stride, 2, GL_FLOAT };
	Emit( immediateMode, position, color, uv0, uv1, 0, (int)immediate.size(), NULL, 0, 0 );
	immediate.clear();
}

/*
=============================================================================

TEXTURES

=============================================================================
*/

void vkglBindTexture( GLenum target, GLuint texture )
{
	if ( target == GL_TEXTURE_2D )
	{
		gl.bound[gl.activeUnit] = texture;
	}
}

void vkglGenTextures( GLsizei n, GLuint *textures )
{
	for ( int i = 0; i < n; i++ )
	{
		textures[i] = nextGeneratedTexture++;
	}
}

void vkglDeleteTextures( GLsizei n, const GLuint *textures )
{
	for ( int i = 0; i < n; i++ )
	{
		for ( int unit = 0; unit < MAX_UNITS; unit++ )
		{
			if ( gl.bound[unit] == textures[i] ) gl.bound[unit] = 0;
		}
		VK_DeleteTexture( textures[i] );
	}
}

void vkglTexImage2D( GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border,
	GLenum format, GLenum type, const GLvoid *pixels )
{
	if ( target == GL_TEXTURE_2D )
	{
		VK_TexImage2D( gl.bound[gl.activeUnit], level, internalformat, width, height, format, type, pixels );
	}
}

void vkglTexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
	GLenum format, GLenum type, const GLvoid *pixels )
{
	if ( target == GL_TEXTURE_2D )
	{
		VK_TexSubImage2D( gl.bound[gl.activeUnit], level, xoffset, yoffset, width, height, format, type, pixels );
	}
}

void vkglTexParameterf( GLenum target, GLenum pname, GLfloat param )
{
	if ( target == GL_TEXTURE_2D )
	{
		VK_TexParameter( gl.bound[gl.activeUnit], pname, param );
	}
}

void vkglTexParameteri( GLenum target, GLenum pname, GLint param )
{
	vkglTexParameterf( target, pname, (GLfloat)param );
}

void vkglTexParameterfv( GLenum target, GLenum pname, const GLfloat *params )
{
	// (the border colour is the one vector parameter, and clamp-to-edge does not use it)
}

void vkglCopyTexImage2D( GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border )
{
	if ( target == GL_TEXTURE_2D && level == 0 )
	{
		VK_CopyFramebuffer( gl.bound[gl.activeUnit], width, height, 0, 0, x, y, width, height );
	}
}

void vkglCopyTexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height )
{
	if ( target == GL_TEXTURE_2D && level == 0 )
	{
		VK_CopyFramebuffer( gl.bound[gl.activeUnit], 0, 0, xoffset, yoffset, x, y, width, height );
	}
}

/*
=============================================================================

CLEARS, QUERIES

=============================================================================
*/

void vkglClearColor( GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha )
{
	vk.clearColor[0] = red;
	vk.clearColor[1] = green;
	vk.clearColor[2] = blue;
	vk.clearColor[3] = alpha;
}

void vkglClearDepth( GLclampd depth )
{
	vk.clearDepth = (float)depth;
}

void vkglClearStencil( GLint s )
{
	vk.clearStencil = s;
}

// OpenGL clears what is inside the scissor box (when the scissor test is on)
void vkglClear( GLbitfield mask )
{
	VK_EnsureRenderPass();

	VkClearAttachment attachments[2] = {};
	uint32_t count = 0;
	if ( mask & GL_COLOR_BUFFER_BIT )
	{
		attachments[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		attachments[count].colorAttachment = 0;
		memcpy( attachments[count].clearValue.color.float32, vk.clearColor, sizeof( vk.clearColor ) );
		count++;
	}
	VkImageAspectFlags depthStencil = 0;
	if ( mask & GL_DEPTH_BUFFER_BIT ) depthStencil |= VK_IMAGE_ASPECT_DEPTH_BIT;
	if ( mask & GL_STENCIL_BUFFER_BIT ) depthStencil |= VK_IMAGE_ASPECT_STENCIL_BIT;
	if ( depthStencil )
	{
		attachments[count].aspectMask = depthStencil;
		attachments[count].clearValue.depthStencil.depth = vk.clearDepth;
		attachments[count].clearValue.depthStencil.stencil = (uint32_t)vk.clearStencil;
		count++;
	}
	if ( !count )
	{
		return;
	}

	VkClearRect rect = {};
	rect.layerCount = 1;
	rect.rect.extent.width = (uint32_t)vk.width;
	rect.rect.extent.height = (uint32_t)vk.height;
	if ( gl.scissorTest )
	{
		int x = Q_max( gl.scissor[0], 0 );
		int y = Q_max( gl.scissor[1], 0 );
		int w = Q_min( gl.scissor[2], vk.width - x );
		int h = Q_min( gl.scissor[3], vk.height - y );
		if ( w <= 0 || h <= 0 )
		{
			return;
		}
		rect.rect.offset.x = x;
		rect.rect.offset.y = vk.height - ( y + h );	// OpenGL's origin is the bottom left
		rect.rect.extent.width = (uint32_t)w;
		rect.rect.extent.height = (uint32_t)h;
	}
	vkCmdClearAttachments( vk.cmd, count, attachments, 1, &rect );
}

// (nothing to wait for: the frame is sent when it is presented)
void vkglFinish( void )
{
}

GLenum vkglGetError( void )
{
	return GL_NO_ERROR;
}

void vkglGetIntegerv( GLenum pname, GLint *params )
{
	switch ( pname )
	{
	case GL_MAX_TEXTURE_SIZE:	*params = glConfig.maxTextureSize; break;
	case GL_PACK_ALIGNMENT:		*params = 4; break;
	case GL_CULL_FACE:			*params = gl.cullFace ? 1 : 0; break;
	default:					*params = 0; break;
	}
}

void vkglGetFloatv( GLenum pname, GLfloat *params )
{
	if ( pname == GL_DEPTH_RANGE )
	{
		params[0] = gl.depthRange[0];
		params[1] = gl.depthRange[1];
		return;
	}
	*params = 0.0f;
}

void vkglGetDoublev( GLenum pname, GLdouble *params )
{
	*params = 0.0;
}

const GLubyte *vkglGetString( GLenum name )
{
	return (const GLubyte *)"";
}

void vkglReadPixels( GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels )
{
	if ( format == GL_DEPTH_COMPONENT && type == GL_FLOAT )
	{
		// (only single values are asked for: the flares looking whether something is in front of them)
		const float depth = VK_ReadDepth( x, y );
		for ( int i = 0; i < width * height; i++ ) ( (float *)pixels )[i] = depth;
		return;
	}
	VK_ReadPixels( x, y, width, height, format, type, pixels );
}

/*
=================
VK_ResetGLState

OpenGL's initial state, which the renderer's GL_SetDefaultState then builds on.
=================
*/
void VK_ResetGLState( void )
{
	memset( &gl, 0, sizeof( gl ) );
	gl.srcBlend = GL_ONE;
	gl.dstBlend = GL_ZERO;
	gl.depthFunc = GL_LESS;
	gl.depthMask = true;
	gl.cullFaceMode = GL_BACK;
	gl.alphaFunc = GL_ALWAYS;
	gl.stencilFunc = GL_ALWAYS;
	gl.stencilCompareMask = 0xFFFFFFFFu;
	gl.stencilWriteMask = 0xFFFFFFFFu;
	for ( int face = 0; face < 2; face++ )
	{
		gl.stencilFail[face] = gl.stencilZFail[face] = gl.stencilZPass[face] = GL_KEEP;
	}
	gl.colorMask = 15;
	gl.depthRange[1] = 1.0f;
	gl.fogMode = GL_EXP;
	gl.fogDensity = 1.0f;
	gl.fogEnd = 1.0f;
	gl.matrixMode = GL_MODELVIEW;
	memcpy( gl.modelview, identity, sizeof( identity ) );
	memcpy( gl.projection, identity, sizeof( identity ) );
	gl.color[0] = gl.color[1] = gl.color[2] = gl.color[3] = 1.0f;
	gl.vertexArray.size = 4;
	gl.vertexArray.type = GL_FLOAT;
	for ( int unit = 0; unit < MAX_UNITS; unit++ )
	{
		gl.texEnv[unit] = GL_MODULATE;
		gl.texCoordArray[unit].size = 4;
		gl.texCoordArray[unit].type = GL_FLOAT;
	}
	gl.viewport[2] = vk.width;
	gl.viewport[3] = vk.height;
	gl.scissor[2] = vk.width;
	gl.scissor[3] = vk.height;
}
