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

// Ray tracing for the Vulkan renderer module, for shadows (see docs/vulkan-renderer.md): the standard Khronos
// extensions VK_KHR_acceleration_structure and VK_KHR_ray_query, which every vendor's ray tracing hardware has
// (AMD RX 6000 and newer, NVIDIA RTX, Intel Arc). Shaders ask "does this ray hit anything" with ray queries; what
// they search are acceleration structures: a bottom level one holds triangles, a top level one places bottom level
// ones in the world. This file has the device side: what the device must offer, buffers with device addresses, and
// building the structures. vk.rayTracing says it is on (the device has it, r_vkRayTracing, and the test at start-up
// worked); r_rtAvail tells the menus.

#include "../server/exe_headers.h"
#include "../rd-vanilla/tr_local.h"
#include "vk_priv.h"
#include "../game/surfaceflags.h"
#include <vector>

static const char *const rtExtensions[] = {
	VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
	VK_KHR_RAY_QUERY_EXTENSION_NAME,
	VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,		// (the acceleration structure extension needs it)
};

// the entry points of the extensions, which the loader does not export
static PFN_vkGetAccelerationStructureBuildSizesKHR		qvkGetAccelerationStructureBuildSizesKHR;
static PFN_vkCreateAccelerationStructureKHR			qvkCreateAccelerationStructureKHR;
static PFN_vkDestroyAccelerationStructureKHR			qvkDestroyAccelerationStructureKHR;
static PFN_vkCmdBuildAccelerationStructuresKHR			qvkCmdBuildAccelerationStructuresKHR;
static PFN_vkGetAccelerationStructureDeviceAddressKHR	qvkGetAccelerationStructureDeviceAddressKHR;
static PFN_vkGetBufferDeviceAddress					qvkGetBufferDeviceAddress;

static VkDeviceSize	scratchAlignment = 256;		// minAccelerationStructureScratchOffsetAlignment

/*
=============================================================================

THE DEVICE

=============================================================================
*/

static bool VK_RT_HasExtension( const VkExtensionProperties *extensions, uint32_t count, const char *name )
{
	for ( uint32_t i = 0; i < count; i++ )
	{
		if ( !strcmp( extensions[i].extensionName, name ) )
		{
			return true;
		}
	}
	return false;
}

/*
=================
VK_RT_DeviceSupports

The device has the extensions and the features ray traced shadows need (Vulkan 1.2 for buffer device addresses).
=================
*/
bool VK_RT_DeviceSupports( VkPhysicalDevice device )
{
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties( device, &properties );
	if ( properties.apiVersion < VK_API_VERSION_1_2 )
	{
		return false;
	}

	uint32_t count = 0;
	vkEnumerateDeviceExtensionProperties( device, NULL, &count, NULL );
	VkExtensionProperties *extensions = (VkExtensionProperties *)R_Malloc( ( count ? count : 1 ) * sizeof( VkExtensionProperties ), TAG_TEMP_WORKSPACE, qfalse );
	vkEnumerateDeviceExtensionProperties( device, NULL, &count, extensions );
	bool found = true;
	for ( size_t i = 0; i < ARRAY_LEN( rtExtensions ); i++ )
	{
		found = found && VK_RT_HasExtension( extensions, count, rtExtensions[i] );
	}
	R_Free( extensions );
	if ( !found )
	{
		return false;
	}

	VkPhysicalDeviceRayQueryFeaturesKHR rayQuery = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
	VkPhysicalDeviceAccelerationStructureFeaturesKHR structure = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
	VkPhysicalDeviceVulkan12Features features12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
	VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	structure.pNext = &rayQuery;
	features12.pNext = &structure;
	features.pNext = &features12;
	vkGetPhysicalDeviceFeatures2( device, &features );
	return features12.bufferDeviceAddress && structure.accelerationStructure && rayQuery.rayQuery;
}

/*
=================
VK_RT_AddToDevice

Adds what ray tracing needs to the creation of the device: the extensions (to extensions, count updated) and the
features (a chain for the pNext of VkPhysicalDeviceFeatures2, kept in rtFeatures until the device is created).
=================
*/
static struct {
	VkPhysicalDeviceVulkan12Features				features12;
	VkPhysicalDeviceAccelerationStructureFeaturesKHR	structure;
	VkPhysicalDeviceRayQueryFeaturesKHR				rayQuery;
} rtFeatures;

void *VK_RT_AddToDevice( const char **extensions, uint32_t *count )
{
	for ( size_t i = 0; i < ARRAY_LEN( rtExtensions ); i++ )
	{
		extensions[( *count )++] = rtExtensions[i];
	}
	memset( &rtFeatures, 0, sizeof( rtFeatures ) );
	rtFeatures.features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	rtFeatures.features12.bufferDeviceAddress = VK_TRUE;
	rtFeatures.features12.pNext = &rtFeatures.structure;
	rtFeatures.structure.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
	rtFeatures.structure.accelerationStructure = VK_TRUE;
	rtFeatures.structure.pNext = &rtFeatures.rayQuery;
	rtFeatures.rayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
	rtFeatures.rayQuery.rayQuery = VK_TRUE;
	return &rtFeatures.features12;
}

/*
=============================================================================

BUFFERS: each its own allocation (there are few), with a device address

=============================================================================
*/

bool VK_RT_NewBuffer( vkRtBuffer_t *buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible )
{
	memset( buffer, 0, sizeof( *buffer ) );

	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	info.size = size;
	info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if ( vkCreateBuffer( vk.device, &info, NULL, &buffer->buffer ) != VK_SUCCESS )
	{
		buffer->buffer = VK_NULL_HANDLE;
		return false;
	}

	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements( vk.device, buffer->buffer, &requirements );
	VkMemoryAllocateFlagsInfo flags = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
	flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocate.pNext = &flags;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = VK_FindMemoryType( requirements.memoryTypeBits, hostVisible
		? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
	if ( vkAllocateMemory( vk.device, &allocate, NULL, &buffer->memory ) != VK_SUCCESS )
	{
		vkDestroyBuffer( vk.device, buffer->buffer, NULL );
		memset( buffer, 0, sizeof( *buffer ) );
		return false;
	}
	VK_CheckResult( vkBindBufferMemory( vk.device, buffer->buffer, buffer->memory, 0 ), "vkBindBufferMemory" );
	if ( hostVisible )
	{
		VK_CheckResult( vkMapMemory( vk.device, buffer->memory, 0, size, 0, &buffer->mapped ), "vkMapMemory" );
	}

	VkBufferDeviceAddressInfo address = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
	address.buffer = buffer->buffer;
	buffer->address = qvkGetBufferDeviceAddress( vk.device, &address );
	buffer->size = size;
	return true;
}

void VK_RT_FreeBuffer( vkRtBuffer_t *buffer )
{
	if ( buffer->buffer )
	{
		if ( buffer->mapped )
		{
			vkUnmapMemory( vk.device, buffer->memory );
		}
		vkDestroyBuffer( vk.device, buffer->buffer, NULL );
		vkFreeMemory( vk.device, buffer->memory, NULL );
	}
	memset( buffer, 0, sizeof( *buffer ) );
}

/*
What is replaced by a larger one may still be used: by builds recorded before into the command buffer that is not sent
yet (a level's first frame builds hundreds of structures with one scratch buffer, which grows on the way), or by the
frame before, still on the GPU. Freeing it at once had those builds write into freed memory, and a ray through the
structure they made could hang the GPU ("device lost"). It is kept until it is surely not used: two frames on (a frame
waits for the one two before it as it starts), or when the device is idle.
*/
typedef struct {
	vkRtStructure_t	structure;
	vkRtBuffer_t	buffer;
	int				frame;		// vk.frameCounter when it was replaced
} rtRetired_t;

static std::vector<rtRetired_t> rtRetired;

static void VK_RT_RetireBuffer( vkRtBuffer_t *buffer )
{
	if ( buffer->buffer )
	{
		rtRetired_t retired = {};
		retired.buffer = *buffer;
		retired.frame = vk.frameCounter;
		rtRetired.push_back( retired );
	}
	memset( buffer, 0, sizeof( *buffer ) );
}

static void VK_RT_RetireStructure( vkRtStructure_t *structure )
{
	if ( structure->handle || structure->storage.buffer )
	{
		rtRetired_t retired = {};
		retired.structure = *structure;
		retired.frame = vk.frameCounter;
		rtRetired.push_back( retired );
	}
	memset( structure, 0, sizeof( *structure ) );
}

// all: the device is idle
static void VK_RT_FreeRetired( bool all )
{
	for ( size_t i = 0; i < rtRetired.size(); )
	{
		if ( all || rtRetired[i].frame <= vk.frameCounter - 2 )
		{
			VK_RT_FreeStructure( &rtRetired[i].structure );
			VK_RT_FreeBuffer( &rtRetired[i].buffer );
			rtRetired[i] = rtRetired.back();
			rtRetired.pop_back();
		}
		else
		{
			i++;
		}
	}
}

// a scratch buffer of at least size, with room to align its address
static bool VK_RT_EnsureScratch( vkRtBuffer_t *scratch, VkDeviceSize size )
{
	if ( scratch->buffer && scratch->size >= size + scratchAlignment )
	{
		return true;
	}
	VK_RT_RetireBuffer( scratch );
	return VK_RT_NewBuffer( scratch, size + scratchAlignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false );
}

static VkDeviceAddress VK_RT_ScratchAddress( const vkRtBuffer_t *scratch )
{
	return ( scratch->address + scratchAlignment - 1 ) & ~( scratchAlignment - 1 );
}

/*
=============================================================================

ACCELERATION STRUCTURES

=============================================================================
*/

void VK_RT_FreeStructure( vkRtStructure_t *structure )
{
	if ( structure->handle )
	{
		qvkDestroyAccelerationStructureKHR( vk.device, structure->handle, NULL );
	}
	VK_RT_FreeBuffer( &structure->storage );
	memset( structure, 0, sizeof( *structure ) );
}

/*
=================
VK_RT_Build

Records the build of structure (made, or made again larger, when it has to be: it must not be in use then) from
geometry into cmd, with scratch (grown when needed: not in use either). Bottom level: geometry holds triangles;
top level: instances.
=================
*/
static bool VK_RT_Build( VkCommandBuffer cmd, vkRtStructure_t *structure, vkRtBuffer_t *scratch, VkAccelerationStructureTypeKHR type,
	const VkAccelerationStructureGeometryKHR *geometry, uint32_t primitives, VkBuildAccelerationStructureFlagsKHR flags )
{
	VkAccelerationStructureBuildGeometryInfoKHR build = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	build.type = type;
	build.flags = flags;
	build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	build.geometryCount = 1;
	build.pGeometries = geometry;

	VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	qvkGetAccelerationStructureBuildSizesKHR( vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &primitives, &sizes );

	if ( !structure->handle || structure->storage.size < sizes.accelerationStructureSize )
	{
		VK_RT_RetireStructure( structure );
		if ( !VK_RT_NewBuffer( &structure->storage, sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false ) )
		{
			return false;
		}
		VkAccelerationStructureCreateInfoKHR create = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
		create.buffer = structure->storage.buffer;
		create.size = sizes.accelerationStructureSize;
		create.type = type;
		if ( qvkCreateAccelerationStructureKHR( vk.device, &create, NULL, &structure->handle ) != VK_SUCCESS )
		{
			structure->handle = VK_NULL_HANDLE;
			VK_RT_FreeStructure( structure );
			return false;
		}
		VkAccelerationStructureDeviceAddressInfoKHR address = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
		address.accelerationStructure = structure->handle;
		structure->address = qvkGetAccelerationStructureDeviceAddressKHR( vk.device, &address );
	}
	if ( !VK_RT_EnsureScratch( scratch, sizes.buildScratchSize ) )
	{
		return false;
	}

	build.dstAccelerationStructure = structure->handle;
	build.scratchData.deviceAddress = VK_RT_ScratchAddress( scratch );
	VkAccelerationStructureBuildRangeInfoKHR range = {};
	range.primitiveCount = primitives;
	const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
	qvkCmdBuildAccelerationStructuresKHR( cmd, 1, &build, &ranges );
	return true;
}

/*
=================
VK_RT_BuildTriangles

A bottom level structure from triangles: vertices (3 floats each) and indexes (3 per triangle, 32 bit) at device
addresses. fastBuild for what is built again every frame, otherwise it is made to be searched fast. Not opaque: ray
queries hand its triangles to the shader as candidates, to be confirmed or not (the alpha test, VK_RT_Alpha*).
=================
*/
bool VK_RT_BuildTriangles( VkCommandBuffer cmd, vkRtStructure_t *blas, vkRtBuffer_t *scratch, VkDeviceAddress vertices,
	uint32_t numVertices, VkDeviceAddress indexes, uint32_t numTriangles, bool fastBuild, bool opaque )
{
	VkAccelerationStructureGeometryKHR geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geometry.flags = opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;	// (not opaque: the shader is asked, alpha test)
	VkAccelerationStructureGeometryTrianglesDataKHR &triangles = geometry.geometry.triangles;
	triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	triangles.vertexData.deviceAddress = vertices;
	triangles.vertexStride = 3 * sizeof( float );
	triangles.maxVertex = numVertices ? numVertices - 1 : 0;
	triangles.indexType = VK_INDEX_TYPE_UINT32;
	triangles.indexData.deviceAddress = indexes;
	return VK_RT_Build( cmd, blas, scratch, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &geometry, numTriangles,
		fastBuild ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR );
}

// a fast built bottom level structure of this many triangles fits in blas as it is (it is not made again)
bool VK_RT_TrianglesFit( const vkRtStructure_t *blas, uint32_t numVertices, uint32_t numTriangles )
{
	VkAccelerationStructureGeometryKHR geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
	geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	geometry.geometry.triangles.vertexStride = 3 * sizeof( float );
	geometry.geometry.triangles.maxVertex = numVertices ? numVertices - 1 : 0;
	geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
	VkAccelerationStructureBuildGeometryInfoKHR build = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
	build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
	build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	build.geometryCount = 1;
	build.pGeometries = &geometry;
	VkAccelerationStructureBuildSizesInfoKHR sizes = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
	qvkGetAccelerationStructureBuildSizesKHR( vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &numTriangles, &sizes );
	return blas->handle && blas->storage.size >= sizes.accelerationStructureSize;
}

/*
=================
VK_RT_BuildInstances

A top level structure from instances (VkAccelerationStructureInstanceKHR, at a device address), built every frame.
=================
*/
bool VK_RT_BuildInstances( VkCommandBuffer cmd, vkRtStructure_t *tlas, vkRtBuffer_t *scratch, VkDeviceAddress instances,
	uint32_t numInstances )
{
	VkAccelerationStructureGeometryKHR geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
	geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	geometry.geometry.instances.data.deviceAddress = instances;
	return VK_RT_Build( cmd, tlas, scratch, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &geometry, numInstances,
		VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR );
}

/*
=================
VK_RT_Barrier

What the builds recorded before it wrote is there for what comes after: further builds, or shaders of stage.
=================
*/
void VK_RT_Barrier( VkCommandBuffer cmd, VkPipelineStageFlags stage )
{
	VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	if ( stage == VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR )
	{
		barrier.dstAccessMask |= VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;	// (a build after it may use the same scratch)
	}
	vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, stage, 0, 1, &barrier, 0, NULL, 0, NULL );
}

/*
=============================================================================

THE SCENE: the level and its brush models (doors, lifts, ...), each in a bottom level structure made the first time a
frame needs it and kept until the level changes, placed every frame by a top level structure of its own frame

=============================================================================
*/

// What a surface's shader makes of it for the rays: nothing (sky, glass, moving vertices), solid, or solid where its
// texture is (grates, foliage, catwalks: cut out by an alpha test, or see-through ones blended by their alpha, which
// count where the alpha is at least a half; test is the RT_ALPHA_* the shaders know, scale the stage's tcMod scale)
enum { RT_NONE, RT_SOLID, RT_ALPHA_TESTED };
enum { RT_ALPHA_GT_0 = 1, RT_ALPHA_LT_80, RT_ALPHA_GE_80, RT_ALPHA_GE_C0 };

static int VK_RT_Blocks( const shader_t *shader, int *test, float scale[2] )
{

	if ( !shader || shader->sky || ( shader->surfaceFlags & ( SURF_SKY | SURF_NODRAW ) ) || shader->fogParms )
	{
		return RT_NONE;
	}
	if ( ( shader->sort != SS_OPAQUE && shader->sort != SS_SEE_THROUGH ) || shader->numDeforms || !shader->numUnfoggedPasses || !shader->stages )
	{
		return RT_NONE;
	}
	const shaderStage_t *stage = &shader->stages[0];
	switch ( stage->stateBits & GLS_ATEST_BITS )
	{
	case 0:
		if ( shader->sort == SS_OPAQUE )
		{
			return RT_SOLID;
		}
		if ( ( stage->stateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) != GLS_ALPHA )
		{
			return RT_NONE;
		}
		*test = RT_ALPHA_GE_80;	// (see-through, blended by its alpha)
		break;
	case GLS_ATEST_GT_0:	*test = RT_ALPHA_GT_0;	break;
	case GLS_ATEST_LT_80:	*test = RT_ALPHA_LT_80;	break;
	case GLS_ATEST_GE_80:	*test = RT_ALPHA_GE_80;	break;
	case GLS_ATEST_GE_C0:	*test = RT_ALPHA_GE_C0;	break;
	default:
		return RT_NONE;
	}
	// (the alpha comes from the stage's texture, as it is laid on the surface)
	const textureBundle_t *bundle = &stage->bundle[0];
	if ( !bundle->image || bundle->isLightmap || bundle->tcGen != TCGEN_TEXTURE || bundle->isVideoMap )
	{
		return RT_NONE;
	}
	scale[0] = scale[1] = 1.0f;
	for ( int i = 0; i < bundle->numTexMods; i++ )
	{
		if ( bundle->texMods[i].type != TMOD_SCALE )
		{
			return RT_NONE;	// (moving texture coordinates)
		}
		scale[0] *= bundle->texMods[i].translate[0];	// (where tcMod scale keeps them)
		scale[1] *= bundle->texMods[i].translate[1];
	}
	return RT_ALPHA_TESTED;
}

typedef struct {
	const bmodel_t	*bmodel;
	vkRtStructure_t	blas;			// (no handle: nothing in it blocks light)
	vkRtBuffer_t	input;			// its vertices and indexes, kept while it may still be being built
	int				triangles;
	vkRtStructure_t	alphaBlas;		// the alpha tested triangles, not opaque
	vkRtBuffer_t	alphaInput;
	int				alphaTriangles;
	int				alphaFirst;		// where their texture coordinates are in rtAlpha.triangles (the instance's custom index)
} rtModel_t;

static struct {
	const world_t			*world;			// the level the models are of
	std::vector<rtModel_t>	models;
	vkRtBuffer_t			scratch;		// for the bottom level builds
	struct {
		vkRtStructure_t	tlas;
		vkRtBuffer_t	instances, scratch;
		int				frame;				// vk.frameCounter + 1 it was built for
		int				count;				// the instances in it
		vkRtStructure_t	characters;			// the characters of the frame (VK_RT_Characters), in a top level structure
		vkRtStructure_t	charactersTop;		// of their own (the level's is read by draws before they are known)
		vkRtBuffer_t	charactersInput, charactersScratch, charactersTopScratch;
		int				charactersBuilt;	// vk.frameCounter + 1 of the frame they were made in (the next frame's level
											// structure places them: the dynamic lights' rays are blocked by characters)
		VkDeviceSize	charactersInputUsed;	// how much of charactersInput the views of the frame took (each its own part)
		int				charactersInputFrame;	// vk.frameCounter + 1 of that frame
	} frames[VK_FRAMES_IN_FLIGHT];
} rtScene;

#define RT_MAX_INSTANCES	1024
#define RT_MASK_WORLD		0x01			// the instance masks: the level, its brush models, the characters
#define RT_MASK_BMODEL		0x02
#define RT_MASK_CHARACTERS	0x04

/*
The alpha tested surfaces: the alpha of each texture is in a tile of an atlas, and for each triangle its texture
coordinates and its material (which tile, which test) are in a storage buffer, as rt_alpha.glsl reads them. Made with
all the level's brush models the first time a frame of the level needs them (VK_RT_PrepareLevel).
*/
#define RT_ALPHA_TILE		128
#define RT_ALPHA_ATLAS		2048
#define RT_ALPHA_MATERIALS	( ( RT_ALPHA_ATLAS / RT_ALPHA_TILE ) * ( RT_ALPHA_ATLAS / RT_ALPHA_TILE ) )	// (256, as in rt_alpha.glsl)
#define RT_ALPHA_ATLAS_ID	0x7ffe0010u

typedef struct {
	float		uv01[4];
	float		uv2[2];
	uint32_t	material, pad;
} rtAlphaTriangle_t;

typedef struct {
	float		rect[4];		// the tile in the atlas: corner, size
	uint32_t	test[4];		// RT_ALPHA_*
} rtAlphaMaterial_t;

static struct {
	std::vector<const image_t *>		images;			// the materials: the texture, the test
	std::vector<int>					tests;
	std::vector<rtAlphaTriangle_t>		triangles;
	bool								collecting;		// the level's models are being made: alpha tested surfaces go in
	bool								ready;			// the atlas and the buffer are those of the level
	vkRtBuffer_t						data;			// the materials (RT_ALPHA_MATERIALS), then the triangles
	bool								atlasMade;
} rtAlpha;

static uint32_t VK_RT_AlphaMaterial( const image_t *image, int test )
{
	for ( size_t i = 0; i < rtAlpha.images.size(); i++ )
	{
		if ( rtAlpha.images[i] == image && rtAlpha.tests[i] == test )
		{
			return (uint32_t)i;
		}
	}
	if ( rtAlpha.images.size() >= RT_ALPHA_MATERIALS )
	{
		return 0xFFFFFFFFu;
	}
	rtAlpha.images.push_back( image );
	rtAlpha.tests.push_back( test );
	return (uint32_t)( rtAlpha.images.size() - 1 );
}

static void VK_RT_AddTriangle( std::vector<float> &xyz, std::vector<uint32_t> &indexes, const float *a, const float *b, const float *c )
{
	const uint32_t first = (uint32_t)( xyz.size() / 3 );
	xyz.insert( xyz.end(), a, a + 3 );
	xyz.insert( xyz.end(), b, b + 3 );
	xyz.insert( xyz.end(), c, c + 3 );
	indexes.push_back( first );
	indexes.push_back( first + 1 );
	indexes.push_back( first + 2 );
}

// where the triangles of a surface go: the solid ones, or the alpha tested ones (with their texture coordinates)
typedef struct {
	std::vector<float>		xyz;
	std::vector<uint32_t>	indexes;
} rtGeometry_t;

static void VK_RT_AddTriangleUV( rtGeometry_t *geometry, uint32_t material, const float *scale, const float *a, const float *b,
	const float *c, const float *sa, const float *sb, const float *sc )
{
	VK_RT_AddTriangle( geometry->xyz, geometry->indexes, a, b, c );
	if ( material != 0xFFFFFFFFu )
	{
		rtAlphaTriangle_t t = { { sa[0] * scale[0], sa[1] * scale[1], sb[0] * scale[0], sb[1] * scale[1] },
			{ sc[0] * scale[0], sc[1] * scale[1] }, material, 0 };
		rtAlpha.triangles.push_back( t );
	}
}

// the triangles of the surfaces of bmodel that block light, in its own space: solid, and alpha tested (while the level's
// models are made, VK_RT_PrepareLevel; otherwise they are left out)
static void VK_RT_Collect( const bmodel_t *bmodel, rtGeometry_t *solid, rtGeometry_t *alpha )
{
	for ( int s = 0; s < bmodel->numSurfaces; s++ )
	{
		const msurface_t *surf = bmodel->firstSurface + s;
		int test = 0;
		float scale[2] = { 1.0f, 1.0f };
		const int blocks = surf->data ? VK_RT_Blocks( surf->shader, &test, scale ) : RT_NONE;
		if ( blocks == RT_NONE || ( blocks == RT_ALPHA_TESTED && !rtAlpha.collecting ) )
		{
			continue;
		}
		uint32_t material = 0xFFFFFFFFu;
		if ( blocks == RT_ALPHA_TESTED )
		{
			material = VK_RT_AlphaMaterial( surf->shader->stages[0].bundle[0].image, test );
			if ( material == 0xFFFFFFFFu )
			{
				continue;	// (no tile left in the atlas)
			}
		}
		rtGeometry_t *to = ( blocks == RT_ALPHA_TESTED ) ? alpha : solid;
		switch ( *surf->data )
		{
		case SF_FACE:
		{
			const srfSurfaceFace_t *face = (const srfSurfaceFace_t *)surf->data;
			const int *idx = (const int *)( (const byte *)face + face->ofsIndices );
			for ( int i = 0; i + 2 < face->numIndices; i += 3 )
			{
				const float *a = face->points[idx[i]], *b = face->points[idx[i + 1]], *c = face->points[idx[i + 2]];
				VK_RT_AddTriangleUV( to, material, scale, a, b, c, a + 3, b + 3, c + 3 );
			}
			break;
		}
		case SF_TRIANGLES:
		{
			const srfTriangles_t *tris = (const srfTriangles_t *)surf->data;
			for ( int i = 0; i + 2 < tris->numIndexes; i += 3 )
			{
				const drawVert_t *a = &tris->verts[tris->indexes[i]], *b = &tris->verts[tris->indexes[i + 1]], *c = &tris->verts[tris->indexes[i + 2]];
				VK_RT_AddTriangleUV( to, material, scale, a->xyz, b->xyz, c->xyz, a->st, b->st, c->st );
			}
			break;
		}
		case SF_GRID:
		{
			const srfGridMesh_t *grid = (const srfGridMesh_t *)surf->data;
			for ( int y = 0; y + 1 < grid->height; y++ )
			{
				for ( int x = 0; x + 1 < grid->width; x++ )
				{
					const drawVert_t *a = &grid->verts[y * grid->width + x], *b = &grid->verts[y * grid->width + x + 1];
					const drawVert_t *c = &grid->verts[( y + 1 ) * grid->width + x], *d = &grid->verts[( y + 1 ) * grid->width + x + 1];
					VK_RT_AddTriangleUV( to, material, scale, a->xyz, c->xyz, b->xyz, a->st, c->st, b->st );
					VK_RT_AddTriangleUV( to, material, scale, b->xyz, c->xyz, d->xyz, b->st, c->st, d->st );
				}
			}
			break;
		}
		default:
			break;
		}
	}
}

// a bottom level structure of geometry into structure (input: where its vertices and indexes are kept)
static int VK_RT_BuildGeometry( VkCommandBuffer cmd, const rtGeometry_t *geometry, vkRtStructure_t *structure, vkRtBuffer_t *input,
	bool opaque )
{
	const int triangles = (int)( geometry->indexes.size() / 3 );
	if ( !triangles )
	{
		return 0;
	}
	const VkDeviceSize vertexBytes = ( geometry->xyz.size() * sizeof( float ) + 15 ) & ~(VkDeviceSize)15;
	const VkDeviceSize bytes = vertexBytes + geometry->indexes.size() * sizeof( uint32_t );
	if ( !VK_RT_NewBuffer( input, bytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true ) )
	{
		return 0;
	}
	memcpy( input->mapped, geometry->xyz.data(), geometry->xyz.size() * sizeof( float ) );
	memcpy( (byte *)input->mapped + vertexBytes, geometry->indexes.data(), geometry->indexes.size() * sizeof( uint32_t ) );
	VK_RT_Barrier( cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR );	// (the scratch may be in use)
	if ( !VK_RT_BuildTriangles( cmd, structure, &rtScene.scratch, input->address, (uint32_t)( geometry->xyz.size() / 3 ),
			input->address + vertexBytes, (uint32_t)triangles, false, opaque ) )
	{
		VK_RT_FreeStructure( structure );
		return 0;
	}
	return triangles;
}

// the model of bmodel, its bottom level structures recorded into cmd if it is new
static const rtModel_t *VK_RT_Model( VkCommandBuffer cmd, const bmodel_t *bmodel )
{
	for ( size_t i = 0; i < rtScene.models.size(); i++ )
	{
		if ( rtScene.models[i].bmodel == bmodel )
		{
			return &rtScene.models[i];
		}
	}

	rtModel_t model = {};
	model.bmodel = bmodel;
	model.alphaFirst = (int)rtAlpha.triangles.size();
	rtGeometry_t solid, alpha;
	VK_RT_Collect( bmodel, &solid, &alpha );
	model.triangles = VK_RT_BuildGeometry( cmd, &solid, &model.blas, &model.input, true );
	model.alphaTriangles = VK_RT_BuildGeometry( cmd, &alpha, &model.alphaBlas, &model.alphaInput, false );
	rtScene.models.push_back( model );
	return &rtScene.models.back();
}

// the alpha of material i's texture, made small into its tile of atlas (RGBA, the alpha in every channel)
static void VK_RT_AlphaTile( byte *atlas, int i )
{
	const int tilesAcross = RT_ALPHA_ATLAS / RT_ALPHA_TILE;
	const int x0 = ( i % tilesAcross ) * RT_ALPHA_TILE, y0 = ( i / tilesAcross ) * RT_ALPHA_TILE;
	byte *pic = NULL;
	int width = 0, height = 0;

	R_LoadImage( rtAlpha.images[i]->imgName, &pic, &width, &height );
	for ( int y = 0; y < RT_ALPHA_TILE; y++ )
	{
		for ( int x = 0; x < RT_ALPHA_TILE; x++ )
		{
			int alpha = 255;	// (no picture: solid)
			if ( pic && width > 0 && height > 0 )
			{
				// the average of the texels the atlas texel covers
				const int sx0 = x * width / RT_ALPHA_TILE, sx1 = Q_max( ( x + 1 ) * width / RT_ALPHA_TILE, sx0 + 1 );
				const int sy0 = y * height / RT_ALPHA_TILE, sy1 = Q_max( ( y + 1 ) * height / RT_ALPHA_TILE, sy0 + 1 );
				int sum = 0;
				for ( int sy = sy0; sy < sy1; sy++ )
				{
					for ( int sx = sx0; sx < sx1; sx++ )
					{
						sum += pic[( sy * width + sx ) * 4 + 3];
					}
				}
				alpha = sum / ( ( sy1 - sy0 ) * ( sx1 - sx0 ) );
			}
			byte *dst = atlas + ( ( y0 + y ) * RT_ALPHA_ATLAS + x0 + x ) * 4;
			dst[0] = dst[1] = dst[2] = dst[3] = (byte)alpha;
		}
	}
	if ( pic )
	{
		R_Free( pic );
	}
}

// the atlas and the buffer of the alpha tested surfaces as they are now (the device is idle)
static void VK_RT_UploadAlpha( void )
{
	const size_t materials = rtAlpha.images.size();
	const VkDeviceSize bytes = RT_ALPHA_MATERIALS * sizeof( rtAlphaMaterial_t ) + Q_max( rtAlpha.triangles.size(), (size_t)1 ) * sizeof( rtAlphaTriangle_t );

	VK_RT_FreeBuffer( &rtAlpha.data );
	if ( !VK_RT_NewBuffer( &rtAlpha.data, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true ) )
	{
		return;
	}
	memset( rtAlpha.data.mapped, 0, (size_t)bytes );
	rtAlphaMaterial_t *material = (rtAlphaMaterial_t *)rtAlpha.data.mapped;
	const int tilesAcross = RT_ALPHA_ATLAS / RT_ALPHA_TILE;
	for ( size_t i = 0; i < materials; i++ )
	{
		// (half a texel in from the edges, so that the filter does not reach the next tile)
		material[i].rect[0] = ( ( i % tilesAcross ) * RT_ALPHA_TILE + 0.5f ) / RT_ALPHA_ATLAS;
		material[i].rect[1] = ( ( i / tilesAcross ) * RT_ALPHA_TILE + 0.5f ) / RT_ALPHA_ATLAS;
		material[i].rect[2] = material[i].rect[3] = ( RT_ALPHA_TILE - 1.0f ) / RT_ALPHA_ATLAS;
		material[i].test[0] = (uint32_t)rtAlpha.tests[i];
	}
	if ( !rtAlpha.triangles.empty() )
	{
		memcpy( material + RT_ALPHA_MATERIALS, rtAlpha.triangles.data(), rtAlpha.triangles.size() * sizeof( rtAlphaTriangle_t ) );
	}

	if ( materials )
	{
		byte *atlas = (byte *)R_Malloc( RT_ALPHA_ATLAS * RT_ALPHA_ATLAS * 4, TAG_TEMP_WORKSPACE, qtrue );
		for ( size_t i = 0; i < materials; i++ )
		{
			VK_RT_AlphaTile( atlas, (int)i );
		}
		VK_TexImage2D( RT_ALPHA_ATLAS_ID, 0, 4, RT_ALPHA_ATLAS, RT_ALPHA_ATLAS, GL_RGBA, GL_UNSIGNED_BYTE, atlas );
		R_Free( atlas );
	}
	else
	{
		const byte white[4] = { 255, 255, 255, 255 };
		VK_TexImage2D( RT_ALPHA_ATLAS_ID, 0, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, white );
	}
	VK_TexParameter( RT_ALPHA_ATLAS_ID, GL_TEXTURE_MIN_FILTER, (GLfloat)GL_LINEAR );
	VK_TexParameter( RT_ALPHA_ATLAS_ID, GL_TEXTURE_MAG_FILTER, (GLfloat)GL_LINEAR );
	VK_TexParameter( RT_ALPHA_ATLAS_ID, GL_TEXTURE_WRAP_S, (GLfloat)GL_CLAMP_TO_EDGE );
	VK_TexParameter( RT_ALPHA_ATLAS_ID, GL_TEXTURE_WRAP_T, (GLfloat)GL_CLAMP_TO_EDGE );
	VK_FlushUploads( true );
	rtAlpha.atlasMade = true;
}

/*
=================
VK_RT_AlphaInputs

What the shaders with ray queries read for the alpha test (rt_alpha.glsl): the atlas and the buffer. There always are
some (empty ones before a level).
=================
*/
void VK_RT_AlphaInputs( VkImageView *atlas, VkSampler *sampler, VkBuffer *data )
{
	if ( !rtAlpha.atlasMade || !rtAlpha.data.buffer )
	{
		VK_RT_UploadAlpha();
	}
	const vkTexture_t *texture = VK_TextureForId( RT_ALPHA_ATLAS_ID, false );
	*atlas = texture->view;
	*sampler = VK_SamplerFor( texture );
	*data = rtAlpha.data.buffer;
}

// all the brush models of the level with their alpha tested surfaces, then the atlas and the buffer for them
static void VK_RT_PrepareLevel( VkCommandBuffer cmd )
{
	const double start = VK_Now();
	rtAlpha.images.clear();
	rtAlpha.tests.clear();
	rtAlpha.triangles.clear();
	rtAlpha.collecting = true;
	int solid = 0, alpha = 0;
	for ( int i = 0; i < tr.numModels; i++ )
	{
		const model_t *model = tr.models[i];
		if ( model && model->type == MOD_BRUSH && model->bmodel && !model->bspInstance )
		{
			const rtModel_t *rt = VK_RT_Model( cmd, model->bmodel );
			solid += rt->triangles;
			alpha += rt->alphaTriangles;
		}
	}
	rtAlpha.collecting = false;

	// (the shaders of the frames before may still read the old buffer and atlas)
	VK_SubmitFrame( true );
	VK_RT_UploadAlpha();
	rtAlpha.ready = true;
	ri.Printf( PRINT_DEVELOPER, "ray tracing: %d models, %d triangles that block light, %d of them alpha tested (%d textures), made in %.1f ms\n",
		(int)rtScene.models.size(), solid + alpha, alpha, (int)rtAlpha.images.size(), VK_Now() - start );
}

static void VK_RT_FreeScene( void )	// (the device is idle)
{
	VK_RT_FreeRetired( true );
	for ( size_t i = 0; i < rtScene.models.size(); i++ )
	{
		VK_RT_FreeStructure( &rtScene.models[i].blas );
		VK_RT_FreeBuffer( &rtScene.models[i].input );
		VK_RT_FreeStructure( &rtScene.models[i].alphaBlas );
		VK_RT_FreeBuffer( &rtScene.models[i].alphaInput );
	}
	rtScene.models.clear();
	rtAlpha.ready = false;	// (the atlas and the buffer stay until the next level's replace them)
	VK_RT_FreeBuffer( &rtScene.scratch );
	for ( int i = 0; i < VK_FRAMES_IN_FLIGHT; i++ )
	{
		VK_RT_FreeStructure( &rtScene.frames[i].tlas );
		VK_RT_FreeBuffer( &rtScene.frames[i].instances );
		VK_RT_FreeBuffer( &rtScene.frames[i].scratch );
		rtScene.frames[i].frame = 0;
		VK_RT_FreeStructure( &rtScene.frames[i].characters );
		VK_RT_FreeStructure( &rtScene.frames[i].charactersTop );
		VK_RT_FreeBuffer( &rtScene.frames[i].charactersInput );
		VK_RT_FreeBuffer( &rtScene.frames[i].charactersScratch );
		VK_RT_FreeBuffer( &rtScene.frames[i].charactersTopScratch );
	}
	rtScene.world = NULL;
}

/*
=================
VK_RT_NewLevel

A level was loaded: the structures of the last one go (their surfaces are gone).
=================
*/
void VK_RT_NewLevel( void )
{
	if ( !vk.rayTracing || ( !rtScene.world && rtScene.models.empty() ) )
	{
		return;
	}
	vkDeviceWaitIdle( vk.device );
	VK_RT_FreeScene();
}

static void VK_RT_Place( VkAccelerationStructureInstanceKHR *instance, const vkRtStructure_t *blas, uint32_t customIndex,
	const float axis[3][3], const float *origin, uint32_t mask )
{
	memset( instance, 0, sizeof( *instance ) );
	for ( int r = 0; r < 3; r++ )
	{
		for ( int c = 0; c < 3; c++ )
		{
			instance->transform.matrix[r][c] = axis ? axis[c][r] : ( r == c ? 1.0f : 0.0f );
		}
		instance->transform.matrix[r][3] = origin ? origin[r] : 0.0f;
	}
	instance->instanceCustomIndex = customIndex;
	instance->mask = mask;
	instance->flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
	instance->accelerationStructureReference = blas->address;
}

/*
=================
VK_RT_Scene

The top level structure of this frame: the level, and the brush models among the entities of the view being drawn,
where they are now. Built by the first call of a frame (it records into vk.cmd, outside a render pass), VK_NULL_HANDLE
when there is no ray tracing or no level. Shaders that use it come after a VK_RT_Barrier for their stage, which this
does for fragment shaders.
=================
*/
VkAccelerationStructureKHR VK_RT_Scene( void )
{
	if ( !vk.rayTracing || !tr.world || ( backEnd.refdef.rdflags & RDF_NOWORLDMODEL ) )
	{
		return VK_NULL_HANDLE;
	}
	if ( rtScene.world != tr.world )
	{
		if ( rtScene.world || !rtScene.models.empty() )
		{
			vkDeviceWaitIdle( vk.device );
			VK_RT_FreeScene();
		}
		rtScene.world = tr.world;
	}

	VK_RT_FreeRetired( false );

	auto &frame = rtScene.frames[vk.frameIndex];
	if ( frame.frame == vk.frameCounter + 1 && frame.tlas.handle )
	{
		return frame.tlas.handle;
	}
	if ( !frame.instances.buffer &&
		 !VK_RT_NewBuffer( &frame.instances, RT_MAX_INSTANCES * sizeof( VkAccelerationStructureInstanceKHR ),
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true ) )
	{
		return VK_NULL_HANDLE;
	}

	if ( !rtAlpha.ready )
	{
		VK_RT_PrepareLevel( vk.cmd );	// (sends what was recorded: vk.cmd is another one after it)
	}

	const double start = VK_Now();
	const size_t modelsBefore = rtScene.models.size();
	VkCommandBuffer cmd = vk.cmd;
	VkAccelerationStructureInstanceKHR *instances = (VkAccelerationStructureInstanceKHR *)frame.instances.mapped;
	uint32_t count = 0;

	const rtModel_t *world = VK_RT_Model( cmd, &tr.world->bmodels[0] );
	if ( world->blas.handle )
	{
		VK_RT_Place( &instances[count++], &world->blas, 0, NULL, NULL, RT_MASK_WORLD );
	}
	if ( world->alphaBlas.handle )
	{
		VK_RT_Place( &instances[count++], &world->alphaBlas, (uint32_t)world->alphaFirst, NULL, NULL, RT_MASK_WORLD );
	}
	// the characters as they were the frame before (they are known only after the dynamic lights are drawn)
	const auto &previous = rtScene.frames[( vk.frameIndex + VK_FRAMES_IN_FLIGHT - 1 ) % VK_FRAMES_IN_FLIGHT];
	if ( previous.characters.handle && previous.charactersBuilt == vk.frameCounter )
	{
		VK_RT_Place( &instances[count++], &previous.characters, 0, NULL, NULL, RT_MASK_CHARACTERS );
	}
	for ( int i = 0; i < backEnd.refdef.num_entities && count + 1 < RT_MAX_INSTANCES; i++ )
	{
		const trRefEntity_t *ent = &backEnd.refdef.entities[i];
		if ( ent->e.reType != RT_MODEL || ( ent->e.renderfx & RF_DEPTHHACK ) )
		{
			continue;
		}
		const model_t *model = R_GetModelByHandle( ent->e.hModel );
		if ( !model || model->type != MOD_BRUSH || !model->bmodel || model->bmodel == &tr.world->bmodels[0] )
		{
			continue;
		}
		const rtModel_t *rt = VK_RT_Model( cmd, model->bmodel );
		if ( rt->blas.handle )
		{
			VK_RT_Place( &instances[count++], &rt->blas, 0, ent->e.axis, ent->e.origin, RT_MASK_BMODEL );
		}
		if ( rt->alphaBlas.handle )
		{
			VK_RT_Place( &instances[count++], &rt->alphaBlas, (uint32_t)rt->alphaFirst, ent->e.axis, ent->e.origin, RT_MASK_BMODEL );
		}
	}
	if ( !count )
	{
		return VK_NULL_HANDLE;
	}

	frame.count = (int)count;
	VK_RT_Barrier( cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR );
	if ( !VK_RT_BuildInstances( cmd, &frame.tlas, &frame.scratch, frame.instances.address, count ) )
	{
		return VK_NULL_HANDLE;
	}
	VK_RT_Barrier( cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT );
	frame.frame = vk.frameCounter + 1;

	if ( rtScene.models.size() != modelsBefore )
	{
		int triangles = 0;
		for ( size_t i = modelsBefore; i < rtScene.models.size(); i++ )
		{
			triangles += rtScene.models[i].triangles;
		}
		ri.Printf( PRINT_DEVELOPER, "ray tracing: %d models, %d triangles that block light, made in %.1f ms\n",
			(int)( rtScene.models.size() - modelsBefore ), triangles, VK_Now() - start );
	}
	return frame.tlas.handle;
}

/*
=================
VK_RT_Characters

The triangles of the characters this frame (xyz: 3 floats a point, world space; indexes: 3 a triangle), in a top level
structure of their own (instance mask 0x04), built fast in vk.cmd (outside a render pass). Not in the level's: that
one is read by the dynamic lights while the view is drawn, before the characters are known (and the lights, often
in a character's hand, would be blocked by it). VK_NULL_HANDLE when there are none.
=================
*/
VkAccelerationStructureKHR VK_RT_Characters( const float *xyz, int numPoints, const int *indexes, int numTriangles )
{
	if ( !vk.rayTracing || numPoints < 3 || numTriangles < 1 )
	{
		return VK_NULL_HANDLE;
	}
	// a triangle with a corner that is not among the points would have the build read past them (a GPU page fault),
	// and a structure made of it, or of points that are no numbers or far outside any level, could hang the rays that
	// go through it
	for ( int i = 0; i < numPoints * 3; i++ )
	{
		if ( !( xyz[i] > -262144.0f && xyz[i] < 262144.0f ) )	// (false for NaN too)
		{
			ri.Printf( PRINT_DEVELOPER, S_COLOR_YELLOW "ray tracing: a character's point is at %f, not traced\n", xyz[i] );
			return VK_NULL_HANDLE;
		}
	}
	for ( int i = 0; i < numTriangles * 3; i++ )
	{
		if ( (unsigned)indexes[i] >= (unsigned)numPoints )
		{
			ri.Printf( PRINT_DEVELOPER, S_COLOR_YELLOW "ray tracing: a character's triangle has a corner %d of %d points, not traced\n", indexes[i], numPoints );
			return VK_NULL_HANDLE;
		}
	}

	// Each view of the frame (a sky portal, a mirror, a camera and the view itself) has its own part of the input: the
	// builds recorded for the views before are sent with the frame, and read theirs then. (One part for all had the
	// next view's points and triangles written over the ones of a build not sent yet, which read past its points.)
	auto &frame = rtScene.frames[vk.frameIndex];
	if ( frame.charactersInputFrame != vk.frameCounter + 1 )
	{
		frame.charactersInputFrame = vk.frameCounter + 1;
		frame.charactersInputUsed = 0;
	}
	const VkDeviceSize vertexBytes = ( (VkDeviceSize)numPoints * 3 * sizeof( float ) + 15 ) & ~(VkDeviceSize)15;
	const VkDeviceSize bytes = vertexBytes + (VkDeviceSize)numTriangles * 3 * sizeof( uint32_t );
	const VkDeviceSize instanceOffset = ( bytes + 15 ) & ~(VkDeviceSize)15;
	const VkDeviceSize partBytes = ( instanceOffset + ( RT_MAX_INSTANCES + 1 ) * sizeof( VkAccelerationStructureInstanceKHR ) + 255 ) & ~(VkDeviceSize)255;
	if ( frame.charactersInput.size < frame.charactersInputUsed + partBytes )
	{
		// (the builds recorded with the one there was still read it: retired, it stays until they are done)
		VK_RT_RetireBuffer( &frame.charactersInput );
		if ( !VK_RT_NewBuffer( &frame.charactersInput, 2 * partBytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true ) )
		{
			frame.charactersInputUsed = 0;
			return VK_NULL_HANDLE;
		}
		frame.charactersInputUsed = 0;
	}
	const VkDeviceSize partOffset = frame.charactersInputUsed;
	frame.charactersInputUsed += partBytes;
	byte *part = (byte *)frame.charactersInput.mapped + partOffset;
	const VkDeviceAddress partAddress = frame.charactersInput.address + partOffset;
	memcpy( part, xyz, (size_t)numPoints * 3 * sizeof( float ) );
	memcpy( part + vertexBytes, indexes, (size_t)numTriangles * 3 * sizeof( uint32_t ) );

	// When the structure has to be made again larger, the frame before (in flight) may still read the old one, through
	// the level's structure of its dynamic lights: what was recorded is sent, and all of it waited for, first.
	if ( frame.characters.handle && !VK_RT_TrianglesFit( &frame.characters, (uint32_t)numPoints, (uint32_t)numTriangles ) )
	{
		VK_SubmitFrame( true );
	}

	// (built again in the same frame for another view, they may have been read by shaders already)
	VkCommandBuffer cmd = vk.cmd;
	VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0, NULL, 0, NULL );
	if ( !VK_RT_BuildTriangles( cmd, &frame.characters, &frame.charactersScratch, partAddress, (uint32_t)numPoints,
			partAddress + vertexBytes, (uint32_t)numTriangles, true, true ) )
	{
		return VK_NULL_HANDLE;
	}

	// the instances, after the indexes: the level's of the frame (its surfaces and brush models, but not the characters
	// of the frame before), as the shadows' rays also look whether the level is between (below the floor under a
	// character: post_rt_shadow.frag), and the characters
	VkAccelerationStructureInstanceKHR *list = (VkAccelerationStructureInstanceKHR *)( part + instanceOffset );
	uint32_t numInstances = 0;
	if ( frame.frame == vk.frameCounter + 1 && frame.instances.mapped )
	{
		const VkAccelerationStructureInstanceKHR *level = (const VkAccelerationStructureInstanceKHR *)frame.instances.mapped;
		for ( int i = 0; i < frame.count && numInstances < RT_MAX_INSTANCES; i++ )
		{
			if ( level[i].mask != RT_MASK_CHARACTERS )
			{
				list[numInstances++] = level[i];
			}
		}
	}
	VK_RT_Place( &list[numInstances++], &frame.characters, 0, NULL, NULL, RT_MASK_CHARACTERS );	// (in world space already)
	VK_RT_Barrier( cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR );
	if ( !VK_RT_BuildInstances( cmd, &frame.charactersTop, &frame.charactersTopScratch, partAddress + instanceOffset, numInstances ) )
	{
		return VK_NULL_HANDLE;
	}
	VK_RT_Barrier( cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT );
	frame.charactersBuilt = vk.frameCounter + 1;
	return frame.charactersTop.handle;
}

/*
=============================================================================

START AND END

=============================================================================
*/

// one triangle in a bottom level structure, placed by a top level one, built and waited for: the driver can do it
static bool VK_RT_SelfTest( void )
{
	vkRtBuffer_t	input = {}, scratch = {};
	vkRtStructure_t	blas = {}, tlas = {};
	VkCommandPool	pool = VK_NULL_HANDLE;
	bool			ok = false;

	if ( VK_RT_NewBuffer( &input, 256, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true ) )
	{
		// the triangle (bytes 0-35), its indexes (48-59), the instance (64-127)
		const float xyz[9] = { 0, 0, 0, 64, 0, 0, 0, 64, 0 };
		const uint32_t indexes[3] = { 0, 1, 2 };
		memcpy( (byte *)input.mapped, xyz, sizeof( xyz ) );
		memcpy( (byte *)input.mapped + 48, indexes, sizeof( indexes ) );

		VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		poolInfo.queueFamilyIndex = vk.queueFamily;
		VK_CheckResult( vkCreateCommandPool( vk.device, &poolInfo, NULL, &pool ), "vkCreateCommandPool" );
		VkCommandBufferAllocateInfo allocate = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		allocate.commandPool = pool;
		allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = 1;
		VkCommandBuffer cmd;
		VK_CheckResult( vkAllocateCommandBuffers( vk.device, &allocate, &cmd ), "vkAllocateCommandBuffers" );
		VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_CheckResult( vkBeginCommandBuffer( cmd, &begin ), "vkBeginCommandBuffer" );

		ok = VK_RT_BuildTriangles( cmd, &blas, &scratch, input.address, 3, input.address + 48, 1, false, true );
		if ( ok )
		{
			VkAccelerationStructureInstanceKHR instance = {};
			instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1.0f;
			instance.mask = 0xFF;
			instance.accelerationStructureReference = blas.address;
			memcpy( (byte *)input.mapped + 64, &instance, sizeof( instance ) );
			VK_RT_Barrier( cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR );
			// (the top level build uses its own scratch: the one of the bottom level may still be written)
			vkRtBuffer_t tlasScratch = {};
			ok = VK_RT_BuildInstances( cmd, &tlas, &tlasScratch, input.address + 64, 1 );
			VK_CheckResult( vkEndCommandBuffer( cmd ), "vkEndCommandBuffer" );
			if ( ok )
			{
				VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
				submit.commandBufferCount = 1;
				submit.pCommandBuffers = &cmd;
				ok = vkQueueSubmit( vk.queue, 1, &submit, VK_NULL_HANDLE ) == VK_SUCCESS && vkQueueWaitIdle( vk.queue ) == VK_SUCCESS;
			}
			ok = ok && blas.address && tlas.address;
			VK_RT_FreeBuffer( &tlasScratch );
		}
		else
		{
			VK_CheckResult( vkEndCommandBuffer( cmd ), "vkEndCommandBuffer" );
		}
		vkDestroyCommandPool( vk.device, pool, NULL );
	}

	VK_RT_FreeStructure( &tlas );
	VK_RT_FreeStructure( &blas );
	VK_RT_FreeBuffer( &scratch );
	VK_RT_FreeBuffer( &input );
	return ok;
}

/*
=================
VK_RT_Init

After the device is made with VK_RT_AddToDevice (vk.rayTracing true): the entry points, and the test.
=================
*/
void VK_RT_Init( void )
{
	ri.Cvar_Get( "r_rtAvail", "0", 0 );
	ri.Cvar_Set( "r_rtAvail", "0" );
	if ( !vk.rayTracing )
	{
		return;
	}

	qvkGetAccelerationStructureBuildSizesKHR = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr( vk.device, "vkGetAccelerationStructureBuildSizesKHR" );
	qvkCreateAccelerationStructureKHR = (PFN_vkCreateAccelerationStructureKHR)vkGetDeviceProcAddr( vk.device, "vkCreateAccelerationStructureKHR" );
	qvkDestroyAccelerationStructureKHR = (PFN_vkDestroyAccelerationStructureKHR)vkGetDeviceProcAddr( vk.device, "vkDestroyAccelerationStructureKHR" );
	qvkCmdBuildAccelerationStructuresKHR = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr( vk.device, "vkCmdBuildAccelerationStructuresKHR" );
	qvkGetAccelerationStructureDeviceAddressKHR = (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr( vk.device, "vkGetAccelerationStructureDeviceAddressKHR" );
	qvkGetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr( vk.device, "vkGetBufferDeviceAddress" );
	if ( !qvkGetAccelerationStructureBuildSizesKHR || !qvkCreateAccelerationStructureKHR || !qvkDestroyAccelerationStructureKHR ||
		 !qvkCmdBuildAccelerationStructuresKHR || !qvkGetAccelerationStructureDeviceAddressKHR || !qvkGetBufferDeviceAddress )
	{
		Com_Printf( "...ray tracing: an entry point is missing, not used\n" );
		vk.rayTracing = false;
		return;
	}

	VkPhysicalDeviceAccelerationStructurePropertiesKHR structure = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR };
	VkPhysicalDeviceProperties2 properties = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
	properties.pNext = &structure;
	vkGetPhysicalDeviceProperties2( vk.physicalDevice, &properties );
	scratchAlignment = Q_max( (VkDeviceSize)structure.minAccelerationStructureScratchOffsetAlignment, (VkDeviceSize)1 );

	if ( !VK_RT_SelfTest() )
	{
		Com_Printf( "...ray tracing: building a test structure failed, not used\n" );
		vk.rayTracing = false;
		return;
	}
	ri.Cvar_Set( "r_rtAvail", "1" );
	Com_Printf( "...using ray tracing (VK_KHR_ray_query) for shadows where asked\n" );
}

void VK_RT_Shutdown( void )
{
	VK_RT_FreeScene();	// (the device is idle)
	VK_RT_FreeBuffer( &rtAlpha.data );
	rtAlpha.atlasMade = false;	// (the atlas is a texture, it goes with the others)
}
