/*
===========================================================================
Copyright (C) 2013 - 2015, OpenJK contributors

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

// tr_shadow_maps.h -- what the shadow maps of characters (cg_shadows 4, see tr_shadow_volumes.h) hand to the renderer
// that darkens the picture with them: RB_ShadowMapsApply in tr_post.h (OpenGL) and vk_post.cpp (Vulkan). Included by
// the tr_local.h of each renderer.

#pragma once

// The shadows made from the characters' own triangles, which use the stencil buffer: the volumes (cg_shadows 2) and
// the shadow maps (4)
#define R_MODEL_SHADOWS( shadows )	( ( shadows ) == 2 || ( shadows ) == 4 )

#define SHADOWMAP_TILE			128		// texels on a side of the depth tile of one caster
#define SHADOWMAP_TILES			4		// tiles on a side of the atlas
#define SHADOWMAP_ATLAS			( SHADOWMAP_TILE * SHADOWMAP_TILES )
#define SHADOWMAP_MAX_CASTERS	( SHADOWMAP_TILES * SHADOWMAP_TILES )

// One caster. Each row maps a position in the view's eye space (x, y, z, 1) to: s and t in the atlas, the height
// towards the light normalized to the caster's own range (0 the lowest point of the caster, 1 the highest; the tile
// stores 1 + that * 254, 0 where there is no caster), and the world z (for the floor test).
typedef struct {
	float	rowS[4], rowT[4], rowN[4], rowZ[4];
	float	floorZ;			// nothing below this is darkened (the floor under the caster, a little lower)
	float	darkness;		// 0 to 1
	float	bias;			// in normalized height: how far below the caster a point has to be to be in its shadow
	float	softness;		// atlas texels of penumbra per normalized height between the caster and the point
	float	tile[2];		// the corner of the caster's tile in the atlas (s, t): nothing outside it is the caster's
	float	rect[4];		// where on the view its shadow can be: left, bottom, right, top, 0 to 1 from the bottom left
} shadowMapCaster_t;

// The renderer can draw them (it has the shaders); r_shadowMapsAvail tells the menus
qboolean	R_ShadowMaps_Available( void );

// Darkens the 3D view with the casters (the atlas: SHADOWMAP_ATLAS square, RGBA, the depth in red). Pixels whose
// stencil has the bit stencilSkip set (the characters) are left alone when stencilSkip is not 0.
void		RB_ShadowMapsApply( const shadowMapCaster_t *casters, int numCasters, const byte *atlas, int stencilSkip );
