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

// snd_hrtf.cpp -- binaural (HRTF) positioning of sounds for headphones

#include "snd_hrtf.h"
#include "snd_resample.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HRTF_SOURCE_RATE	44100
#define HRTF_SOURCE_TAPS	128
#define HRTF_BLOCK			32		// the filter is moved towards its new value in steps of this many samples

// The loudness of a sound heard through these responses, averaged over all directions and over 200Hz-8kHz, is
// (|left|^2 + |right|^2) = 1.71 of the sound unfiltered. The engine's left/right panning averages 0.75 over the same
// directions, so scaling the responses by sqrt(0.75 / 1.71) keeps sounds as loud as they were when switching to HRTF.
#define HRTF_LEVEL			0.66f

static float	*sTable = NULL;		// [response][ear][tap] at the output rate
static int		sTaps = 0;
static int		sRate = 0;

static inline const float *HRTF_Response( int response, int ear )
{
	return sTable + ( response * 2 + ear ) * HRTF_MAX_TAPS;
}

int S_HRTF_Init( int sampleRate )
{
	if ( sampleRate < 8000 || sampleRate > 192000 )
		return 0;
	if ( sTable && sRate == sampleRate )
		return 1;

	free( sTable );
	sTable = (float *)calloc( (size_t)g_hrtfNumResponses * 2 * HRTF_MAX_TAPS, sizeof( float ) );
	if ( !sTable )
		return 0;

	const double dStep = (double)HRTF_SOURCE_RATE / sampleRate;		// source samples per output sample
	sTaps = (int)ceil( HRTF_SOURCE_TAPS / dStep );
	if ( sTaps > HRTF_MAX_TAPS )
		sTaps = HRTF_MAX_TAPS;		// the quiet end of the response is lost at very high rates

	resampleFilter_t filter;
	S_Resample_SetupFilter( &filter, dStep );

	for ( int r = 0; r < g_hrtfNumResponses; r++ )
	{
		for ( int ear = 0; ear < 2; ear++ )
		{
			short src[HRTF_SOURCE_TAPS];
			float *pOut = sTable + ( r * 2 + ear ) * HRTF_MAX_TAPS;

			memcpy( src, g_hrtfResponse[r][ear], sizeof( src ) );
			for ( int i = 0; i < sTaps; i++ )
			{
				float f;
				S_Resample_Interp( src, 1, HRTF_SOURCE_TAPS, i * dStep, &filter, &f );
				// the response is a sampled function of time: at another rate there are more (or fewer) samples
				// of it, each carrying its share of the same energy, so the level is kept by scaling with the step
				pOut[i] = f * (float)dStep * ( HRTF_LEVEL / 32768.0f );
			}
		}
	}

	sRate = sampleRate;
	return 1;
}

int S_HRTF_NumTaps( void )
{
	return sTaps;
}

// adds weight * response (of the given ear) to the filter being built
static void HRTF_Accumulate( float *pDst, int response, int ear, float weight )
{
	if ( weight <= 0.0f )
		return;
	const float *pSrc = HRTF_Response( response, ear );
	for ( int i = 0; i < sTaps; i++ )
		pDst[i] += pSrc[i] * weight;
}

// the filter for a direction within one ring of equal elevation, azimuth being 0..180 on the side of the measurements
static void HRTF_RingFilter( int ring, float azimuth, float *pLeft, float *pRight, float weight )
{
	const int start = g_hrtfRingStart[ring];
	const int count = g_hrtfRingCount[ring];
	int k = 0;

	if ( count > 1 )
	{
		while ( k < count - 2 && azimuth >= g_hrtfAzimuth[start + k + 1] )
			k++;
	}

	float t = 0.0f;
	if ( count > 1 )
	{
		const float a0 = g_hrtfAzimuth[start + k];
		const float a1 = g_hrtfAzimuth[start + k + 1];
		t = ( azimuth - a0 ) / ( a1 - a0 );
		if ( t < 0.0f )			t = 0.0f;
		else if ( t > 1.0f )	t = 1.0f;
	}

	const int r0 = start + k;
	const int r1 = ( count > 1 ) ? start + k + 1 : r0;

	HRTF_Accumulate( pLeft, r0, 0, weight * ( 1.0f - t ) );
	HRTF_Accumulate( pLeft, r1, 0, weight * t );
	HRTF_Accumulate( pRight, r0, 1, weight * ( 1.0f - t ) );
	HRTF_Accumulate( pRight, r1, 1, weight * t );
}

void S_HRTF_GetFilter( float azimuth, float elevation, hrtfFilter_t *pFilter )
{
	memset( pFilter, 0, sizeof( *pFilter ) );
	if ( !sTable )
		return;

	// azimuth to -180..180
	while ( azimuth > 180.0f )		azimuth -= 360.0f;
	while ( azimuth < -180.0f )		azimuth += 360.0f;

	// the measurements are of sources on the right, a source on the left sounds the same with the ears swapped
	const bool bMirror = ( azimuth < 0.0f );
	const float a = fabsf( azimuth );

	// nothing was measured below -40 degrees
	if ( elevation < g_hrtfRingElevation[0] )						elevation = g_hrtfRingElevation[0];
	if ( elevation > g_hrtfRingElevation[g_hrtfNumRings - 1] )		elevation = g_hrtfRingElevation[g_hrtfNumRings - 1];

	int ring = 0;
	while ( ring < g_hrtfNumRings - 2 && elevation >= g_hrtfRingElevation[ring + 1] )
		ring++;

	const float e0 = g_hrtfRingElevation[ring];
	const float e1 = g_hrtfRingElevation[ring + 1];
	const float t = ( elevation - e0 ) / ( e1 - e0 );

	float *pNear = bMirror ? pFilter->right : pFilter->left;		// the ear on the far side of the measured source...
	float *pFar = bMirror ? pFilter->left : pFilter->right;			// ...and the ear on the near side

	HRTF_RingFilter( ring, a, pNear, pFar, 1.0f - t );
	HRTF_RingFilter( ring + 1, a, pNear, pFar, t );
}

void S_HRTF_ResetState( hrtfState_t *pState )
{
	memset( pState, 0, sizeof( *pState ) );
}

// out[i] += h[j] * x[i - j] for a block, written so the compiler can vectorise the loop over i
static void HRTF_ConvolveBlock( const float *pTaps, const float *pX, int n, float *pOut )
{
	for ( int j = 0; j < sTaps; j++ )
	{
		const float h = pTaps[j];
		const float *pXj = pX - j;
		for ( int i = 0; i < n; i++ )
			pOut[i] += h * pXj[i];
	}
}

void S_HRTF_Process( hrtfState_t *pState, const hrtfFilter_t *pTarget, const short *pIn, int n, float *pOutLeft, float *pOutRight )
{
	const int history = sTaps - 1;
	float x[HRTF_MAX_TAPS + 1024];

	if ( sTaps <= 0 )
	{
		memset( pOutLeft, 0, n * sizeof( float ) );
		memset( pOutRight, 0, n * sizeof( float ) );
		return;
	}

	if ( !pState->valid )
	{
		pState->filter = *pTarget;
		pState->valid = 1;
	}

	for ( int done = 0; done < n; )
	{
		int chunk = n - done;
		if ( chunk > 1024 )
			chunk = 1024;

		// the end of the previous signal, then this chunk
		memcpy( x, pState->history + ( HRTF_MAX_TAPS - history ), history * sizeof( float ) );
		for ( int i = 0; i < chunk; i++ )
			x[history + i] = (float)pIn[done + i];

		const int numBlocks = ( chunk + HRTF_BLOCK - 1 ) / HRTF_BLOCK;
		for ( int b = 0; b < numBlocks; b++ )
		{
			const int first = b * HRTF_BLOCK;
			const int len = ( chunk - first < HRTF_BLOCK ) ? chunk - first : HRTF_BLOCK;

			// where the filter has got to at the end of this step of the change
			const float t = (float)( b + 1 ) / numBlocks;
			float coeffL[HRTF_MAX_TAPS], coeffR[HRTF_MAX_TAPS];
			for ( int j = 0; j < sTaps; j++ )
			{
				coeffL[j] = pState->filter.left[j] + ( pTarget->left[j] - pState->filter.left[j] ) * t;
				coeffR[j] = pState->filter.right[j] + ( pTarget->right[j] - pState->filter.right[j] ) * t;
			}

			float *pL = pOutLeft + done + first;
			float *pR = pOutRight + done + first;
			memset( pL, 0, len * sizeof( float ) );
			memset( pR, 0, len * sizeof( float ) );
			HRTF_ConvolveBlock( coeffL, x + history + first, len, pL );
			HRTF_ConvolveBlock( coeffR, x + history + first, len, pR );
		}

		// the filter is now the target, and the next call starts from the end of this chunk
		pState->filter = *pTarget;
		{
			// keep the last HRTF_MAX_TAPS samples of the signal, newest last
			float tail[HRTF_MAX_TAPS];
			const int avail = history + chunk;
			for ( int i = 0; i < HRTF_MAX_TAPS; i++ )
			{
				const int src = avail - HRTF_MAX_TAPS + i;
				tail[i] = ( src >= 0 ) ? x[src] : 0.0f;
			}
			memcpy( pState->history, tail, sizeof( tail ) );
		}

		done += chunk;
	}
}
