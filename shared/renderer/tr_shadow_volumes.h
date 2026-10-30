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
static std::vector<int>			shCaps;		// the triangles facing the light
static std::vector<int>			shWalls;	// directed edges, 2 point indices each
static float					shStrengthSum;
static int						shCasters;

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
	shCaps.clear();
	shWalls.clear();
	shStrengthSum = 0.0f;
	shCasters = 0;
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

How far the shadow leans for each unit of height, from the light direction at the entity
=================
*/
static void RB_ShadowTilt( const trRefEntity_t *ent, float *tx, float *ty )
{
	float lz = ent->lightDir[2];
	if ( lz < 0.35f ) {
		lz = 0.35f;
	}

	float x = ent->lightDir[0] / lz * r_shadowTilt->value;
	float y = ent->lightDir[1] / lz * r_shadowTilt->value;
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

	if ( glConfig.stencilBits < 4 || r_shadows->integer != 2 || !ent || numVerts < 3 || numTris < 1 ) {
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

	// how much of the light comes from a direction, which is what a shadow takes away
	const float directed = Q_max( ent->directedLight[0], Q_max( ent->directedLight[1], ent->directedLight[2] ) );
	const float ambient = Q_max( ent->ambientLight[0], Q_max( ent->ambientLight[1], ent->ambientLight[2] ) );
	shStrengthSum += ( directed + ambient > 1.0f ) ? directed / ( directed + ambient ) : 0.5f;
	shCasters++;
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
void RB_ShadowFinish( void )
{
	if ( r_shadows->integer != 2 || glConfig.stencilBits < 4 || shCaps.empty() ) {
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
