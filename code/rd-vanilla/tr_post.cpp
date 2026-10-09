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

static struct {
	qboolean		ready;				// entry points are there
	int				width, height;		// size of the view the textures below are made for
	GLuint			sceneTexture;		// copy of the colour of the view
	GLuint			depthTexture;		// copy of its depth
	postProgram_t	copy;
	qboolean		copyFailed;
} post;

static const char *postVertexSource =
	"#version 120\n"
	"void main() {\n"
	"	gl_Position = gl_Vertex;\n"	// the quad is given in clip space
	"}\n";

static const char *postCopyFragmentSource =
	"#version 120\n"
	"uniform sampler2D uScene;\n"
	"uniform vec4 uRect;\n"			// x, y, width, height of the view in the window
	"void main() {\n"
	"	gl_FragColor = vec4( texture2D( uScene, ( gl_FragCoord.xy - uRect.xy ) / uRect.zw ).rgb, 1.0 );\n"
	"}\n";

void R_Post_Init( void ) {
	memset( &post, 0, sizeof( post ) );

	if ( !R_GLSL_Available() || !qglActiveTextureARB ) {
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

static void Post_FreeTargets( void ) {
	if ( post.sceneTexture ) {
		qglDeleteTextures( 1, &post.sceneTexture );
	}
	if ( post.depthTexture ) {
		qglDeleteTextures( 1, &post.depthTexture );
	}
	post.sceneTexture = post.depthTexture = 0;
	post.width = post.height = 0;
}

// makes the textures for a view of this size, if they are not there yet
static void Post_EnsureTargets( int width, int height ) {
	if ( post.width == width && post.height == height && post.sceneTexture ) {
		return;
	}
	Post_FreeTargets();
	post.sceneTexture = Post_NewTexture( GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, width, height );
	post.depthTexture = Post_NewTexture( GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, width, height );
	post.width = width;
	post.height = height;
}

static qboolean Post_BuildProgram( postProgram_t *program, const char *name, const char *fragmentSource,
								   const char *const *uniformNames ) {
	int i;

	program->id = R_GLSL_BuildProgram( name, postVertexSource, fragmentSource );
	if ( !program->id ) {
		return qfalse;
	}
	for ( i = 0; uniformNames[i]; i++ ) {
		program->uniforms[i] = qglGetUniformLocation( program->id, uniformNames[i] );
	}
	return qtrue;
}

static void Post_Quad( void ) {
	qglBegin( GL_QUADS );
	qglVertex2f( -1.0f, -1.0f );
	qglVertex2f( 1.0f, -1.0f );
	qglVertex2f( 1.0f, 1.0f );
	qglVertex2f( -1.0f, 1.0f );
	qglEnd();
}

static void Post_BindTexture( int unit, GLuint texture ) {
	qglActiveTextureARB( GL_TEXTURE0_ARB + unit );
	qglBindTexture( GL_TEXTURE_2D, texture );
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
	return (qboolean)( r_postDebug->integer != 0 );
}

/*
================
RB_PostProcess

Called when the 3D view is finished. Leaves the GL state as it found it, so the 2D drawing after it does not
notice.
================
*/
void RB_PostProcess( void ) {
	static const char *const copyUniforms[] = { "uScene", "uRect", NULL };
	const int	x = backEnd.viewParms.viewportX;
	const int	y = backEnd.viewParms.viewportY;
	const int	width = backEnd.viewParms.viewportWidth;
	const int	height = backEnd.viewParms.viewportHeight;
	const float	rect[4] = { (float)x, (float)y, (float)width, (float)height };

	if ( !Post_Wanted() ) {
		return;
	}

	if ( !post.copy.id ) {
		if ( post.copyFailed || !Post_BuildProgram( &post.copy, "post copy", postCopyFragmentSource, copyUniforms ) ) {
			post.copyFailed = qtrue;
			return;
		}
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

	// draw the result over the view
	qglUseProgram( post.copy.id );
	qglUniform1i( post.copy.uniforms[0], 0 );
	qglUniform4fv( post.copy.uniforms[1], 1, rect );
	Post_Quad();
	qglUseProgram( 0 );

	// the engine caches what is bound to its two texture units, make it look again
	Post_BindTexture( 1, 0 );
	Post_BindTexture( 0, 0 );
	glState.currenttextures[0] = glState.currenttextures[1] = 0;
	qglActiveTextureARB( GL_TEXTURE0_ARB + glState.currenttmu );

	qglMatrixMode( GL_PROJECTION );
	qglPopMatrix();
	qglMatrixMode( GL_MODELVIEW );
	qglPopMatrix();
}
