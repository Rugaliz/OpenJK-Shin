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

// snd_hrtf.h -- binaural (HRTF) positioning of sounds for headphones
//
// A head-related transfer function describes how a sound from a given direction is changed on its way to each ear
// (it arrives at the nearer ear earlier and louder, and the head and outer ears colour it differently depending on
// where it came from). Filtering a sound with the pair of impulse responses for its direction makes it appear to come
// from that direction when listened to on headphones, including from behind, above and below, which plain left/right
// volume panning cannot do.
//
// The impulse responses are the MIT Media Lab KEMAR measurements (see snd_hrtf_data.cpp for the credit and terms).
//
// This file does not depend on the rest of the engine, apart from the sample rate conversion in snd_resample.h.

#ifndef SND_HRTF_H
#define SND_HRTF_H

#define HRTF_MAX_TAPS	192		// the measured responses are 128 samples at 44.1kHz, this leaves room up to ~66kHz

// A pair of impulse responses, one per ear
typedef struct
{
	float	left[HRTF_MAX_TAPS];
	float	right[HRTF_MAX_TAPS];
} hrtfFilter_t;

// What one sound needs to remember between calls: the end of its signal, and the filter it was last heard through
typedef struct
{
	float			history[HRTF_MAX_TAPS];
	hrtfFilter_t	filter;
	int				valid;		// 0 until the first Process call
} hrtfState_t;

// Prepares the filters for an output rate (and can be called again if the rate changes). Returns 0 on failure.
int		S_HRTF_Init( int sampleRate );

// Number of taps of every filter at the current rate
int		S_HRTF_NumTaps( void );

// The filter for a direction: azimuth in degrees from straight ahead, positive to the right (-180..180),
// elevation in degrees, positive upwards (-90..90). Directions are interpolated between the measured ones.
void	S_HRTF_GetFilter( float azimuth, float elevation, hrtfFilter_t *pFilter );

void	S_HRTF_ResetState( hrtfState_t *pState );

// Runs n mono samples through the filter, giving n samples for each ear (the sample scale is the input's, a 16 bit
// range signal stays in a 16 bit range). If the filter is different from the one used last time, the change is made
// gradually over the n samples. Any n is fine, but keep the pieces of one sound in order.
void	S_HRTF_Process( hrtfState_t *pState, const hrtfFilter_t *pTarget, const short *pIn, int n, float *pOutLeft, float *pOutRight );

// The data (snd_hrtf_data.cpp)
extern const int	g_hrtfNumRings;
extern const short	g_hrtfRingElevation[];
extern const short	g_hrtfRingCount[];
extern const short	g_hrtfRingStart[];
extern const int	g_hrtfNumResponses;
extern const short	g_hrtfAzimuth[];
extern const short	g_hrtfResponse[][2][128];

#endif	// SND_HRTF_H
