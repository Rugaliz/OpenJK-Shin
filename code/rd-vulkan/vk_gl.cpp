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
// Step 2 of the plan: every call does nothing yet (the module links and starts, nothing is drawn).

#include "../server/exe_headers.h"
#include "../rd-vanilla/tr_local.h"

void vkglAlphaFunc( GLenum func, GLclampf ref ) { }
void vkglArrayElement( GLint i ) { }
void vkglBegin( GLenum mode ) { }
void vkglBindTexture( GLenum target, GLuint texture ) { }
void vkglBlendFunc( GLenum sfactor, GLenum dfactor ) { }
void vkglCallList( GLuint list ) { }
void vkglClear( GLbitfield mask ) { }
void vkglClearColor( GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha ) { }
void vkglClearDepth( GLclampd depth ) { }
void vkglClearStencil( GLint s ) { }
void vkglClipPlane( GLenum plane, const GLdouble *equation ) { }
void vkglColor3f( GLfloat red, GLfloat green, GLfloat blue ) { }
void vkglColor4f( GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha ) { }
void vkglColor4ub( GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha ) { }
void vkglColor4ubv( const GLubyte *v ) { }
void vkglColorMask( GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha ) { }
void vkglColorPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr ) { }
void vkglCopyTexImage2D( GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border ) { }
void vkglCopyTexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height ) { }
void vkglCullFace( GLenum mode ) { }
void vkglDeleteLists( GLuint list, GLsizei range ) { }
void vkglDeleteTextures( GLsizei n, const GLuint *textures ) { }
void vkglDepthFunc( GLenum func ) { }
void vkglDepthMask( GLboolean flag ) { }
void vkglDepthRange( GLclampd near_val, GLclampd far_val ) { }
void vkglDisable( GLenum cap ) { }
void vkglDisableClientState( GLenum cap ) { }
void vkglDrawArrays( GLenum mode, GLint first, GLsizei count ) { }
void vkglDrawBuffer( GLenum mode ) { }
void vkglDrawElements( GLenum mode, GLsizei count, GLenum type, const GLvoid *indices ) { }
void vkglEnable( GLenum cap ) { }
void vkglEnableClientState( GLenum cap ) { }
void vkglEnd( void ) { }
void vkglEndList( void ) { }
void vkglFinish( void ) { }
void vkglFogf( GLenum pname, GLfloat param ) { }
void vkglFogfv( GLenum pname, const GLfloat *params ) { }
void vkglFogi( GLenum pname, GLint param ) { }
GLuint vkglGenLists( GLsizei range ) { return 0; }
void vkglGenTextures( GLsizei n, GLuint *textures ) { }
void vkglGetDoublev( GLenum pname, GLdouble *params ) { }
GLenum vkglGetError( void ) { return 0; }
void vkglGetFloatv( GLenum pname, GLfloat *params ) { }
void vkglGetIntegerv( GLenum pname, GLint *params ) { }
const GLubyte * vkglGetString( GLenum name ) { return (const GLubyte *)""; }
GLboolean vkglIsEnabled( GLenum cap ) { return 0; }
void vkglLineWidth( GLfloat width ) { }
void vkglLoadIdentity( void ) { }
void vkglLoadMatrixf( const GLfloat *m ) { }
void vkglMatrixMode( GLenum mode ) { }
void vkglNewList( GLuint list, GLenum mode ) { }
void vkglNormalPointer( GLenum type, GLsizei stride, const GLvoid *ptr ) { }
void vkglOrtho( GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_val, GLdouble far_val ) { }
void vkglPixelStorei( GLenum pname, GLint param ) { }
void vkglPolygonMode( GLenum face, GLenum mode ) { }
void vkglPolygonOffset( GLfloat factor, GLfloat units ) { }
void vkglPopMatrix( void ) { }
void vkglPushMatrix( void ) { }
void vkglReadPixels( GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels ) { }
void vkglScissor( GLint x, GLint y, GLsizei width, GLsizei height ) { }
void vkglShadeModel( GLenum mode ) { }
void vkglStencilFunc( GLenum func, GLint ref, GLuint mask ) { }
void vkglStencilMask( GLuint mask ) { }
void vkglStencilOp( GLenum fail, GLenum zfail, GLenum zpass ) { }
void vkglTexCoord2f( GLfloat s, GLfloat t ) { }
void vkglTexCoord2fv( const GLfloat *v ) { }
void vkglTexCoordPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr ) { }
void vkglTexEnvf( GLenum target, GLenum pname, GLfloat param ) { }
void vkglTexImage2D( GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels ) { }
void vkglTexParameterf( GLenum target, GLenum pname, GLfloat param ) { }
void vkglTexParameterfv( GLenum target, GLenum pname, const GLfloat *params ) { }
void vkglTexParameteri( GLenum target, GLenum pname, GLint param ) { }
void vkglTexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels ) { }
void vkglTranslatef( GLfloat x, GLfloat y, GLfloat z ) { }
void vkglVertex2f( GLfloat x, GLfloat y ) { }
void vkglVertex3f( GLfloat x, GLfloat y, GLfloat z ) { }
void vkglVertex3fv( const GLfloat *v ) { }
void vkglVertexPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr ) { }
void vkglViewport( GLint x, GLint y, GLsizei width, GLsizei height ) { }

// Extension entry points the renderer looks for through function pointers (set in VK_Init).
void APIENTRY vkglActiveTextureARB( GLenum texture ) {}
void APIENTRY vkglClientActiveTextureARB( GLenum texture ) {}
void APIENTRY vkglMultiTexCoord2fARB( GLenum target, GLfloat s, GLfloat t ) {}
void APIENTRY vkglLockArraysEXT( GLint first, GLsizei count ) {}
void APIENTRY vkglUnlockArraysEXT( void ) {}
void APIENTRY vkglStencilOpSeparate( GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass ) {}
