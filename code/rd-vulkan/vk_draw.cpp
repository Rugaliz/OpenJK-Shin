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

// Drawing for the Vulkan renderer module: the pipelines (one per combination of fixed function state), the buffers
// the vertices of a draw are written to, and the descriptor sets for its textures.

#include "../server/exe_headers.h"
#include "../rd-vanilla/tr_local.h"
#include "vk_priv.h"
#include "vk_spirv.h"
#include <unordered_map>

#define GEOMETRY_SLOT_SIZE	( 64 * 1024 * 1024 )	// per frame in flight
#define MAX_DESCRIPTOR_SETS	8192

static VkShaderModule			vertexShader, fragmentShader;
static VkDescriptorSetLayout	descriptorSetLayout;
static VkPipelineLayout			pipelineLayout;
static VkDescriptorPool			descriptorPool;
static VkPipelineCache			pipelineCache;	// kept between runs: the drivers take a while to build a pipeline

#define PIPELINE_CACHE_FILE	"vk_pipeline_cache.bin"
static int						descriptorSetCount;

// vertex, index buffer: GEOMETRY_SLOT_SIZE for each frame in flight, host visible
static VkBuffer					geometryBuffer;
static VkDeviceMemory			geometryMemory;
static byte						*geometryMapped;
static VkDeviceSize				geometryCursor;
static VkDeviceSize				geometrySlotEnd;

struct KeyHash {
	size_t operator()( const vkPipelineKey_t &key ) const
	{
		const byte *bytes = (const byte *)&key;
		size_t hash = 1469598103934665603ull;
		for ( size_t i = 0; i < sizeof( key ); i++ )
		{
			hash = ( hash ^ bytes[i] ) * 1099511628211ull;
		}
		return hash;
	}
};
struct KeyEqual {
	bool operator()( const vkPipelineKey_t &a, const vkPipelineKey_t &b ) const
	{
		return memcmp( &a, &b, sizeof( a ) ) == 0;
	}
};
static std::unordered_map<vkPipelineKey_t, VkPipeline, KeyHash, KeyEqual>	pipelines;

struct SetKey {
	unsigned long long a, b;
	bool operator==( const SetKey &o ) const { return a == o.a && b == o.b; }
};
struct SetHash {
	size_t operator()( const SetKey &k ) const { return (size_t)( k.a * 0x9E3779B97F4A7C15ull ^ k.b * 0xC2B2AE3D27D4EB4Full ); }
};
static std::unordered_map<SetKey, VkDescriptorSet, SetHash>	descriptorSets;

// what is bound in the current command buffer
static struct bound_s {
	VkPipeline			pipeline;
	VkDescriptorSet		set;
	vkConstants_t		constants;
	bool				constantsValid;
	float				viewport[6];
	bool				viewportValid;
	int					scissor[4];
	bool				scissorValid;
	float				bias[2];
	bool				biasValid;
	unsigned int		stencil[6];
	bool				stencilValid;
} bound;

void VK_ResetCommandBufferState( void )
{
	memset( &bound, 0, sizeof( bound ) );
}

static VkBlendFactor BlendFactor( unsigned short factor )
{
	switch ( factor )
	{
	case GL_ZERO:					return VK_BLEND_FACTOR_ZERO;
	case GL_ONE:					return VK_BLEND_FACTOR_ONE;
	case GL_SRC_COLOR:				return VK_BLEND_FACTOR_SRC_COLOR;
	case GL_ONE_MINUS_SRC_COLOR:	return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
	case GL_DST_COLOR:				return VK_BLEND_FACTOR_DST_COLOR;
	case GL_ONE_MINUS_DST_COLOR:	return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
	case GL_SRC_ALPHA:				return VK_BLEND_FACTOR_SRC_ALPHA;
	case GL_ONE_MINUS_SRC_ALPHA:	return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	case GL_DST_ALPHA:				return VK_BLEND_FACTOR_DST_ALPHA;
	case GL_ONE_MINUS_DST_ALPHA:	return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
	case GL_SRC_ALPHA_SATURATE:		return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
	}
	return VK_BLEND_FACTOR_ONE;
}

static VkCompareOp CompareOp( unsigned short func )
{
	switch ( func )
	{
	case GL_NEVER:		return VK_COMPARE_OP_NEVER;
	case GL_LESS:		return VK_COMPARE_OP_LESS;
	case GL_EQUAL:		return VK_COMPARE_OP_EQUAL;
	case GL_LEQUAL:		return VK_COMPARE_OP_LESS_OR_EQUAL;
	case GL_GREATER:	return VK_COMPARE_OP_GREATER;
	case GL_NOTEQUAL:	return VK_COMPARE_OP_NOT_EQUAL;
	case GL_GEQUAL:		return VK_COMPARE_OP_GREATER_OR_EQUAL;
	}
	return VK_COMPARE_OP_ALWAYS;
}

static VkStencilOp StencilOp( unsigned short op )
{
	switch ( op )
	{
	case GL_ZERO:		return VK_STENCIL_OP_ZERO;
	case GL_REPLACE:	return VK_STENCIL_OP_REPLACE;
	case GL_INCR:		return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
	case GL_DECR:		return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
	case GL_INVERT:		return VK_STENCIL_OP_INVERT;
	case GL_INCR_WRAP:	return VK_STENCIL_OP_INCREMENT_AND_WRAP;
	case GL_DECR_WRAP:	return VK_STENCIL_OP_DECREMENT_AND_WRAP;
	}
	return VK_STENCIL_OP_KEEP;
}

static int AlphaFuncNumber( unsigned short func )
{
	switch ( func )
	{
	case GL_LESS:		return 1;
	case GL_EQUAL:		return 2;
	case GL_LEQUAL:		return 3;
	case GL_GREATER:	return 4;
	case GL_NOTEQUAL:	return 5;
	case GL_GEQUAL:		return 6;
	}
	return 0;
}

static int EnvNumber( unsigned char env )
{
	switch ( env )
	{
	case 0:				return 0;
	default:			return env;	// already 0 modulate, 1 replace, 2 add, 3 decal (see vkglTexEnvf)
	}
}

static VkPipeline CreatePipeline( const vkPipelineKey_t *key )
{
	// vertex input: one binding for each array, a stride of 0 repeats the single value
	VkVertexInputBindingDescription bindings[4] = {
		{ 0, 12, VK_VERTEX_INPUT_RATE_VERTEX },
		{ 1, key->colorConst ? 0u : 4u, VK_VERTEX_INPUT_RATE_VERTEX },
		{ 2, key->uv0Const ? 0u : 8u, VK_VERTEX_INPUT_RATE_VERTEX },
		{ 3, key->uv1Const ? 0u : 8u, VK_VERTEX_INPUT_RATE_VERTEX },
	};
	VkVertexInputAttributeDescription attributes[4] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
		{ 1, 1, VK_FORMAT_R8G8B8A8_UNORM, 0 },
		{ 2, 2, VK_FORMAT_R32G32_SFLOAT, 0 },
		{ 3, 3, VK_FORMAT_R32G32_SFLOAT, 0 },
	};
	VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vertexInput.vertexBindingDescriptionCount = 4;
	vertexInput.pVertexBindingDescriptions = bindings;
	vertexInput.vertexAttributeDescriptionCount = 4;
	vertexInput.pVertexAttributeDescriptions = attributes;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	inputAssembly.topology = key->lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	raster.polygonMode = ( key->wireframe && vk.features.fillModeNonSolid ) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
	raster.cullMode = key->cullMode == 1 ? VK_CULL_MODE_FRONT_BIT : ( key->cullMode == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE );
	// (OpenGL's counter clockwise; the Y flip of the projection and Vulkan's Y down framebuffer cancel out)
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.depthBiasEnable = key->polyOffset ? VK_TRUE : VK_FALSE;
	raster.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	depthStencil.depthTestEnable = key->depthTest ? VK_TRUE : VK_FALSE;
	depthStencil.depthWriteEnable = key->depthWrite ? VK_TRUE : VK_FALSE;
	depthStencil.depthCompareOp = CompareOp( key->depthFunc );
	depthStencil.stencilTestEnable = key->stencilTest ? VK_TRUE : VK_FALSE;
	depthStencil.front.compareOp = CompareOp( key->stencilFunc[0] );
	depthStencil.front.failOp = StencilOp( key->stencilFail[0] );
	depthStencil.front.depthFailOp = StencilOp( key->stencilZFail[0] );
	depthStencil.front.passOp = StencilOp( key->stencilZPass[0] );
	depthStencil.back.compareOp = CompareOp( key->stencilFunc[1] );
	depthStencil.back.failOp = StencilOp( key->stencilFail[1] );
	depthStencil.back.depthFailOp = StencilOp( key->stencilZFail[1] );
	depthStencil.back.passOp = StencilOp( key->stencilZPass[1] );

	VkPipelineColorBlendAttachmentState attachment = {};
	attachment.blendEnable = key->blendEnable ? VK_TRUE : VK_FALSE;
	attachment.srcColorBlendFactor = attachment.srcAlphaBlendFactor = BlendFactor( key->srcBlend );
	attachment.dstColorBlendFactor = attachment.dstAlphaBlendFactor = BlendFactor( key->dstBlend );
	attachment.colorBlendOp = attachment.alphaBlendOp = VK_BLEND_OP_ADD;
	attachment.colorWriteMask = ( key->colorMask & 1 ? VK_COLOR_COMPONENT_R_BIT : 0 ) | ( key->colorMask & 2 ? VK_COLOR_COMPONENT_G_BIT : 0 )
		| ( key->colorMask & 4 ? VK_COLOR_COMPONENT_B_BIT : 0 ) | ( key->colorMask & 8 ? VK_COLOR_COMPONENT_A_BIT : 0 );
	VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	blend.attachmentCount = 1;
	blend.pAttachments = &attachment;

	const VkDynamicState dynamicStates[] = {
		VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
		VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
	};
	VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dynamic.dynamicStateCount = sizeof( dynamicStates ) / sizeof( dynamicStates[0] );
	dynamic.pDynamicStates = dynamicStates;

	struct { int unitMask, env0, env1, alphaFunc; float alphaRef; int fog, clip; } values = {
		key->unitMask, EnvNumber( key->env0 ), EnvNumber( key->env1 ), AlphaFuncNumber( key->alphaFunc ), key->alphaRef, key->fogMode, key->clip };
	VkSpecializationMapEntry entries[7];
	const uint32_t offsets[7] = { 0, 4, 8, 12, 16, 20, 24 };
	for ( uint32_t i = 0; i < 7; i++ )
	{
		entries[i].constantID = i;
		entries[i].offset = offsets[i];
		entries[i].size = 4;
	}
	VkSpecializationInfo specialization = { 7, entries, sizeof( values ), &values };

	VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertexShader;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragmentShader;
	stages[1].pName = "main";
	stages[1].pSpecializationInfo = &specialization;

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
	info.layout = pipelineLayout;
	info.renderPass = vk.renderPassLoad;
	info.subpass = 0;

	VkPipeline pipeline;
	VK_CheckResult( vkCreateGraphicsPipelines( vk.device, pipelineCache, 1, &info, NULL, &pipeline ), "vkCreateGraphicsPipelines" );
	return pipeline;
}

static VkDescriptorSet DescriptorSetFor( const vkTexture_t *texture0, const vkTexture_t *texture1 )
{
	const SetKey key = { texture0->serial, texture1->serial };
	std::unordered_map<SetKey, VkDescriptorSet, SetHash>::iterator found = descriptorSets.find( key );
	if ( found != descriptorSets.end() )
	{
		return found->second;
	}

	VkDescriptorSetAllocateInfo allocate = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	allocate.descriptorPool = descriptorPool;
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts = &descriptorSetLayout;
	VkDescriptorSet set;
	VK_CheckResult( vkAllocateDescriptorSets( vk.device, &allocate, &set ), "vkAllocateDescriptorSets" );
	descriptorSetCount++;

	const vkTexture_t *textures[2] = { texture0, texture1 };
	VkDescriptorImageInfo images[2];
	VkWriteDescriptorSet writes[2];
	for ( int i = 0; i < 2; i++ )
	{
		images[i].sampler = VK_SamplerFor( textures[i] );
		images[i].imageView = textures[i]->view;
		images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		writes[i].dstSet = set;
		writes[i].dstBinding = (uint32_t)i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &images[i];
	}
	vkUpdateDescriptorSets( vk.device, 2, writes, 0, NULL );
	descriptorSets[key] = set;
	return set;
}

/*
=================
VK_BeginFrameDraw

The frame slot is free again (its fence was waited for): the buffer for its vertices starts over.
=================
*/
void VK_BeginFrameDraw( void )
{
	geometryCursor = (VkDeviceSize)vk.frameIndex * GEOMETRY_SLOT_SIZE;
	geometrySlotEnd = geometryCursor + GEOMETRY_SLOT_SIZE;
}

static VkDeviceSize AlignedAllocation( VkDeviceSize size )
{
	const VkDeviceSize offset = ( geometryCursor + 15 ) & ~(VkDeviceSize)15;
	geometryCursor = offset + size;
	return offset;
}

/*
=================
VK_AllocateGeometry

Room for the vertices (and indices) of a draw in the buffer of the frame; the caller fills in the arrays.
=================
*/
void VK_AllocateGeometry( vkGeometry_t *geometry, int vertexCount, int indexCount, const vkPipelineKey_t *key )
{
	VK_EnsureFrame();

	const VkDeviceSize positionSize = (VkDeviceSize)vertexCount * 12;
	const VkDeviceSize colorSize = key->colorConst ? 4 : (VkDeviceSize)vertexCount * 4;
	const VkDeviceSize uv0Size = key->uv0Const ? 8 : (VkDeviceSize)vertexCount * 8;
	const VkDeviceSize uv1Size = key->uv1Const ? 8 : (VkDeviceSize)vertexCount * 8;
	const VkDeviceSize indexSize = (VkDeviceSize)indexCount * 4;
	const VkDeviceSize needed = positionSize + colorSize + uv0Size + uv1Size + indexSize + 5 * 16;
	if ( needed > GEOMETRY_SLOT_SIZE )
	{
		ri.Error( ERR_DROP, "Vulkan: a draw of %d vertices does not fit in the geometry buffer", vertexCount );
	}
	if ( geometryCursor + needed > geometrySlotEnd )
	{
		// everything drawn so far is sent and finished, then the buffer is used again from the start
		VK_SubmitFrame( true );
		geometryCursor = (VkDeviceSize)vk.frameIndex * GEOMETRY_SLOT_SIZE;
	}

	geometry->vertexCount = vertexCount;
	geometry->indexCount = indexCount;
	geometry->positionOffset = AlignedAllocation( positionSize );
	geometry->colorOffset = AlignedAllocation( colorSize );
	geometry->texCoord0Offset = AlignedAllocation( uv0Size );
	geometry->texCoord1Offset = AlignedAllocation( uv1Size );
	geometry->position = (float *)( geometryMapped + geometry->positionOffset );
	geometry->color = geometryMapped + geometry->colorOffset;
	geometry->texCoord0 = (float *)( geometryMapped + geometry->texCoord0Offset );
	geometry->texCoord1 = (float *)( geometryMapped + geometry->texCoord1Offset );
	if ( indexCount )
	{
		geometry->indexOffset = AlignedAllocation( indexSize );
		geometry->indices = (unsigned int *)( geometryMapped + geometry->indexOffset );
	}
	else
	{
		geometry->indexOffset = 0;
		geometry->indices = NULL;
	}
}

/*
=================
VK_Draw
=================
*/
void VK_Draw( const vkPipelineKey_t *key, const vkDynamicState_t *dynamic, const vkConstants_t *constants,
	const vkTexture_t *texture0, const vkTexture_t *texture1, const vkGeometry_t *geometry )
{
	if ( descriptorSetCount >= MAX_DESCRIPTOR_SETS )
	{
		// (the sets of textures that are gone fill the pool up: everything sent is finished, then it starts again)
		VK_SubmitFrame( true );
		vkResetDescriptorPool( vk.device, descriptorPool, 0 );
		descriptorSets.clear();
		descriptorSetCount = 0;
	}
	VK_EnsureRenderPass();
	VkCommandBuffer cmd = vk.cmd;

	std::unordered_map<vkPipelineKey_t, VkPipeline, KeyHash, KeyEqual>::iterator found = pipelines.find( *key );
	VkPipeline pipeline;
	if ( found == pipelines.end() )
	{
		pipeline = CreatePipeline( key );
		pipelines[*key] = pipeline;
	}
	else
	{
		pipeline = found->second;
	}
	if ( bound.pipeline != pipeline )
	{
		vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
		bound.pipeline = pipeline;
	}

	// dynamic state
	const float vx = dynamic->viewport[0], vw = dynamic->viewport[2], vh = dynamic->viewport[3];
	const float vy = (float)vk.height - ( dynamic->viewport[1] + vh );
	const float viewport[6] = { vx, vy, vw, vh, dynamic->depthRange[0], dynamic->depthRange[1] };
	if ( !bound.viewportValid || memcmp( bound.viewport, viewport, sizeof( viewport ) ) )
	{
		VkViewport vp = { vx, vy, vw, vh, dynamic->depthRange[0], dynamic->depthRange[1] };
		vkCmdSetViewport( cmd, 0, 1, &vp );
		memcpy( bound.viewport, viewport, sizeof( viewport ) );
		bound.viewportValid = true;
	}
	if ( !bound.scissorValid || memcmp( bound.scissor, dynamic->scissor, sizeof( bound.scissor ) ) )
	{
		int x = Q_max( dynamic->scissor[0], 0 ), y = Q_max( dynamic->scissor[1], 0 );
		int w = Q_max( Q_min( dynamic->scissor[2], vk.width - x ), 0 );
		int h = Q_max( Q_min( dynamic->scissor[3], vk.height - y ), 0 );
		VkRect2D rect = { { x, vk.height - ( y + h ) }, { (uint32_t)w, (uint32_t)h } };
		vkCmdSetScissor( cmd, 0, 1, &rect );
		memcpy( bound.scissor, dynamic->scissor, sizeof( bound.scissor ) );
		bound.scissorValid = true;
	}
	if ( key->polyOffset )
	{
		const float bias[2] = { dynamic->polyOffsetFactor, dynamic->polyOffsetUnits };
		if ( !bound.biasValid || memcmp( bound.bias, bias, sizeof( bias ) ) )
		{
			vkCmdSetDepthBias( cmd, bias[1], 0.0f, bias[0] );
			memcpy( bound.bias, bias, sizeof( bias ) );
			bound.biasValid = true;
		}
	}
	if ( key->stencilTest )
	{
		const unsigned int stencil[6] = { dynamic->stencilRef[0], dynamic->stencilRef[1], dynamic->stencilCompareMask[0],
			dynamic->stencilCompareMask[1], dynamic->stencilWriteMask[0], dynamic->stencilWriteMask[1] };
		if ( !bound.stencilValid || memcmp( bound.stencil, stencil, sizeof( stencil ) ) )
		{
			vkCmdSetStencilReference( cmd, VK_STENCIL_FACE_FRONT_BIT, stencil[0] );
			vkCmdSetStencilReference( cmd, VK_STENCIL_FACE_BACK_BIT, stencil[1] );
			vkCmdSetStencilCompareMask( cmd, VK_STENCIL_FACE_FRONT_BIT, stencil[2] );
			vkCmdSetStencilCompareMask( cmd, VK_STENCIL_FACE_BACK_BIT, stencil[3] );
			vkCmdSetStencilWriteMask( cmd, VK_STENCIL_FACE_FRONT_BIT, stencil[4] );
			vkCmdSetStencilWriteMask( cmd, VK_STENCIL_FACE_BACK_BIT, stencil[5] );
			memcpy( bound.stencil, stencil, sizeof( stencil ) );
			bound.stencilValid = true;
		}
	}

	// textures
	const vkTexture_t *white = VK_WhiteTexture();
	if ( !texture0 || !texture0->image ) texture0 = white;
	if ( !texture1 || !texture1->image ) texture1 = white;
	const VkDescriptorSet set = DescriptorSetFor( texture0, texture1 );
	if ( bound.set != set )
	{
		vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &set, 0, NULL );
		bound.set = set;
	}
	// (the textures count as used by this frame, and by this command buffer)
	const_cast<vkTexture_t *>( texture0 )->lastUsedFrame = vk.frameCounter;
	const_cast<vkTexture_t *>( texture0 )->lastUsedSubmit = vk.submitSerial;
	const_cast<vkTexture_t *>( texture1 )->lastUsedFrame = vk.frameCounter;
	const_cast<vkTexture_t *>( texture1 )->lastUsedSubmit = vk.submitSerial;

	if ( !bound.constantsValid || memcmp( &bound.constants, constants, sizeof( *constants ) ) )
	{
		vkCmdPushConstants( cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof( *constants ), constants );
		bound.constants = *constants;
		bound.constantsValid = true;
	}

	const VkBuffer buffers[4] = { geometryBuffer, geometryBuffer, geometryBuffer, geometryBuffer };
	const VkDeviceSize offsets[4] = { geometry->positionOffset, geometry->colorOffset, geometry->texCoord0Offset, geometry->texCoord1Offset };
	vkCmdBindVertexBuffers( cmd, 0, 4, buffers, offsets );
	if ( geometry->indexCount )
	{
		vkCmdBindIndexBuffer( cmd, geometryBuffer, geometry->indexOffset, VK_INDEX_TYPE_UINT32 );
		vkCmdDrawIndexed( cmd, (uint32_t)geometry->indexCount, 1, 0, 0, 0 );
	}
	else
	{
		vkCmdDraw( cmd, (uint32_t)geometry->vertexCount, 1, 0, 0 );
	}
}

void VK_InitDraw( void )
{
	// the cache of a previous run (the driver ignores one that is not from this device and driver)
	void *cacheData = NULL;
	const long cacheSize = ri.FS_ReadFile( PIPELINE_CACHE_FILE, &cacheData );
	VkPipelineCacheCreateInfo cacheInfo = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
	if ( cacheData && cacheSize > 0 )
	{
		cacheInfo.initialDataSize = (size_t)cacheSize;
		cacheInfo.pInitialData = cacheData;
	}
	VK_CheckResult( vkCreatePipelineCache( vk.device, &cacheInfo, NULL, &pipelineCache ), "vkCreatePipelineCache" );
	if ( cacheData )
	{
		ri.FS_FreeFile( cacheData );
	}

	VkShaderModuleCreateInfo shaderInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	shaderInfo.codeSize = sizeof( vk_spirv_fixed_vert );
	shaderInfo.pCode = vk_spirv_fixed_vert;
	VK_CheckResult( vkCreateShaderModule( vk.device, &shaderInfo, NULL, &vertexShader ), "vkCreateShaderModule" );
	shaderInfo.codeSize = sizeof( vk_spirv_fixed_frag );
	shaderInfo.pCode = vk_spirv_fixed_frag;
	VK_CheckResult( vkCreateShaderModule( vk.device, &shaderInfo, NULL, &fragmentShader ), "vkCreateShaderModule" );

	VkDescriptorSetLayoutBinding layoutBindings[2] = {};
	for ( uint32_t i = 0; i < 2; i++ )
	{
		layoutBindings[i].binding = i;
		layoutBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		layoutBindings[i].descriptorCount = 1;
		layoutBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	layoutInfo.bindingCount = 2;
	layoutInfo.pBindings = layoutBindings;
	VK_CheckResult( vkCreateDescriptorSetLayout( vk.device, &layoutInfo, NULL, &descriptorSetLayout ), "vkCreateDescriptorSetLayout" );

	VkPushConstantRange range = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof( vkConstants_t ) };
	VkPipelineLayoutCreateInfo pipelineLayoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &range;
	VK_CheckResult( vkCreatePipelineLayout( vk.device, &pipelineLayoutInfo, NULL, &pipelineLayout ), "vkCreatePipelineLayout" );

	VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_DESCRIPTOR_SETS * 2 };
	VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	poolInfo.maxSets = MAX_DESCRIPTOR_SETS;
	poolInfo.poolSizeCount = 1;
	poolInfo.pPoolSizes = &poolSize;
	VK_CheckResult( vkCreateDescriptorPool( vk.device, &poolInfo, NULL, &descriptorPool ), "vkCreateDescriptorPool" );
	descriptorSetCount = 0;

	// the vertex and index buffer: ask for memory the GPU can read fast and the CPU can write
	const VkDeviceSize size = (VkDeviceSize)GEOMETRY_SLOT_SIZE * VK_FRAMES_IN_FLIGHT;
	VkBufferCreateInfo bufferInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufferInfo.size = size;
	bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CheckResult( vkCreateBuffer( vk.device, &bufferInfo, NULL, &geometryBuffer ), "vkCreateBuffer" );
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements( vk.device, geometryBuffer, &requirements );
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocate.allocationSize = requirements.size;
	bool placed = false;
	const VkMemoryPropertyFlags wanted[2] = {
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT };
	for ( int i = 0; i < 2 && !placed; i++ )
	{
		for ( uint32_t t = 0; t < vk.memoryProperties.memoryTypeCount && !placed; t++ )
		{
			if ( ( requirements.memoryTypeBits & ( 1u << t ) ) && ( vk.memoryProperties.memoryTypes[t].propertyFlags & wanted[i] ) == wanted[i] )
			{
				allocate.memoryTypeIndex = t;
				// (the device local + host visible heap can be a small window: then it is not used)
				placed = vkAllocateMemory( vk.device, &allocate, NULL, &geometryMemory ) == VK_SUCCESS;
			}
		}
	}
	if ( !placed )
	{
		ri.Error( ERR_FATAL, "Vulkan: no memory for the geometry buffer" );
	}
	VK_CheckResult( vkBindBufferMemory( vk.device, geometryBuffer, geometryMemory, 0 ), "vkBindBufferMemory" );
	void *mapped;
	VK_CheckResult( vkMapMemory( vk.device, geometryMemory, 0, size, 0, &mapped ), "vkMapMemory" );
	geometryMapped = (byte *)mapped;
	VK_BeginFrameDraw();
}

void VK_ShutdownDraw( void )
{
	size_t cacheSize = 0;
	if ( vkGetPipelineCacheData( vk.device, pipelineCache, &cacheSize, NULL ) == VK_SUCCESS && cacheSize )
	{
		void *cacheData = R_Malloc( (int)cacheSize, TAG_TEMP_WORKSPACE, qfalse );
		if ( vkGetPipelineCacheData( vk.device, pipelineCache, &cacheSize, cacheData ) == VK_SUCCESS )
		{
			ri.FS_WriteFile( PIPELINE_CACHE_FILE, cacheData, (int)cacheSize );
		}
		R_Free( cacheData );
	}
	vkDestroyPipelineCache( vk.device, pipelineCache, NULL );
	for ( std::unordered_map<vkPipelineKey_t, VkPipeline, KeyHash, KeyEqual>::iterator it = pipelines.begin(); it != pipelines.end(); ++it )
	{
		vkDestroyPipeline( vk.device, it->second, NULL );
	}
	pipelines.clear();
	descriptorSets.clear();
	vkDestroyDescriptorPool( vk.device, descriptorPool, NULL );
	vkDestroyPipelineLayout( vk.device, pipelineLayout, NULL );
	vkDestroyDescriptorSetLayout( vk.device, descriptorSetLayout, NULL );
	vkDestroyShaderModule( vk.device, vertexShader, NULL );
	vkDestroyShaderModule( vk.device, fragmentShader, NULL );
	vkUnmapMemory( vk.device, geometryMemory );
	vkDestroyBuffer( vk.device, geometryBuffer, NULL );
	vkFreeMemory( vk.device, geometryMemory, NULL );
}
