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
bool VK_Init( glconfig_t *glConfig );
void VK_Shutdown( void );

// A level was loaded: the ray tracing structures of the last one go (vk_rt.cpp).
void VK_RT_NewLevel( void );

// Before a view is drawn: the ray tracing structure the dynamic lights' rays use (vk_post.cpp).
void VK_RT_BeginView( void );

// Whether nothing is in front of a flare (key: the same for the same flare each frame), from the depth at its pixel (x, y
// from the bottom left) two frames before, read without waiting for the GPU (vk_frame.cpp). eyeZ and the projection's
// [10], [11], [14] are those of this frame's view, kept to judge the depth with when it comes.
bool VK_FlareVisible( const void *key, int x, int y, float eyeZ, float proj10, float proj11, float proj14 );
