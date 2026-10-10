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
#if defined( __SSE2__ ) || defined( _M_X64 ) || ( defined( _M_IX86_FP ) && _M_IX86_FP >= 2 )
#include <emmintrin.h>
#define RESAMPLE_SSE2
#endif
#include <stdlib.h>
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

// Tables are kept for the life of the program (filters point at them); there are only ever a few steps in use
#define RESAMPLE_MAX_TABLES		12
#define RESAMPLE_MAX_PHASES		1024
#define RESAMPLE_MAX_TABLE_SIZE	( 1 << 18 )	// floats

static resampleTable_t	sTables[RESAMPLE_MAX_TABLES];
static int				sNumTables = 0;

static const resampleTable_t *S_Resample_FindTable( double dStep, float cutoff, float support )
{
	for ( int i = 0; i < sNumTables; i++ )
	{
		if ( sTables[i].dStep == dStep && sTables[i].cutoff == cutoff )
			return &sTables[i];
	}
	if ( sNumTables >= RESAMPLE_MAX_TABLES || dStep <= 0.0 )
		return NULL;

	// the smallest number of phases that the positions i * dStep all fall on
	int numPhases = 0;
	for ( int q = 1; q <= RESAMPLE_MAX_PHASES; q++ )
	{
		const double d = dStep * q;
		if ( fabs( d - floor( d + 0.5 ) ) < 1e-9 * q )
		{
			numPhases = q;
			break;
		}
	}
	if ( !numPhases )
		return NULL;

	// every source sample within the support of any position between floor( position ) and the next one
	const int kMin = -(int)ceil( support );
	const int kMax = (int)ceil( support ) + 1;
	const int numTaps = kMax - kMin + 1;
	if ( numPhases * numTaps > RESAMPLE_MAX_TABLE_SIZE )
		return NULL;

	float *pWeights = (float *)malloc( (size_t)numPhases * numTaps * sizeof( float ) );
	if ( !pWeights )
		return NULL;

	for ( int p = 0; p < numPhases; p++ )
	{
		const double frac = (double)p / numPhases;
		float *w = pWeights + p * numTaps;
		float fSum = 0.0f;

		// the same weights S_Resample_Interp works out for a position this far past a source sample
		for ( int k = 0; k < numTaps; k++ )
		{
			w[k] = S_Resample_Kernel( (float)( frac - ( kMin + k ) ) * cutoff );
			fSum += w[k];
		}
		const float fScale = ( fSum > 0.0f ) ? 1.0f / fSum : 0.0f;
		for ( int k = 0; k < numTaps; k++ )
			w[k] *= fScale;
	}

	resampleTable_t *pTable = &sTables[sNumTables++];
	pTable->dStep = dStep;
	pTable->cutoff = cutoff;
	pTable->numPhases = numPhases;
	pTable->kMin = kMin;
	pTable->numTaps = numTaps;
	pTable->pWeights = pWeights;
	return pTable;
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
	pFilter->pTable = S_Resample_FindTable( dStep, pFilter->cutoff, pFilter->support );
}

// S_Resample_Interp with the weights from a table: the position is rounded to the nearest phase (positions i * dStep
// are on one already, give or take the rounding of a double)
static void S_Resample_InterpTable( const short *pSrc, int nChan, int nFrames, double dPos, const resampleTable_t *pTable, float *fOut )
{
	double dWhole = floor( dPos );
	int iPhase = (int)( ( dPos - dWhole ) * pTable->numPhases + 0.5 );
	if ( iPhase >= pTable->numPhases )
	{
		iPhase -= pTable->numPhases;
		dWhole += 1.0;
	}

	const float *w = pTable->pWeights + iPhase * pTable->numTaps;
	const int iBase = (int)dWhole + pTable->kMin;
	int kFirst = 0, kEnd = pTable->numTaps;

	// samples outside the data are silence (their weight still counted when the weights were divided by their sum)
	if ( iBase < 0 )
		kFirst = -iBase;
	if ( iBase + kEnd > nFrames )
		kEnd = nFrames - iBase;

	if ( nChan == 2 )
	{
		float fL = 0.0f, fR = 0.0f;
		for ( int k = kFirst; k < kEnd; k++ )
		{
			fL += w[k] * pSrc[( iBase + k ) * 2];
			fR += w[k] * pSrc[( iBase + k ) * 2 + 1];
		}
		fOut[0] = fL;
		fOut[1] = fR;
	}
	else
	{
		int k = kFirst;
		float f = 0.0f;
#ifdef RESAMPLE_SSE2
		// four taps at a time (the result differs from the plain sum by rounding of the last digit of a float)
		__m128 acc = _mm_setzero_ps();
		const __m128i zero = _mm_setzero_si128();
		for ( ; k + 4 <= kEnd; k += 4 )
		{
			const __m128i s16 = _mm_loadl_epi64( (const __m128i *)( pSrc + iBase + k ) );
			const __m128i s32 = _mm_srai_epi32( _mm_unpacklo_epi16( zero, s16 ), 16 );	// sign extended
			acc = _mm_add_ps( acc, _mm_mul_ps( _mm_loadu_ps( w + k ), _mm_cvtepi32_ps( s32 ) ) );
		}
		float lanes[4];
		_mm_storeu_ps( lanes, acc );
		f = ( lanes[0] + lanes[1] ) + ( lanes[2] + lanes[3] );
#endif
		for ( ; k < kEnd; k++ )
			f += w[k] * pSrc[iBase + k];
		fOut[0] = f;
	}
}

void S_Resample_Interp( const short *pSrc, int nChan, int nFrames, double dPos, const resampleFilter_t *pFilter, float *fOut )
{
	if ( pFilter->pTable )
	{
		S_Resample_InterpTable( pSrc, nChan, nFrames, dPos, pFilter->pTable, fOut );
		return;
	}

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
