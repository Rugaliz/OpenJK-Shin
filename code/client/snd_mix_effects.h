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

// snd_mix_effects.h -- the mixing side of the sound effects: binaural positioning (s_hrtf, see docs/hrtf.md), the
// reverb of the room, sounds muffled by walls and under water (s_reverb, see docs/reverb.md). What is worked out once a
// frame (directions, the room, which sounds are muffled) is in snd_dma_effects.h, the filters in snd_hrtf.cpp and
// snd_reverb.cpp.
//
// Included once by the snd_mix.cpp of each engine (single player and multiplayer), after the paint buffer, snd_vol and
// S_FetchMP3Samples. S_PaintChannels calls S_Mix_EffectsBegin before it paints an update, S_Mix_EffectsChunkBegin and
// S_Mix_EffectsChunkEnd around each piece it paints, and paints the sound of each channel with S_PaintSource.

#pragma once

// What the sounds that are to have reverb send to it, mixed together (mono, on the scale of the paint buffer)
static int	reverbBuffer[PAINTBUFFER_SIZE];

// The mixer is not run once over each moment of sound: every update (every frame) mixes everything from just ahead of
// what is being played to a fifth of a second ahead again, so what is ahead can change when the game does (a sound
// starts). That is fine for adding sounds up but not for the reverb, which remembers what went into it. So:
//  - what is sent to the reverb is kept by time (s_reverbSend), and the reverb is only run a little way ahead, up to
//    where the next update will start painting (what is further ahead has no reverb yet; it is only played if the game
//    stalls, and then gets painted again with it)
//  - the reverb as it was where an update started painting is remembered (S_Reverb_Mark). The next update goes back to
//    it and runs the reverb again from there over what was sent (now final, it has been played) up to where it starts
//    painting itself. So what the sounds started since then send to it is all heard, from their very start.
#define REVERB_RING		65536
static int		s_reverbSend[REVERB_RING];
static int		s_reverbSendEnd = 0;		// what was sent is in the ring up to this time
static int		s_reverbMarkTime = 0;		// the time the reverb was remembered at
static qboolean	s_reverbMarkValid = qfalse;
static int		s_reverbTime = 0;			// the time the reverb has been run up to
static int		s_lastUpdateStart = 0;

// the underwater filter (below) keeps what it made for the same reason
static float	s_uwLeft[REVERB_RING], s_uwRight[REVERB_RING];
static int		s_uwWrittenUpTo = 0;
static qboolean	s_uwValid = qfalse;

void S_Mix_ResetReverb( void )
{
	s_reverbMarkValid = qfalse;
	s_reverbSendEnd = 0;
}

// At the start of an update: brings the reverb to where this update starts painting (see above)
static void S_Mix_ReverbStartUpdate( int updateStart )
{
	if ( s_reverbMarkValid && updateStart >= s_reverbMarkTime && updateStart - s_reverbMarkTime <= dma.speed / 2
		&& S_Reverb_Rewind() )
	{
		float in[PAINTBUFFER_SIZE], wetLeft[PAINTBUFFER_SIZE], wetRight[PAINTBUFFER_SIZE];

		for ( int time = s_reverbMarkTime; time < updateStart; )
		{
			const int n = ( updateStart - time < PAINTBUFFER_SIZE ) ? updateStart - time : PAINTBUFFER_SIZE;
			for ( int i = 0; i < n; i++ )
				in[i] = ( time + i < s_reverbSendEnd ) ? (float)s_reverbSend[( time + i ) & ( REVERB_RING - 1 )] : 0.0f;
			S_Reverb_Process( in, n, wetLeft, wetRight );	// (what comes out was played already)
			time += n;
		}
	}
	// (otherwise the time was not continuous: after a pause, or a new level, and it just carries on from here)

	S_Reverb_Mark();
	s_reverbMarkTime = updateStart;
	s_reverbMarkValid = qtrue;
	s_reverbTime = updateStart;
}

// The mixer does not paint each moment of a sound once, in order: every update paints everything from just ahead of
// what is being played again (see the reverb below). So filters that remember the signal that went before (the muffling
// and the HRTF) must not carry that on from the last time they were used: that was the end of what was painted ahead,
// not what comes before the part that is painted now. Where the sound can be read at any point, what came before it is
// read from the sound itself (a sound loops: from its end, and before it starts there is nothing).
#define OBSTRUCT_WARMUP		96		// samples a muffling filter needs to be in step with the signal (it has no long memory)

// (step: how many samples of the sound apart the painted ones are, more than 1 for a sound that is played faster, see
// S_PaintSource)
static qboolean S_SourceHistory( channel_t *ch, const sfx_t *sc, int sampleOffset, float step, int n, short *out )
{
	if ( sc->eSoundCompressionMethod == ct_MP3 )
	{
		// the decoder keeps what it decoded last in a window, which reaches back well past the few samples asked for
		// here (voices, which are all MP3s, are what get muffled when there is a wall in the way)
		const int first = sampleOffset - n;
		const int silent = ( first < 0 ) ? -first : 0;		// nothing comes before the start of the sound

		memset( out, 0, silent * sizeof( short ) );
		if ( n > silent )
			S_FetchMP3Samples( ch, n - silent, first + silent, out + silent );
		return qtrue;
	}

	if ( sc->eSoundCompressionMethod != ct_16 )
		return qfalse;

	const int length = sc->iSoundLengthInSamples;
	for ( int k = 0; k < n; k++ )
	{
		int p = sampleOffset - (int)( ( n - k ) * step );
		if ( p < 0 )
		{
			if ( !ch->loopSound || length <= 0 )
			{
				out[k] = 0;
				continue;
			}
			p = ( ( p % length ) + length ) % length;
		}
		out[k] = sc->pSoundData[p];
	}
	return qtrue;
}

// The sound of a channel being muffled by something between the sound and the listener (ch->obstruct, 0 to 1): the
// highs are filtered away, and it is a little quieter. The filter is run over the signal before the part that is
// painted (history, if there is any, and if histOut is given the result of that is returned too) to be where it would be.
static void S_ObstructSamples( channel_t *ch, const short *history, int numHistory, short *histOut, const short *src, int count, short *dst )
{
	const float cutoff = 18000.0f * powf( 0.05f, ch->obstruct );	// 18kHz down to 900Hz
	const float a = 1.0f - expf( -2.0f * (float)M_PI * cutoff / dma.speed );
	const float gain = 1.0f - 0.4f * ch->obstruct;
	float state = history ? 0.0f : ch->lpState;

	for ( int i = 0; i < numHistory; i++ )
	{
		state += a * ( history[i] - state );
		if ( histOut )
			histOut[i] = (short)( state * gain );
	}

	for ( int i = 0; i < count; i++ )
	{
		state += a * ( src[i] - state );
		dst[i] = (short)( state * gain );
	}
	ch->lpState = state;
}

// Adds count mono samples to what the channel sends to the reverb
static qboolean	s_reverbSending = qfalse;	// the reverb is fed in this part of the mix (see S_PaintChannels)

static inline void S_SendToReverb( const channel_t *ch, const short *src, int count, int bufferOffset )
{
	if ( !s_reverbSending || ch->reverbvol <= 0 )
		return;

	const int rvol = ch->reverbvol * snd_vol;
	int *pDest = &reverbBuffer[bufferOffset];

	for ( int i = 0; i < count; i++ )
		pDest[i] += (src[i] * rvol)>>8;
}

// Paints count mono samples from the position of the channel, panned between the speakers by its volumes
static void S_PaintMono( channel_t *ch, const short *src, int count, int bufferOffset )
{
	const int leftvol = ch->leftvol*snd_vol;
	const int rightvol = ch->rightvol*snd_vol;
	portable_samplepair_t *samp = &paintbuffer[ bufferOffset ];

	for ( int i = 0; i < count; i++ )
	{
		const int data = src[i];

		samp[i].left += (data * leftvol)>>8;
		samp[i].right += (data * rightvol)>>8;
	}

	S_SendToReverb( ch, src, count, bufferOffset );
}

// Paints count (<= PAINTBUFFER_SIZE) mono samples as if they came from the direction of the channel (s_hrtf), by
// filtering them differently for each ear. The channel's volumes are the same for both ears here (the distance).
static void S_PaintChannelHRTF( channel_t *ch, const short *src, int count, int bufferOffset )
{
	float	left[PAINTBUFFER_SIZE], right[PAINTBUFFER_SIZE];
	hrtfFilter_t	filter;

	S_HRTF_GetFilter( ch->hrtfAzimuth, ch->hrtfElevation, &filter );
	S_HRTF_Process( ch->pHrtfState, &filter, src, count, s_paintedtime + bufferOffset, left, right );

	const int iLeftVol	= ch->leftvol  * snd_vol;
	const int iRightVol	= ch->rightvol * snd_vol;
	portable_samplepair_t *pSamplesDest = &paintbuffer[ bufferOffset ];

	for ( int i = 0; i < count; i++ )
	{
		int l = (int)floorf( left[i] + 0.5f );
		int r = (int)floorf( right[i] + 0.5f );
		if ( l > 32767 )		l = 32767;
		else if ( l < -32768 )	l = -32768;
		if ( r > 32767 )		r = 32767;
		else if ( r < -32768 )	r = -32768;

		pSamplesDest[i].left  += (l * iLeftVol )>>8;
		pSamplesDest[i].right += (r * iRightVol)>>8;
	}

	S_SendToReverb( ch, src, count, bufferOffset );
}


// Paints count samples of the sound of a channel (src, which starts at sampleOffset in the sound and has a sample for
// every step samples of it: 1, unless it is played faster) with the effects it has: muffled by a wall, positioned
// binaurally, sent to the reverb
static void S_PaintSource( channel_t *ch, const sfx_t *sc, const short *src, int count, int sampleOffset, float step, int bufferOffset )
{
	static short	obstructed[PAINTBUFFER_SIZE];

	const qboolean useHrtf = (qboolean)( ch->hrtf && ch->pHrtfState );
	const qboolean muffled = (qboolean)( ch->obstruct > 0.01f );
	const int hrtfHistory = useHrtf ? S_HRTF_NumTaps() - 1 : 0;

	// what came before the part painted now (see S_SourceHistory), for the filters that need it
	short	history[OBSTRUCT_WARMUP + HRTF_MAX_TAPS], historyOut[OBSTRUCT_WARMUP + HRTF_MAX_TAPS];
	const int numHistory = ( muffled ? OBSTRUCT_WARMUP : 0 ) + hrtfHistory;
	const qboolean haveHistory = ( numHistory > 0 ) ? S_SourceHistory( ch, sc, sampleOffset, step, numHistory, history ) : qfalse;

	if ( muffled )
	{
		S_ObstructSamples( ch, haveHistory ? history : NULL, haveHistory ? numHistory : 0, historyOut, src, count, obstructed );
		src = obstructed;
	}

	if ( useHrtf )
	{
		if ( haveHistory )
			S_HRTF_SetHistory( ch->pHrtfState, ( muffled ? historyOut : history ) + numHistory - hrtfHistory, hrtfHistory );
		S_PaintChannelHRTF( ch, src, count, bufferOffset );
	}
	else
		S_PaintMono( ch, src, count, bufferOffset );
}

// Before an update is painted (bReserve: see S_PaintChannels). Returns how far the reverb is run in it.
static int S_Mix_EffectsBegin( qboolean bReserve )
{
	// how far the reverb is run in this update: past where the next one starts painting (twice the time since the
	// last, and at least as far as how far ahead of the play position painting starts, which allows for long frames).
	// (The reserve only matters if the game stalls: it has none, and leaves the reverb and its timing alone.)
	s_reverbSending = (qboolean)( s_reverbActive && !bReserve );
	const int updateStart = s_paintedtime;
	int commitLength = 2 * ( updateStart - s_lastUpdateStart );
	if ( commitLength < updateStart - s_soundtime )		commitLength = updateStart - s_soundtime;
	if ( commitLength < dma.speed / 40 )				commitLength = dma.speed / 40;
	if ( commitLength > dma.speed / 5 )					commitLength = dma.speed / 5;

	if ( !bReserve )
	{
		s_lastUpdateStart = updateStart;
		if ( s_reverbActive )
			S_Mix_ReverbStartUpdate( updateStart );
		else
			s_reverbMarkValid = qfalse;
	}

	return updateStart + commitLength;
}

// Before the channels are painted into a piece of n samples
static void S_Mix_EffectsChunkBegin( int n )
{
	if ( s_reverbSending )
		memset( reverbBuffer, 0, n * sizeof( int ) );
}

// After they were, the piece being from s_paintedtime to end: the reverb and the muffling under water
static void S_Mix_EffectsChunkEnd( int end, int commitEnd )
{
	int i;

	if ( s_reverbSending )
	{
		// keep what was sent to the reverb, and run the reverb over it as far as this update does (see above)
		const int chunkStart = s_paintedtime;

		for ( i = chunkStart; i < end; i++ )
			s_reverbSend[i & ( REVERB_RING - 1 )] = reverbBuffer[i - chunkStart];
		s_reverbSendEnd = end;

		const int upTo = ( end < commitEnd ) ? end : commitEnd;
		if ( s_reverbTime == chunkStart && upTo > chunkStart )
		{
			const int n = upTo - chunkStart;
			float in[PAINTBUFFER_SIZE], wetLeft[PAINTBUFFER_SIZE], wetRight[PAINTBUFFER_SIZE];

			for ( i = 0; i < n; i++ )
				in[i] = (float)reverbBuffer[i];
			S_Reverb_Process( in, n, wetLeft, wetRight );
			for ( i = 0; i < n; i++ )
			{
				paintbuffer[i].left += (int)wetLeft[i];
				paintbuffer[i].right += (int)wetRight[i];
			}
			s_reverbTime = upTo;
		}
	}

	if ( s_underwater > 0.01f )
	{
		// everything is muffled underwater: a low pass filter, from 20kHz (just under the surface) to 800Hz. It
		// carries on from what it made for the moment before this one the last time that was painted (not from
		// the end of the last thing painted, which is far ahead: see the reverb)
		const int chunkStart = s_paintedtime;
		const int n = end - chunkStart;
		const float cutoff = 20000.0f * powf( 0.04f, s_underwater );
		const float a = 1.0f - expf( -2.0f * (float)M_PI * cutoff / dma.speed );
		float stateLeft, stateRight;

		if ( s_uwValid && chunkStart - 1 < s_uwWrittenUpTo && chunkStart - 1 >= s_uwWrittenUpTo - REVERB_RING / 2 )
		{
			stateLeft = s_uwLeft[( chunkStart - 1 ) & ( REVERB_RING - 1 )];
			stateRight = s_uwRight[( chunkStart - 1 ) & ( REVERB_RING - 1 )];
		}
		else
		{
			stateLeft = (float)paintbuffer[0].left;
			stateRight = (float)paintbuffer[0].right;
		}

		for ( i = 0; i < n; i++ )
		{
			stateLeft += a * ( paintbuffer[i].left - stateLeft );
			stateRight += a * ( paintbuffer[i].right - stateRight );
			paintbuffer[i].left = (int)stateLeft;
			paintbuffer[i].right = (int)stateRight;
			s_uwLeft[( chunkStart + i ) & ( REVERB_RING - 1 )] = stateLeft;
			s_uwRight[( chunkStart + i ) & ( REVERB_RING - 1 )] = stateRight;
		}

		if ( !s_uwValid || end > s_uwWrittenUpTo || chunkStart < s_uwWrittenUpTo - REVERB_RING / 2 )
			s_uwWrittenUpTo = end;
		s_uwValid = qtrue;
	}
	else
	{
		s_uwValid = qfalse;		// (the next time underwater starts over)
	}
}
