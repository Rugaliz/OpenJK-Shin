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

// snd_resample.cpp -- band-limited (windowed sinc) sample rate conversion shared by the sound code

#include "snd_resample.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI	3.14159265358979323846
#endif

// Low pass kernel: sinc(u) times a 4 term Blackman-Harris window, for u in [0, RESAMPLE_HALF_TAPS].
// Stored as a table with RESAMPLE_TABLE_RES entries per unit and linearly interpolated.
#define RESAMPLE_TABLE_RES	256

static float	sKernelTable[RESAMPLE_HALF_TAPS * RESAMPLE_TABLE_RES + 2];
static bool		sKernelTableBuilt = false;

void S_Resample_Init( void )
{
	if ( sKernelTableBuilt )
		return;

	for ( int n = 0; n < RESAMPLE_HALF_TAPS * RESAMPLE_TABLE_RES + 2; n++ )
	{
		const double u = (double)n / RESAMPLE_TABLE_RES;
		if ( u >= RESAMPLE_HALF_TAPS )
		{
			sKernelTable[n] = 0.0f;
			continue;
		}
		const double sinc = ( n == 0 ) ? 1.0 : sin( M_PI * u ) / ( M_PI * u );
		const double t = u / RESAMPLE_HALF_TAPS;	// 0..1 across the half window
		const double window = 0.35875 + 0.48829 * cos( M_PI * t ) + 0.14128 * cos( 2.0 * M_PI * t ) + 0.01168 * cos( 3.0 * M_PI * t );
		sKernelTable[n] = (float)( sinc * window );
	}
	sKernelTableBuilt = true;
}

float S_Resample_Kernel( float u )
{
	if ( u < 0.0f )
		u = -u;
	if ( u >= RESAMPLE_HALF_TAPS )
		return 0.0f;
	const float f = u * RESAMPLE_TABLE_RES;
	const int i = (int)f;
	const float t = f - i;
	return sKernelTable[i] + ( sKernelTable[i + 1] - sKernelTable[i] ) * t;
}

void S_Resample_SetupFilter( resampleFilter_t *pFilter, double dStep )
{
	S_Resample_Init();

	// Everything is kept when upsampling, and when downsampling only what the new, lower rate can represent.
	// (The cutoff is limited so the filter, and with it the history a stream has to remember, stays a sane size.)
	pFilter->cutoff = ( dStep > 1.0 ) ? (float)( 1.0 / dStep ) : 1.0f;
	if ( pFilter->cutoff < 0.25f )
		pFilter->cutoff = 0.25f;
	pFilter->support = RESAMPLE_HALF_TAPS / pFilter->cutoff;
}

void S_Resample_Interp( const short *pSrc, int nChan, int nFrames, double dPos, const resampleFilter_t *pFilter, float *fOut )
{
	const int iFirst = (int)ceil( dPos - pFilter->support );
	const int iLast = (int)floor( dPos + pFilter->support );
	float fSum[2] = { 0.0f, 0.0f };
	float fWeightSum = 0.0f;

	for ( int k = iFirst; k <= iLast; k++ )
	{
		const float fWeight = S_Resample_Kernel( (float)( dPos - k ) * pFilter->cutoff );

		// samples outside the data are silence, but their weight still counts so the level stays right
		fWeightSum += fWeight;
		if ( k < 0 || k >= nFrames )
			continue;

		if ( nChan == 2 )
		{
			fSum[0] += fWeight * pSrc[k * 2];
			fSum[1] += fWeight * pSrc[k * 2 + 1];
		}
		else
		{
			fSum[0] += fWeight * pSrc[k];
		}
	}

	const float fScale = ( fWeightSum > 0.0f ) ? 1.0f / fWeightSum : 0.0f;
	fOut[0] = fSum[0] * fScale;
	if ( nChan == 2 )
		fOut[1] = fSum[1] * fScale;
}


void S_ResampleStream_Reset( resampleStream_t *pStream, int nChan, double dStep )
{
	memset( pStream, 0, sizeof( *pStream ) );
	pStream->nChan = ( nChan == 2 ) ? 2 : 1;
	pStream->dStep = dStep;
	S_Resample_SetupFilter( &pStream->filter, dStep );
}

int S_ResampleStream_Process( resampleStream_t *pStream, const short *pIn, int nIn, float *fOut, int nMaxOut )
{
	const int nChan = pStream->nChan;

	if ( nIn > RESAMPLE_STREAM_MAXIN )
		nIn = RESAMPLE_STREAM_MAXIN;
	if ( nIn <= 0 )
		return 0;

	// the remembered frames followed by the new ones
	short work[( RESAMPLE_STREAM_HISTORY + RESAMPLE_STREAM_MAXIN ) * 2];
	const int nTotal = pStream->nHist + nIn;
	memcpy( work, pStream->hist, pStream->nHist * nChan * sizeof( short ) );
	memcpy( work + pStream->nHist * nChan, pIn, nIn * nChan * sizeof( short ) );

	int nOut = 0;
	while ( nOut < nMaxOut && pStream->dPos + pStream->filter.support < nTotal )	// every sample the filter needs is there
	{
		S_Resample_Interp( work, nChan, nTotal, pStream->dPos, &pStream->filter, fOut + nOut * nChan );
		nOut++;
		pStream->dPos += pStream->dStep;
	}

	// keep what the next output position still needs
	int iKeep = (int)floor( pStream->dPos - pStream->filter.support );
	if ( iKeep < 0 )
		iKeep = 0;
	if ( nTotal - iKeep > RESAMPLE_STREAM_HISTORY )
		iKeep = nTotal - RESAMPLE_STREAM_HISTORY;	// output was cut short, give up the oldest input
	pStream->nHist = nTotal - iKeep;
	memcpy( pStream->hist, work + iKeep * nChan, pStream->nHist * nChan * sizeof( short ) );
	pStream->dPos -= iKeep;

	return nOut;
}
