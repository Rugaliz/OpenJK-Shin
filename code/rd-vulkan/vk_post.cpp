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
#include "renderer/tr_smaa_data.h"
#include "vk_priv.h"
#include "vk_spirv.h"
#include "renderer/tr_light_shafts.h"
#include "renderer/tr_eye_adaptation.h"
#include <zlib.h>
#include <string>

#define POST_BLOOM_LEVELS	5
#define POST_FRAME_SETS		96	// descriptor sets a frame: every effect at once uses about 40 (16 of them the shadow maps)
#define POST_AREA_ID		0x7ffe0001u
#define POST_SEARCH_ID		0x7ffe0002u
#define POST_RESULT_ID		0x7ffe0003u
#define POST_GAMMA_ID		0x7ffe0004u	// (the picture through the brightness table, RB_GammaPass)
#define POST_SHADOW_ID		0x7ffe0005u	// (the darkening of the shadow maps, RB_ShadowMapsApply)
#define POST_ATLAS_ID		0x7ffe0006u	// (the atlas of the shadow maps)
#define POST_SOFT_ID		0x7ffe0007u	// (the depth of the opaque surfaces, RB_SoftParticlesBegin)

enum {
	PASS_SSAO, PASS_SSAO_BLUR, PASS_BLOOM_DOWN, PASS_BLOOM_UP, PASS_COMPOSITE,
	PASS_SMAA_EDGES, PASS_SMAA_WEIGHTS, PASS_SMAA_BLEND, PASS_GAMMA, PASS_SHARPEN, PASS_SHADOW, PASS_SHADOW_MUL,
	PASS_SHAFTS, PASS_SHAFTS_ADD, PASS_EYE, PASS_RT_DEBUG,
	PASS_RT_SHADOW, PASS_COUNT
};

typedef struct {
	const unsigned int	*code;
	size_t				size;
	bool				additive;	// blended on top of what the target holds
	bool				multiply;	// multiplied into what the target holds
	bool				rayQuery;	// reads the ray tracing structure of the frame (binding 3)
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
	{ vk_spirv_post_gamma_frag, sizeof( vk_spirv_post_gamma_frag ), false },
	{ vk_spirv_post_sharpen_frag, sizeof( vk_spirv_post_sharpen_frag ), false },
	{ vk_spirv_post_shadow_frag, sizeof( vk_spirv_post_shadow_frag ), false },
	{ vk_spirv_post_shadow_frag, sizeof( vk_spirv_post_shadow_frag ), false, true },
	{ vk_spirv_post_shafts_frag, sizeof( vk_spirv_post_shafts_frag ), false },
	{ vk_spirv_post_shafts_frag, sizeof( vk_spirv_post_shafts_frag ), true },
	{ vk_spirv_post_eye_frag, sizeof( vk_spirv_post_eye_frag ), false },
	{ vk_spirv_post_rt_debug_frag, sizeof( vk_spirv_post_rt_debug_frag ), false, false, true },
	{ vk_spirv_post_rt_shadow_frag, sizeof( vk_spirv_post_rt_shadow_frag ), false, false, true },
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
	VkDescriptorSetLayout	rtSetLayout;			// the passes that use ray queries: the three images and the structure
	VkPipelineLayout	rtPipelineLayout;
	VkDescriptorPool	rtPools[VK_FRAMES_IN_FLIGHT];

	int					width, height;				// size of the view the images are made for
	postImage_t			scene, aoA, aoB, final, edges, weights, result;
	postImage_t			sharp;						// the view sharpened (r_sharpen)
	postImage_t			shadow;						// the darkening of the shadow maps
	VkImage				softImage;					// the depth of the opaque surfaces of the view (RB_SoftParticlesBegin)
	vkAlloc_t			softMemory;
	VkImageView			softView;
	int					softWidth, softHeight;
	bool				softReady;					// for the view being drawn
	bool				softFailed;
	postImage_t			rtShadow;					// the ray traced shadows before the blur (RtShadowSize)
	vkRtBuffer_t		rtCasters[VK_FRAMES_IN_FLIGHT];	// their casters (RT_CASTER_SECTIONS lots a frame, read by address)
	int					rtCastersFrame[VK_FRAMES_IN_FLIGHT], rtCastersUsed[VK_FRAMES_IN_FLIGHT];
	postImage_t			shaftsA, shaftsB;			// light shafts: the sky near the sun, then blurred towards it
	postImage_t			lum, lumAverage;			// eye adaptation: the brightness of the view shrunk, then one pixel
	VkBuffer			eyeBuffer;					// its measurements on their way back, one for each frame in flight
	VkDeviceMemory		eyeMemory;
	const byte			*eyeMapped;
	bool				eyeWritten[VK_FRAMES_IN_FLIGHT];
	int					eyeFrame;					// (measured once a frame: vk.frameCounter + 1 when it was)
	float				eyeExposure;
	bool				eyeFailed;
	bool				atlasMade, shadowFailed;
	bool				rtShadowFailed;
	postImage_t			bloom[POST_BLOOM_LEVELS];
	bool				smaaTextures;

	postImage_t			gammaIn, gammaOut;			// the whole picture, for RB_GammaPass
	bool				gammaFailed;
} post;

/*
=============================================================================

GLSL AS THE RENDERER ASKS FOR IT: the only program is the dynamic light shader, drawn by the layer (vk_gl.cpp)

=============================================================================
*/

static qboolean glslAvailable;
static int glslGeneration;

qboolean R_GLSL_Available( void )
{
	return glslAvailable;
}

int R_GLSL_Generation( void )
{
	return glslGeneration;
}

void R_GLSL_Init( void )
{
	glslGeneration++;
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
	info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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
	FreeImage( &post.sharp );
	FreeImage( &post.shadow );
	FreeImage( &post.rtShadow );
	FreeImage( &post.shaftsA );
	FreeImage( &post.shaftsB );
	FreeImage( &post.lum );
	FreeImage( &post.lumAverage );
	for ( int i = 0; i < POST_BLOOM_LEVELS; i++ )
	{
		FreeImage( &post.bloom[i] );
	}
	post.width = post.height = 0;
}

// the images for a view of this size, if they are not there yet
// the size of the image of the ray traced shadows for a view this wide (or high): half of it, all of it at High
static int RtShadowSize( int size )
{
	return ( r_rayTracing && r_rayTracing->integer >= 2 ) ? size : ( size + 1 ) / 2;
}

static bool EnsureImages( int width, int height )
{
	if ( post.width == width && post.height == height && post.scene.image && post.rtShadow.width == RtShadowSize( width ) )
	{
		return true;
	}
	// (the frame being recorded may use them already, a view of another size before this one: send it and wait)
	if ( vk.frameStarted )
	{
		VK_SubmitFrame( true );
	}
	else
	{
		VK_WaitIdle();
	}
	FreeImages();
	bool ok = NewImage( &post.scene, width, height ) && NewImage( &post.aoA, width, height ) && NewImage( &post.aoB, width, height )
		&& NewImage( &post.final, width, height ) && NewImage( &post.sharp, width, height ) && NewImage( &post.shadow, width, height )
		&& NewImage( &post.shaftsA, Q_max( width / LIGHT_SHAFTS_SCALE, 2 ), Q_max( height / LIGHT_SHAFTS_SCALE, 2 ) )
		&& NewImage( &post.shaftsB, Q_max( width / LIGHT_SHAFTS_SCALE, 2 ), Q_max( height / LIGHT_SHAFTS_SCALE, 2 ) )
		&& NewImage( &post.lum, EYE_LUM_SIZE, EYE_LUM_SIZE ) && NewImage( &post.lumAverage, 1, 1 )
		&& NewImage( &post.rtShadow, RtShadowSize( width ), RtShadowSize( height ) );
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

// the set and pipeline layouts of the passes with ray queries, made the first time one is run
static bool EnsureRtLayout( void )
{
	if ( post.rtPipelineLayout )
	{
		return true;
	}
	if ( !vk.rayTracing )
	{
		return false;
	}
	// the three images, the structure, and the atlas and the buffer of the alpha test (rt_alpha.glsl)
	VkDescriptorSetLayoutBinding bindings[6] = {};
	for ( uint32_t i = 0; i < 6; i++ )
	{
		bindings[i].binding = i;
		bindings[i].descriptorType = ( i < 3 || i == 4 ) ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
			: ( i == 3 ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER );
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	layoutInfo.bindingCount = 6;
	layoutInfo.pBindings = bindings;
	VK_CheckResult( vkCreateDescriptorSetLayout( vk.device, &layoutInfo, NULL, &post.rtSetLayout ), "vkCreateDescriptorSetLayout" );
	VkPushConstantRange range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 * sizeof( float ) };
	VkPipelineLayoutCreateInfo pipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &post.rtSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &range;
	VK_CheckResult( vkCreatePipelineLayout( vk.device, &pipelineLayoutInfo, NULL, &post.rtPipelineLayout ), "vkCreatePipelineLayout" );

	const VkDescriptorPoolSize poolSizes[3] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 * 4 }, { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 16 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16 } };
	VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	poolInfo.maxSets = 16;
	poolInfo.poolSizeCount = 3;
	poolInfo.pPoolSizes = poolSizes;
	for ( int i = 0; i < VK_FRAMES_IN_FLIGHT; i++ )
	{
		VK_CheckResult( vkCreateDescriptorPool( vk.device, &poolInfo, NULL, &post.rtPools[i] ), "vkCreateDescriptorPool" );
	}
	return true;
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
	if ( passInfos[pass].multiply )
	{
		attachment.blendEnable = VK_TRUE;
		attachment.srcColorBlendFactor = attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		attachment.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
		attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		attachment.colorBlendOp = attachment.alphaBlendOp = VK_BLEND_OP_ADD;
	}
	else if ( passInfos[pass].additive )
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
	info.layout = passInfos[pass].rayQuery ? post.rtPipelineLayout : post.pipelineLayout;
	info.renderPass = post.passClear;
	VK_CheckResult( vkCreateGraphicsPipelines( vk.device, VK_NULL_HANDLE, 1, &info, NULL, &post.pipelines[pass] ), "vkCreateGraphicsPipelines" );
	return post.pipelines[pass];
}

// draws the fullscreen triangle of a pass into the target; load: add to what the target holds
static void RunPass( int pass, const postImage_t *target, bool load, const postInput_t inputs[3], const float *push, int numPush = 16,
	const VkRect2D *area = NULL, VkAccelerationStructureKHR scene = VK_NULL_HANDLE )
{
	const bool rayQuery = passInfos[pass].rayQuery;
	const VkPipelineLayout layout = rayQuery ? post.rtPipelineLayout : post.pipelineLayout;
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
	allocate.descriptorPool = rayQuery ? post.rtPools[vk.frameIndex] : post.pools[vk.frameIndex];
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts = rayQuery ? &post.rtSetLayout : &post.setLayout;
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
	if ( rayQuery )
	{
		VkWriteDescriptorSetAccelerationStructureKHR structure = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
		structure.accelerationStructureCount = 1;
		structure.pAccelerationStructures = &scene;
		VkImageView atlas;
		VkSampler sampler;
		VkBuffer alpha;
		VK_RT_AlphaInputs( &atlas, &sampler, &alpha );
		const VkDescriptorImageInfo image = { sampler, atlas, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		const VkDescriptorBufferInfo buffer = { alpha, 0, VK_WHOLE_SIZE };
		VkWriteDescriptorSet rtWrites[3] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
			{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
		for ( uint32_t i = 0; i < 3; i++ )
		{
			rtWrites[i].dstSet = set;
			rtWrites[i].dstBinding = 3 + i;
			rtWrites[i].descriptorCount = 1;
		}
		rtWrites[0].pNext = &structure;
		rtWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
		rtWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		rtWrites[1].pImageInfo = &image;
		rtWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		rtWrites[2].pBufferInfo = &buffer;
		vkUpdateDescriptorSets( vk.device, 3, rtWrites, 0, NULL );
	}
	vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL );
	vkCmdPushConstants( cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, numPush * sizeof( float ), push );

	const VkViewport viewport = { 0.0f, 0.0f, (float)target->width, (float)target->height, 0.0f, 1.0f };
	const VkRect2D scissor = area ? *area : VkRect2D{ { 0, 0 }, { (uint32_t)target->width, (uint32_t)target->height } };
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

#define RT_CASTER_SECTIONS	4	// the views a frame that can have ray traced shadows

static cvar_t *r_rtDebug;	// 1: the view darkened where rays to the sun are blocked (vk_rt.cpp), 2: that alone

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
	return r_ssao->integer || r_bloom->integer || r_smaa->integer || r_postDebug->integer || r_sharpen->value > 0.0f ||
		r_lightShafts->value > 0.0f || r_eyeAdaptation->value > 0.0f || ( vk.rayTracing && r_rtDebug->integer );
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

// the buffer the measurements of the eye adaptation come back in (host visible, mapped for good), false if there is none
static bool EnsureEyeBuffer( void )
{
	if ( post.eyeBuffer || post.eyeFailed )
	{
		return !post.eyeFailed;
	}
	VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufferInfo.size = 256 * VK_FRAMES_IN_FLIGHT;
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CheckResult( vkCreateBuffer( vk.device, &bufferInfo, NULL, &post.eyeBuffer ), "vkCreateBuffer" );
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements( vk.device, post.eyeBuffer, &requirements );
	const uint32_t type = VK_FindMemoryType( requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	void *mapped = NULL;
	if ( vkAllocateMemory( vk.device, &allocate, NULL, &post.eyeMemory ) != VK_SUCCESS )
	{
		vkDestroyBuffer( vk.device, post.eyeBuffer, NULL );
		post.eyeBuffer = VK_NULL_HANDLE;
		post.eyeFailed = true;
		return false;
	}
	VK_CheckResult( vkBindBufferMemory( vk.device, post.eyeBuffer, post.eyeMemory, 0 ), "vkBindBufferMemory" );
	VK_CheckResult( vkMapMemory( vk.device, post.eyeMemory, 0, requirements.size, 0, &mapped ), "vkMapMemory" );
	post.eyeMapped = (const byte *)mapped;
	return true;
}

// eye adaptation (see tr_eye_adaptation.h): reads back the measurement this frame slot made last time (its fence was
// waited for when the frame started), measures post.scene, and returns the exposure for the view
static float EyeAdaptation( void )
{
	if ( r_eyeAdaptation->value <= 0.0f || !EnsureEyeBuffer() )
	{
		memset( post.eyeWritten, 0, sizeof( post.eyeWritten ) );
		return R_EyeAdaptation_Exposure( -1 );
	}
	if ( post.eyeFrame == vk.frameCounter + 1 )
	{
		return post.eyeExposure;	// (another view in the same frame)
	}
	post.eyeFrame = vk.frameCounter + 1;

	const int slot = vk.frameIndex;
	const int measured = post.eyeWritten[slot] ? post.eyeMapped[slot * 256] : -1;

	postInput_t inputs[3] = { Input( &post.scene ), {}, {} };
	float push[16] = {};
	RunPass( PASS_EYE, &post.lum, false, inputs, push );
	inputs[0] = Input( &post.lum );
	push[8] = 1.0f;
	RunPass( PASS_EYE, &post.lumAverage, false, inputs, push );

	Barrier( post.lumAverage.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT );
	VkBufferImageCopy copy = {};
	copy.bufferOffset = (VkDeviceSize)slot * 256;
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent = { 1, 1, 1 };
	vkCmdCopyImageToBuffer( vk.cmd, post.lumAverage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, post.eyeBuffer, 1, &copy );
	Barrier( post.lumAverage.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT );
	post.eyeWritten[slot] = true;

	post.eyeExposure = R_EyeAdaptation_Exposure( measured );
	return post.eyeExposure;
}

// r_rtDebug: rays towards the sun from every pixel through the level's ray tracing structure, into post.shadow
static void RayTracingDebug( int x, int y, int width, int height, const postInput_t &depth, VkAccelerationStructureKHR scene )
{
	const float *proj = backEnd.viewParms.projectionMatrix;
	const float *m = backEnd.viewParms.world.modelMatrix;	// world to eye; its inverse, from the eye to the world, below
	float push[32] = {
		proj[0], proj[5], proj[8], proj[9],
		proj[10], proj[14], (float)width / vk.width, (float)height / vk.height,
		(float)x / vk.width, (float)( vk.height - ( y + height ) ) / vk.height, 0.0f, 0.0f };
	for ( int j = 0; j < 3; j++ )
	{
		float *row = push + 12 + j * 4;
		row[3] = 0.0f;
		for ( int i = 0; i < 3; i++ )
		{
			row[i] = m[j * 4 + i];
			row[3] -= m[j * 4 + i] * m[12 + i];
		}
	}
	push[24] = tr.sunDirection[0];
	push[25] = tr.sunDirection[1];
	push[26] = tr.sunDirection[2];
	push[28] = (float)r_rtDebug->integer;
	postInput_t inputs[3] = { depth, {}, {} };
	RunPass( PASS_RT_DEBUG, &post.shadow, false, inputs, push, 32, NULL, scene );
}

// light shafts (see tr_light_shafts.h): the sky near the sun into post.shaftsA (while the depth can be read)
static void LightShaftsSky( int x, int y, int width, int height, const float sun[3], const postInput_t &depth )
{
	const float push[16] = { sun[0], 1.0f - sun[1], (float)width / height, sun[2],
		(float)width / vk.width, (float)height / vk.height, (float)x / vk.width, (float)( vk.height - ( y + height ) ) / vk.height,
		0.0f };
	postInput_t inputs[3] = { Input( &post.scene ), depth, {} };
	RunPass( PASS_SHAFTS, &post.shaftsA, false, inputs, push );
}

// ... blurred towards the sun into post.shaftsB, and added over the view with the effects on it (post.final)
static void LightShaftsAdd( const float sun[3] )
{
	float push[16] = { sun[0], 1.0f - sun[1], 0.0f, sun[2], 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
	postInput_t inputs[3] = { Input( &post.shaftsA ), {}, {} };
	RunPass( PASS_SHAFTS, &post.shaftsB, false, inputs, push );

	push[8] = 2.0f;
	inputs[0] = Input( &post.shaftsB );
	RunPass( PASS_SHAFTS_ADD, &post.final, true, inputs, push );
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
static void DrawBack( int x, int y, int width, int height, GLuint texture, unsigned int state = GLS_DEPTHTEST_DISABLE, int stencilSkip = 0 )
{
	GL_State( state );
	if ( stencilSkip )
	{
		// (the pixels with the bit set are left alone)
		qglEnable( GL_STENCIL_TEST );
		qglStencilMask( 0 );
		qglStencilFunc( GL_EQUAL, 0, stencilSkip );
		qglStencilOp( GL_KEEP, GL_KEEP, GL_KEEP );
	}
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
	float sun[3];
	const bool shafts = R_LightShaftsSun( sun );
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
	const VkAccelerationStructureKHR rtScene = ( vk.rayTracing && r_rtDebug->integer && EnsureRtLayout() ) ? VK_RT_Scene() : VK_NULL_HANDLE;

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

	const float exposure = EyeAdaptation();

	// the depth is read where it is
	postInput_t depth = { post.depthView, true, true };
	if ( ssao || shafts || rtScene )
	{
		Barrier( vk.depthImage, depthAspects, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT );
		if ( ssao )
		{
			Ssao( x, y, width, height, depth );
		}
		if ( shafts )
		{
			LightShaftsSky( x, y, width, height, sun, depth );
		}
		if ( rtScene )
		{
			RayTracingDebug( x, y, width, height, depth, rtScene );
		}
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
		const float push[16] = { ssao ? 1.0f : 0.0f, r_postDebug->integer == 2 ? 1.0f : 0.0f, bloom ? r_bloomIntensity->value : 0.0f, exposure };
		RunPass( PASS_COMPOSITE, &post.final, false, inputs, push );
	}
	if ( shafts )
	{
		LightShaftsAdd( sun );
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
	if ( r_sharpen->value > 0.0f )
	{
		// contrast adaptive sharpening of the finished view (see tr_post.h)
		const float sharpness = ( r_sharpen->value > 1.0f ) ? 1.0f : r_sharpen->value;
		postInput_t inputs[3] = { Input( finished ), Input( finished ), Input( finished ) };
		inputs[1].view = inputs[2].view = VK_NULL_HANDLE;
		inputs[0].nearest = true;
		const float push[16] = { -1.0f / ( 8.0f - 3.0f * sharpness ) };
		RunPass( PASS_SHARPEN, &post.sharp, false, inputs, push );
		finished = &post.sharp;
	}
	VK_SetExternalTexture( POST_RESULT_ID, finished->image, finished->view, finished->width, finished->height );

	const GLboolean scissored = qglIsEnabled( GL_SCISSOR_TEST );
	DrawBack( x, y, width, height, POST_RESULT_ID );
	if ( rtScene )
	{
		VK_SetExternalTexture( POST_SHADOW_ID, post.shadow.image, post.shadow.view, width, height );
		DrawBack( x, y, width, height, POST_SHADOW_ID, r_rtDebug->integer >= 2 ? GLS_DEPTHTEST_DISABLE
			: GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ZERO | GLS_DSTBLEND_SRC_COLOR );
	}
	if ( scissored )
	{
		qglEnable( GL_SCISSOR_TEST );
	}
	SetViewportAndScissor();
}

/*
================
RB_GammaPass

Brightness and overbright for the whole picture before it is shown, the way the gamma ramps of the original games did
it (see the same in tr_post.h for the OpenGL renderers): the picture is copied, run through the table in a pass, and
drawn back. At brightness 1 without overbright (the defaults) nothing is done.
================
*/
qboolean R_Post_GammaAvailable( void )
{
	return post.ready ? qtrue : qfalse;
}

void RB_GammaPass( void )
{
	const float gamma = r_gamma->value;
	const int overbright = tr.overbrightBits;
	if ( !post.ready || post.gammaFailed || !glConfig.deviceSupportsGamma || ( gamma == 1.0f && overbright <= 0 ) )
	{
		return;
	}
	const int width = vk.width;
	const int height = vk.height;
	if ( !post.gammaIn.image || post.gammaIn.width != width || post.gammaIn.height != height )
	{
		VK_WaitIdle();
		FreeImage( &post.gammaIn );
		FreeImage( &post.gammaOut );
		if ( !NewImage( &post.gammaIn, width, height ) || !NewImage( &post.gammaOut, width, height ) )
		{
			FreeImage( &post.gammaIn );
			FreeImage( &post.gammaOut );
			post.gammaFailed = true;
			return;
		}
	}

	VK_EnsureFrame();
	VK_EndRenderPass();
	VK_FlushUploads( false );

	// the picture into an image of its own (the same way up as RB_PostProcess copies the view)
	Barrier( vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT );
	Barrier( post.gammaIn.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );
	VkImageBlit blit = {};
	blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.layerCount = 1;
	blit.srcOffsets[1] = { width, height, 1 };
	blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.dstSubresource.layerCount = 1;
	blit.dstOffsets[1] = { width, height, 1 };
	vkCmdBlitImage( vk.cmd, vk.colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, post.gammaIn.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blit, VK_FILTER_NEAREST );
	Barrier( post.gammaIn.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT );

	postInput_t inputs[3] = { Input( &post.gammaIn ), Input( &post.gammaIn ), Input( &post.gammaIn ) };
	inputs[1].view = inputs[2].view = VK_NULL_HANDLE;
	const float push[16] = { 1.0f / gamma, (float)( 1 << overbright ) };
	RunPass( PASS_GAMMA, &post.gammaOut, false, inputs, push );

	Barrier( vk.colorImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT );

	VK_SetExternalTexture( POST_GAMMA_ID, post.gammaOut.image, post.gammaOut.view, width, height );
	DrawBack( 0, 0, width, height, POST_GAMMA_ID );
}

/*
================
RB_ShadowMapsApply

The shadow maps of the characters (see tr_shadow_volumes.h, and the same in tr_post.h for OpenGL): a pass per caster
into an image of the view (the first writes it, the others are multiplied into it), which is then multiplied into the
picture, leaving the pixels of the characters alone through the stencil.
================
*/
qboolean R_ShadowMaps_Available( void )
{
	return ( post.ready && !post.shadowFailed ) ? qtrue : qfalse;
}

void RB_ShadowMapsApply( const shadowMapCaster_t *casters, int numCasters, const byte *atlas, int stencilSkip )
{
	const int x = backEnd.viewParms.viewportX;
	const int y = backEnd.viewParms.viewportY;
	const int width = backEnd.viewParms.viewportWidth;
	const int height = backEnd.viewParms.viewportHeight;
	if ( !post.ready || post.shadowFailed || numCasters <= 0 || x < 0 || y < 0 || x + width > vk.width || y + height > vk.height )
	{
		return;
	}
	if ( !EnsureImages( width, height ) )
	{
		post.shadowFailed = true;
		return;
	}

	if ( !post.atlasMade )
	{
		VK_TexImage2D( POST_ATLAS_ID, 0, 4, SHADOWMAP_ATLAS, SHADOWMAP_ATLAS, GL_RGBA, GL_UNSIGNED_BYTE, atlas );
		VK_TexParameter( POST_ATLAS_ID, GL_TEXTURE_MIN_FILTER, (GLfloat)GL_NEAREST );
		VK_TexParameter( POST_ATLAS_ID, GL_TEXTURE_MAG_FILTER, (GLfloat)GL_NEAREST );
		VK_TexParameter( POST_ATLAS_ID, GL_TEXTURE_WRAP_S, (GLfloat)GL_CLAMP_TO_EDGE );
		VK_TexParameter( POST_ATLAS_ID, GL_TEXTURE_WRAP_T, (GLfloat)GL_CLAMP_TO_EDGE );
		post.atlasMade = true;
	}
	else
	{
		VK_TexSubImage2D( POST_ATLAS_ID, 0, 0, 0, SHADOWMAP_ATLAS, SHADOWMAP_ATLAS, GL_RGBA, GL_UNSIGNED_BYTE, atlas );
	}

	VK_EnsureFrame();
	VK_EndRenderPass();	// (resolves the view into the single sample colour and depth)
	VK_FlushUploads( false );

	const VkImageAspectFlags depthAspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	Barrier( vk.depthImage, depthAspects, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT );

	const float *proj = backEnd.viewParms.projectionMatrix;
	postInput_t inputs[3] = { { post.depthView, true, true }, { VK_TextureForId( POST_ATLAS_ID, false )->view, true, false }, {} };
	for ( int i = 0; i < numCasters; i++ )
	{
		const shadowMapCaster_t *c = &casters[i];
		float push[32] = {
			proj[0], proj[5], proj[8], proj[9],
			proj[10], proj[14], (float)width / vk.width, (float)height / vk.height,
			(float)x / vk.width, (float)( vk.height - ( y + height ) ) / vk.height, c->floorZ, c->darkness };
		memcpy( push + 12, c->rowS, sizeof( c->rowS ) );
		memcpy( push + 16, c->rowT, sizeof( c->rowT ) );
		memcpy( push + 20, c->rowN, sizeof( c->rowN ) );
		memcpy( push + 24, c->rowZ, sizeof( c->rowZ ) );
		push[28] = c->bias;
		push[29] = c->softness;
		push[30] = c->tile[0];
		push[31] = c->tile[1];

		// the first caster writes all of the image (white where it has no shadow), the others multiply into it only
		// where their shadow can be (the image is from the top left corner)
		const int left = (int)floorf( c->rect[0] * width ), right = (int)ceilf( c->rect[2] * width );
		const int top = (int)floorf( ( 1.0f - c->rect[3] ) * height ), bottom = (int)ceilf( ( 1.0f - c->rect[1] ) * height );
		const VkRect2D area = { { left, top }, { (uint32_t)Q_max( right - left, 0 ), (uint32_t)Q_max( bottom - top, 0 ) } };
		if ( i && ( area.extent.width == 0 || area.extent.height == 0 ) )
		{
			continue;
		}
		RunPass( i ? PASS_SHADOW_MUL : PASS_SHADOW, &post.shadow, i > 0, inputs, push, 32, i ? &area : NULL );
	}

	Barrier( vk.depthImage, depthAspects, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT );

	VK_SetExternalTexture( POST_SHADOW_ID, post.shadow.image, post.shadow.view, width, height );
	const GLboolean scissored = qglIsEnabled( GL_SCISSOR_TEST );
	DrawBack( x, y, width, height, POST_SHADOW_ID, GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ZERO | GLS_DSTBLEND_SRC_COLOR, stencilSkip );
	if ( scissored )
	{
		qglEnable( GL_SCISSOR_TEST );
	}
	SetViewportAndScissor();
}

/*
=================
RB_RayTracedShadowsApply

The ray traced shadows of the characters (r_rayTracing, see tr_shadow_maps.h): their triangles into the frame's ray
tracing structure, the casters into a buffer the shader reads by its address, then one pass over the view into
post.rtShadow, where each pixel traces rays only for the casters whose shadow can fall on it: at Medium at half the
size of the view (a quarter of the rays), four rays a pixel; at High at its full size, eight rays a pixel where the
first four disagree. Blurred along the surfaces at the size of the view into post.shadow, and multiplied into the
picture where the stencil does not mark a character.
=================
*/
qboolean R_RayTracedShadows_Available( void )
{
	return ( post.ready && vk.rayTracing && !post.rtShadowFailed && !post.shadowFailed ) ? qtrue : qfalse;
}

int R_RayTracing( void )
{
	if ( !r_rayTracing || r_rayTracing->integer <= 0 || !R_RayTracedShadows_Available() )
	{
		return 0;
	}
	return r_rayTracing->integer >= 2 ? 2 : 1;
}

// with ray tracing, the dynamic lights' rays keep them from going through walls (dlight_rt.frag)
qboolean R_DlightShadows( void )
{
	return R_RayTracing() ? qtrue : qfalse;
}

/*
=================
VK_RT_BeginView

Before a view is drawn: the level's ray tracing structure of the frame, for the dynamic lights' rays (built outside a
render pass, so it ends the one that is open; the view's first draw opens another).
=================
*/
void VK_RT_BeginView( void )
{
	post.softReady = false;	// (the soft particles need the depth of this view, RB_SoftParticlesBegin)
	if ( !R_DlightShadows() || ( backEnd.refdef.rdflags & RDF_NOWORLDMODEL ) )
	{
		VK_DrawSetScene( VK_NULL_HANDLE );
		return;
	}
	VK_EnsureFrame();
	VK_EndRenderPass();
	VK_DrawSetScene( VK_RT_Scene() );
}

void RB_RayTracedShadowsApply( const shadowMapCaster_t *casters, int numCasters, const float *xyz, int numPoints,
	const int *indexes, int numTriangles, int stencilSkip )
{
	const int x = backEnd.viewParms.viewportX;
	const int y = backEnd.viewParms.viewportY;
	const int width = backEnd.viewParms.viewportWidth;
	const int height = backEnd.viewParms.viewportHeight;
	if ( !R_RayTracedShadows_Available() || numCasters <= 0 || x < 0 || y < 0 || x + width > vk.width || y + height > vk.height )
	{
		return;
	}
	if ( !EnsureImages( width, height ) || !EnsureRtLayout() )
	{
		post.rtShadowFailed = true;
		return;
	}

	VK_EnsureFrame();

	// the entries, as post_rt_shadow.frag reads them (5 vec4 each)
	const int slot = vk.frameIndex;
	if ( post.rtCastersFrame[slot] != vk.frameCounter + 1 )
	{
		post.rtCastersFrame[slot] = vk.frameCounter + 1;
		post.rtCastersUsed[slot] = 0;
	}
	const VkDeviceSize sectionBytes = SHADOWMAP_MAX_RT * 20 * sizeof( float );
	if ( post.rtCastersUsed[slot] >= RT_CASTER_SECTIONS ||
		 ( !post.rtCasters[slot].buffer && !VK_RT_NewBuffer( &post.rtCasters[slot], sectionBytes * RT_CASTER_SECTIONS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true ) ) )
	{
		return;
	}
	const VkDeviceSize offset = sectionBytes * post.rtCastersUsed[slot]++;
	float *data = (float *)( (byte *)post.rtCasters[slot].mapped + offset );
	numCasters = Q_min( numCasters, SHADOWMAP_MAX_RT );
	for ( int i = 0; i < numCasters; i++, data += 20 )
	{
		const shadowMapCaster_t *c = &casters[i];
		memcpy( data, c->rowS, 4 * sizeof( float ) );
		memcpy( data + 4, c->rowT, 4 * sizeof( float ) );
		memcpy( data + 8, c->area, 4 * sizeof( float ) );
		memcpy( data + 12, c->light, 4 * sizeof( float ) );
		data[16] = c->floorZ;
		data[17] = c->darkness;
		data[18] = (float)c->rtType;
		data[19] = c->reach;
	}

	VK_EndRenderPass();	// (resolves the view into the single sample colour and depth)
	VK_FlushUploads( false );
	const VkAccelerationStructureKHR scene = VK_RT_Characters( xyz, numPoints, indexes, numTriangles );
	if ( !scene )
	{
		return;
	}

	const VkImageAspectFlags depthAspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	Barrier( vk.depthImage, depthAspects, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT );

	const float *proj = backEnd.viewParms.projectionMatrix;
	const float *m = backEnd.viewParms.world.modelMatrix;	// world to eye; its inverse, from the eye to the world, below
	const postInput_t depth = { post.depthView, true, true };
	postInput_t inputs[3] = { depth, {}, {} };

	float push[32] = {
		proj[0], proj[5], proj[8], proj[9],
		proj[10], proj[14], (float)width / vk.width, (float)height / vk.height,
		(float)x / vk.width, (float)( vk.height - ( y + height ) ) / vk.height, 0.0f, 0.0f };
	for ( int j = 0; j < 3; j++ )
	{
		float *row = push + 12 + j * 4;
		row[3] = 0.0f;
		for ( int k = 0; k < 3; k++ )
		{
			row[k] = m[j * 4 + k];
			row[3] -= m[j * 4 + k] * m[12 + k];
		}
	}
	const VkDeviceAddress address = post.rtCasters[slot].address + offset;
	const uint32_t count = (uint32_t)numCasters, quality = (uint32_t)R_RayTracing();
	memcpy( push + 24, &address, sizeof( address ) );
	memcpy( push + 26, &count, sizeof( count ) );
	memcpy( push + 27, &quality, sizeof( quality ) );
	RunPass( PASS_RT_SHADOW, &post.rtShadow, false, inputs, push, 28, NULL, scene );

	// the few rays a pixel has, blurred along the surfaces (the blur of the ambient occlusion)
	float blur[16] = {
		proj[0], proj[5], proj[8], proj[9],
		proj[10], proj[14], (float)width, (float)height,
		1.0f, 0.0f, 0.0f, 0.0f,
		(float)width / vk.width, (float)height / vk.height, (float)x / vk.width, (float)( vk.height - ( y + height ) ) / vk.height };
	inputs[1] = Input( &post.rtShadow );
	RunPass( PASS_SSAO_BLUR, &post.aoB, false, inputs, blur );
	blur[8] = 0.0f;
	blur[9] = 1.0f;
	inputs[1] = Input( &post.aoB );
	RunPass( PASS_SSAO_BLUR, &post.shadow, false, inputs, blur );

	Barrier( vk.depthImage, depthAspects, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT );

	VK_SetExternalTexture( POST_SHADOW_ID, post.shadow.image, post.shadow.view, width, height );
	const GLboolean scissored = qglIsEnabled( GL_SCISSOR_TEST );
	DrawBack( x, y, width, height, POST_SHADOW_ID, GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ZERO | GLS_DSTBLEND_SRC_COLOR, stencilSkip );
	if ( scissored )
	{
		qglEnable( GL_SCISSOR_TEST );
	}
	SetViewportAndScissor();
}

/*
=================
RB_SoftParticlesBegin, RB_SoftParticles

The soft particles (r_softParticles): the stages of effects (smoke, sparks, explosions, the glow of sabers and
bolts) fade out within a few units in front of what is behind them, instead of cutting a hard line where they go into
a wall or the floor. Once the opaque surfaces of a view are drawn, their depth is copied into an image of its own (the
depth buffer itself goes on being drawn into); a stage that fades reads it in the second texture unit's place
(fixed.frag, SOFT).
=================
*/
static void FreeSoftDepth( void )
{
	if ( post.softImage )
	{
		vkDestroyImageView( vk.device, post.softView, NULL );
		vkDestroyImage( vk.device, post.softImage, NULL );
		VK_FreeMemory( &post.softMemory );
	}
	post.softImage = VK_NULL_HANDLE;
	post.softView = VK_NULL_HANDLE;
	post.softWidth = post.softHeight = 0;
}

static bool EnsureSoftDepth( void )
{
	if ( post.softImage && post.softWidth == vk.width && post.softHeight == vk.height )
	{
		return true;
	}
	if ( post.softImage )
	{
		vkDeviceWaitIdle( vk.device );	// (the window changed size: frames in flight may still read the old one)
		FreeSoftDepth();
	}
	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = vk.depthFormat;
	info.extent.width = (uint32_t)vk.width;
	info.extent.height = (uint32_t)vk.height;
	info.extent.depth = 1;
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if ( vkCreateImage( vk.device, &info, NULL, &post.softImage ) != VK_SUCCESS )
	{
		post.softImage = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements( vk.device, post.softImage, &requirements );
	if ( !VK_AllocateMemory( &requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &post.softMemory ) )
	{
		vkDestroyImage( vk.device, post.softImage, NULL );
		post.softImage = VK_NULL_HANDLE;
		return false;
	}
	VK_CheckResult( vkBindImageMemory( vk.device, post.softImage, post.softMemory.memory, post.softMemory.offset ), "vkBindImageMemory" );
	VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	viewInfo.image = post.softImage;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = vk.depthFormat;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;
	VK_CheckResult( vkCreateImageView( vk.device, &viewInfo, NULL, &post.softView ), "vkCreateImageView" );
	post.softWidth = vk.width;
	post.softHeight = vk.height;

	// read as the second texture unit (its first copy makes its layout one for reading)
	VK_SetExternalTexture( POST_SOFT_ID, post.softImage, post.softView, vk.width, vk.height );
	VK_TexParameter( POST_SOFT_ID, GL_TEXTURE_MIN_FILTER, (GLfloat)GL_NEAREST );
	VK_TexParameter( POST_SOFT_ID, GL_TEXTURE_MAG_FILTER, (GLfloat)GL_NEAREST );
	return true;
}

void RB_SoftParticlesBegin( void )
{
	static cvar_t *soft = NULL;
	if ( !soft )
	{
		soft = ri.Cvar_Get( "r_softParticles", "1", CVAR_ARCHIVE_ND );
	}
	post.softReady = false;
	if ( !soft->integer || !post.ready || post.softFailed || backEnd.viewParms.isPortal || ( backEnd.refdef.rdflags & RDF_NOWORLDMODEL ) )
	{
		return;
	}
	VK_EnsureFrame();
	VK_EndRenderPass();	// (resolves the depth of a multisampled view into vk.depthImage)
	if ( !EnsureSoftDepth() )
	{
		post.softFailed = true;
		return;
	}

	const VkImageAspectFlags depthAspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	Barrier( vk.depthImage, depthAspects, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT );
	// (barriers on both aspects of a depth and stencil format; only the depth is copied and read)
	Barrier( post.softImage, depthAspects, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT );
	VkImageCopy region = {};
	region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	region.srcSubresource.layerCount = 1;
	region.dstSubresource = region.srcSubresource;
	region.extent.width = (uint32_t)vk.width;
	region.extent.height = (uint32_t)vk.height;
	region.extent.depth = 1;
	vkCmdCopyImage( vk.cmd, vk.depthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, post.softImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
	Barrier( vk.depthImage, depthAspects, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
		VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT );
	Barrier( post.softImage, depthAspects, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT );
	post.softReady = true;
}

qboolean RB_SoftParticles( float units )
{
	if ( units > 0.0f && !post.softReady )
	{
		return qfalse;
	}
	VK_SetSoftParticles( units, POST_SOFT_ID );
	return qtrue;
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
	ri.Cvar_Get( "r_softAvail", "0", 0 );
	ri.Cvar_Set( "r_softAvail", post.ready ? "1" : "0" );	// (the soft particles, RB_SoftParticlesBegin)
	ri.Cvar_Get( "r_shadowMapsAvail", "0", 0 );
	ri.Cvar_Set( "r_shadowMapsAvail", R_ShadowMaps_Available() ? "1" : "0" );
	// (r_rtAvail, for the Ray Traced shadows, is set by VK_RT_Init)
	if ( post.ready )
	{
		Com_Printf( "...using post processing\n" );
	}
}

void VK_InitPost( void )
{
	memset( &post, 0, sizeof( post ) );
	r_rtDebug = ri.Cvar_Get( "r_rtDebug", "0", 0 );
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
	VkPushConstantRange range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 * sizeof( float ) };	// (128 bytes, what every device has)
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
		if ( post.rtPools[vk.frameIndex] )
		{
			vkResetDescriptorPool( vk.device, post.rtPools[vk.frameIndex], 0 );
		}
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
	FreeSoftDepth();
	if ( post.eyeBuffer )
	{
		vkUnmapMemory( vk.device, post.eyeMemory );
		vkDestroyBuffer( vk.device, post.eyeBuffer, NULL );
		vkFreeMemory( vk.device, post.eyeMemory, NULL );
	}
	FreeImage( &post.gammaIn );
	FreeImage( &post.gammaOut );
	for ( int i = 0; i < PASS_COUNT; i++ )
	{
		if ( post.pipelines[i] ) vkDestroyPipeline( vk.device, post.pipelines[i], NULL );
		if ( post.fragmentShaders[i] ) vkDestroyShaderModule( vk.device, post.fragmentShaders[i], NULL );
	}
	for ( int i = 0; i < VK_FRAMES_IN_FLIGHT; i++ )
	{
		vkDestroyDescriptorPool( vk.device, post.pools[i], NULL );
		if ( post.rtPools[i] ) vkDestroyDescriptorPool( vk.device, post.rtPools[i], NULL );
	}
	if ( post.rtPipelineLayout ) vkDestroyPipelineLayout( vk.device, post.rtPipelineLayout, NULL );
	for ( int i = 0; i < VK_FRAMES_IN_FLIGHT; i++ )
	{
		VK_RT_FreeBuffer( &post.rtCasters[i] );
	}
	if ( post.rtSetLayout ) vkDestroyDescriptorSetLayout( vk.device, post.rtSetLayout, NULL );
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
