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

// The Vulkan renderer module (docs/vulkan-renderer.md). This header is seen by all of the renderer sources; it must
// not include the Vulkan headers (those stay in the vk_*.cpp files).

#pragma once

// Creates the Vulkan instance, device and swapchain for the window made by ri.WIN_Init and fills in the parts of
// glConfig the renderer reads (strings, limits, which optional features exist).
void VK_Init( glconfig_t *glConfig );
void VK_Shutdown( void );
