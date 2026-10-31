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

// snd_dma_effects.h -- the part of the sound effects that is worked out once a frame: binaural positioning (s_hrtf,
// see docs/hrtf.md) and the sound of the room (s_reverb, see docs/reverb.md). Which sounds are positioned
// binaurally and from where, how much each sends to the reverb, what the room is like, which sounds have a wall in
// the way. The mixing side is in snd_mix_effects.h, the filters in snd_hrtf.cpp and snd_reverb.cpp.
//
// Included once by the snd_dma.cpp of each engine (single player and multiplayer), after snd_local.h, cm_public.h
// and the declarations of s_channels, MAX_LOOP_SOUNDS, listener_origin and s_separation. In S_Respatialize the engine
// calls S_Effects_BeginFrame, then for every sound one of S_Effects_ListenerChannel, S_Effects_PlacedChannel or
// S_Effects_LoopChannel, then S_Effects_EndFrame.

#pragma once

cvar_t *s_hrtf;
cvar_t *s_reverb;
cvar_t *s_reverbLevel;
qboolean s_reverbActive = qfalse;
float s_underwater = 0.0f;
static int s_reverbRate = 0;		// the rate the reverb is set up for (0 = not at all, or to be cleared)

static void S_Effects_Register( void )
{
	s_hrtf              = Cvar_Get( "s_hrtf",              "0",       CVAR_ARCHIVE );	// 1 = binaural positioning for headphones
	s_reverb            = Cvar_Get( "s_reverb",            "1",       CVAR_ARCHIVE );	// the sound of the room: reverb, walls, water
	s_reverbLevel       = Cvar_Get( "s_reverbLevel",       "1",       CVAR_ARCHIVE );	// how much reverb (1 = as worked out from the room)
}

// All sounds were stopped: the tail of the last room is cleared when the reverb is next set up
static void S_Effects_StopAll( void )
{
	s_reverbRate = 0;
}

static void S_EffectsTrace( trace_t *trace, const vec3_t start, const vec3_t end )
{
#ifdef _JK2EXE
	CM_BoxTrace( trace, start, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID );
#else
	CM_BoxTrace( trace, start, end, vec3_origin, vec3_origin, 0, CONTENTS_SOLID, qfalse );	// (qfalse = not a capsule)
#endif
}

/*
===============================================================================

BINAURAL POSITIONING (s_hrtf)

===============================================================================
*/
// Binaural positioning (s_hrtf) is in use this frame. Set by S_Effects_BeginFrame.
static qboolean	s_hrtfActive = qfalse;

// One shot sounds keep their filter history here, per channel; looping sounds (whose channels are cleared every frame)
// in a table by sfx, so the history carries on from frame to frame
static hrtfState_t	s_hrtfPool[MAX_CHANNELS];

#define MAX_HRTF_LOOPS	( MAX_LOOP_SOUNDS + 16 )	// every looping sound can have a channel, and some just stopped
static struct
{
	const sfx_t	*sfx;
	int			lastFrame;
	hrtfState_t	state;
} s_hrtfLoops[MAX_HRTF_LOOPS];

// direction (x right, y ahead, z up, as given by S_SpatializeOrigin) to azimuth and elevation in degrees
static void S_HrtfAngles( const vec3_t dir, float *azimuth, float *elevation )
{
	const float len = sqrtf( dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2] );

	*azimuth = 0.0f;
	*elevation = 0.0f;
	if ( len < 0.0001f )
	{
		return;	// right on top of the listener, call it straight ahead
	}

	float up = dir[2] / len;
	if ( up > 1.0f )		up = 1.0f;
	else if ( up < -1.0f )	up = -1.0f;

	*elevation = asinf( up ) * (180.0f / M_PI);
	if ( fabsf( dir[0] ) > 0.0001f || fabsf( dir[1] ) > 0.0001f )
	{
		*azimuth = atan2f( dir[0], dir[1] ) * (180.0f / M_PI);
	}
}

static void S_HrtfSetupChannel( channel_t *ch, int channelNum, const vec3_t dir )
{
	ch->hrtf = qtrue;
	S_HrtfAngles( dir, &ch->hrtfAzimuth, &ch->hrtfElevation );
	ch->pHrtfState = &s_hrtfPool[channelNum];
	if ( !ch->hrtfInit )
	{
		S_HRTF_ResetState( ch->pHrtfState );
		ch->hrtfInit = qtrue;
	}
}

// Binaural filtering costs about ten times what plain panning does, for every sound it is used on, so it is kept for
// the loudest sounds: a quiet sound in a crowd of them is no worse for being panned. (Sounds that were filtered
// binaurally count for a little more, so a sound doesn't keep changing between the two as volumes go up and down.)
#define HRTF_MAX_SOUNDS	32

static int S_HrtfCompare( const void *a, const void *b )
{
	const float pa = ( *(channel_t * const *)a )->hrtfKept ? 1.5f * ( *(channel_t * const *)a )->leftvol : (float)( *(channel_t * const *)a )->leftvol;
	const float pb = ( *(channel_t * const *)b )->hrtfKept ? 1.5f * ( *(channel_t * const *)b )->leftvol : (float)( *(channel_t * const *)b )->leftvol;
	return ( pa > pb ) ? -1 : ( pa < pb ) ? 1 : 0;
}

static void S_HrtfLimit( void )
{
	channel_t	*list[MAX_CHANNELS];
	int			n = 0;
	int			i;
	channel_t	*ch;

	for ( i = 0, ch = s_channels; i < MAX_CHANNELS; i++, ch++ )
	{
		if ( ch->thesfx && ch->hrtf )
			list[n++] = ch;
	}

	if ( n > HRTF_MAX_SOUNDS )
	{
		qsort( list, n, sizeof( list[0] ), S_HrtfCompare );

		// the rest are panned, as they would be without binaural positioning (S_SpatializeOrigin gave the same volume
		// to both sides, the distance only)
		const float sep = s_separation->value;
		for ( i = HRTF_MAX_SOUNDS; i < n; i++ )
		{
			ch = list[i];
			const float right = sinf( DEG2RAD( ch->hrtfAzimuth ) ) * cosf( DEG2RAD( ch->hrtfElevation ) );
			float rscale = sep + ( 1.0f - sep ) * right;
			float lscale = sep - ( 1.0f - sep ) * right;
			if ( rscale < 0.0f )	rscale = 0.0f;
			if ( lscale < 0.0f )	lscale = 0.0f;

			const int vol = ch->leftvol;
			ch->leftvol = (int)( vol * lscale );
			ch->rightvol = (int)( vol * rscale );
			ch->hrtf = qfalse;
		}
	}

	for ( i = 0, ch = s_channels; i < MAX_CHANNELS; i++, ch++ )
		ch->hrtfKept = (qboolean)( ch->thesfx && ch->hrtf );
}

static void S_HrtfSetupLoop( channel_t *ch, const sfx_t *sfx, const vec3_t dir, int frame )
{
	int i, use = -1, oldest = 0;

	for ( i = 0; i < MAX_HRTF_LOOPS; i++ )
	{
		if ( s_hrtfLoops[i].sfx == sfx && s_hrtfLoops[i].lastFrame >= frame - 2 )
		{
			use = i;	// carried on from the last frames
			break;
		}
		if ( s_hrtfLoops[i].lastFrame < s_hrtfLoops[oldest].lastFrame )
		{
			oldest = i;
		}
	}
	if ( use < 0 )
	{
		use = oldest;
		s_hrtfLoops[use].sfx = sfx;
		S_HRTF_ResetState( &s_hrtfLoops[use].state );
	}
	s_hrtfLoops[use].lastFrame = frame;

	ch->hrtf = qtrue;
	S_HrtfAngles( dir, &ch->hrtfAzimuth, &ch->hrtfElevation );
	ch->pHrtfState = &s_hrtfLoops[use].state;
}

/*
===============================================================================

THE SOUND OF THE ROOM (s_reverb)

How a sound is heard depends on where it is: in a big metal hangar it echoes for seconds, in a small padded room it
doesn't at all, outdoors there is hardly any reverb. Rays are sent out from the listener in all directions through the
collision data of the level, and what they hit (how far, and what it is made of) says how long the reverb of the
room is and how loud. The reverb itself is in snd_reverb.cpp, and the sounds are mixed into it in snd_mix_effects.h.

===============================================================================
*/
#define ROOM_RAYS			26
#define ROOM_RAY_LENGTH		3000.0f		// how far a ray looks (units)
#define UNIT_TO_METERS		0.03f		// how long a unit is, about (a person is 2 metres, 56 units + a bit)
#define SPEED_OF_SOUND		343.0f		// metres per second
#define ROOM_UP				0.35f		// rays pointing at least this much upwards (the sine of the angle) look for a ceiling
#define ROOM_CEILING_DISTANCE	25.0f	// (metres) a ceiling further away than this is not one that makes a room

// Per material, how much of the sound a surface of it soaks up at the middle frequencies (0 to 1)
static const float s_materialAbsorption[MATERIAL_LAST] =
{
	0.10f,	// MATERIAL_NONE
	0.10f,	// SOLIDWOOD
	0.15f,	// HOLLOWWOOD
	0.04f,	// SOLIDMETAL
	0.06f,	// HOLLOWMETAL
	0.30f,	// SHORTGRASS
	0.40f,	// LONGGRASS
	0.20f,	// DIRT
	0.30f,	// SAND
	0.25f,	// GRAVEL
	0.04f,	// GLASS
	0.03f,	// CONCRETE
	0.02f,	// MARBLE
	0.02f,	// WATER
	0.50f,	// SNOW
	0.03f,	// ICE
	0.30f,	// FLESH
	0.25f,	// MUD
	0.04f,	// BPGLASS
	0.40f,	// DRYLEAVES
	0.40f,	// GREENLEAVES
	0.50f,	// FABRIC
	0.35f,	// CANVAS
	0.05f,	// ROCK
	0.20f,	// RUBBER
	0.06f,	// PLASTIC
	0.03f,	// TILES
	0.45f,	// CARPET
	0.08f,	// PLASTER
	0.04f,	// SHATTERGLASS
	0.06f,	// ARMOR
	0.15f,	// COMPUTER
};

// ... and how much it soaks up the high frequencies more than that (0 to 1): soft things take the highs first
static const float s_materialSoftness[MATERIAL_LAST] =
{
	0.2f, 0.2f, 0.3f, 0.0f, 0.1f, 0.8f, 0.9f, 0.5f, 0.7f, 0.4f, 0.0f, 0.0f, 0.0f, 0.1f, 1.0f, 0.0f,
	0.8f, 0.6f, 0.0f, 0.9f, 0.9f, 1.0f, 0.8f, 0.1f, 0.5f, 0.1f, 0.0f, 1.0f, 0.4f, 0.0f, 0.1f, 0.3f,
};

static vec3_t	s_roomRays[ROOM_RAYS];
static qboolean	s_roomRaysMade = qfalse;
static int		s_roomNextAnalysis = 0;

// Gets the reverb ready for the output rate (the first time it is wanted, or after the sound was restarted)
static qboolean S_ReverbPrepare( void )
{
	if ( s_reverbRate != dma.speed )
	{
		S_Reverb_Init( dma.speed );
		S_Mix_ResetReverb();
		s_reverbRate = dma.speed;
		s_roomNextAnalysis = 0;		// look at the room at once, and be in it at once
		Com_DPrintf( "Sound: reverb ready at %d Hz\n", dma.speed );
	}
	return qtrue;
}

static void S_MakeRoomRays( void )
{
	// evenly spread over a sphere (a Fibonacci spiral), the same every time so turning round makes no difference
	for ( int i = 0; i < ROOM_RAYS; i++ )
	{
		const float z = 1.0f - 2.0f * ( i + 0.5f ) / ROOM_RAYS;
		const float r = sqrtf( 1.0f - z * z );
		const float phi = i * 2.3999632f;	// the golden angle

		s_roomRays[i][0] = r * cosf( phi );
		s_roomRays[i][1] = r * sinf( phi );
		s_roomRays[i][2] = z;
	}
	s_roomRaysMade = qtrue;
}

/*
===============
S_AnalyseRoom

Works out what the reverb should be like for where the listener is, and sets it (gradually, unless "immediately").
===============
*/
static void S_AnalyseRoom( const vec3_t head, qboolean inwater, qboolean immediately )
{
	reverbParams_t params;
	const float level = ( s_reverbLevel->value < 0.0f ) ? 0.0f : s_reverbLevel->value;

	if ( inwater )
	{
		// underwater everything is dull and close, whatever the room
		params.rt60 = 1.2f;
		params.hfRatio = 0.2f;
		params.preDelay = 0.005f;
		params.erDelay = 0.01f;
		params.erLevel = 0.15f * level;
		params.lateLevel = 0.45f * level;
	}
	else
	{
		if ( !s_roomRaysMade )
			S_MakeRoomRays();

		int hits = 0, upRays = 0, ceilingHits = 0;
		float distanceSum = 0.0f, nearest = ROOM_RAY_LENGTH * UNIT_TO_METERS;
		float absorptionSum = 0.0f, softnessSum = 0.0f;

		for ( int i = 0; i < ROOM_RAYS; i++ )
		{
			vec3_t end;
			trace_t trace;

			const qboolean up = (qboolean)( s_roomRays[i][2] > ROOM_UP );
			if ( up )
				upRays++;

			VectorMA( head, ROOM_RAY_LENGTH, s_roomRays[i], end );
			S_EffectsTrace( &trace, head, end );

			if ( trace.allsolid || trace.startsolid )
				return;		// inside something, this isn't a place to judge

			if ( trace.fraction >= 1.0f || ( trace.surfaceFlags & SURF_SKY ) )
				continue;	// open: nothing to reflect the sound

			const float distance = trace.fraction * ROOM_RAY_LENGTH * UNIT_TO_METERS;
			const int material = trace.surfaceFlags & MATERIAL_MASK;

			hits++;
			if ( up && distance < ROOM_CEILING_DISTANCE )
				ceilingHits++;
			distanceSum += distance;
			if ( distance < nearest )
				nearest = distance;
			absorptionSum += s_materialAbsorption[material];
			softnessSum += s_materialSoftness[material];
		}

		const float hitFraction = (float)hits / ROOM_RAYS;

		// Outdoors there are walls and a ground to reflect the sound but no ceiling, and that is not a room: the sound
		// escapes upwards, there is no reverb to speak of, only the odd echo from a wall. (A canyon is not a hall.)
		const float ceiling = upRays ? (float)ceilingHits / upRays : 0.0f;
		const float meanDistance = hits ? distanceSum / hits : ROOM_RAY_LENGTH * UNIT_TO_METERS;
		const float absorption = hits ? absorptionSum / hits : 0.1f;
		const float softness = hits ? softnessSum / hits : 0.0f;

		// How long the sound lasts: Sabine's rule, 0.161 V / A. The volume over the surface is about a third of
		// the distance to the walls, A is what the walls take up. A bit more is soaked up than the materials say (the
		// furniture, doorways, the rest of the level), and where there is nothing it all escapes.
		const float closed = hitFraction * ceiling;
		const float absorbed = closed * ( absorption + 0.08f ) + ( 1.0f - closed ) * 1.0f;
		float rt60 = 0.053f * meanDistance / absorbed;
		if ( rt60 < 0.12f )		rt60 = 0.12f;
		if ( rt60 > 4.0f )		rt60 = 4.0f;

		// the highs go first in soft rooms and in big ones (the air takes them)
		float hfRatio = ( 0.9f - 0.45f * softness ) / ( 1.0f + meanDistance / 50.0f );
		if ( hfRatio < 0.15f )	hfRatio = 0.15f;

		const float enclosed = hitFraction * sqrtf( hitFraction ) * ceiling * ceiling;	// (more of the sound is lost the more is open)

		params.rt60 = rt60;
		params.hfRatio = hfRatio;
		params.preDelay = Com_Clamp( 0.004f, 0.06f, 0.004f + 0.0015f * meanDistance );
		params.erDelay = Com_Clamp( 0.004f, 0.08f, 2.0f * nearest / SPEED_OF_SOUND );
		params.erLevel = 0.55f * hitFraction * ( 0.15f + 0.85f * ceiling ) * level;
		params.lateLevel = 0.55f * enclosed * level;

		if ( com_developer->integer )
		{
			static int lastReport;
			if ( Sys_Milliseconds() - lastReport > 3000 )
			{
				lastReport = Sys_Milliseconds();
				Com_DPrintf( "Sound: room: %d of %d rays hit, ceiling %.2f, mean %.1fm, nearest %.1fm, absorption %.2f, rt60 %.2fs, hf %.2f, early %.2f, tail %.2f\n",
							hits, ROOM_RAYS, ceiling, meanDistance, nearest, absorption, params.rt60, params.hfRatio, params.erLevel, params.lateLevel );
			}
		}
	}

	if ( immediately )
		S_Reverb_SetNow( &params );
	else
		S_Reverb_SetTarget( &params );
}

// How much of a sound goes to the reverb: the reverb of a room is about as loud for far sounds as for near ones, so
// the sound is attenuated by less than the direct sound is (its volume before and after the distance is allowed for)
static int S_ReverbSend( float master, float direct )
{
	if ( !s_reverbActive || master <= 0.0f )
		return 0;

	int send = (int)sqrtf( master * direct );
	return ( send > 255 ) ? 255 : send;
}

// Lets the sound of a channel be muffled when there is a wall between it and the listener, gradually (it is looked
// at a few times a second, and the muffling follows at its own pace)
static void S_UpdateObstruction( channel_t *ch, const vec3_t origin, int now, float seconds )
{
	if ( !s_reverbActive )
	{
		ch->obstruct = ch->obstructTarget = 0.0f;
		return;
	}

	if ( now >= ch->obstructNextTime )
	{
		trace_t trace;
		const qboolean bNew = (qboolean)( ch->obstructNextTime == 0 );	// the sound has only just started

		S_EffectsTrace( &trace, listener_origin, origin );
		ch->obstructTarget = ( !trace.startsolid && trace.fraction < 0.98f ) ? 1.0f : 0.0f;
		ch->obstructNextTime = now + 120 + ( ch - s_channels ) * 5;	// (not all in the same frame)

		if ( bNew )
			ch->obstruct = ch->obstructTarget;	// (it shouldn't start loud and then be muffled)
	}

	const float step = 6.0f * seconds;
	if ( ch->obstruct < ch->obstructTarget )
		ch->obstruct = Q_min( ch->obstruct + step, ch->obstructTarget );
	else
		ch->obstruct = Q_max( ch->obstruct - step, ch->obstructTarget );
}

/*
===============================================================================

ONCE A FRAME (S_Respatialize)

===============================================================================
*/
static int			s_effectsNow;			// msec, when this frame's effects were worked out
static float		s_effectsSeconds;		// how long since the last time (at most 0.2)
static qboolean		s_effectsInCutscene;	// see S_Effects_BeginFrame

static qboolean S_IsVoiceChannel( int entchannel )
{
	return (qboolean)( entchannel == CHAN_VOICE || entchannel == CHAN_VOICE_ATTEN || entchannel == CHAN_VOICE_GLOBAL );
}

// The room and the filters for this frame. While a scripted cutscene plays (single player) the listener is the camera,
// not somebody in a room: the dialogue is heard as in a film (no echo of the room, and no wall between the camera and
// the speaker muffles it).
static void S_Effects_BeginFrame( const vec3_t head, qboolean inwater, qboolean inCutscene )
{
	// the room: the reverb is set up for the output rate (and cleared) the first time, and the room looked at a few
	// times a second
	const int now = Sys_Milliseconds();
	static int lastRespatialize;
	s_effectsSeconds = Com_Clamp( 0.0f, 0.2f, ( now - lastRespatialize ) * 0.001f );
	lastRespatialize = now;
	s_effectsNow = now;
	s_effectsInCutscene = inCutscene;

	s_reverbActive = (qboolean)( s_reverb->integer && dma.channels == 2 && S_ReverbPrepare() );
	{
		const float target = ( s_reverbActive && inwater ) ? 1.0f : 0.0f;
		if ( s_underwater < target )		s_underwater = Q_min( s_underwater + 4.0f * s_effectsSeconds, target );
		else								s_underwater = Q_max( s_underwater - 4.0f * s_effectsSeconds, target );
	}
	if ( s_reverbActive && now >= s_roomNextAnalysis )
	{
		S_AnalyseRoom( head, inwater, (qboolean)( s_roomNextAnalysis == 0 ) );
		s_roomNextAnalysis = now + 100;
	}

	// binaural positioning needs the filters made for the output rate (the first time this is wanted, and again if
	// the rate changed), and stereo output
	const qboolean bWasHrtf = s_hrtfActive;
	s_hrtfActive = (qboolean)( s_hrtf->integer && dma.channels == 2 && S_HRTF_Init( dma.speed ) );
	if ( s_hrtfActive != bWasHrtf )
	{
		Com_DPrintf( "Sound: binaural (HRTF) positioning %s (%d taps at %d Hz)\n", s_hrtfActive ? "on" : "off", S_HRTF_NumTaps(), dma.speed );
	}
}

// A sound that is not positioned (it comes from the listener, or is an announcement): the player's own sounds are in
// the room too, announcements and the like are not
static void S_Effects_ListenerChannel( channel_t *ch )
{
	ch->hrtf = qfalse;
	ch->reverbvol = ( ch->entchannel == CHAN_VOICE_GLOBAL || ch->entchannel == CHAN_ANNOUNCER || !s_reverbActive
		|| ( s_effectsInCutscene && S_IsVoiceChannel( ch->entchannel ) ) ) ? 0 : (int)( ch->master_vol * 0.7f );
	ch->obstruct = ch->obstructTarget = 0.0f;
}

// A sound from origin, whose volumes S_SpatializeOrigin has just set (binaural if it said so, then from dir)
static void S_Effects_PlacedChannel( channel_t *ch, int channelNum, const vec3_t origin, qboolean binaural, const vec3_t dir )
{
	if ( binaural )
		S_HrtfSetupChannel( ch, channelNum, dir );
	else
		ch->hrtf = qfalse;

	ch->reverbvol = S_ReverbSend( (float)ch->master_vol, 0.5f * ( ch->leftvol + ch->rightvol ) );
	if ( s_effectsInCutscene )
	{
		if ( S_IsVoiceChannel( ch->entchannel ) )
			ch->reverbvol = 0;
		ch->obstruct = ch->obstructTarget = 0.0f;
		ch->obstructNextTime = 0;
	}
	else
		S_UpdateObstruction( ch, origin, s_effectsNow, s_effectsSeconds );
}

// The channel of a looping sound (all the loops of the same sound added together: from dirTotal if binaural, volume
// is that of the first of them, left and right their total)
static void S_Effects_LoopChannel( channel_t *ch, const sfx_t *sfx, qboolean binaural, const vec3_t dirTotal, int loopFrame, int volume, int left, int right )
{
	if ( binaural )
		S_HrtfSetupLoop( ch, sfx, dirTotal, loopFrame );
	ch->reverbvol = S_ReverbSend( (float)volume, 0.5f * ( left + right ) );	// (like a one shot sound)
}

// After all the sounds of the frame were placed
static void S_Effects_EndFrame( void )
{
	if ( !s_hrtfActive )
		return;

	S_HrtfLimit();

	if ( com_developer->integer )
	{
		static int iLastReport;
		if ( Sys_Milliseconds() - iLastReport > 5000 )
		{
			int nBinaural = 0;
			const channel_t *ch = s_channels;
			for ( int i = 0 ; i < MAX_CHANNELS ; i++, ch++ )
			{
				if ( ch->thesfx && ch->hrtf )
					nBinaural++;
			}
			Com_DPrintf( "Sound: %d sounds positioned binaurally\n", nBinaural );
			iLastReport = Sys_Milliseconds();
		}
	}
}
