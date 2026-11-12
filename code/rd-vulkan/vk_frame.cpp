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
#include <chrono>
#include <unordered_map>

static void VK_FlaresCollect( void );
static void VK_FlaresCopy( void );

cvar_t *r_vkProfile;

double VK_Now( void )
{
	return std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now().time_since_epoch() ).count();
}

// says so when something that waits for the GPU took more than 2 ms
void VK_Slow( const char *what, double start )
{
	if ( r_vkProfile && r_vkProfile->integer )
	{
		const double took = VK_Now() - start;
		if ( took > 2.0 )
		{
			Com_Printf( "Vulkan: %s took %.1f ms\n", what, took );
		}
	}
}

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
		// (sent that often in one frame: once all of it is done, the frame's command buffers are used again from the first)
		VK_CheckResult( vkQueueWaitIdle( vk.queue ), "vkQueueWaitIdle" );
		VK_CheckResult( vkResetCommandPool( vk.device, frame->pool, 0 ), "vkResetCommandPool" );
		count = 0;
	}
	// Made once and used again each time the pool is reset (a reset keeps them): one made every time was never given
	// back, and the driver's memory for each grew by megabytes a second until the system ran out
	if ( !vk.frameCommandBuffers[vk.frameIndex][count] )
	{
		VkCommandBufferAllocateInfo allocate = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		allocate.commandPool = frame->pool;
		allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = 1;
		VK_CheckResult( vkAllocateCommandBuffers( vk.device, &allocate, &vk.frameCommandBuffers[vk.frameIndex][count] ), "vkAllocateCommandBuffers" );
	}
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
	cvar_t *swapInterval = ri.Cvar_Get( "r_swapInterval", "1", CVAR_ARCHIVE_ND );
	cvar_t *adaptive = ri.Cvar_Get( "r_adaptiveVsync", "1", CVAR_ARCHIVE_ND );
	cvar_t *levelLoading = ri.Cvar_Get( "com_levelLoading", "0", 0 );
	if ( swapInterval->modified || adaptive->modified || levelLoading->modified )
	{
		swapInterval->modified = qfalse;
		adaptive->modified = qfalse;
		levelLoading->modified = qfalse;
		vk.swapchainDirty = true;
	}
	if ( vk.swapchainDirty )
	{
		VK_CreateSwapchain();
	}

	vkFrame_t *frame = &vk.frames[vk.frameIndex];
	if ( frame->fenceSubmitted )
	{
		const double waitStart = VK_Now();
		VK_CheckResult( vkWaitForFences( vk.device, 1, &frame->fence, VK_TRUE, UINT64_MAX ), "vkWaitForFences" );
		VK_Slow( "waiting for the frame before last", waitStart );
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
	VK_FlaresCollect();
	VK_ProcessDeferredDeletes();
	VK_BeginFrameDraw();
	VK_BeginFramePost();
	vk.waitedForImage = false;
	VK_BeginCommandBuffer();
	// The pass that clears is begun by the first draw (VK_EnsureRenderPass), not here: the ray tracing structure of a
	// view is built outside a pass first (VK_RT_BeginView), and a pass begun here would have been ended at once,
	// writing out and loading again every sample of a multisampled view for nothing.
	vk.clearPending = true;
	vk.frameStarted = true;
}

void VK_EnsureRenderPass( void )
{
	VK_EnsureFrame();
	if ( !vk.inRenderPass )
	{
		VK_BeginRenderPass( vk.clearPending );
		vk.clearPending = false;
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
		const double waitStart = VK_Now();
		VK_CheckResult( vkQueueWaitIdle( vk.queue ), "vkQueueWaitIdle" );
		VK_Slow( "waiting for the GPU (frame sent and finished)", waitStart );
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
VK_LimitLatency

After a frame is presented, before the game reads the input for the next one (r_lowLatency 1). The next frame waits
for the GPU to have drawn the frame before last anyway (VK_EnsureFrame): waiting here instead, the input is read after
that wait, not before it. And with vsync, the display takes the frames one a refresh while the game makes them faster:
they would queue up to the number of swapchain images (4 here) and each would be shown that many refreshes after its
input was read. Waiting for the last but one frame to be on screen keeps at most one waiting. (A window that is not
shown may never show its frames: the wait gives up after a while.)
=================
*/
static void VK_LimitLatency( void )
{
	if ( !r_lowLatency || !r_lowLatency->integer )
	{
		return;
	}

	vkFrame_t *next = &vk.frames[vk.frameIndex];
	if ( next->fenceSubmitted )
	{
		VK_CheckResult( vkWaitForFences( vk.device, 1, &next->fence, VK_TRUE, UINT64_MAX ), "vkWaitForFences" );
		next->fenceSubmitted = false;
	}

#ifdef VK_KHR_present_wait
	const bool queues = vk.presentMode == VK_PRESENT_MODE_FIFO_KHR || vk.presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
	if ( vk.presentWait && queues && vk.presentId > vk.presentIdFirst && !vk.swapchainDirty )
	{
		const VkResult result = qvkWaitForPresentKHR( vk.device, vk.swapchain, vk.presentId - 1, 100ull * 1000 * 1000 );
		if ( result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR )
		{
			vk.swapchainDirty = true;
		}
		else if ( result != VK_SUCCESS && result != VK_TIMEOUT && result != VK_SUBOPTIMAL_KHR )
		{
			VK_CheckResult( result, "vkWaitForPresentKHR" );
		}
	}
#endif
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
	VK_FlaresCopy();

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
#ifdef VK_KHR_present_wait
	VkPresentIdKHR presentId = { VK_STRUCTURE_TYPE_PRESENT_ID_KHR };
	if ( vk.presentWait )
	{
		vk.presentId++;
		presentId.swapchainCount = 1;
		presentId.pPresentIds = &vk.presentId;
		present.pNext = &presentId;
	}
#endif
	const VkResult result = vkQueuePresentKHR( vk.queue, &present );
	if ( result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR )
	{
		vk.swapchainDirty = true;
	}
	else
	{
		VK_CheckResult( result, "vkQueuePresentKHR" );
	}

	if ( r_vkProfile && r_vkProfile->integer )
	{
		static double lastPresent;
		const double now = VK_Now();
		if ( lastPresent && now - lastPresent > 40.0 && now - lastPresent < 3000.0 )
		{
			Com_Printf( "Vulkan: %.0f ms between two frames\n", now - lastPresent );
		}
		lastPresent = now;
	}

	vk.frameStarted = false;
	vk.inRenderPass = false;
	vk.frameIndex = ( vk.frameIndex + 1 ) % VK_FRAMES_IN_FLIGHT;

	VK_LimitLatency();
}

// The buffer the GPU copies into for read backs, kept between calls. It is in cached host memory: the CPU reads
// memory that is not cached (what the upload buffers use) many times slower, which made a screenshot take over 100 ms.
static VkBuffer			readbackBuffer;
static VkDeviceMemory	readbackMemory;
static VkDeviceSize		readbackSize;
static byte				*readbackMapped;
static bool				readbackCoherent;

static void EnsureReadback( VkDeviceSize size )
{
	if ( readbackSize >= size )
	{
		return;
	}
	VK_ShutdownReadback();
	VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufferInfo.size = size;
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CheckResult( vkCreateBuffer( vk.device, &bufferInfo, NULL, &readbackBuffer ), "vkCreateBuffer" );
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements( vk.device, readbackBuffer, &requirements );

	const VkMemoryPropertyFlags wanted[3] = {
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT };
	uint32_t type = 0xFFFFFFFFu;
	for ( int i = 0; i < 3 && type == 0xFFFFFFFFu; i++ )
	{
		for ( uint32_t t = 0; t < vk.memoryProperties.memoryTypeCount; t++ )
		{
			if ( ( requirements.memoryTypeBits & ( 1u << t ) ) && ( vk.memoryProperties.memoryTypes[t].propertyFlags & wanted[i] ) == wanted[i] )
			{
				type = t;
				readbackCoherent = ( wanted[i] & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ) != 0;
				break;
			}
		}
	}
	if ( type == 0xFFFFFFFFu )
	{
		ri.Error( ERR_FATAL, "Vulkan: no memory for read backs" );
	}
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	VK_CheckResult( vkAllocateMemory( vk.device, &allocate, NULL, &readbackMemory ), "vkAllocateMemory" );
	VK_CheckResult( vkBindBufferMemory( vk.device, readbackBuffer, readbackMemory, 0 ), "vkBindBufferMemory" );
	void *mapped;
	VK_CheckResult( vkMapMemory( vk.device, readbackMemory, 0, requirements.size, 0, &mapped ), "vkMapMemory" );
	readbackMapped = (byte *)mapped;
	readbackSize = requirements.size;
}

void VK_ShutdownReadback( void )
{
	if ( readbackBuffer )
	{
		vkDeviceWaitIdle( vk.device );
		vkUnmapMemory( vk.device, readbackMemory );
		vkDestroyBuffer( vk.device, readbackBuffer, NULL );
		vkFreeMemory( vk.device, readbackMemory, NULL );
	}
	readbackBuffer = VK_NULL_HANDLE;
	readbackMemory = VK_NULL_HANDLE;
	readbackMapped = NULL;
	readbackSize = 0;
}

// what the GPU wrote is seen by the CPU
static void ReadbackInvalidate( VkDeviceSize size )
{
	if ( !readbackCoherent )
	{
		VkMappedMemoryRange range = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
		range.memory = readbackMemory;
		range.size = VK_WHOLE_SIZE;
		vkInvalidateMappedMemoryRanges( vk.device, 1, &range );
	}
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
	EnsureReadback( size );

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
	vkCmdCopyImageToBuffer( vk.cmd, vk.colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer, 1, &region );
	VK_ImageBarrier( vk.cmd, vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT );
	VK_SubmitFrame( true );
	ReadbackInvalidate( size );

	const double convertStart = VK_Now();
	const byte *source = readbackMapped;
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
	VK_Slow( "turning the screen read back into pixels", convertStart );
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

	EnsureReadback( 16 );
	VkBuffer buffer = readbackBuffer;

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

	ReadbackInvalidate( 16 );
	const void *mapped = readbackMapped;
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
	return depth;
}

/*
=============================================================================

FLARES

A flare is drawn when nothing is in front of it: the original reads the depth at its pixel as it is drawn
(glReadPixels). Here that sent the frame so far and waited for the GPU, once for each flare in view: slow, and a level
with many (t1_sour has 36) used up the frame's command buffers. Now the pixels of the frame's flares are copied
together when the frame is shown, without waiting, and each flare goes by its own from two frames before (the frame
with the same index, waited for before this one began): about 14 ms at 144 frames a second, too little to see.

=============================================================================
*/

#define VK_MAX_FLARES	256

typedef struct {
	const void	*key;
	int			x, y;
	float		eyeZ, proj10, proj11, proj14;
} vkFlareRead_t;

static struct {
	VkBuffer		buffer[VK_FRAMES_IN_FLIGHT];
	VkDeviceMemory	memory[VK_FRAMES_IN_FLIGHT];
	const byte		*mapped[VK_FRAMES_IN_FLIGHT];
	vkFlareRead_t	reads[VK_FRAMES_IN_FLIGHT][VK_MAX_FLARES];
	int				count[VK_FRAMES_IN_FLIGHT];		// asked for in the frame being recorded with that index
	int				copied[VK_FRAMES_IN_FLIGHT];	// of them, copied when it was shown
} flares;

static std::unordered_map<const void *, bool> flareVisible;	// by key: what the last depth read said

static bool EnsureFlareBuffer( int index )
{
	if ( flares.buffer[index] )
	{
		return true;
	}
	VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufferInfo.size = VK_MAX_FLARES * sizeof( uint32_t );
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if ( vkCreateBuffer( vk.device, &bufferInfo, NULL, &flares.buffer[index] ) != VK_SUCCESS )
	{
		flares.buffer[index] = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements( vk.device, flares.buffer[index], &requirements );
	const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t type = 0xFFFFFFFFu;
	for ( uint32_t t = 0; t < vk.memoryProperties.memoryTypeCount && type == 0xFFFFFFFFu; t++ )
	{
		if ( ( requirements.memoryTypeBits & ( 1u << t ) ) && ( vk.memoryProperties.memoryTypes[t].propertyFlags & wanted ) == wanted )
		{
			type = t;
		}
	}
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	void *mapped = NULL;
	if ( type == 0xFFFFFFFFu || vkAllocateMemory( vk.device, &allocate, NULL, &flares.memory[index] ) != VK_SUCCESS )
	{
		vkDestroyBuffer( vk.device, flares.buffer[index], NULL );
		flares.buffer[index] = VK_NULL_HANDLE;
		flares.memory[index] = VK_NULL_HANDLE;
		return false;
	}
	VK_CheckResult( vkBindBufferMemory( vk.device, flares.buffer[index], flares.memory[index], 0 ), "vkBindBufferMemory" );
	VK_CheckResult( vkMapMemory( vk.device, flares.memory[index], 0, requirements.size, 0, &mapped ), "vkMapMemory" );
	flares.mapped[index] = (const byte *)mapped;
	return true;
}

// as a frame begins (the one before with its index is done): what its flares' pixels said
static void VK_FlaresCollect( void )
{
	const int index = vk.frameIndex;
	for ( int i = 0; i < flares.copied[index] && flares.mapped[index]; i++ )
	{
		const vkFlareRead_t *read = &flares.reads[index][i];
		const uint32_t texel = ( (const uint32_t *)flares.mapped[index] )[i];
		float depth;
		if ( vk.depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT )
		{
			memcpy( &depth, &texel, sizeof( depth ) );
		}
		else
		{
			depth = (float)( texel & 0xFFFFFFu ) / 16777215.0f;	// (24 bit depth comes out in the low bits)
		}
		// (as RB_TestZFlare: the eye distance of what is there, against the flare's own)
		const float screenZ = read->proj14 / ( ( 2.0f * depth - 1.0f ) * read->proj11 - read->proj10 );
		flareVisible[read->key] = ( -read->eyeZ - -screenZ ) < 24.0f;
	}
	flares.count[index] = 0;
	flares.copied[index] = 0;
}

// as the frame is shown (its views drawn, out of the render pass): the depth at the flares' pixels, copied together
static void VK_FlaresCopy( void )
{
	const int index = vk.frameIndex;
	const int count = flares.count[index];
	if ( !count || !EnsureFlareBuffer( index ) )
	{
		return;
	}
	VkBufferImageCopy regions[VK_MAX_FLARES];
	for ( int i = 0; i < count; i++ )
	{
		VkBufferImageCopy &region = regions[i];
		memset( &region, 0, sizeof( region ) );
		region.bufferOffset = (VkDeviceSize)i * sizeof( uint32_t );
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageOffset.x = flares.reads[index][i].x;
		region.imageOffset.y = vk.height - 1 - flares.reads[index][i].y;
		region.imageExtent.width = 1;
		region.imageExtent.height = 1;
		region.imageExtent.depth = 1;
	}
	const VkImageAspectFlags aspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	VK_ImageBarrier( vk.cmd, vk.depthImage, aspects, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT );
	vkCmdCopyImageToBuffer( vk.cmd, vk.depthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, flares.buffer[index], (uint32_t)count, regions );
	VK_ImageBarrier( vk.cmd, vk.depthImage, aspects, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT );
	// (for the host, which reads them once the frame's fence says it is done: VK_FlaresCollect)
	VkMemoryBarrier toHost = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier( vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, NULL, 0, NULL );
	flares.copied[index] = count;
}

bool VK_FlareVisible( const void *key, int x, int y, float eyeZ, float proj10, float proj11, float proj14 )
{
	VK_EnsureFrame();
	const int index = vk.frameIndex;
	if ( x >= 0 && y >= 0 && x < vk.width && y < vk.height && flares.count[index] < VK_MAX_FLARES )
	{
		const vkFlareRead_t read = { key, x, y, eyeZ, proj10, proj11, proj14 };
		flares.reads[index][flares.count[index]++] = read;
	}
	const auto found = flareVisible.find( key );
	return found != flareVisible.end() && found->second;	// (not known yet: not drawn, for the first two frames)
}

void VK_ShutdownFlares( void )
{
	for ( int i = 0; i < VK_FRAMES_IN_FLIGHT; i++ )
	{
		if ( flares.buffer[i] )
		{
			vkUnmapMemory( vk.device, flares.memory[i] );
			vkDestroyBuffer( vk.device, flares.buffer[i], NULL );
			vkFreeMemory( vk.device, flares.memory[i], NULL );
		}
	}
	memset( &flares, 0, sizeof( flares ) );
	flareVisible.clear();
}
