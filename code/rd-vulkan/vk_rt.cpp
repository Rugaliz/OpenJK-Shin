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
	vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, stage, 0, 1, &barrier, 0, NULL, 0, NULL );
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
	// (nothing kept yet: the structures of the level and the characters come with the shadows that use them)
}
