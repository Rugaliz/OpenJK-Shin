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

// vk_renderer.h -- the renderer this Vulkan layer is built into: the single player one (code/rd-vanilla) or, with
// RD_VULKAN_MP, the multiplayer one (codemp/rd-vanilla, found by the include path of its build).

#pragma once

#ifdef RD_VULKAN_MP
#include "tr_local.h"
#include "game/surfaceflags.h"
#else
#include "../server/exe_headers.h"
#include "../rd-vanilla/tr_local.h"
#include "../game/surfaceflags.h"
#endif

// what the layer calls that the two renderers name differently
#ifdef RD_VULKAN_MP
#define VK_CvarGet( name, value, flags )	ri.Cvar_Get( name, value, flags, "" )
static inline void *R_Malloc( int size, memtag_t tag, qboolean zero = qfalse ) { return Z_Malloc( size, tag, zero ); }
static inline void R_Free( void *ptr ) { Z_Free( ptr ); }
#else
#define VK_CvarGet( name, value, flags )	ri.Cvar_Get( name, value, flags )
#endif
