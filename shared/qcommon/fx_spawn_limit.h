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

// fx_spawn_limit.h -- keeps the effects that a game module plays again every frame at the density they have at 125
// frames a second.
//
// Some effects are played by the game modules once per drawn frame for as long as something goes on (the glowing cone
// under a pickup in multiplayer, the sparks where two sabers touch), and every one lives on for a while. So how many
// there are at once grows with the frame rate: at 500 frames a second a 15 bot fight in multiplayer went over the
// 2047 entities a scene can have, and other things stopped being drawn. An effect asked for again at the same place
// less than 8 msec (with a msec of slack) after the last time it was played there is left out, so at any frame rate it
// is played about as often as at 125, the rate the game was made at. Nothing changes at 125 frames a second or fewer,
// and effects played more than once in the same frame (from several places) are all played.
//
// Header only, used by the effects scheduler of each game (the multiplayer engine, the single player cgame modules),
// after q_shared.h.

#pragma once

#include <string.h>

template <int MAX_IDS>
class CFxSpawnLimit
{
public:
	CFxSpawnLimit()
	{
		Clear();
	}

	void Clear()
	{
		memset( mSlots, 0, sizeof( mSlots ) );
	}

	// true if effect id is to be played at org (in the space of tag: a bolt or an entity it follows, or the world) at
	// time, the clock of the effects system in msec
	bool Allow( int id, const vec3_t org, int tag, int time )
	{
		if ( id < 0 || id >= MAX_IDS )
		{
			return true;
		}

		slot_t *slots = mSlots[id];
		slot_t *free = NULL;

		for ( int i = 0; i < SLOTS; i++ )
		{
			slot_t *s = &slots[i];

			if ( !s->used )
			{
				if ( !free || free->used )
				{
					free = s;	// (an unused one rather than one to take over)
				}
				continue;
			}
			if ( s->tag != tag || DistanceSquared( s->org, org ) > RADIUS * RADIUS )
			{
				if ( !free || ( free->used && s->lastTime < free->lastTime ) )
				{
					free = s;	// (the one used longest ago, if there is no unused one)
				}
				continue;
			}

			if ( time == s->lastTime )
			{
				return true;	// another one in the same frame
			}
			if ( time < s->lastTime )
			{
				// the clock started again (a new level)
				s->nextTime = time + INTERVAL;
			}
			else if ( time >= s->nextTime - SLACK )
			{
				// due: the next one is due one interval later, or one from now after a pause
				s->nextTime = ( time - s->nextTime < INTERVAL ) ? s->nextTime + INTERVAL : time + INTERVAL;
			}
			else
			{
				return false;
			}
			s->lastTime = time;
			VectorCopy( org, s->org );
			return true;
		}

		free->used = true;
		free->tag = tag;
		VectorCopy( org, free->org );
		free->lastTime = time;
		free->nextTime = time + INTERVAL;
		return true;
	}

private:
	enum
	{
		SLOTS		= 8,		// places one effect is followed at
		INTERVAL	= 8,		// msec, a frame at 125 frames a second
		SLACK		= 1,		// msec, so that the frames of 7 to 9 msec of a 125 cap are not cut
		RADIUS		= 16		// units: the same place, or as far as something moving fast goes in a frame
	};

	typedef struct
	{
		bool	used;
		int		tag;
		vec3_t	org;
		int		lastTime;		// when it was last played (msec)
		int		nextTime;		// when it is due again
	} slot_t;

	slot_t	mSlots[MAX_IDS][SLOTS];
};
