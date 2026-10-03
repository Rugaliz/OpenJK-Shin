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

// Part of the support for Jedi Outcast multiplayer: Ghoul2 instances for the game modules, which are QVM files
// (see g2_vmhandles.h)

#include "qcommon/qcommon.h"
#include "g2_vmhandles.h"

#include <map>

extern refexport_t *re;

static std::map<g2handle_t, CGhoul2Info_v *>	g2Handles;
static std::map<CGhoul2Info_v *, g2handle_t>	g2Pointers;
static g2handle_t								g2NextHandle = 1;

CGhoul2Info_v *G2VM_Get( g2handle_t handle ) {
	if ( handle == 0 ) {
		return NULL;
	}

	std::map<g2handle_t, CGhoul2Info_v *>::const_iterator it = g2Handles.find( handle );
	return it != g2Handles.end() ? it->second : NULL;
}

g2handle_t G2VM_GetHandle( CGhoul2Info_v *ghoul2 ) {
	if ( !ghoul2 ) {
		return 0;
	}

	std::map<CGhoul2Info_v *, g2handle_t>::const_iterator it = g2Pointers.find( ghoul2 );
	if ( it != g2Pointers.end() ) {
		return it->second;
	}

	// no handle that is still in use may be given out again
	while ( g2NextHandle <= 0 || g2Handles.find( g2NextHandle ) != g2Handles.end() ) {
		g2NextHandle++;
		if ( g2NextHandle <= 0 ) {
			g2NextHandle = 1;
		}
	}

	const g2handle_t handle = g2NextHandle++;
	g2Handles[handle] = ghoul2;
	g2Pointers[ghoul2] = handle;
	return handle;
}

static void G2VM_Forget( g2handle_t handle ) {
	std::map<g2handle_t, CGhoul2Info_v *>::iterator it = g2Handles.find( handle );
	if ( it != g2Handles.end() ) {
		g2Pointers.erase( it->second );
		g2Handles.erase( it );
	}
}

int G2VM_InitGhoul2Model( g2handle_t *handlePtr, const char *fileName, int modelIndex, qhandle_t customSkin, qhandle_t customShader, int modelFlags, int lodBias ) {
	CGhoul2Info_v *ghoul2 = G2VM_Get( *handlePtr );
	const CGhoul2Info_v *before = ghoul2;

	const int result = re->G2API_InitGhoul2Model( &ghoul2, fileName, modelIndex, customSkin, customShader, modelFlags, lodBias );

	if ( ghoul2 != before || *handlePtr == 0 ) {
		if ( before ) {
			G2VM_Forget( *handlePtr );
		}
		*handlePtr = G2VM_GetHandle( ghoul2 );
	}

	return result;
}

void G2VM_CleanGhoul2Models( g2handle_t *handlePtr ) {
	CGhoul2Info_v *ghoul2 = G2VM_Get( *handlePtr );

	if ( ghoul2 ) {
		re->G2API_CleanGhoul2Models( &ghoul2 );
		G2VM_Forget( *handlePtr );
	}
	*handlePtr = 0;
}

qboolean G2VM_HasGhoul2ModelOnIndex( const g2handle_t *handlePtr, int modelIndex ) {
	CGhoul2Info_v *ghoul2 = G2VM_Get( *handlePtr );

	if ( !ghoul2 ) {
		return qfalse;
	}

	return re->G2API_HasGhoul2ModelOnIndex( &ghoul2, modelIndex );
}

qboolean G2VM_RemoveGhoul2Model( g2handle_t *handlePtr, int modelIndex ) {
	CGhoul2Info_v *ghoul2 = G2VM_Get( *handlePtr );

	if ( !ghoul2 ) {
		return qfalse;
	}

	const qboolean result = re->G2API_RemoveGhoul2Model( &ghoul2, modelIndex );

	if ( !ghoul2 ) {
		// the renderer has deleted the instance, it was the last model
		G2VM_Forget( *handlePtr );
		*handlePtr = 0;
	}

	return result;
}

qboolean G2VM_HaveWeGhoul2Models( g2handle_t handle ) {
	CGhoul2Info_v *ghoul2 = G2VM_Get( handle );

	if ( !ghoul2 ) {
		return qfalse;
	}

	return re->G2API_HaveWeGhoul2Models( *ghoul2 );
}

// debugging aids of the modules (the list of the surfaces and of the bones of a model on the console): the interface of the
// renderer wants the model of the instance, which the engine cannot get at (CGhoul2Info_v is only known to the renderer)
void G2VM_ListSurfaces( g2handle_t handle, int modelIndex ) {
}

void G2VM_ListBones( g2handle_t handle, int modelIndex, int frame ) {
}

qboolean G2VM_GetBoltMatrix( g2handle_t handle, int modelIndex, int boltIndex, mdxaBone_t *matrix, const vec3_t angles, const vec3_t position, int frameNum, qhandle_t *modelList, vec3_t scale, qboolean reconstruct, qboolean spMethod ) {
	CGhoul2Info_v *ghoul2 = G2VM_Get( handle );

	if ( !ghoul2 ) {
		return qfalse;
	}

	if ( !reconstruct ) {
		re->G2API_BoltMatrixReconstruction( qfalse );
	}
	if ( spMethod ) {
		re->G2API_BoltMatrixSPMethod( qtrue );
	}

	return re->G2API_GetBoltMatrix( *ghoul2, modelIndex, boltIndex, matrix, angles, position, frameNum, modelList, scale );
}

void G2VM_CollisionDetect( CollisionRecord_t *collRecMap, g2handle_t handle, const vec3_t angles, const vec3_t position, int frameNumber, int entNum, vec3_t rayStart, vec3_t rayEnd, vec3_t scale, IHeapAllocator *G2VertSpace, int traceFlags, int useLod, float fRadius ) {
	CGhoul2Info_v *ghoul2 = G2VM_Get( handle );

	if ( !ghoul2 ) {
		return;
	}

	re->G2API_CollisionDetect( collRecMap, *ghoul2, angles, position, frameNumber, entNum, rayStart, rayEnd, scale, G2VertSpace, traceFlags, useLod, fRadius );
}

qboolean G2VM_SetBoneAngles( g2handle_t handle, int modelIndex, const char *boneName, const vec3_t angles, int flags, int up, int left, int forward, qhandle_t *modelList, int blendTime, int currentTime ) {
	CGhoul2Info_v *ghoul2 = G2VM_Get( handle );

	if ( !ghoul2 ) {
		return qfalse;
	}

	return re->G2API_SetBoneAngles( *ghoul2, modelIndex, boneName, angles, flags, (const Eorientations)up, (const Eorientations)left, (const Eorientations)forward, modelList, blendTime, currentTime );
}

qboolean G2VM_SetBoneAnim( g2handle_t handle, int modelIndex, const char *boneName, int startFrame, int endFrame, int flags, float animSpeed, int currentTime, float setFrame, int blendTime ) {
	CGhoul2Info_v *ghoul2 = G2VM_Get( handle );

	if ( !ghoul2 ) {
		return qfalse;
	}

	return re->G2API_SetBoneAnim( *ghoul2, modelIndex, boneName, startFrame, endFrame, flags, animSpeed, currentTime, setFrame, blendTime );
}

char *G2VM_GetGLAName( g2handle_t handle, int modelIndex ) {
	CGhoul2Info_v *ghoul2 = G2VM_Get( handle );

	if ( !ghoul2 ) {
		return NULL;
	}

	return re->G2API_GetGLAName( *ghoul2, modelIndex );
}

int G2VM_CopyGhoul2Instance( g2handle_t from, g2handle_t to, int modelIndex ) {
	CGhoul2Info_v *g2From = G2VM_Get( from );
	CGhoul2Info_v *g2To = G2VM_Get( to );

	if ( !g2From || !g2To ) {
		return -1;
	}

	return re->G2API_CopyGhoul2Instance( *g2From, *g2To, modelIndex );
}

void G2VM_CopySpecificG2Model( g2handle_t from, int modelFrom, g2handle_t to, int modelTo ) {
	CGhoul2Info_v *g2From = G2VM_Get( from );
	CGhoul2Info_v *g2To = G2VM_Get( to );

	if ( !g2From || !g2To ) {
		return;
	}

	re->G2API_CopySpecificG2Model( *g2From, modelFrom, *g2To, modelTo );
}

void G2VM_DuplicateGhoul2Instance( g2handle_t from, g2handle_t *toPtr ) {
	CGhoul2Info_v *g2From = G2VM_Get( from );
	CGhoul2Info_v *g2To = G2VM_Get( *toPtr );

	if ( !g2From ) {
		return;
	}

	if ( g2To ) {
		// the renderer does not want a destination that is in use
		return;
	}

	re->G2API_DuplicateGhoul2Instance( *g2From, &g2To );
	*toPtr = G2VM_GetHandle( g2To );
}

int G2VM_AddBolt( g2handle_t handle, int modelIndex, const char *boneName ) {
	CGhoul2Info_v *ghoul2 = G2VM_Get( handle );

	if ( !ghoul2 ) {
		return -1;
	}

	return re->G2API_AddBolt( *ghoul2, modelIndex, boneName );
}

void G2VM_SetBoltInfo( g2handle_t handle, int modelIndex, int boltInfo ) {
	CGhoul2Info_v *ghoul2 = G2VM_Get( handle );

	if ( ghoul2 ) {
		re->G2API_SetBoltInfo( *ghoul2, modelIndex, boltInfo );
	}
}

qboolean G2VM_SetRootSurface( g2handle_t handle, int modelIndex, const char *surfaceName ) {
	CGhoul2Info_v *ghoul2 = G2VM_Get( handle );

	if ( !ghoul2 ) {
		return qfalse;
	}

	return re->G2API_SetRootSurface( *ghoul2, modelIndex, surfaceName );
}

qboolean G2VM_SetSurfaceOnOff( g2handle_t handle, const char *surfaceName, int flags ) {
	CGhoul2Info_v *ghoul2 = G2VM_Get( handle );

	if ( !ghoul2 ) {
		return qfalse;
	}

	return re->G2API_SetSurfaceOnOff( *ghoul2, surfaceName, flags );
}

qboolean G2VM_SetNewOrigin( g2handle_t handle, int boltIndex ) {
	CGhoul2Info_v *ghoul2 = G2VM_Get( handle );

	if ( !ghoul2 ) {
		return qfalse;
	}

	return re->G2API_SetNewOrigin( *ghoul2, boltIndex );
}
