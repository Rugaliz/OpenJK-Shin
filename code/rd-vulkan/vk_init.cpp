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

// Vulkan start-up and shutdown for the Vulkan renderer module (docs/vulkan-renderer.md).

#include "../server/exe_headers.h"
#include "../rd-vanilla/tr_local.h"

void APIENTRY vkglActiveTextureARB( GLenum texture );
void APIENTRY vkglClientActiveTextureARB( GLenum texture );
void APIENTRY vkglMultiTexCoord2fARB( GLenum target, GLfloat s, GLfloat t );
void APIENTRY vkglLockArraysEXT( GLint first, GLsizei count );
void APIENTRY vkglUnlockArraysEXT( void );
void APIENTRY vkglStencilOpSeparate( GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass );

extern bool g_bDynamicGlowSupported;

void VK_Init( glconfig_t *glConfig )
{
	glConfig->vendor_string = "OpenJK";
	glConfig->renderer_string = "Vulkan (work in progress)";
	glConfig->version_string = "Vulkan";
	glConfig->extensions_string = "";

	glConfig->colorBits = 24;
	glConfig->depthBits = 24;
	glConfig->stencilBits = 8;

	glConfig->maxTextureSize = 4096;
	glConfig->maxActiveTextures = 4;
	glConfig->clampToEdgeAvailable = qtrue;
	glConfig->textureEnvAddAvailable = qtrue;
	glConfig->textureCompression = TC_NONE;
	glConfig->doStencilShadowsInOneDrawcall = qtrue;

	// what the layer implements (everything else stays NULL: no GLSL, ARB programs, register combiners, framebuffers)
	qglActiveTextureARB = vkglActiveTextureARB;
	qglClientActiveTextureARB = vkglClientActiveTextureARB;
	qglMultiTexCoord2fARB = vkglMultiTexCoord2fARB;
	qglLockArraysEXT = vkglLockArraysEXT;
	qglUnlockArraysEXT = vkglUnlockArraysEXT;
	qglStencilOpSeparate = vkglStencilOpSeparate;

	g_bDynamicGlowSupported = false;
	ri.Cvar_Set( "r_DynamicGlow", "0" );
}

void VK_Shutdown( void )
{
}
