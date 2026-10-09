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

// tr_glsl.cpp -- loads the GLSL entry points and builds shader programs. The renderer stays on the fixed function
// pipeline everywhere a program is not bound, so every use of this has to check R_GLSL_Available() and keep its
// old path as the fallback.

#include "tr_local.h"

PFNGLCREATESHADERPROC			qglCreateShader;
PFNGLSHADERSOURCEPROC			qglShaderSource;
PFNGLCOMPILESHADERPROC			qglCompileShader;
PFNGLGETSHADERIVPROC			qglGetShaderiv;
PFNGLGETSHADERINFOLOGPROC		qglGetShaderInfoLog;
PFNGLDELETESHADERPROC			qglDeleteShader;
PFNGLCREATEPROGRAMPROC			qglCreateProgram;
PFNGLATTACHSHADERPROC			qglAttachShader;
PFNGLLINKPROGRAMPROC			qglLinkProgram;
PFNGLGETPROGRAMIVPROC			qglGetProgramiv;
PFNGLGETPROGRAMINFOLOGPROC		qglGetProgramInfoLog;
PFNGLDELETEPROGRAMPROC			qglDeleteProgram;
PFNGLUSEPROGRAMPROC				qglUseProgram;
PFNGLGETUNIFORMLOCATIONPROC		qglGetUniformLocation;
PFNGLUNIFORM1IPROC				qglUniform1i;
PFNGLUNIFORM1FPROC				qglUniform1f;
PFNGLUNIFORM3FPROC				qglUniform3f;
PFNGLUNIFORM4FVPROC				qglUniform4fv;

static qboolean	glslAvailable = qfalse;

qboolean R_GLSL_Available( void ) {
	return glslAvailable;
}

static GLuint R_GLSL_CompileStage( const char *name, GLenum type, const char *source ) {
	const char	*stageName = ( type == GL_VERTEX_SHADER ) ? "vertex" : "fragment";
	GLint		status = 0;
	GLuint		shader = qglCreateShader( type );

	if ( !shader ) {
		return 0;
	}

	qglShaderSource( shader, 1, &source, NULL );
	qglCompileShader( shader );
	qglGetShaderiv( shader, GL_COMPILE_STATUS, &status );
	if ( !status ) {
		char log[1024];
		qglGetShaderInfoLog( shader, sizeof( log ), NULL, log );
		Com_Printf( S_COLOR_YELLOW "GLSL %s: %s shader does not compile:\n%s\n", name, stageName, log );
		qglDeleteShader( shader );
		return 0;
	}
	return shader;
}

/*
================
R_GLSL_BuildProgram

Compiles and links a vertex and a fragment shader. The sources must carry their own #version line (use 120, which
every GL 2.0 driver accepts). Returns the program object, or 0 if GLSL is not available or the build failed, in
which case the reason is printed and the caller keeps using its fixed function path.
================
*/
GLuint R_GLSL_BuildProgram( const char *name, const char *vertexSource, const char *fragmentSource ) {
	GLint	status = 0;
	GLuint	vertex, fragment, program;

	if ( !glslAvailable ) {
		return 0;
	}

	vertex = R_GLSL_CompileStage( name, GL_VERTEX_SHADER, vertexSource );
	fragment = R_GLSL_CompileStage( name, GL_FRAGMENT_SHADER, fragmentSource );
	if ( !vertex || !fragment ) {
		if ( vertex ) {
			qglDeleteShader( vertex );
		}
		if ( fragment ) {
			qglDeleteShader( fragment );
		}
		return 0;
	}

	program = qglCreateProgram();
	qglAttachShader( program, vertex );
	qglAttachShader( program, fragment );
	qglLinkProgram( program );

	// the program keeps the compiled code, the shader objects are only needed until it is linked
	qglDeleteShader( vertex );
	qglDeleteShader( fragment );

	qglGetProgramiv( program, GL_LINK_STATUS, &status );
	if ( !status ) {
		char log[1024];
		qglGetProgramInfoLog( program, sizeof( log ), NULL, log );
		Com_Printf( S_COLOR_YELLOW "GLSL %s: does not link:\n%s\n", name, log );
		qglDeleteProgram( program );
		return 0;
	}
	return program;
}

void R_GLSL_DeleteProgram( GLuint program ) {
	if ( glslAvailable && program ) {
		qglDeleteProgram( program );
	}
}

/*
================
R_GLSL_Init

GLSL is core in OpenGL 2.0, so the check is the version of the context. Builds a tiny test program, so that a
driver that reports 2.0 but cannot compile is caught here and not at the first use.
================
*/
void R_GLSL_Init( void ) {
	int		major = 0, minor = 0;
	GLuint	test;

	glslAvailable = qfalse;

	if ( !r_glsl->integer ) {
		Com_Printf( "...ignoring GLSL\n" );
		return;
	}

	if ( sscanf( glConfig.version_string, "%d.%d", &major, &minor ) < 1 || major < 2 ) {
		Com_Printf( "...GLSL not found (needs OpenGL 2.0)\n" );
		return;
	}

	qglCreateShader			= (PFNGLCREATESHADERPROC)			ri.GL_GetProcAddress( "glCreateShader" );
	qglShaderSource			= (PFNGLSHADERSOURCEPROC)			ri.GL_GetProcAddress( "glShaderSource" );
	qglCompileShader		= (PFNGLCOMPILESHADERPROC)			ri.GL_GetProcAddress( "glCompileShader" );
	qglGetShaderiv			= (PFNGLGETSHADERIVPROC)			ri.GL_GetProcAddress( "glGetShaderiv" );
	qglGetShaderInfoLog		= (PFNGLGETSHADERINFOLOGPROC)		ri.GL_GetProcAddress( "glGetShaderInfoLog" );
	qglDeleteShader			= (PFNGLDELETESHADERPROC)			ri.GL_GetProcAddress( "glDeleteShader" );
	qglCreateProgram		= (PFNGLCREATEPROGRAMPROC)			ri.GL_GetProcAddress( "glCreateProgram" );
	qglAttachShader			= (PFNGLATTACHSHADERPROC)			ri.GL_GetProcAddress( "glAttachShader" );
	qglLinkProgram			= (PFNGLLINKPROGRAMPROC)			ri.GL_GetProcAddress( "glLinkProgram" );
	qglGetProgramiv			= (PFNGLGETPROGRAMIVPROC)			ri.GL_GetProcAddress( "glGetProgramiv" );
	qglGetProgramInfoLog	= (PFNGLGETPROGRAMINFOLOGPROC)		ri.GL_GetProcAddress( "glGetProgramInfoLog" );
	qglDeleteProgram		= (PFNGLDELETEPROGRAMPROC)			ri.GL_GetProcAddress( "glDeleteProgram" );
	qglUseProgram			= (PFNGLUSEPROGRAMPROC)				ri.GL_GetProcAddress( "glUseProgram" );
	qglGetUniformLocation	= (PFNGLGETUNIFORMLOCATIONPROC)		ri.GL_GetProcAddress( "glGetUniformLocation" );
	qglUniform1i			= (PFNGLUNIFORM1IPROC)				ri.GL_GetProcAddress( "glUniform1i" );
	qglUniform1f			= (PFNGLUNIFORM1FPROC)				ri.GL_GetProcAddress( "glUniform1f" );
	qglUniform3f			= (PFNGLUNIFORM3FPROC)				ri.GL_GetProcAddress( "glUniform3f" );
	qglUniform4fv			= (PFNGLUNIFORM4FVPROC)				ri.GL_GetProcAddress( "glUniform4fv" );

	if ( !qglCreateShader || !qglShaderSource || !qglCompileShader || !qglGetShaderiv || !qglGetShaderInfoLog ||
		 !qglDeleteShader || !qglCreateProgram || !qglAttachShader || !qglLinkProgram || !qglGetProgramiv ||
		 !qglGetProgramInfoLog || !qglDeleteProgram || !qglUseProgram || !qglGetUniformLocation ||
		 !qglUniform1i || !qglUniform1f || !qglUniform3f || !qglUniform4fv ) {
		Com_Printf( "...GLSL found, but an entry point is missing\n" );
		return;
	}

	glslAvailable = qtrue;
	test = R_GLSL_BuildProgram( "test",
		"#version 120\nvoid main() { gl_Position = ftransform(); }\n",
		"#version 120\nvoid main() { gl_FragColor = vec4( 1.0 ); }\n" );
	if ( !test ) {
		glslAvailable = qfalse;
		Com_Printf( "...GLSL found, but the driver does not build a test program\n" );
		return;
	}
	qglDeleteProgram( test );

	Com_Printf( "...using GLSL (OpenGL %d.%d)\n", major, minor );
}
