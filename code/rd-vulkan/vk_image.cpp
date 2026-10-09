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

// Textures of the Vulkan renderer module: what qglTexImage2D / qglTexSubImage2D / qglTexParameter / qglDeleteTextures
// do. The renderer chooses its own texture numbers; a texture is a VkImage with the whole chain of mip levels (the
// levels the renderer does not upload are simply not sampled), always RGBA8. Uploads go through a staging buffer
// and a command buffer of their own that is sent before the frame's drawing.

#include "../server/exe_headers.h"
#include "../rd-vanilla/tr_local.h"
#include "vk_priv.h"
#include <vector>

#define STAGING_SIZE	( 64 * 1024 * 1024 )
#define UPLOAD_BUFFERS	4

static std::vector<vkTexture_t *>	textures;	// by the renderer's texture number
static vkTexture_t					whiteTexture;
static unsigned long long			nextSerial = 1;

// staging
static VkBuffer			stagingBuffer;
static VkDeviceMemory	stagingMemory;
static byte				*stagingMapped;
static VkDeviceSize		stagingSize;
static VkDeviceSize		stagingOffset;

// command buffers for the uploads
static VkCommandPool	uploadPool;
static VkCommandBuffer	uploadCommandBuffers[UPLOAD_BUFFERS];
static VkFence			uploadFences[UPLOAD_BUFFERS];
static bool				uploadFenceSubmitted[UPLOAD_BUFFERS];
static int				uploadIndex;
static bool				uploadRecording;

typedef struct deferredImage_s {
	VkImage		image;
	VkImageView	view;
	vkAlloc_t	memory;
	int			freeAtFrame;
} deferredImage_t;
static std::vector<deferredImage_t>	deferred;

typedef struct samplerEntry_s {
	GLenum	minFilter, magFilter, wrapS, wrapT;
	float	anisotropy;
	VkSampler	sampler;
} samplerEntry_t;
static std::vector<samplerEntry_t>	samplers;

static void CreateStaging( VkDeviceSize size )
{
	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	info.size = size;
	info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CheckResult( vkCreateBuffer( vk.device, &info, NULL, &stagingBuffer ), "vkCreateBuffer" );
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements( vk.device, stagingBuffer, &requirements );
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = VK_FindMemoryType( requirements.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
	VK_CheckResult( vkAllocateMemory( vk.device, &allocate, NULL, &stagingMemory ), "vkAllocateMemory" );
	VK_CheckResult( vkBindBufferMemory( vk.device, stagingBuffer, stagingMemory, 0 ), "vkBindBufferMemory" );
	void *mapped;
	VK_CheckResult( vkMapMemory( vk.device, stagingMemory, 0, size, 0, &mapped ), "vkMapMemory" );
	stagingMapped = (byte *)mapped;
	stagingSize = size;
	stagingOffset = 0;
}

static void ImageBarrier( VkCommandBuffer cmd, VkImage image, uint32_t baseMip, uint32_t mipCount,
	VkImageLayout oldLayout, VkImageLayout newLayout,
	VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess )
{
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.srcAccessMask = srcAccess;
	barrier.dstAccessMask = dstAccess;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = baseMip;
	barrier.subresourceRange.levelCount = mipCount;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier( cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &barrier );
}

static VkCommandBuffer UploadCommandBuffer( void )
{
	if ( !uploadRecording )
	{
		if ( uploadFenceSubmitted[uploadIndex] )
		{
			VK_CheckResult( vkWaitForFences( vk.device, 1, &uploadFences[uploadIndex], VK_TRUE, UINT64_MAX ), "vkWaitForFences" );
			uploadFenceSubmitted[uploadIndex] = false;
		}
		VK_CheckResult( vkResetCommandBuffer( uploadCommandBuffers[uploadIndex], 0 ), "vkResetCommandBuffer" );
		VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_CheckResult( vkBeginCommandBuffer( uploadCommandBuffers[uploadIndex], &begin ), "vkBeginCommandBuffer" );
		uploadRecording = true;
	}
	return uploadCommandBuffers[uploadIndex];
}

/*
=================
VK_FlushUploads

Sends the recorded texture uploads. They come before whatever is sent after them (the queue works in order); wait
makes sure they are done (and the staging buffer free again) when this returns.
=================
*/
void VK_FlushUploads( bool wait )
{
	if ( !uploadRecording )
	{
		if ( wait )
		{
			stagingOffset = 0;
		}
		return;
	}
	VkCommandBuffer cmd = uploadCommandBuffers[uploadIndex];
	VK_CheckResult( vkEndCommandBuffer( cmd ), "vkEndCommandBuffer" );
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	VK_CheckResult( vkResetFences( vk.device, 1, &uploadFences[uploadIndex] ), "vkResetFences" );
	VK_CheckResult( vkQueueSubmit( vk.queue, 1, &submit, uploadFences[uploadIndex] ), "vkQueueSubmit" );
	uploadFenceSubmitted[uploadIndex] = true;
	uploadRecording = false;
	if ( wait )
	{
		VK_CheckResult( vkWaitForFences( vk.device, 1, &uploadFences[uploadIndex], VK_TRUE, UINT64_MAX ), "vkWaitForFences" );
		uploadFenceSubmitted[uploadIndex] = false;
		// (everything sent before was done before this: the queue works in order)
		stagingOffset = 0;
	}
	uploadIndex = ( uploadIndex + 1 ) % UPLOAD_BUFFERS;
}

static VkDeviceSize StageSpace( VkDeviceSize size )
{
	size = ( size + 15 ) & ~(VkDeviceSize)15;
	if ( size > stagingSize )
	{
		// a texture bigger than the staging buffer: make a bigger one (the old one is not in use after the wait)
		VK_FlushUploads( true );
		vkDestroyBuffer( vk.device, stagingBuffer, NULL );
		vkUnmapMemory( vk.device, stagingMemory );
		vkFreeMemory( vk.device, stagingMemory, NULL );
		CreateStaging( size );
	}
	if ( stagingOffset + size > stagingSize )
	{
		VK_FlushUploads( true );
	}
	const VkDeviceSize offset = stagingOffset;
	stagingOffset += size;
	return offset;
}

static bool RgbOnlyFormat( GLint internalFormat )
{
	switch ( internalFormat )
	{
	case 3: case GL_RGB: case GL_RGB4: case GL_RGB5: case GL_RGB8: case GL_COMPRESSED_RGB_S3TC_DXT1_EXT: case GL_RGB4_S3TC:
		return true;
	}
	return false;
}

/*
=================
VK_TextureForId
=================
*/
vkTexture_t *VK_TextureForId( GLuint id, bool create )
{
	if ( id >= textures.size() )
	{
		if ( !create )
		{
			return NULL;
		}
		textures.resize( id + 1024, NULL );
	}
	if ( !textures[id] && create )
	{
		vkTexture_t *texture = new vkTexture_t();
		texture->serial = nextSerial++;
		texture->minFilter = GL_NEAREST_MIPMAP_LINEAR;	// the OpenGL defaults
		texture->magFilter = GL_LINEAR;
		texture->wrapS = texture->wrapT = GL_REPEAT;
		texture->anisotropy = 1.0f;
		textures[id] = texture;
	}
	return textures[id];
}

vkTexture_t *VK_WhiteTexture( void )
{
	return &whiteTexture;
}

static void DestroyImage( VkImage image, VkImageView view, vkAlloc_t *memory, int lastUsedFrame )
{
	if ( !image )
	{
		return;
	}
	if ( lastUsedFrame > vk.frameCounter - 2 )
	{
		// maybe still in use by a frame the GPU is working on
		deferredImage_t item = { image, view, *memory, lastUsedFrame + 2 };
		deferred.push_back( item );
		return;
	}
	vkDestroyImageView( vk.device, view, NULL );
	vkDestroyImage( vk.device, image, NULL );
	VK_FreeMemory( memory );
}

void VK_ProcessDeferredDeletes( void )
{
	for ( size_t i = 0; i < deferred.size(); )
	{
		if ( vk.frameCounter >= deferred[i].freeAtFrame )
		{
			vkDestroyImageView( vk.device, deferred[i].view, NULL );
			vkDestroyImage( vk.device, deferred[i].image, NULL );
			VK_FreeMemory( &deferred[i].memory );
			deferred[i] = deferred.back();
			deferred.pop_back();
		}
		else
		{
			i++;
		}
	}
}

// makes the storage of a texture (all mip levels, in the layout for sampling)
static bool CreateStorage( vkTexture_t *texture, int width, int height )
{
	int levels = 1;
	for ( int size = Q_max( width, height ); size > 1; size >>= 1 )
	{
		levels++;
	}

	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = VK_FORMAT_R8G8B8A8_UNORM;
	info.extent.width = (uint32_t)width;
	info.extent.height = (uint32_t)height;
	info.extent.depth = 1;
	info.mipLevels = (uint32_t)levels;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if ( vkCreateImage( vk.device, &info, NULL, &texture->image ) != VK_SUCCESS )
	{
		texture->image = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements( vk.device, texture->image, &requirements );
	if ( !VK_AllocateMemory( &requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &texture->memory ) )
	{
		vkDestroyImage( vk.device, texture->image, NULL );
		texture->image = VK_NULL_HANDLE;
		return false;
	}
	VK_CheckResult( vkBindImageMemory( vk.device, texture->image, texture->memory.memory, texture->memory.offset ), "vkBindImageMemory" );

	VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = texture->image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = info.format;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = (uint32_t)levels;
	viewInfo.subresourceRange.layerCount = 1;
	VK_CheckResult( vkCreateImageView( vk.device, &viewInfo, NULL, &texture->view ), "vkCreateImageView" );

	texture->width = width;
	texture->height = height;
	texture->mipLevels = levels;
	texture->serial = nextSerial++;

	// every level starts out readable (a level that is never uploaded must still be in a valid layout)
	ImageBarrier( UploadCommandBuffer(), texture->image, 0, (uint32_t)levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT );
	return true;
}

static void UploadRegion( vkTexture_t *texture, int level, int x, int y, int width, int height, const byte *rgba, bool forceOpaque )
{
	if ( !texture->image || level < 0 || level >= texture->mipLevels )
	{
		return;
	}
	const int levelWidth = Q_max( texture->width >> level, 1 );
	const int levelHeight = Q_max( texture->height >> level, 1 );
	if ( x < 0 || y < 0 || x + width > levelWidth || y + height > levelHeight )
	{
		return;
	}

	// a texture that is changed while the frame being recorded already draws with it: send that first
	if ( vk.frameStarted && texture->lastUsedSubmit == vk.submitSerial && texture->lastUsedFrame == vk.frameCounter )
	{
		VK_SubmitFrame( false );
	}

	const VkDeviceSize size = (VkDeviceSize)width * height * 4;
	const VkDeviceSize offset = StageSpace( size );
	if ( forceOpaque )
	{
		const byte *in = rgba;
		byte *out = stagingMapped + offset;
		for ( int i = 0; i < width * height; i++, in += 4, out += 4 )
		{
			out[0] = in[0];
			out[1] = in[1];
			out[2] = in[2];
			out[3] = 255;
		}
	}
	else
	{
		memcpy( stagingMapped + offset, rgba, (size_t)size );
	}

	VkCommandBuffer cmd = UploadCommandBuffer();
	ImageBarrier( cmd, texture->image, (uint32_t)level, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );
	VkBufferImageCopy region = {};
	region.bufferOffset = offset;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.mipLevel = (uint32_t)level;
	region.imageSubresource.layerCount = 1;
	region.imageOffset.x = x;
	region.imageOffset.y = y;
	region.imageExtent.width = (uint32_t)width;
	region.imageExtent.height = (uint32_t)height;
	region.imageExtent.depth = 1;
	vkCmdCopyBufferToImage( cmd, stagingBuffer, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
	ImageBarrier( cmd, texture->image, (uint32_t)level, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT );
}

void VK_TexImage2D( GLuint id, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels )
{
	if ( id == 0 || width <= 0 || height <= 0 || format != GL_RGBA || type != GL_UNSIGNED_BYTE )
	{
		return;
	}
	vkTexture_t *texture = VK_TextureForId( id, true );
	if ( level == 0 && ( !texture->image || texture->width != width || texture->height != height ) )
	{
		DestroyImage( texture->image, texture->view, &texture->memory, texture->lastUsedFrame );
		texture->image = VK_NULL_HANDLE;
		texture->view = VK_NULL_HANDLE;
		if ( !CreateStorage( texture, width, height ) )
		{
			Com_Printf( S_COLOR_YELLOW "Vulkan: out of memory for a %dx%d texture\n", width, height );
			return;
		}
	}
	if ( pixels )
	{
		UploadRegion( texture, level, 0, 0, width, height, (const byte *)pixels, RgbOnlyFormat( internalFormat ) );
	}
}

void VK_TexSubImage2D( GLuint id, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels )
{
	vkTexture_t *texture = VK_TextureForId( id, false );
	if ( !texture || !pixels || format != GL_RGBA || type != GL_UNSIGNED_BYTE )
	{
		return;
	}
	// (the cinematic textures are made with an RGB internal format: their alpha means nothing)
	UploadRegion( texture, level, x, y, width, height, (const byte *)pixels, false );
}

/*
=================
VK_CopyFramebuffer

glCopyTexImage2D / glCopyTexSubImage2D: a region of the colour buffer (as it is now) goes into a texture, upside
down relative to the buffer's rows so that it reads like an OpenGL texture (row 0 is the bottom row of the region).
With createWidth > 0 the texture is made that size first.
=================
*/
void VK_CopyFramebuffer( GLuint id, int createWidth, int createHeight, int dstX, int dstY, int srcX, int srcY, int width, int height )
{
	if ( id == 0 || width <= 0 || height <= 0 )
	{
		return;
	}
	vkTexture_t *texture = VK_TextureForId( id, true );
	if ( createWidth > 0 && ( !texture->image || texture->width != createWidth || texture->height != createHeight ) )
	{
		DestroyImage( texture->image, texture->view, &texture->memory, texture->lastUsedFrame );
		texture->image = VK_NULL_HANDLE;
		texture->view = VK_NULL_HANDLE;
		if ( !CreateStorage( texture, createWidth, createHeight ) )
		{
			return;
		}
	}
	if ( !texture->image || srcX < 0 || srcY < 0 || srcX + width > vk.width || srcY + height > vk.height
		|| dstX < 0 || dstY < 0 || dstX + width > texture->width || dstY + height > texture->height )
	{
		return;
	}

	VK_EnsureFrame();
	VK_EndRenderPass();
	VkCommandBuffer cmd = vk.cmd;
	VK_ImageBarrier( cmd, vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT );
	ImageBarrier( cmd, texture->image, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );

	VkImageBlit blit = {};
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.layerCount = 1;
	blit.srcOffsets[0] = { srcX, vk.height - srcY, 0 };
	blit.srcOffsets[1] = { srcX + width, vk.height - ( srcY + height ), 1 };
	blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.dstSubresource.layerCount = 1;
	blit.dstOffsets[0] = { dstX, dstY, 0 };
	blit.dstOffsets[1] = { dstX + width, dstY + height, 1 };
	vkCmdBlitImage( cmd, vk.colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blit, VK_FILTER_NEAREST );

	ImageBarrier( cmd, texture->image, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT );
	VK_ImageBarrier( cmd, vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT );
}

void VK_TexParameter( GLuint id, GLenum pname, GLfloat value )
{
	if ( id == 0 )
	{
		return;
	}
	vkTexture_t *texture = VK_TextureForId( id, true );
	switch ( pname )
	{
	case GL_TEXTURE_MIN_FILTER:				texture->minFilter = (GLenum)value; break;
	case GL_TEXTURE_MAG_FILTER:				texture->magFilter = (GLenum)value; break;
	case GL_TEXTURE_WRAP_S:					texture->wrapS = (GLenum)value; break;
	case GL_TEXTURE_WRAP_T:					texture->wrapT = (GLenum)value; break;
	case GL_TEXTURE_MAX_ANISOTROPY_EXT:		texture->anisotropy = value; break;
	}
}

void VK_DeleteTexture( GLuint id )
{
	vkTexture_t *texture = VK_TextureForId( id, false );
	if ( !texture )
	{
		return;
	}
	DestroyImage( texture->image, texture->view, &texture->memory, texture->lastUsedFrame );
	delete texture;
	textures[id] = NULL;
}

static VkSamplerAddressMode WrapMode( GLenum wrap )
{
	// (GL_CLAMP would blend with the border colour; the renderer only asks for it when it has nothing better)
	return wrap == GL_REPEAT ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

VkSampler VK_SamplerFor( const vkTexture_t *texture )
{
	float anisotropy = vk.features.samplerAnisotropy ? texture->anisotropy : 1.0f;
	anisotropy = Q_min( anisotropy, vk.properties.limits.maxSamplerAnisotropy );
	if ( anisotropy < 1.0f )
	{
		anisotropy = 1.0f;
	}
	for ( size_t i = 0; i < samplers.size(); i++ )
	{
		const samplerEntry_t &s = samplers[i];
		if ( s.minFilter == texture->minFilter && s.magFilter == texture->magFilter && s.wrapS == texture->wrapS
			&& s.wrapT == texture->wrapT && s.anisotropy == anisotropy )
		{
			return s.sampler;
		}
	}

	VkSamplerCreateInfo info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	info.magFilter = texture->magFilter == GL_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
	bool mipmapped = false;
	switch ( texture->minFilter )
	{
	case GL_NEAREST:				info.minFilter = VK_FILTER_NEAREST; break;
	case GL_LINEAR:					info.minFilter = VK_FILTER_LINEAR; break;
	case GL_NEAREST_MIPMAP_NEAREST:	info.minFilter = VK_FILTER_NEAREST; info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; mipmapped = true; break;
	case GL_LINEAR_MIPMAP_NEAREST:	info.minFilter = VK_FILTER_LINEAR; info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; mipmapped = true; break;
	case GL_NEAREST_MIPMAP_LINEAR:	info.minFilter = VK_FILTER_NEAREST; info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; mipmapped = true; break;
	default:						info.minFilter = VK_FILTER_LINEAR; info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; mipmapped = true; break;
	}
	info.addressModeU = WrapMode( texture->wrapS );
	info.addressModeV = WrapMode( texture->wrapT );
	info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	info.anisotropyEnable = anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
	info.maxAnisotropy = anisotropy;
	info.minLod = 0.0f;
	info.maxLod = mipmapped ? VK_LOD_CLAMP_NONE : 0.25f;	// (without a mip filter only the first level is used)
	info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;

	samplerEntry_t entry = { texture->minFilter, texture->magFilter, texture->wrapS, texture->wrapT, anisotropy, VK_NULL_HANDLE };
	VK_CheckResult( vkCreateSampler( vk.device, &info, NULL, &entry.sampler ), "vkCreateSampler" );
	samplers.push_back( entry );
	return entry.sampler;
}

void VK_InitImages( void )
{
	textures.clear();
	deferred.clear();
	samplers.clear();
	uploadIndex = 0;
	uploadRecording = false;
	CreateStaging( STAGING_SIZE );

	VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = vk.queueFamily;
	VK_CheckResult( vkCreateCommandPool( vk.device, &poolInfo, NULL, &uploadPool ), "vkCreateCommandPool" );
	VkCommandBufferAllocateInfo allocate = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	allocate.commandPool = uploadPool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = UPLOAD_BUFFERS;
	VK_CheckResult( vkAllocateCommandBuffers( vk.device, &allocate, uploadCommandBuffers ), "vkAllocateCommandBuffers" );
	for ( int i = 0; i < UPLOAD_BUFFERS; i++ )
	{
		VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		VK_CheckResult( vkCreateFence( vk.device, &fenceInfo, NULL, &uploadFences[i] ), "vkCreateFence" );
		uploadFenceSubmitted[i] = false;
	}

	// the texture that stands in for "nothing bound": opaque white
	memset( &whiteTexture, 0, sizeof( whiteTexture ) );
	whiteTexture.minFilter = whiteTexture.magFilter = GL_NEAREST;
	whiteTexture.wrapS = whiteTexture.wrapT = GL_REPEAT;
	whiteTexture.anisotropy = 1.0f;
	if ( CreateStorage( &whiteTexture, 1, 1 ) )
	{
		const byte white[4] = { 255, 255, 255, 255 };
		UploadRegion( &whiteTexture, 0, 0, 0, 1, 1, white, false );
	}
}

void VK_ShutdownImages( void )
{
	VK_FlushUploads( true );
	vkDeviceWaitIdle( vk.device );
	for ( size_t i = 0; i < textures.size(); i++ )
	{
		if ( textures[i] )
		{
			vkDestroyImageView( vk.device, textures[i]->view, NULL );
			vkDestroyImage( vk.device, textures[i]->image, NULL );
			delete textures[i];
		}
	}
	textures.clear();
	vkDestroyImageView( vk.device, whiteTexture.view, NULL );
	vkDestroyImage( vk.device, whiteTexture.image, NULL );
	for ( size_t i = 0; i < deferred.size(); i++ )
	{
		vkDestroyImageView( vk.device, deferred[i].view, NULL );
		vkDestroyImage( vk.device, deferred[i].image, NULL );
	}
	deferred.clear();
	for ( size_t i = 0; i < samplers.size(); i++ )
	{
		vkDestroySampler( vk.device, samplers[i].sampler, NULL );
	}
	samplers.clear();
	for ( int i = 0; i < UPLOAD_BUFFERS; i++ )
	{
		vkDestroyFence( vk.device, uploadFences[i], NULL );
	}
	vkDestroyCommandPool( vk.device, uploadPool, NULL );
	vkDestroyBuffer( vk.device, stagingBuffer, NULL );
	vkUnmapMemory( vk.device, stagingMemory );
	vkFreeMemory( vk.device, stagingMemory, NULL );
	VK_ShutdownMemory();
}
