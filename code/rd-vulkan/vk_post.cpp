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

// Screen effects of the Vulkan renderer module (docs/post-processing.md): ambient occlusion, bloom and SMAA run on the
// finished 3D view, before the HUD is drawn, and the dynamic light shader program is announced here. The effects are
// those of tr_post.cpp of the OpenGL renderer, as fullscreen passes: the view is taken from the resolved colour and
// depth images, every pass draws into an image of its own, and the result is drawn back over the view by the
// renderer's ordinary drawing (so it ends up in the multisampled target like everything else).

#include "../server/exe_headers.h"
#include "../rd-vanilla/tr_local.h"
#include "../rd-vanilla/tr_smaa_data.h"
#include "vk_priv.h"
#include "vk_spirv.h"
#include <zlib.h>
#include <string>

#define POST_BLOOM_LEVELS	5
#define POST_FRAME_SETS		32
#define POST_AREA_ID		0x7ffe0001u
#define POST_SEARCH_ID		0x7ffe0002u
#define POST_RESULT_ID		0x7ffe0003u

enum {
	PASS_SSAO, PASS_SSAO_BLUR, PASS_BLOOM_DOWN, PASS_BLOOM_UP, PASS_COMPOSITE,
	PASS_SMAA_EDGES, PASS_SMAA_WEIGHTS, PASS_SMAA_BLEND, PASS_COUNT
};

typedef struct {
	const unsigned int	*code;
	size_t				size;
	bool				additive;	// blended on top of what the target holds
} passInfo_t;

static const passInfo_t passInfos[PASS_COUNT] = {
	{ vk_spirv_post_ssao_frag, sizeof( vk_spirv_post_ssao_frag ), false },
	{ vk_spirv_post_ssao_blur_frag, sizeof( vk_spirv_post_ssao_blur_frag ), false },
	{ vk_spirv_post_bloom_down_frag, sizeof( vk_spirv_post_bloom_down_frag ), false },
	{ vk_spirv_post_bloom_up_frag, sizeof( vk_spirv_post_bloom_up_frag ), true },
	{ vk_spirv_post_composite_frag, sizeof( vk_spirv_post_composite_frag ), false },
	{ vk_spirv_smaa_edges_frag, sizeof( vk_spirv_smaa_edges_frag ), false },
	{ vk_spirv_smaa_weights_frag, sizeof( vk_spirv_smaa_weights_frag ), false },
	{ vk_spirv_smaa_blend_frag, sizeof( vk_spirv_smaa_blend_frag ), false },
};

typedef struct {
	VkImage			image;
	VkImageView		view;
	vkAlloc_t		memory;
	VkFramebuffer	framebuffer;
	int				width, height;
} postImage_t;

// an image a pass reads
typedef struct {
	VkImageView		view;
	bool			nearest;	// point sampling (depth and the SMAA lookup)
	bool			depth;
} postInput_t;

static struct {
	bool				ready;
	bool				failed;						// something did not build, stop trying
	bool				smaaOk, smaaFailed;
	VkRenderPass		passClear, passLoad;
	VkDescriptorSetLayout	setLayout;
	VkPipelineLayout	pipelineLayout;
	VkShaderModule		vertexShader;
	VkShaderModule		fragmentShaders[PASS_COUNT];
	VkPipeline			pipelines[PASS_COUNT];
	VkSampler			linear, nearest;
	VkDescriptorPool	pools[VK_FRAMES_IN_FLIGHT];
	VkImageView			depthView;					// the depth of the view alone, for sampling

	int					width, height;				// size of the view the images are made for
	postImage_t			scene, aoA, aoB, final, edges, weights, result;
	postImage_t			bloom[POST_BLOOM_LEVELS];
	bool				smaaTextures;
} post;

/*
=============================================================================

GLSL AS THE RENDERER ASKS FOR IT: the only program is the dynamic light shader, drawn by the layer (vk_gl.cpp)

=============================================================================
*/

static qboolean glslAvailable;

qboolean R_GLSL_Available( void )
{
	return glslAvailable;
}

void R_GLSL_Init( void )
{
	glslAvailable = r_glsl->integer ? qtrue : qfalse;
	ri.Cvar_Get( "r_glslAvail", "0", 0 );
	ri.Cvar_Set( "r_glslAvail", glslAvailable ? "1" : "0" );	// what the menus look at to show the rows for what needs it
	if ( glslAvailable )
	{
		Com_Printf( "...using shaders (Vulkan)\n" );
	}
}

GLuint R_GLSL_BuildProgram( const char *name, const char *vertexSource, const char *fragmentSource )
{
	// (there is a Vulkan shader for the program of the dynamic lights, built when the renderer started)
	return !strcmp( name, "dlight" ) ? 1 : 0;
}

void R_GLSL_DeleteProgram( GLuint program )
{
}

/*
=============================================================================

IMAGES

=============================================================================
*/

static bool NewImage( postImage_t *image, int width, int height )
{
	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = VK_FORMAT_R8G8B8A8_UNORM;
	info.extent.width = (uint32_t)width;
	info.extent.height = (uint32_t)height;
	info.extent.depth = 1;
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if ( vkCreateImage( vk.device, &info, NULL, &image->image ) != VK_SUCCESS )
	{
		return false;
	}
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements( vk.device, image->image, &requirements );
	if ( !VK_AllocateMemory( &requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &image->memory ) )
	{
		vkDestroyImage( vk.device, image->image, NULL );
		image->image = VK_NULL_HANDLE;
		return false;
	}
	VK_CheckResult( vkBindImageMemory( vk.device, image->image, image->memory.memory, image->memory.offset ), "vkBindImageMemory" );

	VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = image->image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = info.format;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;
	VK_CheckResult( vkCreateImageView( vk.device, &viewInfo, NULL, &image->view ), "vkCreateImageView" );

	VkFramebufferCreateInfo framebufferInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	framebufferInfo.renderPass = post.passClear;
	framebufferInfo.attachmentCount = 1;
	framebufferInfo.pAttachments = &image->view;
	framebufferInfo.width = (uint32_t)width;
	framebufferInfo.height = (uint32_t)height;
	framebufferInfo.layers = 1;
	VK_CheckResult( vkCreateFramebuffer( vk.device, &framebufferInfo, NULL, &image->framebuffer ), "vkCreateFramebuffer" );
	image->width = width;
	image->height = height;
	return true;
}

static void FreeImage( postImage_t *image )
{
	if ( !image->image )
	{
		return;
	}
	vkDestroyFramebuffer( vk.device, image->framebuffer, NULL );
	vkDestroyImageView( vk.device, image->view, NULL );
	vkDestroyImage( vk.device, image->image, NULL );
	VK_FreeMemory( &image->memory );
	memset( image, 0, sizeof( *image ) );
}

static void FreeImages( void )
{
	FreeImage( &post.scene );
	FreeImage( &post.aoA );
	FreeImage( &post.aoB );
	FreeImage( &post.final );
	FreeImage( &post.edges );
	FreeImage( &post.weights );
	FreeImage( &post.result );
	for ( int i = 0; i < POST_BLOOM_LEVELS; i++ )
	{
		FreeImage( &post.bloom[i] );
	}
	post.width = post.height = 0;
}

// the images for a view of this size, if they are not there yet
static bool EnsureImages( int width, int height )
{
	if ( post.width == width && post.height == height && post.scene.image )
	{
		return true;
	}
	VK_WaitIdle();
	FreeImages();
	bool ok = NewImage( &post.scene, width, height ) && NewImage( &post.aoA, width, height ) && NewImage( &post.aoB, width, height )
		&& NewImage( &post.final, width, height );
	for ( int i = 0; ok && i < POST_BLOOM_LEVELS; i++ )
	{
		ok = NewImage( &post.bloom[i], Q_max( width >> ( i + 1 ), 2 ), Q_max( height >> ( i + 1 ), 2 ) );
	}
	if ( ok && post.smaaOk )
	{
		ok = NewImage( &post.edges, width, height ) && NewImage( &post.weights, width, height ) && NewImage( &post.result, width, height );
	}
	if ( !ok )
	{
		FreeImages();
		return false;
	}
	post.width = width;
	post.height = height;
	return true;
}

/*
=============================================================================

PASSES

=============================================================================
*/

static VkRenderPass NewRenderPass( bool load )
{
	VkAttachmentDescription attachment = {};
	attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = load ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkAttachmentReference reference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &reference;

	VkSubpassDependency dependencies[2] = {};
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

	VkRenderPassCreateInfo info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	info.attachmentCount = 1;
	info.pAttachments = &attachment;
	info.subpassCount = 1;
	info.pSubpasses = &subpass;
	info.dependencyCount = 2;
	info.pDependencies = dependencies;
	VkRenderPass pass;
	VK_CheckResult( vkCreateRenderPass( vk.device, &info, NULL, &pass ), "vkCreateRenderPass" );
	return pass;
}

static VkPipeline GetPipeline( int pass )
{
	if ( post.pipelines[pass] )
	{
		return post.pipelines[pass];
	}
	if ( !post.fragmentShaders[pass] )
	{
		VkShaderModuleCreateInfo shaderInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		shaderInfo.codeSize = passInfos[pass].size;
		shaderInfo.pCode = passInfos[pass].code;
		VK_CheckResult( vkCreateShaderModule( vk.device, &shaderInfo, NULL, &post.fragmentShaders[pass] ), "vkCreateShaderModule" );
	}

	VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = post.vertexShader;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = post.fragmentShaders[pass];
	stages[1].pName = "main";

	VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	VkPipelineColorBlendAttachmentState attachment = {};
	attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	if ( passInfos[pass].additive )
	{
		attachment.blendEnable = VK_TRUE;
		attachment.srcColorBlendFactor = attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		attachment.dstColorBlendFactor = attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		attachment.colorBlendOp = attachment.alphaBlendOp = VK_BLEND_OP_ADD;
	}
	VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	blend.attachmentCount = 1;
	blend.pAttachments = &attachment;
	const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynamicStates;

	VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertexInput;
	info.pInputAssemblyState = &inputAssembly;
	info.pViewportState = &viewportState;
	info.pRasterizationState = &raster;
	info.pMultisampleState = &multisample;
	info.pDepthStencilState = &depthStencil;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamic;
	info.layout = post.pipelineLayout;
	info.renderPass = post.passClear;
	VK_CheckResult( vkCreateGraphicsPipelines( vk.device, VK_NULL_HANDLE, 1, &info, NULL, &post.pipelines[pass] ), "vkCreateGraphicsPipelines" );
	return post.pipelines[pass];
}

// draws the fullscreen triangle of a pass into the target; load: add to what the target holds
static void RunPass( int pass, const postImage_t *target, bool load, const postInput_t inputs[3], const float push[16] )
{
	VkCommandBuffer cmd = vk.cmd;
	const VkClearValue clear = {};
	VkRenderPassBeginInfo begin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	begin.renderPass = load ? post.passLoad : post.passClear;
	begin.framebuffer = target->framebuffer;
	begin.renderArea.extent.width = (uint32_t)target->width;
	begin.renderArea.extent.height = (uint32_t)target->height;
	begin.clearValueCount = 1;
	begin.pClearValues = &clear;
	vkCmdBeginRenderPass( cmd, &begin, VK_SUBPASS_CONTENTS_INLINE );

	vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, GetPipeline( pass ) );

	VkDescriptorSetAllocateInfo allocate = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	allocate.descriptorPool = post.pools[vk.frameIndex];
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts = &post.setLayout;
	VkDescriptorSet set;
	VK_CheckResult( vkAllocateDescriptorSets( vk.device, &allocate, &set ), "vkAllocateDescriptorSets" );
	VkDescriptorImageInfo images[3];
	VkWriteDescriptorSet writes[3];
	for ( int i = 0; i < 3; i++ )
	{
		images[i].sampler = inputs[i].nearest ? post.nearest : post.linear;
		images[i].imageView = inputs[i].view ? inputs[i].view : VK_WhiteTexture()->view;
		images[i].imageLayout = inputs[i].depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		writes[i].dstSet = set;
		writes[i].dstBinding = (uint32_t)i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &images[i];
	}
	vkUpdateDescriptorSets( vk.device, 3, writes, 0, NULL );
	vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, post.pipelineLayout, 0, 1, &set, 0, NULL );
	vkCmdPushConstants( cmd, post.pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 * sizeof( float ), push );

	const VkViewport viewport = { 0.0f, 0.0f, (float)target->width, (float)target->height, 0.0f, 1.0f };
	const VkRect2D scissor = { { 0, 0 }, { (uint32_t)target->width, (uint32_t)target->height } };
	vkCmdSetViewport( cmd, 0, 1, &viewport );
	vkCmdSetScissor( cmd, 0, 1, &scissor );
	vkCmdDraw( cmd, 3, 1, 0, 0 );
	vkCmdEndRenderPass( cmd );
}

static postInput_t Input( const postImage_t *image )
{
	postInput_t input = { image->view, false, false };
	return input;
}

/*
=============================================================================

SMAA'S LOOKUP TEXTURES (from tr_smaa_data.h, as the OpenGL renderer makes them)

=============================================================================
*/

static int Base64Value( char c )
{
	if ( c >= 'A' && c <= 'Z' ) return c - 'A';
	if ( c >= 'a' && c <= 'z' ) return c - 'a' + 26;
	if ( c >= '0' && c <= '9' ) return c - '0' + 52;
	return c == '+' ? 62 : 63;
}

// the pixels of one of the compressed pieces of data (channels bytes each), or an empty string
static std::string UnpackData( const char *const *pieces, size_t numPieces, int width, int height, int channels )
{
	std::string text, packed, pixels;
	uLongf pixelsLength = (uLongf)( width * height * channels );
	unsigned int bits = 0;
	int numBits = 0;

	for ( size_t i = 0; i < numPieces; i++ )
	{
		text += pieces[i];
	}
	for ( size_t i = 0; i < text.size() && text[i] != '='; i++ )
	{
		bits = ( bits << 6 ) | Base64Value( text[i] );
		numBits += 6;
		if ( numBits >= 8 )
		{
			numBits -= 8;
			packed += (char)( ( bits >> numBits ) & 0xFF );
		}
	}
	pixels.resize( pixelsLength );
	if ( uncompress( (Bytef *)&pixels[0], &pixelsLength, (const Bytef *)packed.data(), (uLong)packed.size() ) != Z_OK
		|| pixelsLength != (uLongf)( width * height * channels ) )
	{
		return std::string();
	}
	return pixels;
}

// as a texture of the renderer (always RGBA8: the channels that are not there are 0)
static bool MakeDataTexture( GLuint id, const char *const *pieces, size_t numPieces, int width, int height, int channels, GLenum filter )
{
	const std::string pixels = UnpackData( pieces, numPieces, width, height, channels );
	if ( pixels.empty() )
	{
		return false;
	}
	std::string rgba( (size_t)width * height * 4, '\0' );
	for ( int i = 0; i < width * height; i++ )
	{
		for ( int c = 0; c < channels; c++ )
		{
			rgba[i * 4 + c] = pixels[i * channels + c];
		}
		rgba[i * 4 + 3] = (char)255;
	}
	VK_TexImage2D( id, 0, 4, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data() );
	VK_TexParameter( id, GL_TEXTURE_MIN_FILTER, (GLfloat)filter );
	VK_TexParameter( id, GL_TEXTURE_MAG_FILTER, (GLfloat)filter );
	VK_TexParameter( id, GL_TEXTURE_WRAP_S, (GLfloat)GL_CLAMP_TO_EDGE );
	VK_TexParameter( id, GL_TEXTURE_WRAP_T, (GLfloat)GL_CLAMP_TO_EDGE );
	return true;
}

static bool SmaaReady( void )
{
	if ( post.smaaFailed )
	{
		return false;
	}
	if ( !post.smaaTextures )
	{
		if ( !MakeDataTexture( POST_AREA_ID, smaaAreaTexZ, ARRAY_LEN( smaaAreaTexZ ), SMAA_AREATEX_WIDTH, SMAA_AREATEX_HEIGHT, 2, GL_LINEAR )
			|| !MakeDataTexture( POST_SEARCH_ID, smaaSearchTexZ, ARRAY_LEN( smaaSearchTexZ ), SMAA_SEARCHTEX_WIDTH, SMAA_SEARCHTEX_HEIGHT, 1, GL_NEAREST ) )
		{
			Com_Printf( S_COLOR_YELLOW "SMAA: the lookup textures do not unpack\n" );
			post.smaaFailed = true;
			return false;
		}
		post.smaaTextures = true;
	}
	return true;
}

/*
=============================================================================

THE EFFECTS

=============================================================================
*/

// a barrier for an image that goes from one use to another outside of a render pass
static void Barrier( VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
	VkPipelineStageFlags srcStage, VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess )
{
	VK_ImageBarrier( vk.cmd, image, aspect, from, to, srcStage, srcAccess, dstStage, dstAccess );
}

static bool PostWanted( void )
{
	if ( !post.ready || post.failed )
	{
		return false;
	}
	if ( backEnd.refdef.rdflags & ( RDF_NOWORLDMODEL | RDF_SKYBOXPORTAL ) )
	{
		return false;	// the views that are not the one the player looks through
	}
	if ( backEnd.viewParms.isPortal || backEnd.viewParms.viewportWidth < 64 || backEnd.viewParms.viewportHeight < 64 )
	{
		return false;
	}
	return r_ssao->integer || r_bloom->integer || r_smaa->integer || r_postDebug->integer;
}

// ambient occlusion, blurred, into post.aoA
static void Ssao( int x, int y, int width, int height, const postInput_t &depth )
{
	const float *proj = backEnd.viewParms.projectionMatrix;
	const float dx = (float)width / vk.width, dy = (float)height / vk.height;
	const float bx = (float)x / vk.width, by = (float)( vk.height - ( y + height ) ) / vk.height;
	float push[16] = {
		proj[0], proj[5], proj[8], proj[9],
		proj[10], proj[14], (float)width, (float)height,
		r_ssaoRadius->value, r_ssaoStrength->value, 0.1f, 0.0f,
		dx, dy, bx, by };
	postInput_t inputs[3] = { depth, {}, {} };
	RunPass( PASS_SSAO, &post.aoA, false, inputs, push );

	inputs[1] = Input( &post.aoA );
	push[8] = 1.0f; push[9] = 0.0f; push[10] = 0.0f;
	RunPass( PASS_SSAO_BLUR, &post.aoB, false, inputs, push );
	inputs[1] = Input( &post.aoB );
	push[8] = 0.0f; push[9] = 1.0f;
	RunPass( PASS_SSAO_BLUR, &post.aoA, false, inputs, push );
}

// the bright parts shrunk and grown again, into post.bloom[0]
static void Bloom( void )
{
	for ( int i = 0; i < POST_BLOOM_LEVELS; i++ )
	{
		const postImage_t *source = i ? &post.bloom[i - 1] : &post.scene;
		postInput_t inputs[3] = { Input( source ), {}, {} };
		const float push[16] = { 1.0f / source->width, 1.0f / source->height, r_bloomThreshold->value, 0.5f * r_bloomThreshold->value,
			i ? 0.0f : 1.0f };
		RunPass( PASS_BLOOM_DOWN, &post.bloom[i], false, inputs, push );
	}
	for ( int i = POST_BLOOM_LEVELS - 1; i > 0; i-- )
	{
		postInput_t inputs[3] = { Input( &post.bloom[i] ), {}, {} };
		const float push[16] = { 1.0f / post.bloom[i].width, 1.0f / post.bloom[i].height, 0.8f };
		RunPass( PASS_BLOOM_UP, &post.bloom[i - 1], true, inputs, push );
	}
}

// the three passes of SMAA, from post.final to post.result
static void Smaa( void )
{
	const float metrics[16] = { 1.0f / post.width, 1.0f / post.height, (float)post.width, (float)post.height };
	postInput_t inputs[3] = { Input( &post.final ), {}, {} };
	RunPass( PASS_SMAA_EDGES, &post.edges, false, inputs, metrics );

	postInput_t weights[3] = { Input( &post.edges ), Input( &post.edges ), Input( &post.edges ) };
	weights[1].view = VK_TextureForId( POST_AREA_ID, false )->view;
	weights[2].view = VK_TextureForId( POST_SEARCH_ID, false )->view;
	weights[2].nearest = true;
	RunPass( PASS_SMAA_WEIGHTS, &post.weights, false, weights, metrics );

	postInput_t blend[3] = { Input( &post.final ), Input( &post.weights ), {} };
	RunPass( PASS_SMAA_BLEND, &post.result, false, blend, metrics );
}

// draws the finished view over the view, with the renderer's own drawing
static void DrawBack( int x, int y, int width, int height, GLuint texture )
{
	GL_State( GLS_DEPTHTEST_DISABLE );
	GL_Cull( CT_TWO_SIDED );
	qglDisable( GL_CLIP_PLANE0 );
	qglDisable( GL_SCISSOR_TEST );
	const GLboolean fogged = qglIsEnabled( GL_FOG );
	qglDisable( GL_FOG );

	qglMatrixMode( GL_PROJECTION );
	qglPushMatrix();
	qglLoadIdentity();
	qglOrtho( 0, width, height, 0, -1, 1 );
	qglMatrixMode( GL_MODELVIEW );
	qglPushMatrix();
	qglLoadIdentity();
	qglViewport( x, y, width, height );

	const int oldEnv = glState.texEnv[0];
	GL_SelectTexture( 0 );
	GL_TexEnv( GL_REPLACE );
	qglEnable( GL_TEXTURE_2D );
	qglBindTexture( GL_TEXTURE_2D, texture );
	qglColor4f( 1.0f, 1.0f, 1.0f, 1.0f );
	qglBegin( GL_QUADS );
	qglTexCoord2f( 0.0f, 0.0f );
	qglVertex2f( 0.0f, 0.0f );
	qglTexCoord2f( 1.0f, 0.0f );
	qglVertex2f( (float)width, 0.0f );
	qglTexCoord2f( 1.0f, 1.0f );
	qglVertex2f( (float)width, (float)height );
	qglTexCoord2f( 0.0f, 1.0f );
	qglVertex2f( 0.0f, (float)height );
	qglEnd();

	// the renderer caches what is bound to its texture units, make it look again
	qglBindTexture( GL_TEXTURE_2D, 0 );
	glState.currenttextures[0] = glState.currenttextures[1] = 0;
	GL_TexEnv( oldEnv );
	if ( fogged )
	{
		qglEnable( GL_FOG );
	}
	qglMatrixMode( GL_PROJECTION );
	qglPopMatrix();
	qglMatrixMode( GL_MODELVIEW );
	qglPopMatrix();
}

/*
================
RB_PostProcess

Called when the 3D view is finished.
================
*/
void RB_PostProcess( void )
{
	if ( !PostWanted() )
	{
		return;
	}
	const int x = backEnd.viewParms.viewportX;
	const int y = backEnd.viewParms.viewportY;
	const int width = backEnd.viewParms.viewportWidth;
	const int height = backEnd.viewParms.viewportHeight;
	if ( x < 0 || y < 0 || x + width > vk.width || y + height > vk.height )
	{
		return;
	}
	const bool ssao = r_ssao->integer != 0;
	const bool bloom = r_bloom->integer != 0;
	const bool smaa = r_smaa->integer && post.smaaOk && SmaaReady();
	if ( !EnsureImages( width, height ) )
	{
		post.failed = true;
		return;
	}

	VK_EnsureFrame();
	VK_EndRenderPass();	// (resolves the view into the single sample colour and depth)
	VK_FlushUploads( false );

	const VkImageAspectFlags depthAspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

	// the colour of the view into an image of its own
	Barrier( vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT );
	Barrier( post.scene.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );
	VkImageBlit blit = {};
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.layerCount = 1;
	blit.srcOffsets[0] = { x, vk.height - ( y + height ), 0 };
	blit.srcOffsets[1] = { x + width, vk.height - y, 1 };
	blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.dstSubresource.layerCount = 1;
	blit.dstOffsets[1] = { width, height, 1 };
	vkCmdBlitImage( vk.cmd, vk.colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, post.scene.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blit, VK_FILTER_NEAREST );
	Barrier( post.scene.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT );

	// the depth is read where it is
	postInput_t depth = { post.depthView, true, true };
	if ( ssao )
	{
		Barrier( vk.depthImage, depthAspects, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT );
		Ssao( x, y, width, height, depth );
		Barrier( vk.depthImage, depthAspects, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
			VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT );
	}
	if ( bloom )
	{
		Bloom();
	}

	// everything together
	{
		postInput_t inputs[3] = { Input( &post.scene ), Input( &post.aoA ), Input( &post.bloom[0] ) };
		if ( !ssao ) inputs[1].view = VK_NULL_HANDLE;
		if ( !bloom ) inputs[2].view = VK_NULL_HANDLE;
		const float push[16] = { ssao ? 1.0f : 0.0f, r_postDebug->integer == 2 ? 1.0f : 0.0f, bloom ? r_bloomIntensity->value : 0.0f };
		RunPass( PASS_COMPOSITE, &post.final, false, inputs, push );
	}
	if ( smaa )
	{
		Smaa();
	}

	// the colour image is a render target again
	Barrier( vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT );

	const postImage_t *finished = smaa ? &post.result : &post.final;
	VK_SetExternalTexture( POST_RESULT_ID, finished->image, finished->view, finished->width, finished->height );

	const GLboolean scissored = qglIsEnabled( GL_SCISSOR_TEST );
	DrawBack( x, y, width, height, POST_RESULT_ID );
	if ( scissored )
	{
		qglEnable( GL_SCISSOR_TEST );
	}
	SetViewportAndScissor();
}

/*
=============================================================================

START AND END

=============================================================================
*/

void R_Post_Init( void )
{
	// (what the menus look at to show the rows for what needs it)
	ri.Cvar_Get( "r_postAvail", "0", 0 );
	ri.Cvar_Set( "r_postAvail", post.ready ? "1" : "0" );
	ri.Cvar_Get( "r_smaaAvail", "0", 0 );
	ri.Cvar_Set( "r_smaaAvail", post.ready && post.smaaOk ? "1" : "0" );
	if ( post.ready )
	{
		Com_Printf( "...using post processing\n" );
	}
}

void VK_InitPost( void )
{
	memset( &post, 0, sizeof( post ) );
	post.passClear = NewRenderPass( false );
	post.passLoad = NewRenderPass( true );

	VkDescriptorSetLayoutBinding bindings[3] = {};
	for ( uint32_t i = 0; i < 3; i++ )
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	layoutInfo.bindingCount = 3;
	layoutInfo.pBindings = bindings;
	VK_CheckResult( vkCreateDescriptorSetLayout( vk.device, &layoutInfo, NULL, &post.setLayout ), "vkCreateDescriptorSetLayout" );
	VkPushConstantRange range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 * sizeof( float ) };
	VkPipelineLayoutCreateInfo pipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &post.setLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &range;
	VK_CheckResult( vkCreatePipelineLayout( vk.device, &pipelineLayoutInfo, NULL, &post.pipelineLayout ), "vkCreatePipelineLayout" );

	VkShaderModuleCreateInfo shaderInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	shaderInfo.codeSize = sizeof( vk_spirv_post_vert );
	shaderInfo.pCode = vk_spirv_post_vert;
	VK_CheckResult( vkCreateShaderModule( vk.device, &shaderInfo, NULL, &post.vertexShader ), "vkCreateShaderModule" );

	VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.maxLod = 0.25f;
	VK_CheckResult( vkCreateSampler( vk.device, &samplerInfo, NULL, &post.linear ), "vkCreateSampler" );
	samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
	VK_CheckResult( vkCreateSampler( vk.device, &samplerInfo, NULL, &post.nearest ), "vkCreateSampler" );

	VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, POST_FRAME_SETS * 3 };
	VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	poolInfo.maxSets = POST_FRAME_SETS;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	for ( int i = 0; i < VK_FRAMES_IN_FLIGHT; i++ )
	{
		VK_CheckResult( vkCreateDescriptorPool( vk.device, &poolInfo, NULL, &post.pools[i] ), "vkCreateDescriptorPool" );
	}

	// the depth of the view on its own (the sampled view of a depth and stencil image has to name one aspect)
	VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = vk.depthImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = vk.depthFormat;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;
	VK_CheckResult( vkCreateImageView( vk.device, &viewInfo, NULL, &post.depthView ), "vkCreateImageView" );

	post.ready = true;
	post.smaaOk = true;
}

void VK_BeginFramePost( void )
{
	if ( post.ready )
	{
		vkResetDescriptorPool( vk.device, post.pools[vk.frameIndex], 0 );
	}
}

void VK_ShutdownPost( void )
{
	if ( !post.ready )
	{
		return;
	}
	vkDeviceWaitIdle( vk.device );
	FreeImages();
	for ( int i = 0; i < PASS_COUNT; i++ )
	{
		if ( post.pipelines[i] ) vkDestroyPipeline( vk.device, post.pipelines[i], NULL );
		if ( post.fragmentShaders[i] ) vkDestroyShaderModule( vk.device, post.fragmentShaders[i], NULL );
	}
	for ( int i = 0; i < VK_FRAMES_IN_FLIGHT; i++ )
	{
		vkDestroyDescriptorPool( vk.device, post.pools[i], NULL );
	}
	vkDestroyImageView( vk.device, post.depthView, NULL );
	vkDestroySampler( vk.device, post.linear, NULL );
	vkDestroySampler( vk.device, post.nearest, NULL );
	vkDestroyShaderModule( vk.device, post.vertexShader, NULL );
	vkDestroyPipelineLayout( vk.device, post.pipelineLayout, NULL );
	vkDestroyDescriptorSetLayout( vk.device, post.setLayout, NULL );
	vkDestroyRenderPass( vk.device, post.passClear, NULL );
	vkDestroyRenderPass( vk.device, post.passLoad, NULL );
	memset( &post, 0, sizeof( post ) );
}
