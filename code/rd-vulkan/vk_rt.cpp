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

// a scratch buffer of at least size, with room to align its address
static bool VK_RT_EnsureScratch( vkRtBuffer_t *scratch, VkDeviceSize size )
{
	if ( scratch->buffer && scratch->size >= size + scratchAlignment )
	{
		return true;
	}
	VK_RT_FreeBuffer( scratch );
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
		VK_RT_FreeStructure( structure );
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
addresses. fastBuild for what is built again every frame, otherwise it is made to be searched fast.
=================
*/
bool VK_RT_BuildTriangles( VkCommandBuffer cmd, vkRtStructure_t *blas, vkRtBuffer_t *scratch, VkDeviceAddress vertices,
	uint32_t numVertices, VkDeviceAddress indexes, uint32_t numTriangles, bool fastBuild )
{
	VkAccelerationStructureGeometryKHR geometry = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
	geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
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

// what blocks light: solid surfaces that are drawn as they are (no sky, nothing see-through or cut out by an alpha
// test, nothing that moves its vertices)
static bool VK_RT_Blocks( const shader_t *shader )
{
	if ( !shader || shader->sky || ( shader->surfaceFlags & ( SURF_SKY | SURF_NODRAW ) ) || shader->fogParms )
	{
		return false;
	}
	if ( shader->sort != SS_OPAQUE || shader->numDeforms || !shader->numUnfoggedPasses || !shader->stages )
	{
		return false;
	}
	return ( shader->stages[0].stateBits & GLS_ATEST_BITS ) == 0;
}

typedef struct {
	const bmodel_t	*bmodel;
	vkRtStructure_t	blas;			// (no handle: nothing in it blocks light)
	vkRtBuffer_t	input;			// its vertices and indexes, kept while it may still be being built
	int				triangles;
} rtModel_t;

static struct {
	const world_t			*world;			// the level the models are of
	std::vector<rtModel_t>	models;
	vkRtBuffer_t			scratch;		// for the bottom level builds
	struct {
		vkRtStructure_t	tlas;
		vkRtBuffer_t	instances, scratch;
		int				frame;				// vk.frameCounter + 1 it was built for
		vkRtStructure_t	characters;			// the characters of the frame (VK_RT_SetCharacters)
		vkRtBuffer_t	charactersInput, charactersScratch;
		int				charactersFrame;	// vk.frameCounter + 1 they were built for
	} frames[VK_FRAMES_IN_FLIGHT];
} rtScene;

#define RT_MAX_INSTANCES	1024
#define RT_MASK_WORLD		0x01			// the instance masks: the level, its brush models, the characters
#define RT_MASK_BMODEL		0x02
#define RT_MASK_CHARACTERS	0x04

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

// the triangles of the surfaces of bmodel that block light, in its own space
static void VK_RT_Collect( const bmodel_t *bmodel, std::vector<float> &xyz, std::vector<uint32_t> &indexes )
{
	for ( int s = 0; s < bmodel->numSurfaces; s++ )
	{
		const msurface_t *surf = bmodel->firstSurface + s;
		if ( !VK_RT_Blocks( surf->shader ) || !surf->data )
		{
			continue;
		}
		switch ( *surf->data )
		{
		case SF_FACE:
		{
			const srfSurfaceFace_t *face = (const srfSurfaceFace_t *)surf->data;
			const int *idx = (const int *)( (const byte *)face + face->ofsIndices );
			for ( int i = 0; i + 2 < face->numIndices; i += 3 )
			{
				VK_RT_AddTriangle( xyz, indexes, face->points[idx[i]], face->points[idx[i + 1]], face->points[idx[i + 2]] );
			}
			break;
		}
		case SF_TRIANGLES:
		{
			const srfTriangles_t *tris = (const srfTriangles_t *)surf->data;
			for ( int i = 0; i + 2 < tris->numIndexes; i += 3 )
			{
				VK_RT_AddTriangle( xyz, indexes, tris->verts[tris->indexes[i]].xyz, tris->verts[tris->indexes[i + 1]].xyz,
					tris->verts[tris->indexes[i + 2]].xyz );
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
					const float *a = grid->verts[y * grid->width + x].xyz, *b = grid->verts[y * grid->width + x + 1].xyz;
					const float *c = grid->verts[( y + 1 ) * grid->width + x].xyz, *d = grid->verts[( y + 1 ) * grid->width + x + 1].xyz;
					VK_RT_AddTriangle( xyz, indexes, a, c, b );
					VK_RT_AddTriangle( xyz, indexes, b, c, d );
				}
			}
			break;
		}
		default:
			break;
		}
	}
}

// the model of bmodel, its bottom level structure recorded into cmd if it is new (NULL: there is no room)
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
	std::vector<float> xyz;
	std::vector<uint32_t> indexes;
	VK_RT_Collect( bmodel, xyz, indexes );
	model.triangles = (int)( indexes.size() / 3 );
	if ( model.triangles )
	{
		const VkDeviceSize vertexBytes = ( xyz.size() * sizeof( float ) + 15 ) & ~(VkDeviceSize)15;
		const VkDeviceSize bytes = vertexBytes + indexes.size() * sizeof( uint32_t );
		if ( VK_RT_NewBuffer( &model.input, bytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true ) )
		{
			memcpy( model.input.mapped, xyz.data(), xyz.size() * sizeof( float ) );
			memcpy( (byte *)model.input.mapped + vertexBytes, indexes.data(), indexes.size() * sizeof( uint32_t ) );
			VK_RT_Barrier( cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR );	// (the scratch may be in use)
			if ( !VK_RT_BuildTriangles( cmd, &model.blas, &rtScene.scratch, model.input.address, (uint32_t)( xyz.size() / 3 ),
					model.input.address + vertexBytes, (uint32_t)model.triangles, false ) )
			{
				VK_RT_FreeStructure( &model.blas );
			}
		}
	}
	rtScene.models.push_back( model );
	return &rtScene.models.back();
}

static void VK_RT_FreeScene( void )
{
	for ( size_t i = 0; i < rtScene.models.size(); i++ )
	{
		VK_RT_FreeStructure( &rtScene.models[i].blas );
		VK_RT_FreeBuffer( &rtScene.models[i].input );
	}
	rtScene.models.clear();
	VK_RT_FreeBuffer( &rtScene.scratch );
	for ( int i = 0; i < VK_FRAMES_IN_FLIGHT; i++ )
	{
		VK_RT_FreeStructure( &rtScene.frames[i].tlas );
		VK_RT_FreeBuffer( &rtScene.frames[i].instances );
		VK_RT_FreeBuffer( &rtScene.frames[i].scratch );
		rtScene.frames[i].frame = 0;
		VK_RT_FreeStructure( &rtScene.frames[i].characters );
		VK_RT_FreeBuffer( &rtScene.frames[i].charactersInput );
		VK_RT_FreeBuffer( &rtScene.frames[i].charactersScratch );
		rtScene.frames[i].charactersFrame = 0;
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

static void VK_RT_Place( VkAccelerationStructureInstanceKHR *instance, const rtModel_t *model, const float axis[3][3],
	const float *origin, uint32_t mask )
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
	instance->mask = mask;
	instance->flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
	instance->accelerationStructureReference = model->blas.address;
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

	const double start = VK_Now();
	const size_t modelsBefore = rtScene.models.size();
	VkCommandBuffer cmd = vk.cmd;
	VkAccelerationStructureInstanceKHR *instances = (VkAccelerationStructureInstanceKHR *)frame.instances.mapped;
	uint32_t count = 0;

	const rtModel_t *world = VK_RT_Model( cmd, &tr.world->bmodels[0] );
	if ( world->blas.handle )
	{
		VK_RT_Place( &instances[count++], world, NULL, NULL, RT_MASK_WORLD );
	}
	if ( frame.charactersFrame == vk.frameCounter + 1 && frame.characters.handle )
	{
		rtModel_t characters = {};
		characters.blas = frame.characters;
		VK_RT_Place( &instances[count++], &characters, NULL, NULL, RT_MASK_CHARACTERS );	// (in world space already)
	}
	for ( int i = 0; i < backEnd.refdef.num_entities && count < RT_MAX_INSTANCES; i++ )
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
			VK_RT_Place( &instances[count++], rt, ent->e.axis, ent->e.origin, RT_MASK_BMODEL );
		}
	}
	if ( !count )
	{
		return VK_NULL_HANDLE;
	}

	// (built again in the same frame, it may have been read by shaders already)
	VkMemoryBarrier barrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
	barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0, NULL, 0, NULL );
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
VK_RT_SetCharacters

The triangles of the characters this frame (xyz: 3 floats a point, world space; indexes: 3 a triangle), into a
bottom level structure built fast in vk.cmd (outside a render pass); the next VK_RT_Scene of the frame places it.
=================
*/
bool VK_RT_SetCharacters( const float *xyz, int numPoints, const int *indexes, int numTriangles )
{
	if ( !vk.rayTracing || numPoints < 3 || numTriangles < 1 )
	{
		return false;
	}
	auto &frame = rtScene.frames[vk.frameIndex];
	const VkDeviceSize vertexBytes = ( (VkDeviceSize)numPoints * 3 * sizeof( float ) + 15 ) & ~(VkDeviceSize)15;
	const VkDeviceSize bytes = vertexBytes + (VkDeviceSize)numTriangles * 3 * sizeof( uint32_t );
	if ( frame.charactersInput.size < bytes )
	{
		VK_RT_FreeBuffer( &frame.charactersInput );
		if ( !VK_RT_NewBuffer( &frame.charactersInput, bytes + bytes / 2, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, true ) )
		{
			return false;
		}
	}
	memcpy( frame.charactersInput.mapped, xyz, (size_t)numPoints * 3 * sizeof( float ) );
	memcpy( (byte *)frame.charactersInput.mapped + vertexBytes, indexes, (size_t)numTriangles * 3 * sizeof( uint32_t ) );

	VkCommandBuffer cmd = vk.cmd;
	VK_RT_Barrier( cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR );
	if ( !VK_RT_BuildTriangles( cmd, &frame.characters, &frame.charactersScratch, frame.charactersInput.address, (uint32_t)numPoints,
			frame.charactersInput.address + vertexBytes, (uint32_t)numTriangles, true ) )
	{
		return false;
	}
	frame.charactersFrame = vk.frameCounter + 1;
	frame.frame = 0;	// (the top level structure of the frame is made again with them)
	return true;
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

		ok = VK_RT_BuildTriangles( cmd, &blas, &scratch, input.address, 3, input.address + 48, 1, false );
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
}
