/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
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

#pragma once

#if defined( __LINT__ )
#	include <GL/gl.h>
#elif defined( _WIN32 )
#	include <windows.h>
#	include <GL/gl.h>
#elif defined(MACOS_X)
// Prevent OS X headers from including its own glext header
#	define GL_GLEXT_LEGACY
#	include <OpenGL/gl.h>
#elif defined( __linux__ )
#	include <GL/gl.h>
#	include <GL/glx.h>
// bk001129 - from cvs1.17 (mkv)
#	if defined(__FX__)
#		include <GL/fxmesa.h>
#	endif
#elif defined( __FreeBSD__ ) || defined(__OpenBSD__) // rb010123
#	include <GL/gl.h>
#	include <GL/glx.h>
#	if defined(__FX__)
#		include <GL/fxmesa.h>
#	endif
#else
#	include <gl.h>
#endif

#include "../rd-vanilla/glext.h"

// Vulkan renderer: the qgl* calls of the original OpenGL renderer are answered by a fixed function layer built on
// Vulkan (vk_gl.cpp). The core OpenGL 1.x calls the renderer uses are vkgl* functions declared here; the extension
// entry points stay function pointers (as in rd-vanilla/qgl.h) and are only set where the layer implements them,
// so everything else (GLSL, ARB programs, register combiners, framebuffers) reports "not available".
// Only the declarations used by the renderer exist; add one when a new call is needed.

void vkglAlphaFunc( GLenum func, GLclampf ref );
void vkglArrayElement( GLint i );
void vkglBegin( GLenum mode );
void vkglBindTexture( GLenum target, GLuint texture );
void vkglBlendFunc( GLenum sfactor, GLenum dfactor );
void vkglCallList( GLuint list );
void vkglClear( GLbitfield mask );
void vkglClearColor( GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha );
void vkglClearDepth( GLclampd depth );
void vkglClearStencil( GLint s );
void vkglClipPlane( GLenum plane, const GLdouble *equation );
void vkglColor3f( GLfloat red, GLfloat green, GLfloat blue );
void vkglColor4f( GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha );
void vkglColor4ub( GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha );
void vkglColor4ubv( const GLubyte *v );
void vkglColorMask( GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha );
void vkglColorPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr );
void vkglCopyTexImage2D( GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border );
void vkglCopyTexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height );
void vkglCullFace( GLenum mode );
void vkglDeleteLists( GLuint list, GLsizei range );
void vkglDeleteTextures( GLsizei n, const GLuint *textures );
void vkglDepthFunc( GLenum func );
void vkglDepthMask( GLboolean flag );
void vkglDepthRange( GLclampd near_val, GLclampd far_val );
void vkglDisable( GLenum cap );
void vkglDisableClientState( GLenum cap );
void vkglDrawArrays( GLenum mode, GLint first, GLsizei count );
void vkglDrawBuffer( GLenum mode );
void vkglDrawElements( GLenum mode, GLsizei count, GLenum type, const GLvoid *indices );
void vkglEnable( GLenum cap );
void vkglEnableClientState( GLenum cap );
void vkglEnd( void );
void vkglEndList( void );
void vkglFinish( void );
void vkglFogf( GLenum pname, GLfloat param );
void vkglFogfv( GLenum pname, const GLfloat *params );
void vkglFogi( GLenum pname, GLint param );
GLuint vkglGenLists( GLsizei range );
void vkglGenTextures( GLsizei n, GLuint *textures );
void vkglGetDoublev( GLenum pname, GLdouble *params );
GLenum vkglGetError( void );
void vkglGetFloatv( GLenum pname, GLfloat *params );
void vkglGetIntegerv( GLenum pname, GLint *params );
const GLubyte * vkglGetString( GLenum name );
GLboolean vkglIsEnabled( GLenum cap );
void vkglLineWidth( GLfloat width );
void vkglLoadIdentity( void );
void vkglLoadMatrixf( const GLfloat *m );
void vkglMatrixMode( GLenum mode );
void vkglNewList( GLuint list, GLenum mode );
void vkglNormalPointer( GLenum type, GLsizei stride, const GLvoid *ptr );
void vkglOrtho( GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_val, GLdouble far_val );
void vkglPixelStorei( GLenum pname, GLint param );
void vkglPolygonMode( GLenum face, GLenum mode );
void vkglPolygonOffset( GLfloat factor, GLfloat units );
void vkglPopMatrix( void );
void vkglPushMatrix( void );
void vkglReadPixels( GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels );
void vkglScissor( GLint x, GLint y, GLsizei width, GLsizei height );
void vkglShadeModel( GLenum mode );
void vkglStencilFunc( GLenum func, GLint ref, GLuint mask );
void vkglStencilMask( GLuint mask );
void vkglStencilOp( GLenum fail, GLenum zfail, GLenum zpass );
void vkglTexCoord2f( GLfloat s, GLfloat t );
void vkglTexCoord2fv( const GLfloat *v );
void vkglTexCoordPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr );
void vkglTexEnvf( GLenum target, GLenum pname, GLfloat param );
void vkglTexImage2D( GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels );
void vkglTexParameterf( GLenum target, GLenum pname, GLfloat param );
void vkglTexParameterfv( GLenum target, GLenum pname, const GLfloat *params );
void vkglTexParameteri( GLenum target, GLenum pname, GLint param );
void vkglTexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels );
void vkglTranslatef( GLfloat x, GLfloat y, GLfloat z );
void vkglVertex2f( GLfloat x, GLfloat y );
void vkglVertex3f( GLfloat x, GLfloat y, GLfloat z );
void vkglVertex3fv( const GLfloat *v );
void vkglVertexPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *ptr );
void vkglViewport( GLint x, GLint y, GLsizei width, GLsizei height );

#define qglAccum glAccum
#define qglAlphaFunc vkglAlphaFunc
#define qglAreTexturesResident glAreTexturesResident
#define qglArrayElement vkglArrayElement
#define qglBegin vkglBegin
#define qglBindTexture vkglBindTexture
#define qglBitmap glBitmap
#define qglBlendFunc vkglBlendFunc
#define qglCallList vkglCallList
#define qglCallLists glCallLists
#define qglClear vkglClear
#define qglClearAccum glClearAccum
#define qglClearColor vkglClearColor
#define qglClearDepth vkglClearDepth
#define qglClearIndex glClearIndex
#define qglClearStencil vkglClearStencil
#define qglClipPlane vkglClipPlane
#define qglColor3b glColor3b
#define qglColor3bv glColor3bv
#define qglColor3d glColor3d
#define qglColor3dv glColor3dv
#define qglColor3f vkglColor3f
#define qglColor3fv glColor3fv
#define qglColor3i glColor3i
#define qglColor3iv glColor3iv
#define qglColor3s glColor3s
#define qglColor3sv glColor3sv
#define qglColor3ub glColor3ub
#define qglColor3ubv glColor3ubv
#define qglColor3ui glColor3ui
#define qglColor3uiv glColor3uiv
#define qglColor3us glColor3us
#define qglColor3usv glColor3usv
#define qglColor4b glColor4b
#define qglColor4bv glColor4bv
#define qglColor4d glColor4d
#define qglColor4dv glColor4dv
#define qglColor4f vkglColor4f
#define qglColor4fv glColor4fv
#define qglColor4i glColor4i
#define qglColor4iv glColor4iv
#define qglColor4s glColor4s
#define qglColor4sv glColor4sv
#define qglColor4ub vkglColor4ub
#define qglColor4ubv vkglColor4ubv
#define qglColor4ui glColor4ui
#define qglColor4uiv glColor4uiv
#define qglColor4us glColor4us
#define qglColor4usv glColor4usv
#define qglColorMask vkglColorMask
#define qglColorMaterial glColorMaterial
#define qglColorPointer vkglColorPointer
#define qglCopyPixels glCopyPixels
#define qglCopyTexImage1D glCopyTexImage1D
#define qglCopyTexImage2D vkglCopyTexImage2D
#define qglCopyTexSubImage1D glCopyTexSubImage1D
#define qglCopyTexSubImage2D vkglCopyTexSubImage2D
#define qglCullFace vkglCullFace
#define qglDeleteLists vkglDeleteLists
#define qglDeleteTextures vkglDeleteTextures
#define qglDepthFunc vkglDepthFunc
#define qglDepthMask vkglDepthMask
#define qglDepthRange vkglDepthRange
#define qglDisable vkglDisable
#define qglDisableClientState vkglDisableClientState
#define qglDrawArrays vkglDrawArrays
#define qglDrawBuffer vkglDrawBuffer
#define qglDrawElements vkglDrawElements
#define qglDrawPixels glDrawPixels
#define qglEdgeFlag glEdgeFlag
#define qglEdgeFlagPointer glEdgeFlagPointer
#define qglEdgeFlagv glEdgeFlagv
#define qglEnable vkglEnable
#define qglEnableClientState vkglEnableClientState
#define qglEnd vkglEnd
#define qglEndList vkglEndList
#define qglEvalCoord1d glEvalCoord1d
#define qglEvalCoord1dv glEvalCoord1dv
#define qglEvalCoord1f glEvalCoord1f
#define qglEvalCoord1fv glEvalCoord1fv
#define qglEvalCoord2d glEvalCoord2d
#define qglEvalCoord2dv glEvalCoord2dv
#define qglEvalCoord2f glEvalCoord2f
#define qglEvalCoord2fv glEvalCoord2fv
#define qglEvalMesh1 glEvalMesh1
#define qglEvalMesh2 glEvalMesh2
#define qglEvalPoint1 glEvalPoint1
#define qglEvalPoint2 glEvalPoint2
#define qglFeedbackBuffer glFeedbackBuffer
#define qglFinish vkglFinish
#define qglFlush glFlush
#define qglFogf vkglFogf
#define qglFogfv vkglFogfv
#define qglFogi vkglFogi
#define qglFogiv glFogiv
#define qglFrontFace glFrontFace
#define qglFrustum glFrustum
#define qglGenLists vkglGenLists
#define qglGenTextures vkglGenTextures
#define qglGetBooleanv glGetBooleanv
#define qglGetClipPlane glGetClipPlane
#define qglGetDoublev vkglGetDoublev
#define qglGetError vkglGetError
#define qglGetFloatv vkglGetFloatv
#define qglGetIntegerv vkglGetIntegerv
#define qglGetLightfv glGetLightfv
#define qglGetLightiv glGetLightiv
#define qglGetMapdv glGetMapdv
#define qglGetMapfv glGetMapfv
#define qglGetMapiv glGetMapiv
#define qglGetMaterialfv glGetMaterialfv
#define qglGetMaterialiv glGetMaterialiv
#define qglGetPixelMapfv glGetPixelMapfv
#define qglGetPixelMapuiv glGetPixelMapuiv
#define qglGetPixelMapusv glGetPixelMapusv
#define qglGetPointerv glGetPointerv
#define qglGetPolygonStipple glGetPolygonStipple
#define qglGetString vkglGetString
#define qglGetTexGendv glGetTexGendv
#define qglGetTexGenfv glGetTexGenfv
#define qglGetTexGeniv glGetTexGeniv
#define qglGetTexImage glGetTexImage
#define qglGetTexLevelParameterfv glGetTexLevelParameterfv
#define qglGetTexLevelParameteriv glGetTexLevelParameteriv
#define qglGetTexParameterfv glGetTexParameterfv
#define qglGetTexParameteriv glGetTexParameteriv
#define qglHint glHint
#define qglIndexMask glIndexMask
#define qglIndexPointer glIndexPointer
#define qglIndexd glIndexd
#define qglIndexdv glIndexdv
#define qglIndexf glIndexf
#define qglIndexfv glIndexfv
#define qglIndexi glIndexi
#define qglIndexiv glIndexiv
#define qglIndexs glIndexs
#define qglIndexsv glIndexsv
#define qglIndexub glIndexub
#define qglIndexubv glIndexubv
#define qglInitNames glInitNames
#define qglInterleavedArrays glInterleavedArrays
#define qglIsEnabled vkglIsEnabled
#define qglIsList glIsList
#define qglIsTexture glIsTexture
#define qglLightModelf glLightModelf
#define qglLightModelfv glLightModelfv
#define qglLightModeli glLightModeli
#define qglLightModeliv glLightModeliv
#define qglLightf glLightf
#define qglLightfv glLightfv
#define qglLighti glLighti
#define qglLightiv glLightiv
#define qglLineStipple glLineStipple
#define qglLineWidth vkglLineWidth
#define qglListBase glListBase
#define qglLoadIdentity vkglLoadIdentity
#define qglLoadMatrixd glLoadMatrixd
#define qglLoadMatrixf vkglLoadMatrixf
#define qglLoadName glLoadName
#define qglLogicOp glLogicOp
#define qglMap1d glMap1d
#define qglMap1f glMap1f
#define qglMap2d glMap2d
#define qglMap2f glMap2f
#define qglMapGrid1d glMapGrid1d
#define qglMapGrid1f glMapGrid1f
#define qglMapGrid2d glMapGrid2d
#define qglMapGrid2f glMapGrid2f
#define qglMaterialf glMaterialf
#define qglMaterialfv glMaterialfv
#define qglMateriali glMateriali
#define qglMaterialiv glMaterialiv
#define qglMatrixMode vkglMatrixMode
#define qglMultMatrixd glMultMatrixd
#define qglMultMatrixf glMultMatrixf
#define qglNewList vkglNewList
#define qglNormal3b glNormal3b
#define qglNormal3bv glNormal3bv
#define qglNormal3d glNormal3d
#define qglNormal3dv glNormal3dv
#define qglNormal3f glNormal3f
#define qglNormal3fv glNormal3fv
#define qglNormal3i glNormal3i
#define qglNormal3iv glNormal3iv
#define qglNormal3s glNormal3s
#define qglNormal3sv glNormal3sv
#define qglNormalPointer vkglNormalPointer
#define qglOrtho vkglOrtho
#define qglPassThrough glPassThrough
#define qglPixelMapfv glPixelMapfv
#define qglPixelMapuiv glPixelMapuiv
#define qglPixelMapusv glPixelMapusv
#define qglPixelStoref glPixelStoref
#define qglPixelStorei vkglPixelStorei
#define qglPixelTransferf glPixelTransferf
#define qglPixelTransferi glPixelTransferi
#define qglPixelZoom glPixelZoom
#define qglPointSize glPointSize
#define qglPolygonMode vkglPolygonMode
#define qglPolygonOffset vkglPolygonOffset
#define qglPolygonStipple glPolygonStipple
#define qglPopAttrib glPopAttrib
#define qglPopClientAttrib glPopClientAttrib
#define qglPopMatrix vkglPopMatrix
#define qglPopName glPopName
#define qglPrioritizeTextures glPrioritizeTextures
#define qglPushAttrib glPushAttrib
#define qglPushClientAttrib glPushClientAttrib
#define qglPushMatrix vkglPushMatrix
#define qglPushName glPushName
#define qglRasterPos2d glRasterPos2d
#define qglRasterPos2dv glRasterPos2dv
#define qglRasterPos2f glRasterPos2f
#define qglRasterPos2fv glRasterPos2fv
#define qglRasterPos2i glRasterPos2i
#define qglRasterPos2iv glRasterPos2iv
#define qglRasterPos2s glRasterPos2s
#define qglRasterPos2sv glRasterPos2sv
#define qglRasterPos3d glRasterPos3d
#define qglRasterPos3dv glRasterPos3dv
#define qglRasterPos3f glRasterPos3f
#define qglRasterPos3fv glRasterPos3fv
#define qglRasterPos3i glRasterPos3i
#define qglRasterPos3iv glRasterPos3iv
#define qglRasterPos3s glRasterPos3s
#define qglRasterPos3sv glRasterPos3sv
#define qglRasterPos4d glRasterPos4d
#define qglRasterPos4dv glRasterPos4dv
#define qglRasterPos4f glRasterPos4f
#define qglRasterPos4fv glRasterPos4fv
#define qglRasterPos4i glRasterPos4i
#define qglRasterPos4iv glRasterPos4iv
#define qglRasterPos4s glRasterPos4s
#define qglRasterPos4sv glRasterPos4sv
#define qglReadBuffer glReadBuffer
#define qglReadPixels vkglReadPixels
#define qglRectd glRectd
#define qglRectdv glRectdv
#define qglRectf glRectf
#define qglRectfv glRectfv
#define qglRecti glRecti
#define qglRectiv glRectiv
#define qglRects glRects
#define qglRectsv glRectsv
#define qglRenderMode glRenderMode
#define qglRotated glRotated
#define qglRotatef glRotatef
#define qglScaled glScaled
#define qglScalef glScalef
#define qglScissor vkglScissor
#define qglSelectBuffer glSelectBuffer
#define qglShadeModel vkglShadeModel
#define qglStencilFunc vkglStencilFunc
#define qglStencilMask vkglStencilMask
#define qglStencilOp vkglStencilOp
#if defined(__APPLE__)
#define qglStencilOpSeparate glStencilOpSeparate
#endif
#define qglTexCoord1d glTexCoord1d
#define qglTexCoord1dv glTexCoord1dv
#define qglTexCoord1f glTexCoord1f
#define qglTexCoord1fv glTexCoord1fv
#define qglTexCoord1i glTexCoord1i
#define qglTexCoord1iv glTexCoord1iv
#define qglTexCoord1s glTexCoord1s
#define qglTexCoord1sv glTexCoord1sv
#define qglTexCoord2d glTexCoord2d
#define qglTexCoord2dv glTexCoord2dv
#define qglTexCoord2f vkglTexCoord2f
#define qglTexCoord2fv vkglTexCoord2fv
#define qglTexCoord2i glTexCoord2i
#define qglTexCoord2iv glTexCoord2iv
#define qglTexCoord2s glTexCoord2s
#define qglTexCoord2sv glTexCoord2sv
#define qglTexCoord3d glTexCoord3d
#define qglTexCoord3dv glTexCoord3dv
#define qglTexCoord3f glTexCoord3f
#define qglTexCoord3fv glTexCoord3fv
#define qglTexCoord3i glTexCoord3i
#define qglTexCoord3iv glTexCoord3iv
#define qglTexCoord3s glTexCoord3s
#define qglTexCoord3sv glTexCoord3sv
#define qglTexCoord4d glTexCoord4d
#define qglTexCoord4dv glTexCoord4dv
#define qglTexCoord4f glTexCoord4f
#define qglTexCoord4fv glTexCoord4fv
#define qglTexCoord4i glTexCoord4i
#define qglTexCoord4iv glTexCoord4iv
#define qglTexCoord4s glTexCoord4s
#define qglTexCoord4sv glTexCoord4sv
#define qglTexCoordPointer vkglTexCoordPointer
#define qglTexEnvf vkglTexEnvf
#define qglTexEnvfv glTexEnvfv
#define qglTexEnvi glTexEnvi
#define qglTexEnviv glTexEnviv
#define qglTexGend glTexGend
#define qglTexGendv glTexGendv
#define qglTexGenf glTexGenf
#define qglTexGenfv glTexGenfv
#define qglTexGeni glTexGeni
#define qglTexGeniv glTexGeniv
#define qglTexImage1D glTexImage1D
#define qglTexImage2D vkglTexImage2D
#define qglTexParameterf vkglTexParameterf
#define qglTexParameterfv vkglTexParameterfv
#define qglTexParameteri vkglTexParameteri
#define qglTexParameteriv glTexParameteriv
#define qglTexSubImage1D glTexSubImage1D
#define qglTexSubImage2D vkglTexSubImage2D
#define qglTranslated glTranslated
#define qglTranslatef vkglTranslatef
#define qglVertex2d glVertex2d
#define qglVertex2dv glVertex2dv
#define qglVertex2f vkglVertex2f
#define qglVertex2fv glVertex2fv
#define qglVertex2i glVertex2i
#define qglVertex2iv glVertex2iv
#define qglVertex2s glVertex2s
#define qglVertex2sv glVertex2sv
#define qglVertex3d glVertex3d
#define qglVertex3dv glVertex3dv
#define qglVertex3f vkglVertex3f
#define qglVertex3fv vkglVertex3fv
#define qglVertex3i glVertex3i
#define qglVertex3iv glVertex3iv
#define qglVertex3s glVertex3s
#define qglVertex3sv glVertex3sv
#define qglVertex4d glVertex4d
#define qglVertex4dv glVertex4dv
#define qglVertex4f glVertex4f
#define qglVertex4fv glVertex4fv
#define qglVertex4i glVertex4i
#define qglVertex4iv glVertex4iv
#define qglVertex4s glVertex4s
#define qglVertex4sv glVertex4sv
#define qglVertexPointer vkglVertexPointer
#define qglViewport vkglViewport

#if !defined(__APPLE__)
extern PFNGLSTENCILOPSEPARATEPROC qglStencilOpSeparate;
#endif

extern PFNGLACTIVETEXTUREARBPROC qglActiveTextureARB;
extern PFNGLMINSAMPLESHADINGARBPROC qglMinSampleShadingARB;
extern PFNGLCLIENTACTIVETEXTUREARBPROC qglClientActiveTextureARB;
extern PFNGLMULTITEXCOORD2FARBPROC qglMultiTexCoord2fARB;

extern PFNGLCOMBINERPARAMETERFVNVPROC qglCombinerParameterfvNV;
extern PFNGLCOMBINERPARAMETERIVNVPROC qglCombinerParameterivNV;
extern PFNGLCOMBINERPARAMETERFNVPROC qglCombinerParameterfNV;
extern PFNGLCOMBINERPARAMETERINVPROC qglCombinerParameteriNV;
extern PFNGLCOMBINERINPUTNVPROC qglCombinerInputNV;
extern PFNGLCOMBINEROUTPUTNVPROC qglCombinerOutputNV;

extern PFNGLFINALCOMBINERINPUTNVPROC qglFinalCombinerInputNV;
extern PFNGLGETCOMBINERINPUTPARAMETERFVNVPROC qglGetCombinerInputParameterfvNV;
extern PFNGLGETCOMBINERINPUTPARAMETERIVNVPROC qglGetCombinerInputParameterivNV;
extern PFNGLGETCOMBINEROUTPUTPARAMETERFVNVPROC qglGetCombinerOutputParameterfvNV;
extern PFNGLGETCOMBINEROUTPUTPARAMETERIVNVPROC qglGetCombinerOutputParameterivNV;
extern PFNGLGETFINALCOMBINERINPUTPARAMETERFVNVPROC qglGetFinalCombinerInputParameterfvNV;
extern PFNGLGETFINALCOMBINERINPUTPARAMETERIVNVPROC qglGetFinalCombinerInputParameterivNV;

extern PFNGLPROGRAMSTRINGARBPROC qglProgramStringARB;
extern PFNGLBINDPROGRAMARBPROC qglBindProgramARB;
extern PFNGLDELETEPROGRAMSARBPROC qglDeleteProgramsARB;
extern PFNGLGENPROGRAMSARBPROC qglGenProgramsARB;
extern PFNGLPROGRAMENVPARAMETER4DARBPROC qglProgramEnvParameter4dARB;
extern PFNGLPROGRAMENVPARAMETER4DVARBPROC qglProgramEnvParameter4dvARB;
extern PFNGLPROGRAMENVPARAMETER4FARBPROC qglProgramEnvParameter4fARB;
extern PFNGLPROGRAMENVPARAMETER4FVARBPROC qglProgramEnvParameter4fvARB;
extern PFNGLPROGRAMLOCALPARAMETER4DARBPROC qglProgramLocalParameter4dARB;
extern PFNGLPROGRAMLOCALPARAMETER4DVARBPROC qglProgramLocalParameter4dvARB;
extern PFNGLPROGRAMLOCALPARAMETER4FARBPROC qglProgramLocalParameter4fARB;
extern PFNGLPROGRAMLOCALPARAMETER4FVARBPROC qglProgramLocalParameter4fvARB;
extern PFNGLGETPROGRAMENVPARAMETERDVARBPROC qglGetProgramEnvParameterdvARB;
extern PFNGLGETPROGRAMENVPARAMETERFVARBPROC qglGetProgramEnvParameterfvARB;
extern PFNGLGETPROGRAMLOCALPARAMETERDVARBPROC qglGetProgramLocalParameterdvARB;
extern PFNGLGETPROGRAMLOCALPARAMETERFVARBPROC qglGetProgramLocalParameterfvARB;
extern PFNGLGETPROGRAMIVARBPROC qglGetProgramivARB;
extern PFNGLGETPROGRAMSTRINGARBPROC qglGetProgramStringARB;
extern PFNGLISPROGRAMARBPROC qglIsProgramARB;

// GLSL, core since OpenGL 2.0 (tr_glsl.cpp)
extern PFNGLCREATESHADERPROC qglCreateShader;
extern PFNGLSHADERSOURCEPROC qglShaderSource;
extern PFNGLCOMPILESHADERPROC qglCompileShader;
extern PFNGLGETSHADERIVPROC qglGetShaderiv;
extern PFNGLGETSHADERINFOLOGPROC qglGetShaderInfoLog;
extern PFNGLDELETESHADERPROC qglDeleteShader;
extern PFNGLCREATEPROGRAMPROC qglCreateProgram;
extern PFNGLATTACHSHADERPROC qglAttachShader;
extern PFNGLLINKPROGRAMPROC qglLinkProgram;
extern PFNGLGETPROGRAMIVPROC qglGetProgramiv;
extern PFNGLGETPROGRAMINFOLOGPROC qglGetProgramInfoLog;
extern PFNGLDELETEPROGRAMPROC qglDeleteProgram;
extern PFNGLUSEPROGRAMPROC qglUseProgram;
extern PFNGLGETUNIFORMLOCATIONPROC qglGetUniformLocation;
extern PFNGLUNIFORM1IPROC qglUniform1i;
extern PFNGLUNIFORM1FPROC qglUniform1f;
extern PFNGLUNIFORM3FPROC qglUniform3f;
extern PFNGLUNIFORM2FPROC qglUniform2f;
extern PFNGLUNIFORM2FVPROC qglUniform2fv;
extern PFNGLUNIFORM4FPROC qglUniform4f;
extern PFNGLUNIFORM4FVPROC qglUniform4fv;

extern PFNGLLOCKARRAYSEXTPROC qglLockArraysEXT;
extern PFNGLUNLOCKARRAYSEXTPROC qglUnlockArraysEXT;
