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

// g_lagcomp.c -- lag compensation (g_lagCompensation, see docs/network.md)
//
// A player sees the others where they were a little while ago: the snapshot they are drawn from took as long as the
// network does to arrive, and the client draws a little behind the newest one. Their commands, and the shots in them,
// take as long again to reach the server. Without lag compensation the server decides a shot against where everybody
// is when it arrives, so with a ping of 100 a running target has moved on by a body's width or more and the shooter
// has to aim ahead of what they see.
//
// Each command carries the moment of the game its player was looking at (usercmd_t serverTime, the client's view of
// the server's time). The server keeps where every player was at the end of each of its frames for the last second
// (G_LagComp_Record), and for a shot moves the others back to where the shooter saw them (G_LagComp_Rewind), decides
// the shot, and puts them back (G_LagComp_Restore):
//
// - g_lagCompensation 1 (the default): the weapons that hit at once (the disruptor, the beam of the concussion rifle,
//   the stun baton, punches): FireWeapon
//
// How far back it goes is limited by g_lagCompensationMax (msec), so a very high ping can't shoot somebody who has long
// since got away. Players are moved whole: their bounding box, their position and the way they face (which places the
// model the model accurate hit tests use); the pose of the model is the current one. Sabers are not compensated: both
// fighters' blades and poses would have to go back, and a hit the defender saw blocked could land.

#include "g_local.h"

#define LAGCOMP_FRAMES			64		// frames remembered (1.6 seconds at 40 a second)
#define LAGCOMP_TELEPORT		256.0f	// units moved in one frame that are not movement (a respawn, a teleport)

typedef struct {
	int			time;		// level.time of the frame
	vec3_t		origin;		// where the clients drew the player at that time
	vec3_t		mins, maxs;
	float		yaw;
	qboolean	valid;		// the player could be hit
	qboolean	jumped;		// moved too far since the frame before to be drawn in between
} lagCompFrame_t;

static struct {
	lagCompFrame_t	frames[LAGCOMP_FRAMES];
	int				head;	// where the next frame goes
	int				count;
} lagHistory[MAX_CLIENTS];

static struct {
	qboolean	moved;
	vec3_t		origin, mins, maxs;
	float		yaw;
} lagSaved[MAX_CLIENTS];

static qboolean	lagRewound;

static qboolean G_LagComp_CanBeHit( const gentity_t *ent ) {
	const gclient_t *client = ent->client;

	return (qboolean)( ent->inuse && client && client->pers.connected == CON_CONNECTED
		&& client->sess.sessionTeam != TEAM_SPECTATOR && client->ps.pm_type != PM_SPECTATOR
		&& client->ps.pm_type != PM_DEAD && client->ps.stats[STAT_HEALTH] > 0
		&& ent->r.contents && !client->ps.m_iVehicleNum );
}

/*
================
G_LagComp_Record

At the end of every frame: where each player is drawn (the snapshot about to be sent has them there).
================
*/
void G_LagComp_Record( void ) {
	int i;

	for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ ) {
		gentity_t *ent = &g_entities[i];
		lagCompFrame_t *frame = &lagHistory[i].frames[lagHistory[i].head];

		if ( !ent->inuse || !ent->client ) {
			lagHistory[i].count = 0;
			continue;
		}

		frame->time = level.time;
		BG_EvaluateTrajectory( &ent->s.pos, level.time, frame->origin );	// (with g_smoothClients it runs on a little)
		VectorCopy( ent->r.mins, frame->mins );
		VectorCopy( ent->r.maxs, frame->maxs );
		frame->yaw = ent->s.apos.trBase[YAW];
		frame->valid = G_LagComp_CanBeHit( ent );
		frame->jumped = qtrue;
		if ( lagHistory[i].count ) {
			const lagCompFrame_t *prev = &lagHistory[i].frames[( lagHistory[i].head + LAGCOMP_FRAMES - 1 ) % LAGCOMP_FRAMES];
			frame->jumped = (qboolean)( !prev->valid || Distance( prev->origin, frame->origin ) > LAGCOMP_TELEPORT );
		}

		lagHistory[i].head = ( lagHistory[i].head + 1 ) % LAGCOMP_FRAMES;
		if ( lagHistory[i].count < LAGCOMP_FRAMES ) {
			lagHistory[i].count++;
		}
	}
}

// The moment the command of a player that is being run now was looking at, limited to how far back may be gone
static int G_LagComp_TimeOf( const gentity_t *shooter ) {
	int maxBack = g_lagCompensationMax.integer;
	int time;

	if ( !shooter || !shooter->client || shooter->s.number >= MAX_CLIENTS || ( shooter->r.svFlags & SVF_BOT ) ) {
		return level.time;	// (bots and the like see the game as it is)
	}

	if ( maxBack < 0 ) {
		maxBack = 0;
	} else if ( maxBack > 1000 ) {
		maxBack = 1000;
	}
	time = shooter->client->pers.cmd.serverTime;
	if ( time < level.time - maxBack ) {
		time = level.time - maxBack;
	}
	return time;
}

/*
================
G_LagComp_RewindTo

Moves every player but skip to where they were drawn at time, until G_LagComp_Restore.
================
*/
void G_LagComp_RewindTo( int time, const gentity_t *skip ) {
	int i;

	if ( lagRewound ) {
		return;		// (already, by whoever started it)
	}

	for ( i = 0; i < level.maxclients && i < MAX_CLIENTS; i++ ) {
		gentity_t *ent = &g_entities[i];
		const lagCompFrame_t *a = NULL, *b = NULL;
		vec3_t origin;
		float yaw;
		int k;

		lagSaved[i].moved = qfalse;
		if ( ent == skip || !G_LagComp_CanBeHit( ent ) || !lagHistory[i].count ) {
			continue;
		}

		// the frames on either side of time (newest first)
		for ( k = 0; k < lagHistory[i].count; k++ ) {
			const lagCompFrame_t *frame = &lagHistory[i].frames[( lagHistory[i].head + LAGCOMP_FRAMES - 1 - k ) % LAGCOMP_FRAMES];
			if ( frame->time <= time ) {
				a = frame;
				break;
			}
			b = frame;
		}

		if ( !b ) {
			continue;		// as recent as what was recorded last: where it is now is near enough
		}
		if ( !a ) {
			a = b;			// further back than remembered: the oldest
		}
		if ( !a->valid ) {
			continue;		// it couldn't be hit then (dead, not there yet)
		}

		if ( a == b || b->jumped || b->time <= a->time ) {
			VectorCopy( a->origin, origin );
			yaw = a->yaw;
		} else {
			const float frac = (float)( time - a->time ) / (float)( b->time - a->time );
			origin[0] = a->origin[0] + frac * ( b->origin[0] - a->origin[0] );
			origin[1] = a->origin[1] + frac * ( b->origin[1] - a->origin[1] );
			origin[2] = a->origin[2] + frac * ( b->origin[2] - a->origin[2] );
			yaw = LerpAngle( a->yaw, b->yaw, frac );
		}

		lagSaved[i].moved = qtrue;
		VectorCopy( ent->r.currentOrigin, lagSaved[i].origin );
		VectorCopy( ent->r.mins, lagSaved[i].mins );
		VectorCopy( ent->r.maxs, lagSaved[i].maxs );
		lagSaved[i].yaw = ent->s.apos.trBase[YAW];

		VectorCopy( origin, ent->r.currentOrigin );
		VectorCopy( a->mins, ent->r.mins );
		VectorCopy( a->maxs, ent->r.maxs );
		ent->s.apos.trBase[YAW] = yaw;
		trap->LinkEntity( (sharedEntity_t *)ent );
	}

	lagRewound = qtrue;
}

// Moves everybody back to where the command of shooter, which is being run now, saw them
void G_LagComp_Rewind( gentity_t *shooter ) {
	const int time = G_LagComp_TimeOf( shooter );

	if ( time < level.time ) {
		G_LagComp_RewindTo( time, shooter );
	}
}

// Puts everybody back where they are
void G_LagComp_Restore( void ) {
	int i;

	if ( !lagRewound ) {
		return;
	}

	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		gentity_t *ent = &g_entities[i];

		if ( !lagSaved[i].moved ) {
			continue;
		}
		VectorCopy( lagSaved[i].origin, ent->r.currentOrigin );
		VectorCopy( lagSaved[i].mins, ent->r.mins );
		VectorCopy( lagSaved[i].maxs, ent->r.maxs );
		ent->s.apos.trBase[YAW] = lagSaved[i].yaw;
		trap->LinkEntity( (sharedEntity_t *)ent );
		lagSaved[i].moved = qfalse;
	}

	lagRewound = qfalse;
}

/*
================
G_LagComp_FireStart / G_LagComp_FireEnd

Around FireWeapon: the weapons that hit at once are decided with everybody rewound.
================
*/
void G_LagComp_FireStart( gentity_t *ent, qboolean altFire ) {
	if ( g_lagCompensation.integer <= 0 || !ent || !ent->client || ent->s.number >= MAX_CLIENTS ) {
		return;
	}

	switch ( ent->s.weapon ) {
	case WP_DISRUPTOR:
	case WP_STUN_BATON:
	case WP_MELEE:
		G_LagComp_Rewind( ent );
		break;
	case WP_CONCUSSION:
		if ( altFire ) {
			G_LagComp_Rewind( ent );	// (the beam; its main fire is a projectile)
		}
		break;
	default:
		break;
	}
}

void G_LagComp_FireEnd( void ) {
	G_LagComp_Restore();
}

// A player connected or left: forget where they were
void G_LagComp_ResetClient( int clientNum ) {
	if ( clientNum >= 0 && clientNum < MAX_CLIENTS ) {
		lagHistory[clientNum].count = 0;
		lagHistory[clientNum].head = 0;
	}
}
