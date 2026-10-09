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

// Device memory for the Vulkan renderer module: big blocks that textures are placed in, so that the number of
// allocations stays far below the limit of the driver (4096 on some). First fit with free ranges that merge again.

#include "../server/exe_headers.h"
#include "../rd-vanilla/tr_local.h"
#include "vk_priv.h"
#include <vector>

#define VK_BLOCK_SIZE	( 128 * 1024 * 1024 )

typedef struct vkRange_s {
	VkDeviceSize offset, size;
} vkRange_t;

typedef struct vkBlock_s {
	VkDeviceMemory		memory;
	VkDeviceSize		size;
	uint32_t			type;
	std::vector<vkRange_t>	free;	// sorted by offset
} vkBlock_t;

static std::vector<vkBlock_t *>	blocks;

static VkDeviceSize AlignUp( VkDeviceSize value, VkDeviceSize alignment )
{
	return ( value + alignment - 1 ) / alignment * alignment;
}

/*
=================
VK_AllocateMemory

Memory of the given requirements (device local unless asked otherwise); false when the device is out of it.
=================
*/
bool VK_AllocateMemory( const VkMemoryRequirements *requirements, VkMemoryPropertyFlags properties, vkAlloc_t *alloc )
{
	const uint32_t type = VK_FindMemoryType( requirements->memoryTypeBits, properties );
	// resources of different kinds in a block must not share a "granularity" page
	const VkDeviceSize granularity = vk.properties.limits.bufferImageGranularity;
	const VkDeviceSize alignment = requirements->alignment > granularity ? requirements->alignment : granularity;
	const VkDeviceSize size = AlignUp( requirements->size, granularity );

	for ( int b = 0; b < (int)blocks.size(); b++ )
	{
		vkBlock_t *block = blocks[b];
		if ( !block || block->type != type )
		{
			continue;
		}
		for ( size_t r = 0; r < block->free.size(); r++ )
		{
			vkRange_t &range = block->free[r];
			const VkDeviceSize start = AlignUp( range.offset, alignment );
			if ( start + size > range.offset + range.size )
			{
				continue;
			}
			// carve [start, start+size) out of the range
			const VkDeviceSize rangeEnd = range.offset + range.size;
			const VkDeviceSize before = start - range.offset;
			const VkDeviceSize after = rangeEnd - ( start + size );
			if ( before && after )
			{
				vkRange_t tail = { start + size, after };
				range.size = before;
				block->free.insert( block->free.begin() + r + 1, tail );
			}
			else if ( before )
			{
				range.size = before;
			}
			else if ( after )
			{
				range.offset = start + size;
				range.size = after;
			}
			else
			{
				block->free.erase( block->free.begin() + r );
			}
			alloc->memory = block->memory;
			alloc->offset = start;
			alloc->size = size;
			alloc->block = b;
			return true;
		}
	}

	// a new block (one of its own for something large)
	vkBlock_t *block = new vkBlock_t;
	block->size = size > VK_BLOCK_SIZE ? size : VK_BLOCK_SIZE;
	block->type = type;
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocate.allocationSize = block->size;
	allocate.memoryTypeIndex = type;
	if ( vkAllocateMemory( vk.device, &allocate, NULL, &block->memory ) != VK_SUCCESS )
	{
		// maybe the big block is too much, try one just big enough
		block->size = size;
		allocate.allocationSize = size;
		if ( vkAllocateMemory( vk.device, &allocate, NULL, &block->memory ) != VK_SUCCESS )
		{
			delete block;
			return false;
		}
	}
	int index = -1;
	for ( int b = 0; b < (int)blocks.size(); b++ )
	{
		if ( !blocks[b] )
		{
			index = b;
			break;
		}
	}
	if ( index < 0 )
	{
		index = (int)blocks.size();
		blocks.push_back( NULL );
	}
	blocks[index] = block;
	if ( block->size > size )
	{
		vkRange_t rest = { size, block->size - size };
		block->free.push_back( rest );
	}
	alloc->memory = block->memory;
	alloc->offset = 0;
	alloc->size = size;
	alloc->block = index;
	return true;
}

void VK_FreeMemory( vkAlloc_t *alloc )
{
	if ( !alloc->memory )
	{
		return;
	}
	vkBlock_t *block = blocks[alloc->block];
	vkRange_t freed = { alloc->offset, alloc->size };
	size_t i = 0;
	while ( i < block->free.size() && block->free[i].offset < freed.offset )
	{
		i++;
	}
	block->free.insert( block->free.begin() + i, freed );
	// merge with the neighbours
	if ( i + 1 < block->free.size() && block->free[i].offset + block->free[i].size == block->free[i + 1].offset )
	{
		block->free[i].size += block->free[i + 1].size;
		block->free.erase( block->free.begin() + i + 1 );
	}
	if ( i > 0 && block->free[i - 1].offset + block->free[i - 1].size == block->free[i].offset )
	{
		block->free[i - 1].size += block->free[i].size;
		block->free.erase( block->free.begin() + i );
	}
	memset( alloc, 0, sizeof( *alloc ) );
}

void VK_ShutdownMemory( void )
{
	for ( size_t b = 0; b < blocks.size(); b++ )
	{
		if ( blocks[b] )
		{
			vkFreeMemory( vk.device, blocks[b]->memory, NULL );
			delete blocks[b];
		}
	}
	blocks.clear();
}
