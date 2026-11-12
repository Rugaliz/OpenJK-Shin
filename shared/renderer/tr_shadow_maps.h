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

// The shadows made from the characters' own triangles, which use the stencil buffer: the volumes (cg_shadows 2), the
// shadow maps (4) and the ray traced ones (5, the Vulkan renderer where the graphics card has ray tracing)
#define R_MODEL_SHADOWS( shadows )	( ( shadows ) == 2 || ( shadows ) == 4 || ( shadows ) == 5 )

#define SHADOWMAP_TILE			128		// texels on a side of the depth tile of one caster
#define SHADOWMAP_TILES			4		// tiles on a side of the atlas
#define SHADOWMAP_ATLAS			( SHADOWMAP_TILE * SHADOWMAP_TILES )
#define SHADOWMAP_MAX_CASTERS	( SHADOWMAP_TILES * SHADOWMAP_TILES )
#define SHADOWMAP_MAX_RT		( SHADOWMAP_MAX_CASTERS * 6 )	// ray traced: entries for all of them (up to 4 lamps, a contact shadow each)

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
	float	light[4];		// ray traced: the direction to the light (world), and how wide the rays spread around it (a tangent);
					// for a lamp its position and radius, for a contact shadow the middle of the caster and how far its rays go
	float	area[4];		// ray traced: where the caster can be in (s, t) of rowS and rowT: lowest s and t, highest s and t
	int		rtType;			// ray traced: SHADOW_RT_DIRECTION, SHADOW_RT_LAMP, SHADOW_RT_CONTACT
	float	reach;			// ray traced: the contact shadow, how far from light it can be; a lamp, how far the caster is from it
} shadowMapCaster_t;

#define SHADOW_RT_DIRECTION		0	// from the light grid's direction
#define SHADOW_RT_LAMP			1	// from a lamp of the level (light entity)
#define SHADOW_RT_CONTACT		2	// all around, close to the caster

// the lamps of the level (its "light" entities), read as it loads: the ray traced shadows are cast from them, and the
// single player renderers light the characters with them (r_lampLighting)
void		R_ShadowLampsLoad( const char *entities, int length );

#ifdef _JK2EXE
#define LAMPS_AT_MAX	4

// The lamps that light a point most and that it can see (the server's traces, so single player only), with how much
// light each gave it in the map compiler's units, colour included (kept for the 16 units cube the point is in)
typedef struct {
	int		count;
	int		lamps[LAMPS_AT_MAX];
	float	weights[LAMPS_AT_MAX];
} lampChoice_t;

const lampChoice_t	*R_LampsAt( const vec3_t point );
// where a lamp is, its colour (the brightest component 1), the luminance of that colour and its light style
void				R_LampInfo( int lamp, vec3_t origin, vec3_t color, float *luminance, int *style );
#endif

// The renderer can draw them (it has the shaders); r_shadowMapsAvail tells the menus
qboolean	R_ShadowMaps_Available( void );

// Darkens the 3D view with the casters (the atlas: SHADOWMAP_ATLAS square, RGBA, the depth in red). Pixels whose
// stencil has the bit stencilSkip set (the characters) are left alone when stencilSkip is not 0.
void		RB_ShadowMapsApply( const shadowMapCaster_t *casters, int numCasters, const byte *atlas, int stencilSkip );

// Ray traced (r_rayTracing, Setup > More Video > Ray Tracing, makes the shadows of cg_shadows 2 and 4 ray traced): the
// renderer can (Vulkan with ray tracing, r_rtAvail), what is asked of it (0 off, 1 medium, 2 high), and darkening the
// 3D view by the entries (several for a caster, see rtType) with rays against the triangles of all casters (xyz: 3
// floats a point, world space; indexes: 3 a triangle)
qboolean	R_RayTracedShadows_Available( void );
int			R_RayTracing( void );
qboolean	R_DlightShadows( void );		// with them, dynamic lights do not go through walls (the shader dynamic lights' rays)
void		RB_RayTracedShadowsApply( const shadowMapCaster_t *casters, int numCasters, const float *xyz, int numPoints,
				const int *indexes, int numTriangles, int stencilSkip );
