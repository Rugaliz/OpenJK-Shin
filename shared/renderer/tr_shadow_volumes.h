/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
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

// tr_shadow_volumes.h -- the stencil shadow volumes (cg_shadows 2) of the single player and multiplayer renderers.
//
// Included once, by the tr_shadows.cpp of each renderer, after its tr_local.h (which has tess, backEnd, the GL state
// functions and the r_shadow* cvars). R_SHADOWS_IN_ONE_DRAWCALL tells whether glStencilOpSeparate is there (the two
// renderers keep that flag in different structs). See docs/shadows.md.

#pragma once

#include <vector>
#include <algorithm>
#include <string>
#include <unordered_map>

#ifndef R_SHADOWS_IN_ONE_DRAWCALL
#error "define R_SHADOWS_IN_ONE_DRAWCALL before including tr_shadow_volumes.h"
#endif

/*
Stencil shadow volumes (cg_shadows 2)

Every shadow casting surface only records its geometry here (RB_ShadowTessEnd).
RB_ShadowFinish then renders all the volumes in one go, so that they can be
replayed several times with a slightly different light direction to get soft
edges, and so that the characters themselves can be left out of the darkening.

The volume of a surface is the sweep of its light facing triangles from where
they are down to the shadow plane of the entity.  It is built from a closed
chain of triangles: the light facing triangles (front cap), the same triangles
moved to the end of the sweep (back cap) and one wall for every edge the facing
triangles do not share with each other.  Vertices are welded by position first,
which makes models whose vertices are split along texture seams behave like the
closed meshes they really are, and the walls come from a signed edge count, so
it also copes with edges shared by more than two triangles and open meshes.
The volumes are drawn "Carmack reverse" style (z fail), so they need no
clipping against the near plane.

The low seven stencil bits count the volumes, bit 7 marks pixels that show a
shadow casting character.
*/


typedef struct {
	float	xyz[3];			// point on the caster, world space
	float	ext[3];			// from xyz to the end of the sweep
	float	h;				// length of the sweep, per unit of height
} shadowPt_t;

static std::vector<shadowPt_t>	shPts;
static std::vector<int>			shTris;		// all triangles of all casters, 3 point indices each
static std::vector<int>			shRtTris;	// ray traced: the same, and those of the casters only there for their shadow
static std::vector<int>			shCaps;		// the triangles facing the light
static std::vector<int>			shWalls;	// directed edges, 2 point indices each
static float					shStrengthSum;
static int						shCasters;

// For the shadow maps (cg_shadows 4): every recorded surface, with the entity it belongs to
typedef struct {
	const trRefEntity_t	*ent;
	int					capsStart, capsEnd;	// its triangles in shCaps
	vec3_t				lightAxis;			// towards the light (tilted as for the volumes)
	float				shadowPlane;
	float				strength;			// share of the light that comes from one direction
} shadowSurf_t;

static std::vector<shadowSurf_t>	shSurfs;

// a caster whose shadow plane is further below it than this has no floor under it that a cgame module would have
// found (they look 512 units down at most): its volume would reach down through everything, so it casts nothing
#define	SHADOW_MAX_DROP			1024.0f

// A caster the cgame gave no floor (RF_SHADOW_PLANE), like the MD3 models of items, shadows down to this far below
// its origin: about where the floor under a floating pickup is (the cgame modules look 128 down for their blob
// shadows). It used to be down to height 0 of the world, through every floor in between, and no shadow at all on
// maps below 0.
#define	SHADOW_NO_PLANE_DROP	128.0f

#define	SHADOW_STENCIL_COUNT	0x7F
#define	SHADOW_STENCIL_SKIP		0x80
#define	SHADOW_MAX_POINTS		1000000

#define	SHADOW_WELD_SIZE		4096
#define	SHADOW_EDGE_SIZE		16384

typedef struct {
	int		stamp;
	int		vert;
} weldSlot_t;

typedef struct {
	int		stamp;
	int		key;
	int		net;
} edgeSlot_t;

static weldSlot_t	weldTable[SHADOW_WELD_SIZE];
static edgeSlot_t	edgeTable[SHADOW_EDGE_SIZE];
static int			shStamp;
static vec3_t		shWorld[SHADER_MAX_VERTEXES];
static int			shRep[SHADER_MAX_VERTEXES];		// first vertex at the same position
static int			shPtOfRep[SHADER_MAX_VERTEXES];	// point index of a first vertex
static int			shEdgeUsed[SHADER_MAX_INDEXES + 1];

static void RB_ShadowClear( void )
{
	shPts.clear();
	shTris.clear();
	shRtTris.clear();
	shCaps.clear();
	shWalls.clear();
	shStrengthSum = 0.0f;
	shCasters = 0;
	shSurfs.clear();
}

// 2 = stencil volumes, 4 = shadow maps (cg_shadows 4, or 2 with r_shadowMaps 1; volumes where the renderer can't
// draw maps), 5 = ray traced (r_rayTracing with 2 or 4; maps, then volumes, where the renderer can't), 0 = none
static int RB_ShadowMode( void )
{
	const int shadows = r_shadows->integer;

	if ( glConfig.stencilBits < 4 || !R_MODEL_SHADOWS( shadows ) ) {
		return 0;
	}
	if ( R_RayTracing() ) {
		return 5;	// (ray traced, whichever of the model shadows is chosen)
	}
	if ( ( shadows >= 4 || r_shadowMaps->integer ) && R_ShadowMaps_Available() ) {
		return 4;
	}
	return 2;
}

// RB_ShadowFinish darkens the view: the light passes of the opaque surfaces wait for it (RB_DrawDeferredDlights)
qboolean RB_ShadowsDarken( void )
{
	return RB_ShadowMode() ? qtrue : qfalse;
}

static inline int RB_ShadowCell( float v )
{
	return (int)floorf( v * 2.0f );
}

static inline unsigned RB_ShadowCellHash( int cx, int cy, int cz )
{
	return ( (unsigned)cx * 73856093u ^ (unsigned)cy * 19349663u ^ (unsigned)cz * 83492791u ) & ( SHADOW_WELD_SIZE - 1 );
}

// finds the first vertex that sits at the same position as the given one, or -1
static int RB_ShadowFindWeld( const vec3_t p )
{
	const int cx = RB_ShadowCell( p[0] ), cy = RB_ShadowCell( p[1] ), cz = RB_ShadowCell( p[2] );

	for ( int dx = -1 ; dx <= 1 ; dx++ ) {
		for ( int dy = -1 ; dy <= 1 ; dy++ ) {
			for ( int dz = -1 ; dz <= 1 ; dz++ ) {
				unsigned slot = RB_ShadowCellHash( cx + dx, cy + dy, cz + dz );
				while ( weldTable[slot].stamp == shStamp ) {
					if ( DistanceSquared( shWorld[weldTable[slot].vert], p ) < 0.0025f ) {
						return weldTable[slot].vert;
					}
					slot = ( slot + 1 ) & ( SHADOW_WELD_SIZE - 1 );
				}
			}
		}
	}
	return -1;
}

static void RB_ShadowAddWeld( int vert )
{
	unsigned slot = RB_ShadowCellHash( RB_ShadowCell( shWorld[vert][0] ), RB_ShadowCell( shWorld[vert][1] ), RB_ShadowCell( shWorld[vert][2] ) );

	while ( weldTable[slot].stamp == shStamp ) {
		slot = ( slot + 1 ) & ( SHADOW_WELD_SIZE - 1 );
	}
	weldTable[slot].stamp = shStamp;
	weldTable[slot].vert = vert;
}

// counts how often an edge is used, in one direction minus the other
static void RB_ShadowCountEdge( int a, int b, int *numUsed )
{
	const int lo = ( a < b ) ? a : b;
	const int hi = ( a < b ) ? b : a;
	const int key = lo * 1024 + hi;
	unsigned slot = ( (unsigned)key * 2654435761u ) & ( SHADOW_EDGE_SIZE - 1 );

	while ( edgeTable[slot].stamp == shStamp && edgeTable[slot].key != key ) {
		slot = ( slot + 1 ) & ( SHADOW_EDGE_SIZE - 1 );
	}
	if ( edgeTable[slot].stamp != shStamp ) {
		edgeTable[slot].stamp = shStamp;
		edgeTable[slot].key = key;
		edgeTable[slot].net = 0;
		shEdgeUsed[(*numUsed)++] = slot;
	}
	edgeTable[slot].net += ( a < b ) ? 1 : -1;
}

/*
=================
RB_ShadowTilt

How far the shadow leans for each unit of height, from the direction of the level's light at the entity (in the
world: ent->lightDir is turned with the entity, and turned towards the dynamic lights)
=================
*/
static void RB_ShadowTilt( const trRefEntity_t *ent, float *tx, float *ty )
{
	float lz = ent->shadowLightDir[2];
	if ( lz < 0.35f ) {
		lz = 0.35f;
	}

	float x = ent->shadowLightDir[0] / lz * r_shadowTilt->value;
	float y = ent->shadowLightDir[1] / lz * r_shadowTilt->value;
	const float len = sqrtf( x * x + y * y );
	const float maxTilt = 1.2f;

	if ( len > maxTilt ) {
		x *= maxTilt / len;
		y *= maxTilt / len;
	}
	*tx = x;
	*ty = y;
}

/*
=================
RB_ShadowTessEnd

Records the surface in tess as a shadow caster.
=================
*/
void RB_ShadowTessEnd( void )
{
	const trRefEntity_t	*ent = backEnd.currentEntity;
	const orientationr_t *ori = &backEnd.ori;
	int			numVerts = tess.numVertexes;
	int			numTris = tess.numIndexes / 3;
	int			i, numUsed = 0;
	float		tx, ty;

	const int mode = RB_ShadowMode();
	if ( !mode || !ent || numVerts < 3 || numTris < 1 ) {
		return;
	}
	if ( shPts.size() > SHADOW_MAX_POINTS ) {
		return;
	}
	const float shadowPlane = ( ent->e.renderfx & RF_SHADOW_PLANE ) ? ent->e.shadowPlane : ent->e.origin[2] - SHADOW_NO_PLANE_DROP;
	if ( ent->e.origin[2] - shadowPlane > SHADOW_MAX_DROP ) {
		return;
	}

	// a character that is only there for its shadow (your own, in first person) is not seen, so it must not keep
	// the shadows off what is behind it (see RB_ShadowMarkCasters)
	const bool visible = !( ent->e.renderfx & RF_SHADOW_ONLY );

	RB_ShadowTilt( ent, &tx, &ty );
	const vec3_t lightAxis = { tx, ty, 1.0f };

	shStamp++;
	const int capsStart = (int)shCaps.size();

	// weld the vertices at the same position, working in world space
	for ( i = 0 ; i < numVerts ; i++ ) {
		const float *v = tess.xyz[i];

		VectorCopy( ori->origin, shWorld[i] );
		VectorMA( shWorld[i], v[0], ori->axis[0], shWorld[i] );
		VectorMA( shWorld[i], v[1], ori->axis[1], shWorld[i] );
		VectorMA( shWorld[i], v[2], ori->axis[2], shWorld[i] );

		const int rep = RB_ShadowFindWeld( shWorld[i] );
		if ( rep >= 0 ) {
			shRep[i] = rep;
			continue;
		}

		shRep[i] = i;
		RB_ShadowAddWeld( i );

		float h = shWorld[i][2] - shadowPlane + 16.0f;	// the fudge keeps it below the floor
		if ( h < 0.0f ) {
			h = 0.0f;
		}

		shadowPt_t pt;
		VectorCopy( shWorld[i], pt.xyz );
		pt.ext[0] = -h * lightAxis[0];
		pt.ext[1] = -h * lightAxis[1];
		pt.ext[2] = -h;
		pt.h = h;
		shPtOfRep[i] = (int)shPts.size();
		shPts.push_back( pt );
	}

	// find the triangles facing the light, and the edges between them and the rest
	for ( i = 0 ; i < numTris ; i++ ) {
		const int r1 = shRep[tess.indexes[i*3 + 0]];
		const int r2 = shRep[tess.indexes[i*3 + 1]];
		const int r3 = shRep[tess.indexes[i*3 + 2]];

		if ( r1 == r2 || r2 == r3 || r3 == r1 ) {
			continue;
		}

		const int p1 = shPtOfRep[r1], p2 = shPtOfRep[r2], p3 = shPtOfRep[r3];
		if ( visible ) {
			shTris.push_back( p1 );
			shTris.push_back( p2 );
			shTris.push_back( p3 );
		}
		if ( mode == 5 ) {
			shRtTris.push_back( p1 );
			shRtTris.push_back( p2 );
			shRtTris.push_back( p3 );
		}

		vec3_t d1, d2, normal;
		VectorSubtract( shWorld[r2], shWorld[r1], d1 );
		VectorSubtract( shWorld[r3], shWorld[r1], d2 );
		CrossProduct( d1, d2, normal );

		if ( DotProduct( normal, lightAxis ) <= 0.0f ) {
			continue;
		}

		shCaps.push_back( p1 );
		shCaps.push_back( p2 );
		shCaps.push_back( p3 );

		if ( mode >= 4 ) {
			continue;	// (the shadow maps and the rays only need the triangles)
		}
		RB_ShadowCountEdge( r1, r2, &numUsed );
		RB_ShadowCountEdge( r2, r3, &numUsed );
		RB_ShadowCountEdge( r3, r1, &numUsed );
	}

	// every edge that is not cancelled by a facing neighbour is the border of the volume
	for ( i = 0 ; i < numUsed ; i++ ) {
		const edgeSlot_t *e = &edgeTable[shEdgeUsed[i]];
		const int lo = e->key >> 10;
		const int hi = e->key & 1023;
		const int n = abs( e->net );

		for ( int j = 0 ; j < n ; j++ ) {
			shWalls.push_back( shPtOfRep[( e->net > 0 ) ? lo : hi] );
			shWalls.push_back( shPtOfRep[( e->net > 0 ) ? hi : lo] );
		}
	}

	// how much of the level's light comes from a direction, which is what a shadow takes away
	const float strength = ent->shadowStrength;
	shStrengthSum += strength;
	shCasters++;

	if ( mode >= 4 && (int)shCaps.size() > capsStart ) {
		shadowSurf_t surf;
		surf.ent = ent;
		surf.capsStart = capsStart;
		surf.capsEnd = (int)shCaps.size();
		VectorCopy( lightAxis, surf.lightAxis );
		surf.shadowPlane = shadowPlane;
		surf.strength = strength;
		shSurfs.push_back( surf );
	}
}

static void RB_ShadowEmitVolumes( const float *bottom )
{
	const shadowPt_t *pts = &shPts[0];

	qglBegin( GL_TRIANGLES );

	for ( size_t i = 0 ; i < shCaps.size() ; i += 3 ) {
		const int a = shCaps[i], b = shCaps[i+1], c = shCaps[i+2];

		qglVertex3fv( pts[a].xyz );
		qglVertex3fv( pts[b].xyz );
		qglVertex3fv( pts[c].xyz );

		qglVertex3fv( bottom + c*3 );
		qglVertex3fv( bottom + b*3 );
		qglVertex3fv( bottom + a*3 );
	}

	for ( size_t i = 0 ; i < shWalls.size() ; i += 2 ) {
		const int a = shWalls[i], b = shWalls[i+1];

		qglVertex3fv( pts[a].xyz );
		qglVertex3fv( bottom + a*3 );
		qglVertex3fv( pts[b].xyz );

		qglVertex3fv( pts[b].xyz );
		qglVertex3fv( bottom + a*3 );
		qglVertex3fv( bottom + b*3 );
	}

	qglEnd();
}

static void RB_ShadowDrawVolumes( const float *bottom )
{
	qglDepthFunc( GL_LESS );

	if ( R_SHADOWS_IN_ONE_DRAWCALL ) {
		GL_Cull( CT_TWO_SIDED );
		qglStencilOpSeparate( GL_FRONT, GL_KEEP, GL_INCR_WRAP, GL_KEEP );
		qglStencilOpSeparate( GL_BACK, GL_KEEP, GL_DECR_WRAP, GL_KEEP );
		RB_ShadowEmitVolumes( bottom );
	} else {
		GL_Cull( CT_FRONT_SIDED );
		qglStencilOp( GL_KEEP, GL_INCR, GL_KEEP );
		RB_ShadowEmitVolumes( bottom );

		GL_Cull( CT_BACK_SIDED );
		qglStencilOp( GL_KEEP, GL_DECR, GL_KEEP );
		RB_ShadowEmitVolumes( bottom );
	}

	qglDepthFunc( GL_LEQUAL );
}

// Marks the pixels that show a shadow casting character.  The triangles are drawn a little
// behind the depth that is already there, so the stencil op for a failed depth test runs
// exactly where the character itself is visible (the floor behind a cut out hair texture
// is further away than that, and passes).
static void RB_ShadowMarkCasters( void )
{
	const shadowPt_t *pts = &shPts[0];

	GL_Cull( CT_TWO_SIDED );
	qglStencilMask( SHADOW_STENCIL_SKIP );
	qglStencilFunc( GL_ALWAYS, SHADOW_STENCIL_SKIP, SHADOW_STENCIL_SKIP );
	qglStencilOp( GL_KEEP, GL_REPLACE, GL_KEEP );

	qglEnable( GL_POLYGON_OFFSET_FILL );
	qglPolygonOffset( 2.0f, 16.0f );

	qglBegin( GL_TRIANGLES );
	for ( size_t i = 0 ; i < shTris.size() ; i++ ) {
		qglVertex3fv( pts[shTris[i]].xyz );
	}
	qglEnd();

	qglDisable( GL_POLYGON_OFFSET_FILL );
}

// a screen filling quad, to be drawn with the identity as modelview matrix
static void RB_ShadowScreenQuad( void )
{
	qglBegin( GL_QUADS );
	qglVertex3f( -100, 100, -10 );
	qglVertex3f( 100, 100, -10 );
	qglVertex3f( 100, -100, -10 );
	qglVertex3f( -100, -100, -10 );
	qglEnd();
}

/*
=================
RB_ShadowFinish

Renders the recorded volumes and darkens everything that is in a shadow.
We have to delay this until everything has been shadowed,
because otherwise shadows from different body parts would
overlap and double darken.
=================
*/
static void RB_ShadowMapsFinish( void );

void RB_ShadowFinish( void )
{
	if ( RB_ShadowMode() >= 4 ) {
		RB_ShadowMapsFinish();	// (the ray traced shadows too: the casters are found the same way)
		return;
	}
	if ( RB_ShadowMode() != 2 || shCaps.empty() ) {
		RB_ShadowClear();
		return;
	}

	int samples = r_shadowSamples->integer;
	if ( samples < 1 || r_shadowSoftness->value <= 0.0f ) {
		samples = 1;
	} else if ( samples > 16 ) {
		samples = 16;
	}
	const float spread = r_shadowSoftness->value * 0.12f;
	const bool skipCasters = !r_shadowSelf->integer;

	float darkness = shStrengthSum / (float)shCasters;
	darkness = Com_Clamp( 0.2f, 0.75f, darkness ) * r_shadowStrength->value;
	darkness = Com_Clamp( 0.0f, 0.9f, darkness );
	// the darkening is applied once for each sample, they add up to the whole
	const float sampleAlpha = 1.0f - powf( 1.0f - darkness, 1.0f / (float)samples );

	float oldDepthRange[2];
	qglGetFloatv( GL_DEPTH_RANGE, oldDepthRange );
	qglDepthRange( 0, 1 );

	bool planeZeroBack = false;
	if ( qglIsEnabled( GL_CLIP_PLANE0 ) ) {
		planeZeroBack = true;
	}

	qglPushMatrix();

	GL_Bind( tr.whiteImage );
	qglEnable( GL_STENCIL_TEST );

	std::vector<float> bottom( shPts.size() * 3 );

	for ( int k = 0 ; k < samples ; k++ ) {
		// the light direction wobbles around in a circle, wider the higher the caster is
		float jx = 0.0f, jy = 0.0f;
		if ( samples > 1 ) {
			const float r = spread * sqrtf( ( k + 0.5f ) / (float)samples );
			const float a = k * 2.39996323f;
			jx = r * cosf( a );
			jy = r * sinf( a );
		}
		for ( size_t i = 0 ; i < shPts.size() ; i++ ) {
			const shadowPt_t *p = &shPts[i];
			bottom[i*3 + 0] = p->xyz[0] + p->ext[0] - p->h * jx;
			bottom[i*3 + 1] = p->xyz[1] + p->ext[1] - p->h * jy;
			bottom[i*3 + 2] = p->xyz[2] + p->ext[2];
		}

		// count the volumes, drawing only into the stencil buffer
		qglLoadMatrixf( backEnd.viewParms.world.modelMatrix );
		GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );
		qglColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
		qglStencilMask( SHADOW_STENCIL_COUNT );
		qglStencilFunc( GL_ALWAYS, 0, 0xFF );
		RB_ShadowDrawVolumes( &bottom[0] );
		if ( k == 0 && skipCasters && !shTris.empty() ) {
			RB_ShadowMarkCasters();
		}

		// the screen filling passes ignore depth and the portal clip plane
		qglLoadIdentity();
		GL_Cull( CT_TWO_SIDED );
		GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );
		if ( planeZeroBack ) {
			qglDisable( GL_CLIP_PLANE0 );
		}

		if ( skipCasters ) {
			// whatever is in the volumes behind a character is not for it to be darkened
			qglStencilMask( SHADOW_STENCIL_COUNT );
			qglStencilFunc( GL_EQUAL, SHADOW_STENCIL_SKIP, SHADOW_STENCIL_SKIP );
			qglStencilOp( GL_KEEP, GL_KEEP, GL_ZERO );
			RB_ShadowScreenQuad();
		}

		qglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
		qglStencilMask( 0 );
		qglStencilFunc( GL_NOTEQUAL, 0, SHADOW_STENCIL_COUNT );
		qglStencilOp( GL_KEEP, GL_KEEP, GL_KEEP );
		qglColor4f( 0.0f, 0.0f, 0.0f, sampleAlpha );
		GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
		RB_ShadowScreenQuad();

		// and clear the counts for the next sample
		qglColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
		qglStencilMask( ( k == samples - 1 ) ? 0xFF : SHADOW_STENCIL_COUNT );
		qglStencilFunc( GL_ALWAYS, 0, 0xFF );
		qglStencilOp( GL_ZERO, GL_ZERO, GL_ZERO );
		GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );
		RB_ShadowScreenQuad();

		if ( planeZeroBack ) {
			qglEnable( GL_CLIP_PLANE0 );
		}
	}

	qglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	qglStencilMask( 0xFF );
	qglStencilOp( GL_KEEP, GL_KEEP, GL_KEEP );
	qglStencilFunc( GL_ALWAYS, 0, 0xFF );
	qglDisable( GL_STENCIL_TEST );
	qglColor4f( 1, 1, 1, 1 );
	qglPopMatrix();
	qglDepthRange( oldDepthRange[0], oldDepthRange[1] );

	RB_ShadowClear();
}

/*
=============================================================================

SHADOW MAPS (cg_shadows 4)

The casters are the same as for the volumes, and their triangles that face the light are recorded the same way
(RB_ShadowTessEnd). Instead of being swept down to the floor, each caster is seen from its light: its triangles are
drawn, on the CPU, into a small depth tile (how high towards the light the caster is at each point), and every pixel of
the view is then compared with it (RB_ShadowMapsApply): a point lower than the caster above it is in its shadow. So the
shadow falls on the floor, on walls, on steps higher than the floor and on slopes, in the shape the caster has from its
light, and its edge is soft, the softer the further the point is from the caster (as with a real light of some size).
The characters themselves are not darkened, as with the volumes (r_shadowSelf).

The tiles are drawn on the CPU because the Vulkan renderer only draws into the picture; a character is a few thousand
triangles, of which about half face the light, into 128 x 128 texels. The nearest SHADOWMAP_MAX_CASTERS casters to the
view get a shadow.

=============================================================================
*/

typedef struct {
	const trRefEntity_t	*ent;
	int					firstSurf;		// in the order list
	int					numSurfs;
	float				dist2;
} shadowCaster_t;

static float	shTile[SHADOWMAP_TILE * SHADOWMAP_TILE];
static byte		shAtlas[SHADOWMAP_ATLAS * SHADOWMAP_ATLAS * 4];

// Draws the triangle (x, y in texels, n the normalized height) into shTile, keeping the highest n
static void RB_ShadowMapTriangle( const float *a, const float *b, const float *c )
{
	const float area = ( b[0] - a[0] ) * ( c[1] - a[1] ) - ( b[1] - a[1] ) * ( c[0] - a[0] );
	if ( fabsf( area ) < 1e-6f ) {
		return;
	}
	const int x0 = Q_max( 0, (int)floorf( Q_min( a[0], Q_min( b[0], c[0] ) ) ) );
	const int y0 = Q_max( 0, (int)floorf( Q_min( a[1], Q_min( b[1], c[1] ) ) ) );
	const int x1 = Q_min( SHADOWMAP_TILE - 1, (int)ceilf( Q_max( a[0], Q_max( b[0], c[0] ) ) ) );
	const int y1 = Q_min( SHADOWMAP_TILE - 1, (int)ceilf( Q_max( a[1], Q_max( b[1], c[1] ) ) ) );
	const float inv = 1.0f / area;

	for ( int y = y0; y <= y1; y++ ) {
		const float py = y + 0.5f;
		for ( int x = x0; x <= x1; x++ ) {
			const float px = x + 0.5f;
			// barycentric weights; a texel whose centre is inside the triangle (either winding) is covered
			const float wa = ( ( b[0] - px ) * ( c[1] - py ) - ( b[1] - py ) * ( c[0] - px ) ) * inv;
			const float wb = ( ( c[0] - px ) * ( a[1] - py ) - ( c[1] - py ) * ( a[0] - px ) ) * inv;
			const float wc = 1.0f - wa - wb;
			if ( wa < 0.0f || wb < 0.0f || wc < 0.0f ) {
				continue;
			}
			const float n = wa * a[2] + wb * b[2] + wc * c[2];
			float *texel = &shTile[y * SHADOWMAP_TILE + x];
			if ( n > *texel ) {
				*texel = n;
			}
		}
	}
}

// A row of the caster: what a world position gives (dot( a, world ) + b), as a row on the view's eye space
static void RB_ShadowMapEyeRow( const vec3_t a, float b, float row[4] )
{
	const float *m = backEnd.viewParms.world.modelMatrix;	// world to eye, column major: eye = R * world + t

	for ( int i = 0; i < 3; i++ ) {
		row[i] = m[i] * a[0] + m[4 + i] * a[1] + m[8 + i] * a[2];		// (R a)[i]
	}
	row[3] = b - ( row[0] * m[12] + row[1] * m[13] + row[2] * m[14] );
}

// Where on the view the shadow of a caster can be (rect: left, bottom, right, top, 0 to 1 from the bottom left corner):
// around the corners of its tile, from the top of the caster down along the light to the floor. All of the view if
// one of them is behind the eye.
static void RB_ShadowMapRect( const vec3_t U, const vec3_t V, const vec3_t L, const float uv[4], float hmax, float floorZ, float rect[4] )
{
	const float *m = backEnd.viewParms.world.modelMatrix;
	const float *p = backEnd.viewParms.projectionMatrix;

	rect[0] = rect[1] = 1.0f;
	rect[2] = rect[3] = 0.0f;
	for ( int k = 0; k < 8; k++ ) {
		vec3_t w, e;
		const float u = uv[k & 1], v = uv[2 + ( ( k >> 1 ) & 1 )];

		for ( int i = 0; i < 3; i++ ) {
			w[i] = u * U[i] + v * V[i] + hmax * L[i];
		}
		if ( k & 4 ) {
			VectorMA( w, -( w[2] - floorZ ) / Q_max( L[2], 0.05f ), L, w );
		}
		for ( int i = 0; i < 3; i++ ) {
			e[i] = m[i] * w[0] + m[4 + i] * w[1] + m[8 + i] * w[2] + m[12 + i];
		}
		const float cw = p[3] * e[0] + p[7] * e[1] + p[11] * e[2] + p[15];
		if ( cw < 1.0f ) {
			rect[0] = rect[1] = 0.0f;
			rect[2] = rect[3] = 1.0f;
			return;
		}
		const float x = 0.5f + 0.5f * ( p[0] * e[0] + p[4] * e[1] + p[8] * e[2] + p[12] ) / cw;
		const float y = 0.5f + 0.5f * ( p[1] * e[0] + p[5] * e[1] + p[9] * e[2] + p[13] ) / cw;
		rect[0] = Q_min( rect[0], x ); rect[1] = Q_min( rect[1], y );
		rect[2] = Q_max( rect[2], x ); rect[3] = Q_max( rect[3], y );
	}
	for ( int i = 0; i < 4; i++ ) {
		rect[i] = Com_Clamp( 0.0f, 1.0f, rect[i] );
	}
}

/*
=============================================================================

RAY TRACED: THE LEVEL'S LAMPS AND THE CONTACT SHADOW

With ray tracing a character's shadow is cast from the lamps of the level where it keeps them: the "light" entities
q3map lit it with, which the Jedi Outcast levels have (most Jedi Academy levels were compiled without them). From each
of the few lamps that light the character most, and that it can see, a soft shadow away from the lamp, as dark as the
lamp's share of the light it has there: the darkness the light grid gives (how much of the light comes from a
direction, RB_ShadowTessEnd) is shared out between them as q3map shared it out, by their brightness at the character.
Where there are none, the grid's one direction, fainter and softer than a lamp's as it is a guess. And around every
character a short contact shadow from all around: the light that comes from no one direction, which it takes away
close to it.

The lamps a character takes are kept for the 16 units cube it is in (what can see them is found with the server's
traces through the level): one that stands still costs nothing.

=============================================================================
*/

typedef struct {
	vec3_t	origin;
	float	photons;			// q3map's: the intensity times its point scale
	vec3_t	color;				// the brightest component 1
	float	luminance;			// of its colour
	int		style;				// its light style (flickering, switched), 0 none
	float	radius;				// how big it is: how soft its shadows are
	bool	linear;				// it falls off with the distance (spawnflags 1), not with its square
	bool	spot;				// a spotlight: towards its target, in a cone
	vec3_t	spotDir;
	float	spotRadiusByDist;	// the radius of the cone for each unit along it
} shadowLamp_t;

#define SHADOW_LAMPS_MAX		LAMPS_AT_MAX	// for a caster
#define SHADOW_LAMP_TRACES		12		// at most this many tried for one, the brightest first
#define SHADOW_LAMP_RADIUS		12.0f	// when the entity does not say (_deviance)
#define SHADOW_CONTACT_REACH	32.0f
#define SHADOW_CONTACT_SHARE	0.5f	// of the light from no one direction, what the contact shadow takes where all its rays are blocked
#define SHADOW_GRID_GUESS		0.6f	// the grid direction's darkness, where there are no lamps
#define SHADOW_RT_BELOW			128.0f	// how far below its floor a lamp's shadow is looked for

static std::vector<shadowLamp_t>						shLamps;
#ifdef _JK2EXE
static std::unordered_map<long long, lampChoice_t>		shLampChoices;
#endif
static std::vector<shadowMapCaster_t>					shRtEntries;

static float RB_EntityFloat( const std::vector<std::pair<std::string, std::string> > &ent, const char *key, float def )
{
	for ( size_t i = 0; i < ent.size(); i++ ) {
		if ( !Q_stricmp( ent[i].first.c_str(), key ) ) {
			return (float)atof( ent[i].second.c_str() );
		}
	}
	return def;
}

static const char *RB_EntityString( const std::vector<std::pair<std::string, std::string> > &ent, const char *key )
{
	for ( size_t i = 0; i < ent.size(); i++ ) {
		if ( !Q_stricmp( ent[i].first.c_str(), key ) ) {
			return ent[i].second.c_str();
		}
	}
	return "";
}

void R_ShadowLampsLoad( const char *text, int length )
{
	std::vector<std::vector<std::pair<std::string, std::string> > > ents;
	std::vector<std::pair<std::string, std::string> > *ent = NULL;
	std::string token[2];
	int numTokens = 0;

	shLamps.clear();
#ifdef _JK2EXE
	shLampChoices.clear();
#endif

	// { "key" "value" ... } ...
	for ( int i = 0; i < length && text[i]; i++ ) {
		if ( text[i] == '{' ) {
			ents.emplace_back();
			ent = &ents.back();
			numTokens = 0;
		} else if ( text[i] == '}' ) {
			ent = NULL;
		} else if ( text[i] == '"' && ent ) {
			int j = i + 1;
			while ( j < length && text[j] && text[j] != '"' ) {
				j++;
			}
			token[numTokens++].assign( text + i + 1, j - i - 1 );
			if ( numTokens == 2 ) {
				ent->emplace_back( token[0], token[1] );
				numTokens = 0;
			}
			i = j;
		}
	}

	for ( size_t e = 0; e < ents.size(); e++ ) {
		if ( Q_stricmp( RB_EntityString( ents[e], "classname" ), "light" ) ) {
			continue;
		}
		shadowLamp_t lamp = {};
		if ( sscanf( RB_EntityString( ents[e], "origin" ), "%f %f %f", &lamp.origin[0], &lamp.origin[1], &lamp.origin[2] ) != 3 ) {
			continue;
		}
		// as q3map: "light" (or "_light"), 300 when there is none, times "scale"
		float intensity = RB_EntityFloat( ents[e], "light", 0.0f );
		if ( intensity == 0.0f ) {
			intensity = RB_EntityFloat( ents[e], "_light", 300.0f );
		}
		intensity *= RB_EntityFloat( ents[e], "scale", 1.0f );
		if ( intensity <= 0.0f ) {
			continue;	// (a negative light takes light away, it casts no shadow)
		}
		lamp.photons = intensity * 7500.0f;
		vec3_t color = { 1.0f, 1.0f, 1.0f };
		sscanf( RB_EntityString( ents[e], "_color" ), "%f %f %f", &color[0], &color[1], &color[2] );
		const float brightest = Q_max( color[0], Q_max( color[1], color[2] ) );
		if ( brightest > 0.0f ) {
			VectorScale( color, 1.0f / brightest, lamp.color );
		} else {
			VectorSet( lamp.color, 1.0f, 1.0f, 1.0f );
		}
		lamp.luminance = 0.299f * lamp.color[0] + 0.587f * lamp.color[1] + 0.114f * lamp.color[2];
		lamp.style = (int)RB_EntityFloat( ents[e], "style", 0.0f );
		if ( lamp.style < 0 || lamp.style >= MAX_LIGHT_STYLES ) {
			lamp.style = 0;
		}
		lamp.radius = RB_EntityFloat( ents[e], "_deviance", 0.0f );
		if ( lamp.radius <= 0.0f ) {
			lamp.radius = SHADOW_LAMP_RADIUS;
		}
		lamp.linear = ( (int)RB_EntityFloat( ents[e], "spawnflags", 0.0f ) & 1 ) != 0;

		// a spotlight aims at the entity it targets
		const char *target = RB_EntityString( ents[e], "target" );
		for ( size_t t = 0; *target && t < ents.size(); t++ ) {
			vec3_t at;
			if ( Q_stricmp( RB_EntityString( ents[t], "targetname" ), target ) ||
				 sscanf( RB_EntityString( ents[t], "origin" ), "%f %f %f", &at[0], &at[1], &at[2] ) != 3 ) {
				continue;
			}
			VectorSubtract( at, lamp.origin, lamp.spotDir );
			const float dist = VectorNormalize( lamp.spotDir );
			if ( dist > 0.0f ) {
				lamp.spot = true;
				lamp.spotRadiusByDist = ( RB_EntityFloat( ents[e], "radius", 64.0f ) + 16.0f ) / dist;
			}
			break;
		}
		shLamps.push_back( lamp );
	}
	ri.Printf( PRINT_DEVELOPER, "shadows: %d lamps in the level\n", (int)shLamps.size() );
}

#ifdef _JK2EXE
// How much light the lamp gave a point in q3map, for its colour (without the angle: the point is a character, lit
// from all sides); 0 outside a spotlight's cone
static float RB_LampAt( const shadowLamp_t *lamp, const vec3_t point )
{
	vec3_t d;
	VectorSubtract( point, lamp->origin, d );
	float dist = VectorLength( d );
	float scale = 1.0f;

	if ( lamp->spot ) {
		const float along = DotProduct( d, lamp->spotDir );
		if ( along <= 0.0f ) {
			return 0.0f;
		}
		vec3_t off;
		VectorMA( d, -along, lamp->spotDir, off );
		const float radiusAt = lamp->spotRadiusByDist * along, r = VectorLength( off );
		if ( r >= radiusAt ) {
			return 0.0f;
		}
		if ( r > radiusAt - 32.0f ) {
			scale = ( radiusAt - r ) / 32.0f;	// (q3map's soft edge)
		}
	}
	if ( dist < 16.0f ) {
		dist = 16.0f;
	}
	const float add = lamp->linear ? lamp->photons / 8000.0f - dist : lamp->photons / ( dist * dist );
	return add > 0.0f ? add * scale * lamp->luminance : 0.0f;
}

const lampChoice_t *R_LampsAt( const vec3_t point )
{
	const long long key = ( (long long)( (int)floorf( point[0] / 16.0f ) & 0xFFFFF ) << 40 ) |
		( (long long)( (int)floorf( point[1] / 16.0f ) & 0xFFFFF ) << 20 ) | ( (int)floorf( point[2] / 16.0f ) & 0xFFFFF );
	auto known = shLampChoices.find( key );
	if ( known != shLampChoices.end() ) {
		return &known->second;
	}
	if ( shLampChoices.size() > 8192 ) {
		shLampChoices.clear();
	}

	// the brightest there
	std::pair<float, int> best[SHADOW_LAMP_TRACES];
	int numBest = 0;
	for ( size_t i = 0; i < shLamps.size(); i++ ) {
		const float add = RB_LampAt( &shLamps[i], point );
		if ( add <= 0.0f || ( numBest == SHADOW_LAMP_TRACES && add <= best[numBest - 1].first ) ) {
			continue;
		}
		int k = numBest < SHADOW_LAMP_TRACES ? numBest++ : numBest - 1;
		for ( ; k > 0 && best[k - 1].first < add; k-- ) {
			best[k] = best[k - 1];
		}
		best[k] = std::make_pair( add, (int)i );
	}

	// those it can see, brightest first; one much fainter than the brightest of them is left out
	lampChoice_t choice = {};
	for ( int b = 0; b < numBest && choice.count < SHADOW_LAMPS_MAX; b++ ) {
		const shadowLamp_t *lamp = &shLamps[best[b].second];
		if ( choice.count && best[b].first < 0.05f * choice.weights[0] ) {
			break;
		}
		trace_t tr;
		ri.SV_Trace( &tr, point, vec3_origin, vec3_origin, lamp->origin, ENTITYNUM_NONE, CONTENTS_SOLID, G2_NOCOLLIDE, 0 );
		if ( tr.fraction < 1.0f && Distance( tr.endpos, lamp->origin ) > 24.0f ) {
			continue;	// (a lamp often sits in its fixture: what stops near it does not hide it)
		}
		choice.lamps[choice.count] = best[b].second;
		choice.weights[choice.count++] = best[b].first;
	}
	return &( shLampChoices[key] = choice );
}

void R_LampInfo( int lamp, vec3_t origin, vec3_t color, float *luminance, int *style )
{
	VectorCopy( shLamps[lamp].origin, origin );
	VectorCopy( shLamps[lamp].color, color );
	*luminance = shLamps[lamp].luminance;
	*style = shLamps[lamp].style;
}
#endif

// A ray traced entry seen along L: rows that put the box of the caster (mins, maxs) seen so onto 0 to 1 around its
// middle, grown by magnify and by pad on each side
static void RB_ShadowRtArea( shadowMapCaster_t *e, const vec3_t mins, const vec3_t maxs, const vec3_t L, float magnify, float pad )
{
	vec3_t U, V, a, mid;
	const vec3_t ref = { 1.0f, 0.0f, 0.0f }, ref2 = { 0.0f, 1.0f, 0.0f };
	VectorMA( fabsf( L[0] ) < 0.9f ? ref : ref2, -DotProduct( fabsf( L[0] ) < 0.9f ? ref : ref2, L ), L, U );
	VectorNormalize( U );
	CrossProduct( L, U, V );

	float extent = 0.0f;
	VectorAdd( mins, maxs, mid );
	VectorScale( mid, 0.5f, mid );
	for ( int k = 0; k < 8; k++ ) {
		const vec3_t corner = { ( k & 1 ) ? maxs[0] : mins[0], ( k & 2 ) ? maxs[1] : mins[1], ( k & 4 ) ? maxs[2] : mins[2] };
		vec3_t d;
		VectorSubtract( corner, mid, d );
		extent = Q_max( extent, Q_max( fabsf( DotProduct( d, U ) ), fabsf( DotProduct( d, V ) ) ) );
	}
	const float size = 2.0f * ( extent * magnify + pad );
	VectorScale( U, 1.0f / size, a );
	RB_ShadowMapEyeRow( a, 0.5f - DotProduct( mid, U ) / size, e->rowS );
	VectorScale( V, 1.0f / size, a );
	RB_ShadowMapEyeRow( a, 0.5f - DotProduct( mid, V ) / size, e->rowT );
	e->area[0] = e->area[1] = 0.0f;
	e->area[2] = e->area[3] = 1.0f;
}

// The ray traced entries of a caster (its box, the floor under it, the grid's direction L and its spread, darkness
// the grid's darkness, strength the share of the grid's light that has a direction)
static void RB_ShadowRtEntries( const vec3_t mins, const vec3_t maxs, float floorZ, const vec3_t L, float spread, float darkness, float strength )
{
	shadowMapCaster_t e = {};
	vec3_t mid;
	VectorAdd( mins, maxs, mid );
	VectorScale( mid, 0.5f, mid );
	e.floorZ = floorZ;
	const float pad = 4.0f + 24.0f * r_shadowSoftness->value;

	int numLamps = 0;
#ifdef _JK2EXE
	static cvar_t *lampsCvar = NULL;
	if ( !lampsCvar ) {
		lampsCvar = ri.Cvar_Get( "r_rtLamps", "1", CVAR_ARCHIVE_ND );	// 0: the grid's direction, as without the lamps
	}
	// (a view of the level only: the lamps are of the last level loaded, the server's traces need it to be running)
	if ( lampsCvar->integer && !shLamps.empty() && tr.world && !( backEnd.refdef.rdflags & RDF_NOWORLDMODEL ) ) {
		const lampChoice_t *choice = R_LampsAt( mid );
		float total = 0.0f;
		for ( int i = 0; i < choice->count; i++ ) {
			total += choice->weights[i];
		}
		for ( int i = 0; i < choice->count && shRtEntries.size() < SHADOWMAP_MAX_RT; i++ ) {
			const shadowLamp_t *lamp = &shLamps[choice->lamps[i]];
			vec3_t toLamp;
			VectorSubtract( lamp->origin, mid, toLamp );
			const float dist = VectorNormalize( toLamp );
			// the shadow is the caster's seen from the lamp, larger the further the ground is behind it than the caster
			// (down to SHADOW_RT_BELOW under its floor: down a step or off a ledge, post_rt_shadow.frag)
			const float toFloor = ( lamp->origin[2] - ( floorZ - SHADOW_RT_BELOW ) ) / Q_max( toLamp[2], 0.1f );
			RB_ShadowRtArea( &e, mins, maxs, toLamp, Com_Clamp( 1.0f, 4.0f, toFloor / Q_max( dist, 1.0f ) ), pad );
			VectorCopy( lamp->origin, e.light );
			e.light[3] = lamp->radius;
			e.darkness = darkness * choice->weights[i] / total;
			// (where the ground is further from the lamp than the floor under the character, the shadow fades:
			// post_rt_shadow.frag)
			const vec3_t underfoot = { mid[0], mid[1], floorZ + 24.0f };
			e.reach = Distance( lamp->origin, underfoot );
			e.rtType = SHADOW_RT_LAMP;
			shRtEntries.push_back( e );
			numLamps++;
		}
	}
#endif
	if ( !numLamps && shRtEntries.size() < SHADOWMAP_MAX_RT ) {
		RB_ShadowRtArea( &e, mins, maxs, L, 1.0f, pad * 2.0f );	// (seen along a direction, the shadow is as large as the caster)
		VectorCopy( L, e.light );
		e.light[3] = spread * 2.0f;
		e.darkness = darkness * SHADOW_GRID_GUESS;
		e.rtType = SHADOW_RT_DIRECTION;
		shRtEntries.push_back( e );
	}
	if ( shRtEntries.size() < SHADOWMAP_MAX_RT ) {
		const vec3_t up = { 0.0f, 0.0f, 1.0f };
		RB_ShadowRtArea( &e, mins, maxs, up, 1.0f, SHADOW_CONTACT_REACH );
		VectorCopy( mid, e.light );
		e.light[3] = SHADOW_CONTACT_REACH;
		e.reach = 0.5f * Distance( mins, maxs ) + SHADOW_CONTACT_REACH;	// (nothing further from its middle)
		e.darkness = Com_Clamp( 0.0f, 0.5f, ( 1.0f - strength ) * SHADOW_CONTACT_SHARE * r_shadowStrength->value );
		e.rtType = SHADOW_RT_CONTACT;
		shRtEntries.push_back( e );
	}
}

static void RB_ShadowMapsFinish( void )
{
	static std::vector<int>				order;
	static std::vector<shadowCaster_t>	casters;
	shadowMapCaster_t	maps[SHADOWMAP_MAX_CASTERS];
	int					numMaps = 0;
	const bool			rayTraced = RB_ShadowMode() == 5;	// (then there are no tiles to draw)

	if ( backEnd.viewParms.isPortal || shSurfs.empty() ) {
		RB_ShadowClear();
		return;
	}

	// the surfaces by caster
	casters.clear();
	order.clear();
	for ( size_t i = 0; i < shSurfs.size(); i++ ) {
		size_t c;
		for ( c = 0; c < casters.size() && casters[c].ent != shSurfs[i].ent; c++ ) {
		}
		if ( c == casters.size() ) {
			shadowCaster_t caster;
			vec3_t d;
			caster.ent = shSurfs[i].ent;
			caster.firstSurf = 0;
			caster.numSurfs = 0;
			VectorSubtract( caster.ent->e.origin, backEnd.viewParms.ori.origin, d );
			caster.dist2 = DotProduct( d, d );
			casters.push_back( caster );
		}
		casters[c].numSurfs++;
	}
	for ( size_t c = 0, first = 0; c < casters.size(); c++ ) {
		casters[c].firstSurf = (int)first;
		first += casters[c].numSurfs;
		casters[c].numSurfs = 0;
	}
	order.resize( shSurfs.size() );
	for ( size_t i = 0; i < shSurfs.size(); i++ ) {
		size_t c;
		for ( c = 0; casters[c].ent != shSurfs[i].ent; c++ ) {
		}
		order[casters[c].firstSurf + casters[c].numSurfs++] = (int)i;
	}
	std::sort( casters.begin(), casters.end(), []( const shadowCaster_t &a, const shadowCaster_t &b ) { return a.dist2 < b.dist2; } );

	if ( !rayTraced ) {
		memset( shAtlas, 0, sizeof( shAtlas ) );
	}
	shRtEntries.clear();

	for ( size_t c = 0; c < casters.size() && numMaps < SHADOWMAP_MAX_CASTERS; c++ ) {
		const shadowCaster_t	*caster = &casters[c];
		const shadowSurf_t		*first = &shSurfs[order[caster->firstSurf]];
		vec3_t					L, U, V;
		float					umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f, hmin = 1e9f, hmax = -1e9f;

		// the light's frame: L towards it, U and V across
		VectorCopy( first->lightAxis, L );
		VectorNormalize( L );
		const vec3_t ref = { 1.0f, 0.0f, 0.0f };
		VectorMA( ref, -DotProduct( ref, L ), L, U );
		VectorNormalize( U );
		CrossProduct( L, U, V );

		for ( int s = 0; s < caster->numSurfs; s++ ) {
			const shadowSurf_t *surf = &shSurfs[order[caster->firstSurf + s]];
			for ( int k = surf->capsStart; k < surf->capsEnd; k++ ) {
				const float *p = shPts[shCaps[k]].xyz;
				const float u = DotProduct( p, U ), v = DotProduct( p, V ), h = DotProduct( p, L );
				umin = Q_min( umin, u ); umax = Q_max( umax, u );
				vmin = Q_min( vmin, v ); vmax = Q_max( vmax, v );
				hmin = Q_min( hmin, h ); hmax = Q_max( hmax, h );
			}
		}
		const float pad = 4.0f + 24.0f * r_shadowSoftness->value;	// (room for the soft edge)
		const float size = Q_max( umax - umin, vmax - vmin ) + 2.0f * pad;
		const float uc = 0.5f * ( umin + umax ), vc = 0.5f * ( vmin + vmax );
		const float hrange = Q_max( hmax - hmin, 1.0f );
		const float toTexels = SHADOWMAP_TILE / size;

		// the tile
		const int tx = numMaps % SHADOWMAP_TILES, ty = numMaps / SHADOWMAP_TILES;
		for ( int i = 0; !rayTraced && i < SHADOWMAP_TILE * SHADOWMAP_TILE; i++ ) {
			shTile[i] = -1.0f;
		}
		for ( int s = 0; !rayTraced && s < caster->numSurfs; s++ ) {
			const shadowSurf_t *surf = &shSurfs[order[caster->firstSurf + s]];
			for ( int k = surf->capsStart; k + 2 < surf->capsEnd; k += 3 ) {
				float tri[3][3];
				for ( int j = 0; j < 3; j++ ) {
					const float *p = shPts[shCaps[k + j]].xyz;
					tri[j][0] = ( DotProduct( p, U ) - uc ) * toTexels + 0.5f * SHADOWMAP_TILE;
					tri[j][1] = ( DotProduct( p, V ) - vc ) * toTexels + 0.5f * SHADOWMAP_TILE;
					tri[j][2] = ( DotProduct( p, L ) - hmin ) / hrange;
				}
				RB_ShadowMapTriangle( tri[0], tri[1], tri[2] );
			}
		}
		for ( int y = 0; !rayTraced && y < SHADOWMAP_TILE; y++ ) {
			byte *dst = shAtlas + ( ( ty * SHADOWMAP_TILE + y ) * SHADOWMAP_ATLAS + tx * SHADOWMAP_TILE ) * 4;
			for ( int x = 0; x < SHADOWMAP_TILE; x++, dst += 4 ) {
				const float n = shTile[y * SHADOWMAP_TILE + x];
				dst[0] = ( n < 0.0f ) ? 0 : (byte)( 1.5f + Com_Clamp( 0.0f, 1.0f, n ) * 254.0f );
				dst[3] = 255;
			}
		}

		// how to find a point of the view in it
		shadowMapCaster_t *map = &maps[numMaps];
		vec3_t a;
		VectorScale( U, toTexels / SHADOWMAP_ATLAS, a );
		RB_ShadowMapEyeRow( a, ( -uc * toTexels + 0.5f * SHADOWMAP_TILE + tx * SHADOWMAP_TILE ) / SHADOWMAP_ATLAS, map->rowS );
		VectorScale( V, toTexels / SHADOWMAP_ATLAS, a );
		RB_ShadowMapEyeRow( a, ( -vc * toTexels + 0.5f * SHADOWMAP_TILE + ty * SHADOWMAP_TILE ) / SHADOWMAP_ATLAS, map->rowT );
		VectorScale( L, 1.0f / hrange, a );
		RB_ShadowMapEyeRow( a, -hmin / hrange, map->rowN );
		const vec3_t up = { 0.0f, 0.0f, 1.0f };
		RB_ShadowMapEyeRow( up, 0.0f, map->rowZ );

		float darkness = Com_Clamp( 0.2f, 0.75f, first->strength ) * r_shadowStrength->value;
		map->darkness = Com_Clamp( 0.0f, 0.9f, darkness );
		map->floorZ = first->shadowPlane - 24.0f;
		map->bias = 3.0f / hrange;
		map->softness = hrange * r_shadowSoftness->value * 0.12f * toTexels;
		map->tile[0] = (float)tx / SHADOWMAP_TILES;
		map->tile[1] = (float)ty / SHADOWMAP_TILES;
		const float uv[4] = { uc - 0.5f * size, uc + 0.5f * size, vc - 0.5f * size, vc + 0.5f * size };
		RB_ShadowMapRect( U, V, L, uv, hmax, map->floorZ, map->rect );
		VectorCopy( L, map->light );
		map->light[3] = r_shadowSoftness->value * 0.12f;	// (the spread of the volumes' samples)
		numMaps++;

		if ( rayTraced ) {
			vec3_t mins = { 1e9f, 1e9f, 1e9f }, maxs = { -1e9f, -1e9f, -1e9f };
			for ( int s = 0; s < caster->numSurfs; s++ ) {
				const shadowSurf_t *surf = &shSurfs[order[caster->firstSurf + s]];
				for ( int k = surf->capsStart; k < surf->capsEnd; k++ ) {
					AddPointToBounds( shPts[shCaps[k]].xyz, mins, maxs );
				}
			}
			RB_ShadowRtEntries( mins, maxs, map->floorZ, L, map->light[3], map->darkness, first->strength );
		}
	}

	if ( numMaps ) {
		const bool skipCasters = !r_shadowSelf->integer && !shTris.empty();
		float oldDepthRange[2];

		qglGetFloatv( GL_DEPTH_RANGE, oldDepthRange );
		qglDepthRange( 0, 1 );
		qglPushMatrix();
		GL_Bind( tr.whiteImage );
		qglEnable( GL_STENCIL_TEST );

		// the characters, which are not to be darkened, marked in the stencil
		if ( skipCasters ) {
			qglLoadMatrixf( backEnd.viewParms.world.modelMatrix );
			GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );
			qglColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
			RB_ShadowMarkCasters();
			qglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
		}

		qglLoadIdentity();
		if ( rayTraced ) {
			static std::vector<float> xyz;
			xyz.resize( shPts.size() * 3 );
			for ( size_t i = 0; i < shPts.size(); i++ ) {
				VectorCopy( shPts[i].xyz, &xyz[i * 3] );
			}
			RB_RayTracedShadowsApply( shRtEntries.data(), (int)shRtEntries.size(), xyz.data(), (int)shPts.size(), shRtTris.data(), (int)shRtTris.size() / 3,
				skipCasters ? SHADOW_STENCIL_SKIP : 0 );
		} else {
			RB_ShadowMapsApply( maps, numMaps, shAtlas, skipCasters ? SHADOW_STENCIL_SKIP : 0 );
		}

		// and the marks cleared again
		if ( skipCasters ) {
			const bool planeZeroBack = qglIsEnabled( GL_CLIP_PLANE0 ) != 0;
			if ( planeZeroBack ) {
				qglDisable( GL_CLIP_PLANE0 );
			}
			GL_Bind( tr.whiteImage );
			GL_Cull( CT_TWO_SIDED );
			qglEnable( GL_STENCIL_TEST );
			qglColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
			qglStencilMask( 0xFF );
			qglStencilFunc( GL_ALWAYS, 0, 0xFF );
			qglStencilOp( GL_ZERO, GL_ZERO, GL_ZERO );
			GL_State( GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ZERO );
			RB_ShadowScreenQuad();
			qglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
			if ( planeZeroBack ) {
				qglEnable( GL_CLIP_PLANE0 );
			}
		}

		qglStencilMask( 0xFF );
		qglStencilOp( GL_KEEP, GL_KEEP, GL_KEEP );
		qglStencilFunc( GL_ALWAYS, 0, 0xFF );
		qglDisable( GL_STENCIL_TEST );
		qglColor4f( 1, 1, 1, 1 );
		qglPopMatrix();
		qglDepthRange( oldDepthRange[0], oldDepthRange[1] );
	}

	RB_ShadowClear();
}
