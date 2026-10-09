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
