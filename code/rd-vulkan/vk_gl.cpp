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
// Calls that are not implemented yet (see the plan) do nothing.

#include "../server/exe_headers.h"
#include "../rd-vanilla/tr_local.h"
#include "vk_priv.h"

void vkglAlphaFunc( GLenum func, GLclampf ref ) { }
void vkglArrayElement( GLint i ) { }
void vkglBegin( GLenum mode ) { }
void vkglBindTexture( GLenum target, GLuint texture ) { }
void vkglBlendFunc( GLenum sfactor, GLenum dfactor ) { }
void vkglCallList( GLuint list ) { }
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
void vkglDisableClientState( GLenum cap ) { }
void vkglDrawArrays( GLenum mode, GLint first, GLsizei count ) { }
void vkglDrawBuffer( GLenum mode ) { }
void vkglDrawElements( GLenum mode, GLsizei count, GLenum type, const GLvoid *indices ) { }
void vkglEnableClientState( GLenum cap ) { }
void vkglEnd( void ) { }
void vkglEndList( void ) { }
void vkglFogf( GLenum pname, GLfloat param ) { }
void vkglFogfv( GLenum pname, const GLfloat *params ) { }
void vkglFogi( GLenum pname, GLint param ) { }
GLuint vkglGenLists( GLsizei range ) { return 0; }
void vkglGenTextures( GLsizei n, GLuint *textures ) { }
void vkglGetDoublev( GLenum pname, GLdouble *params ) { }
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

// Extension entry points the renderer looks for through function pointers (set in VK_Init).
void APIENTRY vkglActiveTextureARB( GLenum texture ) {}
void APIENTRY vkglClientActiveTextureARB( GLenum texture ) {}
void APIENTRY vkglMultiTexCoord2fARB( GLenum target, GLfloat s, GLfloat t ) {}
void APIENTRY vkglLockArraysEXT( GLint first, GLsizei count ) {}
void APIENTRY vkglUnlockArraysEXT( void ) {}
void APIENTRY vkglStencilOpSeparate( GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass ) {}

/*
=============================================================================

STATE

=============================================================================
*/

static struct vkglState_s {
	bool		scissorTest;
	int			scissor[4];		// x, y, w, h, OpenGL style (origin bottom left)
	int			viewport[4];
} vkgl;

void vkglEnable( GLenum cap )
{
	if ( cap == GL_SCISSOR_TEST ) vkgl.scissorTest = true;
}

void vkglDisable( GLenum cap )
{
	if ( cap == GL_SCISSOR_TEST ) vkgl.scissorTest = false;
}

GLboolean vkglIsEnabled( GLenum cap )
{
	if ( cap == GL_SCISSOR_TEST ) return vkgl.scissorTest ? GL_TRUE : GL_FALSE;
	return GL_FALSE;
}

void vkglViewport( GLint x, GLint y, GLsizei width, GLsizei height )
{
	vkgl.viewport[0] = x;
	vkgl.viewport[1] = y;
	vkgl.viewport[2] = width;
	vkgl.viewport[3] = height;
}

void vkglScissor( GLint x, GLint y, GLsizei width, GLsizei height )
{
	vkgl.scissor[0] = x;
	vkgl.scissor[1] = y;
	vkgl.scissor[2] = width;
	vkgl.scissor[3] = height;
}

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
	if ( vkgl.scissorTest )
	{
		int x = Q_max( vkgl.scissor[0], 0 );
		int y = Q_max( vkgl.scissor[1], 0 );
		int w = Q_min( vkgl.scissor[2], vk.width - x );
		int h = Q_min( vkgl.scissor[3], vk.height - y );
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
	case GL_SAMPLES:			*params = 0; break;
	default:					*params = 0; break;
	}
}

void vkglGetFloatv( GLenum pname, GLfloat *params )
{
	*params = 0.0f;
}

const GLubyte *vkglGetString( GLenum name )
{
	return (const GLubyte *)"";
}

void vkglReadPixels( GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels )
{
	if ( format == GL_DEPTH_COMPONENT && type == GL_FLOAT )
	{
		// (depth read backs are not done yet: report the far plane, which means "nothing in front")
		for ( int i = 0; i < width * height; i++ ) ( (float *)pixels )[i] = 1.0f;
		return;
	}
	VK_ReadPixels( x, y, width, height, format, type, pixels );
}
