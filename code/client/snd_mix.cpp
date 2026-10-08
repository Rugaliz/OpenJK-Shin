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

// snd_mix.c -- portable code to mix sounds for snd_dma.c

#include "../server/exe_headers.h"

#include "snd_local.h"
#include "sdl/sdl_sound.h"
#include <math.h>

portable_samplepair_t paintbuffer[PAINTBUFFER_SIZE];
int 	*snd_p, snd_linear_count, snd_vol;
short	*snd_out;

// Statistics on how often the mix exceeded full scale, reported by S_Update when developer is on
int		s_clippedSamples;	// samples over full scale since the last report
int		s_clipPeak;			// largest absolute value seen since the last report (16 bit scale, before limiting)

/*
===================
S_LimitSample

Brings a mixed sample (16 bit scale, but can be far outside it when many loud sounds overlap) into 16 bit range.

Hard clipping chops the tops off the waveform, which sounds like harsh distortion. Instead, anything within the top
quarter of the range is passed through untouched, and louder peaks are smoothly compressed towards full scale.
Quiet and normal-volume sound is completely unaffected.
===================
*/
#define LIMITER_THRESHOLD	24576		// 0.75 of full scale
static inline short S_LimitSample( int val )
{
	const int abs_val = (val < 0) ? -val : val;

	if ( abs_val <= LIMITER_THRESHOLD )
	{
		return (short)val;
	}

	if ( abs_val > 0x7fff )
	{
		s_clippedSamples++;
	}
	if ( abs_val > s_clipPeak )
	{
		s_clipPeak = abs_val;
	}

	const float range = (float)( 0x7fff - LIMITER_THRESHOLD );
	const int limited = LIMITER_THRESHOLD + (int)( range * tanhf( (abs_val - LIMITER_THRESHOLD) / range ) );

	return (short)( (val < 0) ? -limited : limited );
}

// Where an update's mix meets what is in the ring already (its start, and its end), the two are crossfaded over a
// moment instead of one simply following the other. Normally they are the same there (it is the same sound mixed
// again) and nothing changes, but after a long frame the ring holds the reserve (see S_Update_), mixed a while ago,
// and going from that to the fresh mix (and back) would click.
#define MIX_CROSSFADE	64
static int	s_crossfadeStart = 0, s_crossfadeEnd = 0;
static qboolean	s_crossfade = qfalse;

static void S_WriteLinearBlastStereo16 ( int time )
{
	int		i;

	if ( s_crossfade && ( time < s_crossfadeStart + MIX_CROSSFADE || time + ( snd_linear_count >> 1 ) > s_crossfadeEnd - MIX_CROSSFADE ) )
	{
		for (i=0 ; i<snd_linear_count ; i+=2, time++)
		{
			int w = MIX_CROSSFADE;	// how much of the new mix, of MIX_CROSSFADE
			if ( time - s_crossfadeStart < w )	w = time - s_crossfadeStart + 1;
			if ( s_crossfadeEnd - time < w )	w = s_crossfadeEnd - time;
			const int l = S_LimitSample( snd_p[i]>>8 );
			const int r = S_LimitSample( snd_p[i+1]>>8 );
			if ( w >= MIX_CROSSFADE )
			{
				snd_out[i] = l;
				snd_out[i+1] = r;
			}
			else
			{
				snd_out[i]   = (short)( snd_out[i]   + ( ( l - snd_out[i]   ) * w ) / MIX_CROSSFADE );
				snd_out[i+1] = (short)( snd_out[i+1] + ( ( r - snd_out[i+1] ) * w ) / MIX_CROSSFADE );
			}
		}
		return;
	}

	for (i=0 ; i<snd_linear_count ; i+=2)
	{
		snd_out[i]   = S_LimitSample( snd_p[i]>>8 );
		snd_out[i+1] = S_LimitSample( snd_p[i+1]>>8 );
	}
}

void S_TransferStereo16 (unsigned long *pbuf, int endtime)
{
	int		lpos;
	int		ls_paintedtime;

	snd_p = (int *) paintbuffer;
	ls_paintedtime = s_paintedtime;

	while (ls_paintedtime < endtime)
	{
	// handle recirculating buffer issues
		lpos = ls_paintedtime & ((dma.samples>>1)-1);

		snd_out = (short *) pbuf + (lpos<<1);

		snd_linear_count = (dma.samples>>1) - lpos;
		if (ls_paintedtime + snd_linear_count > endtime)
			snd_linear_count = endtime - ls_paintedtime;

		snd_linear_count <<= 1;

	// write a linear blast of samples
		S_WriteLinearBlastStereo16 ( ls_paintedtime );

		snd_p += snd_linear_count;
		ls_paintedtime += (snd_linear_count>>1);
	}
}

/*
===================
S_TransferPaintBuffer

===================
*/
void S_TransferPaintBuffer(int endtime)
{
	int 	out_idx;
	int 	count;
	int 	out_mask;
	int 	*p;
	int 	step;
	int		val;
	unsigned long *pbuf;

	pbuf = (unsigned long *)dma.buffer;

	// the device takes what is in the ring from another thread: hold it off while writing (only this, the copy, so
	// it is never kept waiting for the mixing)
	SNDDMA_BeginPainting ();

	if ( s_testsound->integer ) {
		int		i;
		int		count;

		// write a fixed sine wave
		count = (endtime - s_paintedtime);
		for (i=0 ; i<count ; i++)
			paintbuffer[i].left = paintbuffer[i].right = (int)(sin((s_paintedtime+i)*0.1)*20000*256);
	}


	if (dma.samplebits == 16 && dma.channels == 2)
	{	// optimized case
		S_TransferStereo16 (pbuf, endtime);
	}
	else
	{	// general case
		p = (int *) paintbuffer;
		count = (endtime - s_paintedtime) * dma.channels;
		out_mask = dma.samples - 1;
		out_idx = s_paintedtime * dma.channels & out_mask;
		step = 3 - dma.channels;

		if (dma.samplebits == 16)
		{
			short *out = (short *) pbuf;
			while (count--)
			{
				val = *p >> 8;
				p+= step;
				out[out_idx] = S_LimitSample( val );
				out_idx = (out_idx + 1) & out_mask;
			}
		}
		else if (dma.samplebits == 8)
		{
			unsigned char *out = (unsigned char *) pbuf;
			while (count--)
			{
				val = S_LimitSample( *p >> 8 );
				p+= step;
				out[out_idx] = (short)((val>>8) + 128);
				out_idx = (out_idx + 1) & out_mask;
			}
		}
	}

	SNDDMA_Submit ();
}


/*
===============================================================================

CHANNEL MIXING

===============================================================================
*/
// The most decoded samples a single request to the MP3 decoder's sliding window should ask for (see
// MP3SlidingDecodeBuffer in snd_local.h), which also sizes the source buffer used when converting the rate.
#define MP3_PAINT_MAX_SOURCE	3000

// Gets count (<= PAINTBUFFER_SIZE) samples of an MP3 sound, starting at sampleOffset, at the output rate
static void S_FetchMP3Samples( channel_t *ch, int count, int sampleOffset, short *dst )
{
	if ( dma.speed == MP3_SAMPLE_RATE )
	{
		MP3Stream_GetSamples( ch, sampleOffset, count, dst, qfalse );	// qfalse = not stereo
		return;
	}

	// The MP3 is always decoded at its own rate, so convert to the output rate here. The position of every output
	// sample is worked out from its number alone (so, unlike the pieces of a stream, the pieces this is called
	// with need nothing carried over between them), and only the source samples around it are decoded.
	static short srcBuffer[MP3_PAINT_MAX_SOURCE + 8];
	const double dStep = (double)MP3_SAMPLE_RATE / dma.speed;	// source samples per output sample
	resampleFilter_t filter;
	S_Resample_SetupFilter( &filter, dStep );

	int maxRun = (int)( ( MP3_PAINT_MAX_SOURCE - 2.0 * filter.support - 8.0 ) / dStep );
	if ( maxRun < 1 )
		maxRun = 1;

	while ( count > 0 )
	{
		const int n = ( count < maxRun ) ? count : maxRun;
		const int iFirst = (int)ceil( sampleOffset * dStep - filter.support ) - 1;
		const int iLast = (int)floor( ( sampleOffset + n - 1 ) * dStep + filter.support ) + 1;
		const int iFetchFirst = ( iFirst < 0 ) ? 0 : iFirst;	// before the start of the sound is silence
		const int iFetchCount = iLast - iFetchFirst + 1;

		MP3Stream_GetSamples( ch, iFetchFirst, iFetchCount, srcBuffer, qfalse );	// qfalse = not stereo

		for ( int i = 0; i < n; i++ )
		{
			float f;
			S_Resample_Interp( srcBuffer, 1, iFetchCount, ( sampleOffset + i ) * dStep - iFetchFirst, &filter, &f );
			if ( f > 32767.0f )			f = 32767.0f;
			else if ( f < -32768.0f )	f = -32768.0f;
			*dst++ = (short)floorf( f + 0.5f );
		}

		sampleOffset += n;
		count -= n;
	}
}

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

static qboolean S_SourceHistory( channel_t *ch, const sfx_t *sc, int sampleOffset, int n, short *out )
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
		int p = sampleOffset - n + k;
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


// subroutinised to save code dup (called twice)	-ste
//
void ChannelPaint(channel_t *ch, sfx_t *sc, int count, int sampleOffset, int bufferOffset)
{
	static short	mp3Samples[PAINTBUFFER_SIZE], obstructed[PAINTBUFFER_SIZE];
	const short		*src;

	switch (sc->eSoundCompressionMethod)
	{
		case ct_16:

			src = sc->pSoundData + sampleOffset;
			break;

		case ct_MP3:

			S_FetchMP3Samples( ch, count, sampleOffset, mp3Samples );
			src = mp3Samples;
			break;

		default:

			assert(0);	// debug aid, ignored in release. FIXME: Should we ERR_DROP here for badness-catch?
			return;
	}

	const qboolean useHrtf = (qboolean)( ch->hrtf && ch->pHrtfState );
	const qboolean muffled = (qboolean)( ch->obstruct > 0.01f );
	const int hrtfHistory = useHrtf ? S_HRTF_NumTaps() - 1 : 0;

	// what came before the part painted now (see S_SourceHistory), for the filters that need it
	short	history[OBSTRUCT_WARMUP + HRTF_MAX_TAPS], historyOut[OBSTRUCT_WARMUP + HRTF_MAX_TAPS];
	const int numHistory = ( muffled ? OBSTRUCT_WARMUP : 0 ) + hrtfHistory;
	const qboolean haveHistory = ( numHistory > 0 ) ? S_SourceHistory( ch, sc, sampleOffset, numHistory, history ) : qfalse;

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



void S_PaintChannels( int endtime, qboolean bReserve ) {
	int 	i;
	int 	end;
	channel_t *ch;
	sfx_t	*sc;
	int		ltime, count;
	int		sampleOffset;
	int	normal_vol,voice_vol;

	// (a sample times a volume of 255 times more than 256 doesn't fit in 32 bits)
	snd_vol = normal_vol = Com_Clampi( 0, 256, (int)( s_volume->value*256.0f ) );
	voice_vol = Com_Clampi( 0, 256, (int)( s_volumeVoice->value*256.0f ) );

	// how far the reverb is run in this update: past where the next one starts painting (twice the time since the
	// last, and at least as far as how far ahead of the play position painting starts, which allows for long frames).
	// (The reserve only matters if the game stalls: it has none, and leaves the reverb and its timing alone.)
	const qboolean bReverb = (qboolean)( s_reverbActive && !bReserve );
	s_reverbSending = bReverb;
	const int updateStart = s_paintedtime;
	int commitLength = 2 * ( updateStart - s_lastUpdateStart );
	if ( commitLength < updateStart - s_soundtime )		commitLength = updateStart - s_soundtime;
	if ( commitLength < dma.speed / 40 )				commitLength = dma.speed / 40;
	if ( commitLength > dma.speed / 5 )					commitLength = dma.speed / 5;
	const int commitEnd = updateStart + commitLength;

	// the update's mix is crossfaded with the ring where it starts and ends; the reserve carries on from it as it is
	s_crossfade = (qboolean)!bReserve;
	if ( !bReserve )
	{
		s_crossfadeStart = updateStart;
		s_crossfadeEnd = endtime;
		s_lastUpdateStart = updateStart;
		if ( s_reverbActive )
			S_Mix_ReverbStartUpdate( updateStart );
		else
			s_reverbMarkValid = qfalse;
	}

//Com_Printf ("%i to %i\n", s_paintedtime, endtime);
	while ( s_paintedtime < endtime ) {
		// if paintbuffer is smaller than DMA buffer
		// we may need to fill it multiple times
		end = endtime;
		if ( endtime - s_paintedtime > PAINTBUFFER_SIZE ) {
			end = s_paintedtime + PAINTBUFFER_SIZE;
		}

		// clear the paint buffer to either music or zeros
		if ( s_rawend < s_paintedtime ) {
			if ( s_rawend ) {
				//Com_DPrintf ("background sound underrun\n");
			}
			memset(paintbuffer, 0, (end - s_paintedtime) * sizeof(portable_samplepair_t));
		} else {
			// copy from the streaming sound source
			int		s;
			int		stop;

			stop = (end < s_rawend) ? end : s_rawend;

			for ( i = s_paintedtime ; i < stop ; i++ ) {
				s = i&(MAX_RAW_SAMPLES-1);
				paintbuffer[i-s_paintedtime] = s_rawsamples[s];
			}
//		if (i != end)
//			Com_Printf ("partial stream\n");
//		else
//			Com_Printf ("full stream\n");
			for ( ; i < end ; i++ ) {
				paintbuffer[i-s_paintedtime].left =
				paintbuffer[i-s_paintedtime].right = 0;
			}
		}

		if ( bReverb )
			memset( reverbBuffer, 0, ( end - s_paintedtime ) * sizeof( int ) );

		// paint in the channels.
		ch = s_channels;
		for ( i = 0; i < MAX_CHANNELS ; i++, ch++ ) {
			if ( !ch->thesfx || (ch->leftvol<0.25 && ch->rightvol<0.25 )) {
				continue;
			}

			if ( ch->entchannel == CHAN_VOICE || ch->entchannel == CHAN_VOICE_ATTEN || ch->entchannel == CHAN_VOICE_GLOBAL )
				snd_vol = voice_vol;
			else
				snd_vol = normal_vol;

			ltime = s_paintedtime;
			sc = ch->thesfx;

			// we might have to make 2 passes if it is
			//	a looping sound effect and the end of
			//	the sameple is hit...
			//
			do
			{
				if (ch->loopSound) {
					sampleOffset = ltime % sc->iSoundLengthInSamples;
				} else {
					sampleOffset = ltime - ch->startSample;
					if ( sampleOffset < 0 ) {
						// the sound starts later in this piece
						ltime -= sampleOffset;
						sampleOffset = 0;
						if ( ltime >= end ) {
							break;
						}
					}
				}

				count = end - ltime;
				if ( sampleOffset + count > sc->iSoundLengthInSamples ) {
					count = sc->iSoundLengthInSamples - sampleOffset;
				}

				if ( count > 0 ) {
					ChannelPaint(ch, sc, count, sampleOffset, ltime - s_paintedtime);
					ltime += count;
				}
			} while ( ltime < end && ch->loopSound );
		}
/* temprem
		// paint in the looped channels.
		ch = loop_channels;
		for ( i = 0; i < numLoopChannels ; i++, ch++ ) {
			if ( !ch->thesfx || (!ch->leftvol && !ch->rightvol )) {
				continue;
			}

			{

				ltime = s_paintedtime;
				sc = ch->thesfx;

				if (sc->soundData==NULL || sc->soundLength==0) {
					continue;
				}
				// we might have to make two passes if it
				// is a looping sound effect and the end of
				// the sample is hit
				do {
					sampleOffset = (ltime % sc->soundLength);

					count = end - ltime;
					if ( sampleOffset + count > sc->soundLength ) {
						count = sc->soundLength - sampleOffset;
					}

					if ( count > 0 )
					{
						ChannelPaint(ch, sc, count, sampleOffset, ltime - s_paintedtime);
						ltime += count;
					}

				} while ( ltime < end);
			}
		}
*/
		if ( bReverb )
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

		// transfer out according to DMA format
		S_TransferPaintBuffer( end );
		s_paintedtime = end;
	}
}
