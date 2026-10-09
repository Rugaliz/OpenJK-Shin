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

// Internals of the Vulkan renderer module: only the vk_*.cpp files include this (it pulls in the Vulkan headers).

#pragma once

#include <vulkan/vulkan.h>

#define VK_FRAMES_IN_FLIGHT	2
#define VK_MAX_SWAPCHAIN_IMAGES	8

typedef struct vkFrame_s {
	VkCommandPool	pool;
	VkFence			fence;
	VkSemaphore		imageAvailable;
	bool			fenceSubmitted;		// the fence will be signalled by a pending submit
} vkFrame_t;

typedef struct vk_s {
	// instance, device
	VkInstance					instance;
	VkDebugUtilsMessengerEXT	messenger;
	VkSurfaceKHR				surface;
	VkPhysicalDevice			physicalDevice;
	VkPhysicalDeviceProperties	properties;
	VkPhysicalDeviceMemoryProperties	memoryProperties;
	VkPhysicalDeviceFeatures	features;
	VkDevice					device;
	uint32_t					queueFamily;
	VkQueue						queue;

	// swapchain: what is shown. The renderer draws into colorImage and blits it here at the end of the frame
	VkSwapchainKHR				swapchain;
	VkFormat					swapchainFormat;
	VkExtent2D					swapchainExtent;
	uint32_t					swapchainImageCount;
	VkImage						swapchainImages[VK_MAX_SWAPCHAIN_IMAGES];
	VkSemaphore					renderFinished[VK_MAX_SWAPCHAIN_IMAGES];
	VkPresentModeKHR			presentMode;

	// the render target (vidWidth x vidHeight)
	int							width, height;
	VkFormat					colorFormat;
	VkFormat					depthFormat;
	VkImage						colorImage;
	VkDeviceMemory				colorMemory;
	VkImageView					colorView;
	VkImage						depthImage;
	VkDeviceMemory				depthMemory;
	VkImageView					depthView;
	VkRenderPass				renderPassClear;	// first pass of the frame: clears colour, depth and stencil
	VkRenderPass				renderPassLoad;		// after the frame was interrupted (copies, read backs): keeps them
	VkFramebuffer				framebuffer;

	// frames
	vkFrame_t					frames[VK_FRAMES_IN_FLIGHT];
	int							frameIndex;
	bool						frameStarted;		// the command buffer is recording, the swapchain image is acquired
	bool						inRenderPass;
	bool						waitedForImage;		// the first submit of the frame waits for the acquired image
	uint32_t					imageIndex;
	VkCommandBuffer				cmd;				// current command buffer of the frame
	VkCommandBuffer				frameCommandBuffers[VK_FRAMES_IN_FLIGHT][16];
	int							frameCommandBufferCount[VK_FRAMES_IN_FLIGHT];

	// what the GL state machine of the layer says is wanted
	float						clearColor[4];
	float						clearDepth;
	int							clearStencil;

	bool						swapchainDirty;		// recreate before the next frame (vsync mode changed, window resized)

	// time, for knowing when the GPU is done with something
	int							frameCounter;		// frames presented so far
	unsigned int				submitSerial;		// the main command buffer of the frame is sent: this counts them
	bool						commandBufferFresh;	// nothing was recorded in the current command buffer yet (dynamic state is unset)
} vk_t;

extern vk_t vk;

// vk_init.cpp
uint32_t	VK_FindMemoryType( uint32_t typeBits, VkMemoryPropertyFlags properties );
void		VK_CreateSwapchain( void );
void		VK_DestroySwapchain( void );
void		VK_CheckResult( VkResult result, const char *what );

// vk_frame.cpp
void		VK_EnsureFrame( void );			// starts the frame if it was not started yet (acquire, record, begin the render pass)
void		VK_EnsureRenderPass( void );		// inside a frame: makes sure the render pass is open
void		VK_EndRenderPass( void );
void		VK_SubmitFrame( bool waitIdle );	// sends what was recorded so far (mid frame: read backs), a new command buffer follows
void		VK_PresentFrame( window_t *window );
void		VK_ImageBarrier( VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
				VkImageLayout oldLayout, VkImageLayout newLayout,
				VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
				VkPipelineStageFlags dstStage, VkAccessFlags dstAccess );
void		VK_ReadPixels( int x, int y, int width, int height, GLenum format, GLenum type, void *pixels );
void		VK_WaitIdle( void );

// vk_mem.cpp
typedef struct vkAlloc_s {
	VkDeviceMemory	memory;
	VkDeviceSize	offset;
	VkDeviceSize	size;
	int				block;
} vkAlloc_t;

bool		VK_AllocateMemory( const VkMemoryRequirements *requirements, VkMemoryPropertyFlags properties, vkAlloc_t *alloc );
void		VK_FreeMemory( vkAlloc_t *alloc );
void		VK_ShutdownMemory( void );

// vk_image.cpp
typedef struct vkTexture_s {
	unsigned long long	serial;			// never reused, so descriptor sets can be matched on it
	VkImage				image;
	VkImageView			view;
	vkAlloc_t			memory;
	int					width, height;
	int					mipLevels;
	GLenum				minFilter, magFilter, wrapS, wrapT;
	float				anisotropy;
	int					lastUsedFrame;	// vk.frameCounter of the last draw with it
	unsigned int		lastUsedSubmit;	// vk.submitSerial of the last draw with it
} vkTexture_t;

vkTexture_t	*VK_TextureForId( GLuint id, bool create );
vkTexture_t	*VK_WhiteTexture( void );
void		VK_TexImage2D( GLuint id, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels );
void		VK_TexSubImage2D( GLuint id, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels );
void		VK_TexParameter( GLuint id, GLenum pname, GLfloat value );
void		VK_DeleteTexture( GLuint id );
VkSampler	VK_SamplerFor( const vkTexture_t *texture );
void		VK_FlushUploads( bool wait );
void		VK_ProcessDeferredDeletes( void );
void		VK_InitImages( void );
void		VK_ShutdownImages( void );

// vk_draw.cpp
// Everything that makes a pipeline of its own (fixed function state the renderer sets). Compared and hashed as bytes:
// clear it with memset before filling it in.
typedef struct vkPipelineKey_s {
	unsigned short	srcBlend, dstBlend;			// GL enums
	unsigned short	depthFunc;
	unsigned short	alphaFunc;
	unsigned short	stencilFunc[2];				// front, back
	unsigned short	stencilFail[2], stencilZFail[2], stencilZPass[2];
	float			alphaRef;
	unsigned char	blendEnable;
	unsigned char	depthTest, depthWrite;
	unsigned char	cullMode;					// 0 none, 1 front, 2 back
	unsigned char	lines;						// primitives are lines, not triangles
	unsigned char	wireframe;					// polygon mode line
	unsigned char	polyOffset;
	unsigned char	unitMask, env0, env1;		// texture units on and their GL_TEXTURE_ENV_MODE
	unsigned char	fogMode;					// 0 off, 1 linear, 2 exp2
	unsigned char	clip;
	unsigned char	colorMask;					// bit per channel
	unsigned char	colorConst, uv0Const, uv1Const;	// no array: one value for all vertices
	unsigned char	stencilTest;
	unsigned char	pad[1];
} vkPipelineKey_t;

// Set per draw (dynamic state of the pipelines). Viewport and scissor are OpenGL style (origin bottom left).
typedef struct vkDynamicState_s {
	float			viewport[4];
	float			depthRange[2];
	int				scissor[4];
	float			polyOffsetFactor, polyOffsetUnits;
	unsigned int	stencilRef[2], stencilCompareMask[2], stencilWriteMask[2];
	float			lineWidth;
} vkDynamicState_t;

// the push constants of fixed.vert / fixed.frag
typedef struct vkConstants_s {
	float	mvp[16];
	float	clipPlane[4];
	float	eyeZ[4];
	float	fogColorDensity[4];
	float	fogRange[4];
} vkConstants_t;

// where a draw's vertices go (inside the buffer for the frame); pointers are valid until the next call
typedef struct vkGeometry_s {
	float			*position;		// 3 per vertex
	byte			*color;			// 4 per vertex (just 4 in all when colorConst)
	float			*texCoord0;		// 2 per vertex (just 2 when uv0Const)
	float			*texCoord1;
	unsigned int	*indices;		// NULL: draw the vertices in order
	VkDeviceSize	positionOffset, colorOffset, texCoord0Offset, texCoord1Offset, indexOffset;
	int				vertexCount, indexCount;
} vkGeometry_t;

void		VK_AllocateGeometry( vkGeometry_t *geometry, int vertexCount, int indexCount, const vkPipelineKey_t *key );
void		VK_Draw( const vkPipelineKey_t *key, const vkDynamicState_t *dynamic, const vkConstants_t *constants,
				const vkTexture_t *texture0, const vkTexture_t *texture1, const vkGeometry_t *geometry );
void		VK_InitDraw( void );
void		VK_ShutdownDraw( void );
void		VK_BeginFrameDraw( void );		// the frame slot is free again: rewind its buffers
void		VK_ResetCommandBufferState( void );	// a new command buffer starts: nothing is bound, no dynamic state is set

// vk_gl.cpp
void		VK_ResetGLState( void );
