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

#pragma once

// Part of the support for Jedi Outcast multiplayer.
//
// The game modules of Jedi Outcast are QVM files, which cannot hold a pointer, so for a Ghoul2 instance they hold
// an integer (g2handle_t, 0 = none). The renderer wants a CGhoul2Info_v pointer (the interface of the renderer is the
// one of Jedi Academy). This table, shared by the game module on the server and the cgame module on the client,
// translates between the two. The G2VM_ functions are the calls the modules make, with a handle in place of the pointer.

#include "qcommon/q_shared.h"
#include "rd-common/tr_public.h"

// the Ghoul2 instance for a handle, NULL if there is none
CGhoul2Info_v	*G2VM_Get( g2handle_t handle );

// the handle for a Ghoul2 instance of the renderer (a new one the first time), 0 for NULL
g2handle_t		G2VM_GetHandle( CGhoul2Info_v *ghoul2 );

int				G2VM_InitGhoul2Model( g2handle_t *handlePtr, const char *fileName, int modelIndex, qhandle_t customSkin, qhandle_t customShader, int modelFlags, int lodBias );
void			G2VM_CleanGhoul2Models( g2handle_t *handlePtr );
qboolean		G2VM_HasGhoul2ModelOnIndex( const g2handle_t *handlePtr, int modelIndex );
qboolean		G2VM_RemoveGhoul2Model( g2handle_t *handlePtr, int modelIndex );
qboolean		G2VM_HaveWeGhoul2Models( g2handle_t handle );
void			G2VM_ListSurfaces( g2handle_t handle, int modelIndex );
void			G2VM_ListBones( g2handle_t handle, int modelIndex, int frame );

// reconstruct: the normal bolt matrix, qfalse = do not reconstruct; spMethod = the way single player does it (no rotation)
qboolean		G2VM_GetBoltMatrix( g2handle_t handle, int modelIndex, int boltIndex, mdxaBone_t *matrix, const vec3_t angles, const vec3_t position, int frameNum, qhandle_t *modelList, vec3_t scale, qboolean reconstruct, qboolean spMethod );

void			G2VM_CollisionDetect( CollisionRecord_t *collRecMap, g2handle_t handle, const vec3_t angles, const vec3_t position, int frameNumber, int entNum, vec3_t rayStart, vec3_t rayEnd, vec3_t scale, IHeapAllocator *G2VertSpace, int traceFlags, int useLod, float fRadius );
qboolean		G2VM_SetBoneAngles( g2handle_t handle, int modelIndex, const char *boneName, const vec3_t angles, int flags, int up, int left, int forward, qhandle_t *modelList, int blendTime, int currentTime );
qboolean		G2VM_SetBoneAnim( g2handle_t handle, int modelIndex, const char *boneName, int startFrame, int endFrame, int flags, float animSpeed, int currentTime, float setFrame, int blendTime );
char			*G2VM_GetGLAName( g2handle_t handle, int modelIndex );
int				G2VM_CopyGhoul2Instance( g2handle_t from, g2handle_t to, int modelIndex );
void			G2VM_CopySpecificG2Model( g2handle_t from, int modelFrom, g2handle_t to, int modelTo );
void			G2VM_DuplicateGhoul2Instance( g2handle_t from, g2handle_t *toPtr );
int				G2VM_AddBolt( g2handle_t handle, int modelIndex, const char *boneName );
void			G2VM_SetBoltInfo( g2handle_t handle, int modelIndex, int boltInfo );
qboolean		G2VM_SetRootSurface( g2handle_t handle, int modelIndex, const char *surfaceName );
qboolean		G2VM_SetSurfaceOnOff( g2handle_t handle, const char *surfaceName, int flags );
qboolean		G2VM_SetNewOrigin( g2handle_t handle, int boltIndex );
