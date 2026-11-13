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

// g_corpses.h -- how many bodies stay (single player, Jedi Outcast and Jedi Academy; included by each game's NPC.cpp)
//
// The original removed a body some seconds after the death (10 in both games), once the player did not see it, and
// the limbs cut off 5 to 15 seconds after they came to rest. Now (g_corpseRemovalTime 0, the default) they stay, and
// only a number of them is kept: past g_corpseLimit bodies, or when the entities run short (every body, limb and saber
// is one of 1024, and with none free the game stops: "G_Spawn: no free entities"), the bodies furthest from the player
// go first, with the limbs cut from them, where the player does not see them, as the original did. Only when the
// entities are nearly all used do they go where he sees them too. Furthest rather than oldest: when a body died is not
// kept (NPC->timeOfDeath is used for other waits after it), and the furthest are the ones the player has left behind.
//
// The including file defines, before it:
//   static bool G_CorpseHeld( gentity_t *body )	-- the body must stay for now (a key, a script, held by a creature...)
//   static bool G_CorpseInPlayerFOV( gentity_t *ent )	-- in the player's view (as the original removal tests it)
// and has REMOVE_DISTANCE_SQR and G_ClearLOS.

#pragma once

#include <stdlib.h>

#define CORPSE_FREE_RESERVE		128		// entities kept free for shots, effects and what scripts spawn
#define CORPSE_FREE_CRITICAL	48		// fewer free than this: bodies go even where the player sees them

extern cvar_t	*g_corpseRemovalTime;
extern cvar_t	*g_corpseLimit;

typedef struct {
	gentity_t	*ent;
	float		distSqr;
} corpseEntry_t;

static int G_CorpseFurthestFirst( const void *a, const void *b )
{
	const float da = ( (const corpseEntry_t *)a )->distSqr, db = ( (const corpseEntry_t *)b )->distSqr;
	return ( da < db ) ? 1 : ( da > db ) ? -1 : 0;
}

// a dead NPC's body that may be taken away now
static bool G_CorpseRemovable( gentity_t *ent )
{
	if ( !ent->inuse || !ent->client || !ent->NPC || ent->s.number == 0 || ent->health > 0 )
		return false;
	if ( ent->e_ThinkFunc == thinkF_G_FreeEntity || ( ent->client->ps.eFlags & EF_NODRAW ) )
		return false;	// (going already)
	if ( !ent->enemy )
		return false;	// placed in the level as a body: never taken away (as in the original)
	if ( ent->client->NPC_class == CLASS_GALAKMECH )
		return false;
	return !G_CorpseHeld( ent );
}

// where the player sees it, or is next to it (the original's test)
static bool G_CorpseSeen( gentity_t *ent )
{
	gentity_t *player = &g_entities[0];
	if ( !player->inuse || !player->client )
		return false;
	if ( DistanceSquared( player->currentOrigin, ent->currentOrigin ) <= REMOVE_DISTANCE_SQR )
		return true;
	if ( !G_CorpseInPlayerFOV( ent ) )
		return false;
	vec3_t eyes;
	CalcEntitySpot( player, SPOT_HEAD_LEAN, eyes );
	return G_ClearLOS( player, eyes, ent->currentOrigin ) != qfalse;
}

// takes the body away, with its saber and the limbs cut from it; how many entities that freed
static int G_RemoveCorpse( gentity_t *body )
{
	int freed = 0;
	for ( int i = 1; i < globals.num_entities; i++ )
	{
		gentity_t *limb = &g_entities[i];
		if ( limb->inuse && limb->owner == body && limb->classname && !Q_stricmp( limb->classname, "limb" ) )
		{
			G_FreeEntity( limb );
			freed++;
		}
	}
	const int saberNum = body->client ? body->client->ps.saberEntityNum : 0;
	if ( saberNum > 0 && saberNum < ENTITYNUM_WORLD && g_entities[saberNum].inuse && g_entities[saberNum].owner == body )
	{
		G_FreeEntity( &g_entities[saberNum] );
		freed++;
	}
	G_FreeEntity( body );
	return freed + 1;
}

/*
================
G_ManageCorpses

Once a second (G_RunFrame): takes bodies away when there are more than g_corpseLimit (0: no limit) or too few free
entities, and then limbs whose body is gone, when the entities are still short.
================
*/
static inline void G_ManageCorpses( void )
{
	static int lastCheck = -1000000;
	if ( level.time >= lastCheck && level.time < lastCheck + 1000 )
		return;
	lastCheck = level.time;

	static corpseEntry_t bodies[MAX_GENTITIES], limbs[MAX_GENTITIES];
	int numBodies = 0, numLimbs = 0, used = 0;
	const vec_t *eye = g_entities[0].currentOrigin;

	for ( int i = 0; i < globals.num_entities; i++ )
	{
		gentity_t *ent = &g_entities[i];
		if ( !ent->inuse )
			continue;
		used++;
		if ( G_CorpseRemovable( ent ) )
		{
			bodies[numBodies].ent = ent;
			bodies[numBodies++].distSqr = DistanceSquared( eye, ent->currentOrigin );
		}
		else if ( ent->classname && !Q_stricmp( ent->classname, "limb" ) && ent->e_ThinkFunc != thinkF_G_FreeEntity &&
				  ( !ent->owner || !ent->owner->inuse || ent->owner->health > 0 ) )
		{	// (a limb whose body is gone, or still alive)
			limbs[numLimbs].ent = ent;
			limbs[numLimbs++].distSqr = DistanceSquared( eye, ent->currentOrigin );
		}
	}

	const int freeEntities = ENTITYNUM_MAX_NORMAL - used;
	int extraBodies = ( g_corpseLimit && g_corpseLimit->integer > 0 ) ? numBodies - g_corpseLimit->integer : 0;
	int missing = CORPSE_FREE_RESERVE - freeEntities;
	if ( extraBodies <= 0 && missing <= 0 )
		return;
	const bool critical = freeEntities < CORPSE_FREE_CRITICAL;

	qsort( bodies, numBodies, sizeof( bodies[0] ), G_CorpseFurthestFirst );
	for ( int i = 0; i < numBodies && ( extraBodies > 0 || missing > 0 ); i++ )
	{
		if ( !critical && G_CorpseSeen( bodies[i].ent ) )
			continue;
		missing -= G_RemoveCorpse( bodies[i].ent );
		extraBodies--;
	}

	if ( missing > 0 )
	{
		qsort( limbs, numLimbs, sizeof( limbs[0] ), G_CorpseFurthestFirst );
		for ( int i = 0; i < numLimbs && missing > 0; i++ )
		{
			if ( !limbs[i].ent->inuse || ( !critical && G_CorpseSeen( limbs[i].ent ) ) )
				continue;
			G_FreeEntity( limbs[i].ent );
			missing--;
		}
	}
}
