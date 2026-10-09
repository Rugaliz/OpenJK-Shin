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

// Frames of the Vulkan renderer module: acquire, record, blit the render target to the swapchain and present.
// A frame starts the first time something is drawn (or cleared) and ends in ri.WIN_Present (VK_PresentFrame).

#include "../server/exe_headers.h"
#include "../rd-vanilla/tr_local.h"
#include "vk_priv.h"

void VK_ImageBarrier( VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
	VkImageLayout oldLayout, VkImageLayout newLayout,
	VkPipelineStageFlags srcStage, VkAccessFlags srcAccess,
	VkPipelineStageFlags dstStage, VkAccessFlags dstAccess )
{
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.srcAccessMask = srcAccess;
	barrier.dstAccessMask = dstAccess;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = aspect;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier( cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &barrier );
}

static void VK_BeginCommandBuffer( void )
{
	vkFrame_t *frame = &vk.frames[vk.frameIndex];
	int &count = vk.frameCommandBufferCount[vk.frameIndex];
	if ( count >= (int)( sizeof( vk.frameCommandBuffers[0] ) / sizeof( vk.frameCommandBuffers[0][0] ) ) )
	{
		ri.Error( ERR_FATAL, "Vulkan: too many command buffers in a frame" );
	}
	VkCommandBufferAllocateInfo allocate = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	allocate.commandPool = frame->pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1;
	VK_CheckResult( vkAllocateCommandBuffers( vk.device, &allocate, &vk.frameCommandBuffers[vk.frameIndex][count] ), "vkAllocateCommandBuffers" );
	vk.cmd = vk.frameCommandBuffers[vk.frameIndex][count++];

	VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VK_CheckResult( vkBeginCommandBuffer( vk.cmd, &begin ), "vkBeginCommandBuffer" );
	VK_ResetCommandBufferState();
}

static void VK_BeginRenderPass( bool clear )
{
	VkClearValue clearValues[2] = {};
	clearValues[0].color.float32[0] = vk.clearColor[0];
	clearValues[0].color.float32[1] = vk.clearColor[1];
	clearValues[0].color.float32[2] = vk.clearColor[2];
	clearValues[0].color.float32[3] = vk.clearColor[3];
	clearValues[1].depthStencil.depth = vk.clearDepth;
	clearValues[1].depthStencil.stencil = (uint32_t)vk.clearStencil;

	VkRenderPassBeginInfo begin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	begin.renderPass = clear ? vk.renderPassClear : vk.renderPassLoad;
	begin.framebuffer = vk.framebuffer;
	begin.renderArea.extent.width = (uint32_t)vk.width;
	begin.renderArea.extent.height = (uint32_t)vk.height;
	begin.clearValueCount = 2;
	begin.pClearValues = clearValues;
	vkCmdBeginRenderPass( vk.cmd, &begin, VK_SUBPASS_CONTENTS_INLINE );
	vk.inRenderPass = true;
	VK_ResetCommandBufferState();
}

/*
=================
VK_EnsureFrame
=================
*/
void VK_EnsureFrame( void )
{
	if ( vk.frameStarted )
	{
		return;
	}

	// vsync settings changed
	cvar_t *swapInterval = ri.Cvar_Get( "r_swapInterval", "0", CVAR_ARCHIVE_ND );
	cvar_t *adaptive = ri.Cvar_Get( "r_adaptiveVsync", "0", CVAR_ARCHIVE_ND );
	if ( swapInterval->modified || adaptive->modified )
	{
		swapInterval->modified = qfalse;
		adaptive->modified = qfalse;
		vk.swapchainDirty = true;
	}
	if ( vk.swapchainDirty )
	{
		VK_CreateSwapchain();
	}

	vkFrame_t *frame = &vk.frames[vk.frameIndex];
	if ( frame->fenceSubmitted )
	{
		VK_CheckResult( vkWaitForFences( vk.device, 1, &frame->fence, VK_TRUE, UINT64_MAX ), "vkWaitForFences" );
		frame->fenceSubmitted = false;
	}

	for ( ;; )
	{
		const VkResult result = vkAcquireNextImageKHR( vk.device, vk.swapchain, UINT64_MAX, frame->imageAvailable, VK_NULL_HANDLE, &vk.imageIndex );
		if ( result == VK_ERROR_OUT_OF_DATE_KHR )
		{
			VK_CreateSwapchain();
			continue;
		}
		if ( result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR )
		{
			VK_CheckResult( result, "vkAcquireNextImageKHR" );
		}
		break;
	}

	VK_CheckResult( vkResetCommandPool( vk.device, frame->pool, 0 ), "vkResetCommandPool" );
	vk.frameCommandBufferCount[vk.frameIndex] = 0;
	VK_ProcessDeferredDeletes();
	VK_BeginFrameDraw();
	vk.waitedForImage = false;
	VK_BeginCommandBuffer();
	VK_BeginRenderPass( true );
	vk.frameStarted = true;
}

void VK_EnsureRenderPass( void )
{
	VK_EnsureFrame();
	if ( !vk.inRenderPass )
	{
		VK_BeginRenderPass( false );
	}
}

void VK_EndRenderPass( void )
{
	if ( vk.inRenderPass )
	{
		vkCmdEndRenderPass( vk.cmd );
		vk.inRenderPass = false;
	}
}

/*
=================
VK_SubmitFrame

Sends what was recorded so far. With waitIdle the work is done when this returns (for read backs), and recording
goes on in a new command buffer. The frame's last submit is made by VK_PresentFrame.
=================
*/
void VK_SubmitFrame( bool waitIdle )
{
	VK_EndRenderPass();
	VK_FlushUploads( false );	// (the texture uploads come first, the queue works in order)
	VK_CheckResult( vkEndCommandBuffer( vk.cmd ), "vkEndCommandBuffer" );

	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &vk.cmd;
	const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	if ( !vk.waitedForImage )
	{
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &vk.frames[vk.frameIndex].imageAvailable;
		submit.pWaitDstStageMask = &waitStage;
		vk.waitedForImage = true;
	}
	VK_CheckResult( vkQueueSubmit( vk.queue, 1, &submit, VK_NULL_HANDLE ), "vkQueueSubmit" );
	vk.submitSerial++;
	if ( waitIdle )
	{
		VK_CheckResult( vkQueueWaitIdle( vk.queue ), "vkQueueWaitIdle" );
	}
	VK_BeginCommandBuffer();
}

void VK_WaitIdle( void )
{
	if ( vk.device )
	{
		vkDeviceWaitIdle( vk.device );
	}
}

/*
=================
VK_PresentFrame

Replaces ri.WIN_Present: blits the render target to the swapchain image and shows it.
=================
*/
void VK_PresentFrame( window_t *window )
{
	VK_EnsureFrame();
	VK_EndRenderPass();
	VK_FlushUploads( false );

	vkFrame_t *frame = &vk.frames[vk.frameIndex];
	VkImage target = vk.swapchainImages[vk.imageIndex];

	VK_ImageBarrier( vk.cmd, vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT );
	VK_ImageBarrier( vk.cmd, target, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );

	VkImageBlit blit = {};
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.layerCount = 1;
	blit.srcOffsets[1].x = vk.width;
	blit.srcOffsets[1].y = vk.height;
	blit.srcOffsets[1].z = 1;
	blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.dstSubresource.layerCount = 1;
	blit.dstOffsets[1].x = (int32_t)vk.swapchainExtent.width;
	blit.dstOffsets[1].y = (int32_t)vk.swapchainExtent.height;
	blit.dstOffsets[1].z = 1;
	const bool sameSize = (int)vk.swapchainExtent.width == vk.width && (int)vk.swapchainExtent.height == vk.height;
	vkCmdBlitImage( vk.cmd, vk.colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blit, sameSize ? VK_FILTER_NEAREST : VK_FILTER_LINEAR );

	VK_ImageBarrier( vk.cmd, target, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0 );
	VK_ImageBarrier( vk.cmd, vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT );

	VK_CheckResult( vkEndCommandBuffer( vk.cmd ), "vkEndCommandBuffer" );

	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &vk.cmd;
	const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	if ( !vk.waitedForImage )
	{
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &frame->imageAvailable;
		submit.pWaitDstStageMask = &waitStage;
	}
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &vk.renderFinished[vk.imageIndex];
	VK_CheckResult( vkResetFences( vk.device, 1, &frame->fence ), "vkResetFences" );
	VK_CheckResult( vkQueueSubmit( vk.queue, 1, &submit, frame->fence ), "vkQueueSubmit" );
	frame->fenceSubmitted = true;
	vk.submitSerial++;
	vk.frameCounter++;

	VkPresentInfoKHR present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &vk.renderFinished[vk.imageIndex];
	present.swapchainCount = 1;
	present.pSwapchains = &vk.swapchain;
	present.pImageIndices = &vk.imageIndex;
	const VkResult result = vkQueuePresentKHR( vk.queue, &present );
	if ( result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR )
	{
		vk.swapchainDirty = true;
	}
	else
	{
		VK_CheckResult( result, "vkQueuePresentKHR" );
	}

	vk.frameStarted = false;
	vk.inRenderPass = false;
	vk.frameIndex = ( vk.frameIndex + 1 ) % VK_FRAMES_IN_FLIGHT;
}

/*
=================
VK_ReadPixels

glReadPixels of the colour buffer as it is now (the frame is sent and waited for). Rows go bottom to top like OpenGL.
=================
*/
void VK_ReadPixels( int x, int y, int width, int height, GLenum format, GLenum type, void *pixels )
{
	if ( width <= 0 || height <= 0 || type != GL_UNSIGNED_BYTE || ( format != GL_RGB && format != GL_RGBA ) )
	{
		return;
	}
	if ( x < 0 || y < 0 || x + width > vk.width || y + height > vk.height )
	{
		return;
	}

	VK_EnsureFrame();
	VK_EndRenderPass();

	const VkDeviceSize size = (VkDeviceSize)width * height * 4;
	VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufferInfo.size = size;
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBuffer buffer;
	VK_CheckResult( vkCreateBuffer( vk.device, &bufferInfo, NULL, &buffer ), "vkCreateBuffer" );
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements( vk.device, buffer, &requirements );
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = VK_FindMemoryType( requirements.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
	VkDeviceMemory memory;
	VK_CheckResult( vkAllocateMemory( vk.device, &allocate, NULL, &memory ), "vkAllocateMemory" );
	VK_CheckResult( vkBindBufferMemory( vk.device, buffer, memory, 0 ), "vkBindBufferMemory" );

	VK_ImageBarrier( vk.cmd, vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT );
	VkBufferImageCopy region = {};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageOffset.x = x;
	region.imageOffset.y = vk.height - ( y + height );
	region.imageExtent.width = (uint32_t)width;
	region.imageExtent.height = (uint32_t)height;
	region.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer( vk.cmd, vk.colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region );
	VK_ImageBarrier( vk.cmd, vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT );
	VK_SubmitFrame( true );

	void *mapped = NULL;
	VK_CheckResult( vkMapMemory( vk.device, memory, 0, size, 0, &mapped ), "vkMapMemory" );
	const byte *source = (const byte *)mapped;
	byte *out = (byte *)pixels;
	const int outBytes = format == GL_RGBA ? 4 : 3;
	// GL packs rows to GL_PACK_ALIGNMENT, which the renderer leaves at its default of 4
	const int packAlignment = 4;
	const int rowBytes = ( ( width * outBytes + packAlignment - 1 ) / packAlignment ) * packAlignment;
	for ( int row = 0; row < height; row++ )
	{
		const byte *in = source + (size_t)( height - 1 - row ) * width * 4;
		byte *dest = out + (size_t)row * rowBytes;
		if ( outBytes == 4 )
		{
			memcpy( dest, in, (size_t)width * 4 );
		}
		else
		{
			for ( int col = 0; col < width; col++ )
			{
				dest[col * 3 + 0] = in[col * 4 + 0];
				dest[col * 3 + 1] = in[col * 4 + 1];
				dest[col * 3 + 2] = in[col * 4 + 2];
			}
		}
	}
	vkUnmapMemory( vk.device, memory );
	vkDestroyBuffer( vk.device, buffer, NULL );
	vkFreeMemory( vk.device, memory, NULL );
}

/*
=================
VK_ReadDepth

glReadPixels of one depth value (the flares ask whether something is in front of them), 0 (near) to 1 (far).
=================
*/
float VK_ReadDepth( int x, int y )
{
	if ( x < 0 || y < 0 || x >= vk.width || y >= vk.height )
	{
		return 1.0f;
	}

	VK_EnsureFrame();
	VK_EndRenderPass();

	VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufferInfo.size = 16;
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBuffer buffer;
	VK_CheckResult( vkCreateBuffer( vk.device, &bufferInfo, NULL, &buffer ), "vkCreateBuffer" );
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements( vk.device, buffer, &requirements );
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = VK_FindMemoryType( requirements.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
	VkDeviceMemory memory;
	VK_CheckResult( vkAllocateMemory( vk.device, &allocate, NULL, &memory ), "vkAllocateMemory" );
	VK_CheckResult( vkBindBufferMemory( vk.device, buffer, memory, 0 ), "vkBindBufferMemory" );

	const VkImageAspectFlags aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	VK_ImageBarrier( vk.cmd, vk.depthImage, aspects, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT );
	VkBufferImageCopy region = {};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageOffset.x = x;
	region.imageOffset.y = vk.height - 1 - y;
	region.imageExtent.width = 1;
	region.imageExtent.height = 1;
	region.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer( vk.cmd, vk.depthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region );
	VK_ImageBarrier( vk.cmd, vk.depthImage, aspects, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT );
	VK_SubmitFrame( true );

	void *mapped = NULL;
	VK_CheckResult( vkMapMemory( vk.device, memory, 0, 16, 0, &mapped ), "vkMapMemory" );
	float depth;
	if ( vk.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT )
	{
		depth = *(const float *)mapped;
	}
	else
	{
		// (24 bit depth comes out in the low bits of a 32 bit texel)
		depth = (float)( *(const unsigned int *)mapped & 0xFFFFFFu ) / 16777215.0f;
	}
	vkUnmapMemory( vk.device, memory );
	vkDestroyBuffer( vk.device, buffer, NULL );
	vkFreeMemory( vk.device, memory, NULL );
	return depth;
}
