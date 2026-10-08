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

#include "client.h"
#include "snd_local.h"
#include "sdl/sdl_sound.h"

portable_samplepair_t paintbuffer[PAINTBUFFER_SIZE];
int 	*snd_p, snd_linear_count, snd_vol;
short	*snd_out;



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

		if( CL_VideoRecording( ) )
		{
			if ( cls.state == CA_ACTIVE || cl_forceavidemo->integer) {
				CL_WriteAVIAudioFrame( (byte *)snd_out, snd_linear_count << 1 );
			}
		}
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
static void S_PaintChannelFrom16( channel_t *ch, const sfx_t *sfx, int count, int sampleOffset, int bufferOffset )
{
	portable_samplepair_t	*pSamplesDest;
	int iData;
	float ofst = sampleOffset;

	int iLeftVol	= ch->leftvol  * snd_vol;
	int iRightVol	= ch->rightvol * snd_vol;

	pSamplesDest	= &paintbuffer[ bufferOffset ];

	if ( !( ch->doppler && ch->dopplerScale > 1 ) )
	{
		// (the usual case: one sample after another)
		const short *pSrc = sfx->pSoundData + sampleOffset;
		for ( int i=0 ; i<count ; i++ )
		{
			iData = pSrc[i];
			pSamplesDest[i].left  += (iData * iLeftVol )>>8;
			pSamplesDest[i].right += (iData * iRightVol)>>8;
		}
		return;
	}

	for ( int i=0 ; i<count ; i++ )
	{
		iData = sfx->pSoundData[ (int)ofst ];

		pSamplesDest[i].left  += (iData * iLeftVol )>>8;
		pSamplesDest[i].right += (iData * iRightVol)>>8;
		ofst += ch->dopplerScale;
	}
}


void S_PaintChannelFromMP3( channel_t *ch, const sfx_t *sc, int count, int sampleOffset, int bufferOffset )
{
	int data;
	int leftvol, rightvol;
	signed short *sfx;
	int	i;
	portable_samplepair_t	*samp;
	static short tempMP3Buffer[PAINTBUFFER_SIZE];

	MP3Stream_GetSamples( ch, sampleOffset, count, tempMP3Buffer, qfalse );	// qfalse = not stereo

	leftvol = ch->leftvol*snd_vol;
	rightvol = ch->rightvol*snd_vol;
	sfx = tempMP3Buffer;

	samp = &paintbuffer[ bufferOffset ];


	while ( count & 3 ) {
		data = *sfx;
		samp->left += (data * leftvol)>>8;
		samp->right += (data * rightvol)>>8;

		sfx++;
		samp++;
		count--;
	}

	for ( i=0 ; i<count ; i += 4 ) {
		data = sfx[i];
		samp[i].left += (data * leftvol)>>8;
		samp[i].right += (data * rightvol)>>8;

		data = sfx[i+1];
		samp[i+1].left += (data * leftvol)>>8;
		samp[i+1].right += (data * rightvol)>>8;

		data = sfx[i+2];
		samp[i+2].left += (data * leftvol)>>8;
		samp[i+2].right += (data * rightvol)>>8;

		data = sfx[i+3];
		samp[i+3].left += (data * leftvol)>>8;
		samp[i+3].right += (data * rightvol)>>8;
	}
}


// subroutinised to save code dup (called twice)	-ste
//
void ChannelPaint(channel_t *ch, sfx_t *sc, int count, int sampleOffset, int bufferOffset)
{
	switch (sc->eSoundCompressionMethod)
	{
		case ct_16:

			S_PaintChannelFrom16		(ch, sc, count, sampleOffset, bufferOffset);
			break;

		case ct_MP3:

			S_PaintChannelFromMP3		(ch, sc, count, sampleOffset, bufferOffset);
			break;

		default:

			assert(0);	// debug aid, ignored in release. FIXME: Should we ERR_DROP here for badness-catch?
			break;
	}
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
	snd_vol = normal_vol = Com_Clampi( 0, 256, (int)( s_volume->value*256 ) );
	voice_vol = Com_Clampi( 0, 256, (int)( s_volumeVoice->value*256 ) );

	// the update's mix is crossfaded with the ring where it starts and ends (unless it is recorded, which takes it as it
	// is); the reserve carries on from it as it is
	s_crossfade = (qboolean)( !bReserve && !CL_VideoRecording() );
	s_crossfadeStart = s_paintedtime;
	s_crossfadeEnd = endtime;

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
				if ( ch->doppler && ch->dopplerScale > 1 ) {
					if ( sampleOffset + (count * ch->dopplerScale) > sc->iSoundLengthInSamples ) {
						count = (sc->iSoundLengthInSamples - sampleOffset) / ch->dopplerScale;

						// avoid infinite loop once length of remaining pSoundData (numerator)
						//	is smaller than dopplerScale (denominator), resulting in 0.
						if ( count == 0 ) {
							break;
						}
					}
				} else {
					if ( sampleOffset + count > sc->iSoundLengthInSamples ) {
						count = sc->iSoundLengthInSamples - sampleOffset;
					}
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
		// transfer out according to DMA format
		S_TransferPaintBuffer( end );
		s_paintedtime = end;
	}
}
