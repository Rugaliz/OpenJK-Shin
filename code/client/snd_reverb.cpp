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

// snd_reverb.cpp -- reverberation of the sound of a room
//
// The tail is a feedback delay network: eight delay lines whose outputs are mixed together by an orthogonal matrix
// (so no energy is gained or lost by the mixing) and fed back, each through a gain and a low pass filter that make
// the tail fade at the wanted rate, and faster for the high frequencies. Short all-pass filters first spread the
// input out, so single clicks don't turn into a pattern of echoes. This is the usual way of making a smooth reverb
// from very little processing (see Jot and Chaigne, "Digital delay networks for designing artificial reverberators",
// 1991, and Schroeder, "Natural sounding artificial reverberation", 1962).

#include "snd_reverb.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI	3.14159265358979323846
#endif

#define NUM_LINES		8
#define NUM_ER_TAPS		8
#define NUM_DIFFUSERS	2
#define MAX_PRE_DELAY	0.12f		// seconds
#define MAX_ER_SPAN		0.30f		// seconds, how late the last of the early reflections can be
#define HF_REFERENCE	4000.0f		// the frequency the high frequency decay time is for
#define SMOOTH_SECONDS	0.4f		// how quickly the room follows a change
#define DIFFUSER_GAIN	0.6f

// delays of the lines in samples at 48kHz, prime numbers so the echoes don't line up (22ms to 65ms)
static const int sLineDelays48k[NUM_LINES] = { 1087, 1327, 1559, 1823, 2087, 2411, 2729, 3109 };
static const int sDiffuserDelays48k[NUM_DIFFUSERS] = { 113, 337 };

// how the lines are added up for the left and the right output, and into the input
static const float sLeftSign[NUM_LINES]  = { 1, -1, 1, -1, 1, -1, 1, -1 };
static const float sRightSign[NUM_LINES] = { 1, 1, -1, -1, 1, 1, -1, -1 };
static const float sInputSign[NUM_LINES] = { 1, 1, 1, -1, 1, -1, -1, 1 };

// the early reflections: when (as multiples of the first one), how loud (the sign is a reflection that turns the
// sound over) and to which side (-1 left, +1 right)
static const float sErTime[NUM_ER_TAPS] = { 1.0f, 1.24f, 1.53f, 1.81f, 2.20f, 2.60f, 3.10f, 3.70f };
static const float sErGain[NUM_ER_TAPS] = { 0.80f, -0.70f, 0.60f, -0.55f, 0.50f, -0.40f, 0.35f, -0.30f };
static const float sErPan[NUM_ER_TAPS]  = { -0.6f, 0.7f, 0.3f, -0.8f, 0.8f, -0.3f, 0.5f, -0.5f };

typedef struct
{
	float	*pBuf;
	int		size;
	int		pos;
} delay_t;

typedef struct
{
	delay_t	line[NUM_LINES];
	float	lowPassState[NUM_LINES];
	float	lineGain[NUM_LINES];
	float	lineLowPass[NUM_LINES];		// how much of the last output the low pass filter of a line keeps

	delay_t	diffuser[NUM_DIFFUSERS];
	delay_t	preDelay;
	delay_t	earlyReflections;

	int		rate;
	reverbParams_t	current, target;
	float	tailGain;					// keeps the tail as loud whatever its length

	// what the delays and levels were at the end of the last block: they are ramped from there to the new values
	// across a block, because a step in a delay or a level is a click (and the room changes all the time)
	float	lastPreSamples, lastErFirst, lastErLevel, lastTailLevel;
	bool	ready;
} reverbState_t;

static reverbState_t	sR;
static reverbState_t	sMark;		// sR as it was at S_Reverb_Mark
static bool				sMarkValid = false;

static void Delay_Alloc( delay_t *pDelay, int size )
{
	free( pDelay->pBuf );
	pDelay->size = size < 2 ? 2 : size;
	pDelay->pBuf = (float *)calloc( pDelay->size, sizeof( float ) );
	pDelay->pos = 0;
}

// the sample written n (1 or more) samples ago
static inline float Delay_Read( const delay_t *pDelay, int n )
{
	int i = pDelay->pos - n;
	if ( i < 0 )
		i += pDelay->size;
	return pDelay->pBuf[i];
}

// the same for a delay that is not a whole number of samples
static inline float Delay_ReadFraction( const delay_t *pDelay, float n )
{
	const int whole = (int)n;
	const float frac = n - whole;
	return Delay_Read( pDelay, whole ) * ( 1.0f - frac ) + Delay_Read( pDelay, whole + 1 ) * frac;
}

static inline void Delay_Write( delay_t *pDelay, float value )
{
	pDelay->pBuf[pDelay->pos] = value;
	if ( ++pDelay->pos >= pDelay->size )
		pDelay->pos = 0;
}

void S_Reverb_Init( int sampleRate )
{
	const double scale = (double)sampleRate / 48000.0;

	for ( int i = 0; i < NUM_LINES; i++ )
	{
		Delay_Alloc( &sR.line[i], (int)( sLineDelays48k[i] * scale + 0.5 ) );
		sR.lowPassState[i] = 0.0f;
	}
	for ( int i = 0; i < NUM_DIFFUSERS; i++ )
	{
		Delay_Alloc( &sR.diffuser[i], (int)( sDiffuserDelays48k[i] * scale + 0.5 ) );
	}
	Delay_Alloc( &sR.preDelay, (int)( MAX_PRE_DELAY * sampleRate ) + 4 );
	Delay_Alloc( &sR.earlyReflections, (int)( MAX_ER_SPAN * sampleRate ) + 4 );

	sR.rate = sampleRate;
	memset( &sR.current, 0, sizeof( sR.current ) );
	sR.current.rt60 = 1.0f;
	sR.current.hfRatio = 0.7f;
	sR.target = sR.current;
	sR.lastPreSamples = -1.0f;	// (the first block starts where it ends)
	sR.ready = true;
	sMarkValid = false;
}

void S_Reverb_SetTarget( const reverbParams_t *pParams )
{
	sR.target = *pParams;
}

void S_Reverb_SetNow( const reverbParams_t *pParams )
{
	sR.target = sR.current = *pParams;
	sMark.current = *pParams;
}

static void Delay_Copy( delay_t *pDst, const delay_t *pSrc )
{
	if ( pDst->size != pSrc->size || !pDst->pBuf )
	{
		free( pDst->pBuf );
		pDst->pBuf = (float *)malloc( pSrc->size * sizeof( float ) );
		pDst->size = pSrc->size;
	}
	if ( pDst->pBuf && pSrc->pBuf )
		memcpy( pDst->pBuf, pSrc->pBuf, pSrc->size * sizeof( float ) );
	pDst->pos = pSrc->pos;
}

// everything the reverb has in it and where it has got to (not where it is heading: that is set from outside)
static bool Reverb_CopyState( reverbState_t *pDst, const reverbState_t *pSrc )
{
	for ( int i = 0; i < NUM_LINES; i++ )
		Delay_Copy( &pDst->line[i], &pSrc->line[i] );
	for ( int i = 0; i < NUM_DIFFUSERS; i++ )
		Delay_Copy( &pDst->diffuser[i], &pSrc->diffuser[i] );
	Delay_Copy( &pDst->preDelay, &pSrc->preDelay );
	Delay_Copy( &pDst->earlyReflections, &pSrc->earlyReflections );

	memcpy( pDst->lowPassState, pSrc->lowPassState, sizeof( pDst->lowPassState ) );
	memcpy( pDst->lineGain, pSrc->lineGain, sizeof( pDst->lineGain ) );
	memcpy( pDst->lineLowPass, pSrc->lineLowPass, sizeof( pDst->lineLowPass ) );
	pDst->rate = pSrc->rate;
	pDst->current = pSrc->current;
	pDst->tailGain = pSrc->tailGain;
	pDst->lastPreSamples = pSrc->lastPreSamples;
	pDst->lastErFirst = pSrc->lastErFirst;
	pDst->lastErLevel = pSrc->lastErLevel;
	pDst->lastTailLevel = pSrc->lastTailLevel;
	pDst->ready = pSrc->ready;

	for ( int i = 0; i < NUM_LINES; i++ )
		if ( !pDst->line[i].pBuf ) return false;
	for ( int i = 0; i < NUM_DIFFUSERS; i++ )
		if ( !pDst->diffuser[i].pBuf ) return false;
	return pDst->preDelay.pBuf && pDst->earlyReflections.pBuf;
}

void S_Reverb_Mark( void )
{
	sMarkValid = sR.ready && Reverb_CopyState( &sMark, &sR );
}

int S_Reverb_Rewind( void )
{
	if ( !sMarkValid )
		return 0;

	const reverbParams_t target = sR.target;
	if ( !Reverb_CopyState( &sR, &sMark ) )
	{
		sR.ready = false;	// (out of memory: silence rather than garbage)
		sMarkValid = false;
		return 0;
	}
	sR.target = target;
	return 1;
}

// what the filters of the lines have to be for the current room
static void Reverb_UpdateFilters( void )
{
	float rt60 = sR.current.rt60;
	float hfRatio = sR.current.hfRatio;
	if ( rt60 < 0.05f )		rt60 = 0.05f;
	if ( hfRatio < 0.05f )	hfRatio = 0.05f;
	if ( hfRatio > 1.0f )	hfRatio = 1.0f;

	const double omega = 2.0 * M_PI * HF_REFERENCE / sR.rate;
	const double cosOmega = cos( omega );
	double gainSum = 0.0;

	for ( int i = 0; i < NUM_LINES; i++ )
	{
		const double delay = (double)sR.line[i].size / sR.rate;		// seconds a trip through this line takes

		// the gain per trip that makes the tail fade by 60dB in rt60 seconds (and in rt60 * hfRatio for the highs)
		const double gLow = pow( 10.0, -3.0 * delay / rt60 );
		const double gHigh = pow( 10.0, -3.0 * delay / ( rt60 * hfRatio ) );
		const double r = gHigh / gLow;				// what the low pass has to hold the highs back to

		// a one pole low pass, y = (1 - p) x + p y', with gain r at the reference frequency: solve for p
		double p = 0.0;
		if ( r < 0.999 )
		{
			const double a = r * r - 1.0;
			const double b = 2.0 - 2.0 * r * r * cosOmega;
			const double disc = b * b - 4.0 * a * a;
			if ( disc >= 0.0 )
			{
				const double p1 = ( -b + sqrt( disc ) ) / ( 2.0 * a );
				const double p2 = ( -b - sqrt( disc ) ) / ( 2.0 * a );
				p = ( p1 >= 0.0 && p1 < 1.0 ) ? p1 : p2;
				if ( p < 0.0 )		p = 0.0;
				if ( p > 0.98 )		p = 0.98;
			}
		}

		sR.lineGain[i] = (float)gLow;
		sR.lineLowPass[i] = (float)p;
		gainSum += gLow;
	}

	// The tail builds up more the longer it lasts. Scaling it by what is left after a trip through a line keeps
	// its loudness about the same for every length (which would otherwise be very different).
	const double g = gainSum / NUM_LINES;
	sR.tailGain = (float)( sqrt( 1.0 - g * g ) * 0.9 );
}

void S_Reverb_Process( const float *pIn, int n, float *pOutLeft, float *pOutRight )
{
	if ( !sR.ready )
	{
		memset( pOutLeft, 0, n * sizeof( float ) );
		memset( pOutRight, 0, n * sizeof( float ) );
		return;
	}

	// follow the target
	{
		const float k = 1.0f - expf( -(float)n / ( sR.rate * SMOOTH_SECONDS ) );
		sR.current.rt60 += ( sR.target.rt60 - sR.current.rt60 ) * k;
		sR.current.hfRatio += ( sR.target.hfRatio - sR.current.hfRatio ) * k;
		sR.current.preDelay += ( sR.target.preDelay - sR.current.preDelay ) * k;
		sR.current.erDelay += ( sR.target.erDelay - sR.current.erDelay ) * k;
		sR.current.erLevel += ( sR.target.erLevel - sR.current.erLevel ) * k;
		sR.current.lateLevel += ( sR.target.lateLevel - sR.current.lateLevel ) * k;
		Reverb_UpdateFilters();
	}

	float preSamples = sR.current.preDelay * sR.rate;
	if ( preSamples < 1.0f )												preSamples = 1.0f;
	if ( preSamples > MAX_PRE_DELAY * sR.rate )								preSamples = MAX_PRE_DELAY * sR.rate;

	float erFirst = sR.current.erDelay * sR.rate;
	if ( erFirst < 8.0f )													erFirst = 8.0f;
	if ( erFirst * sErTime[NUM_ER_TAPS - 1] > MAX_ER_SPAN * sR.rate )		erFirst = MAX_ER_SPAN * sR.rate / sErTime[NUM_ER_TAPS - 1];

	const float erLevelEnd = sR.current.erLevel * 0.5f;
	const float tailLevelEnd = sR.current.lateLevel * sR.tailGain;
	const float hadamard = 0.35355339f;		// 1 / sqrt( 8 )

	if ( sR.lastPreSamples < 0.0f )
	{
		sR.lastPreSamples = preSamples;
		sR.lastErFirst = erFirst;
		sR.lastErLevel = erLevelEnd;
		sR.lastTailLevel = tailLevelEnd;
	}

	// A delay that changes is a change of pitch for what goes through it: let it move by 2% of the time that passes
	// at the most (the room is followed for as long as it takes)
	const float maxDelayChange = 0.02f * n;
	preSamples = fminf( fmaxf( preSamples, sR.lastPreSamples - maxDelayChange ), sR.lastPreSamples + maxDelayChange );
	erFirst = fminf( fmaxf( erFirst, sR.lastErFirst - maxDelayChange ), sR.lastErFirst + maxDelayChange );

	const float preStep = ( preSamples - sR.lastPreSamples ) / n;
	const float erStep = ( erFirst - sR.lastErFirst ) / n;
	const float erLevelStep = ( erLevelEnd - sR.lastErLevel ) / n;
	const float tailLevelStep = ( tailLevelEnd - sR.lastTailLevel ) / n;
	float preNow = sR.lastPreSamples, erNow = sR.lastErFirst, erLevel = sR.lastErLevel, tailLevel = sR.lastTailLevel;

	for ( int s = 0; s < n; s++ )
	{
		// (the tiny offset, far below anything audible, keeps the fading tail from ending up as denormal numbers,
		// which are very slow to work with on some processors)
		const float x = pIn[s] + 1e-18f;

		preNow += preStep;
		erNow += erStep;
		erLevel += erLevelStep;
		tailLevel += tailLevelStep;

		// early reflections, straight from the input
		Delay_Write( &sR.earlyReflections, x );
		float erLeft = 0.0f, erRight = 0.0f;
		for ( int t = 0; t < NUM_ER_TAPS; t++ )
		{
			const float v = Delay_ReadFraction( &sR.earlyReflections, erNow * sErTime[t] ) * sErGain[t];
			erLeft += v * ( 1.0f - sErPan[t] );
			erRight += v * ( 1.0f + sErPan[t] );
		}

		// the tail: delayed, then spread out
		Delay_Write( &sR.preDelay, x );
		float a = Delay_ReadFraction( &sR.preDelay, preNow );
		for ( int d = 0; d < NUM_DIFFUSERS; d++ )
		{
			const int len = sR.diffuser[d].size;
			const float z = Delay_Read( &sR.diffuser[d], len );
			const float y = z - DIFFUSER_GAIN * a;
			Delay_Write( &sR.diffuser[d], a + DIFFUSER_GAIN * y );
			a = y;
		}

		// what comes out of the lines, after what happens to it on the way
		float y[NUM_LINES];
		for ( int l = 0; l < NUM_LINES; l++ )
		{
			const float out = Delay_Read( &sR.line[l], sR.line[l].size );
			sR.lowPassState[l] = out * ( 1.0f - sR.lineLowPass[l] ) + sR.lowPassState[l] * sR.lineLowPass[l];
			y[l] = sR.lowPassState[l] * sR.lineGain[l];
		}

		// the output is the sum of that, differently for each ear
		float tailLeft = 0.0f, tailRight = 0.0f;
		for ( int l = 0; l < NUM_LINES; l++ )
		{
			tailLeft += y[l] * sLeftSign[l];
			tailRight += y[l] * sRightSign[l];
		}

		// mixing all of it with all of it (a Hadamard matrix: three rounds of sums and differences)
		float m[NUM_LINES];
		for ( int l = 0; l < NUM_LINES; l += 2 ) { m[l] = y[l] + y[l + 1]; m[l + 1] = y[l] - y[l + 1]; }
		for ( int l = 0; l < NUM_LINES; l += 4 ) { float v0 = m[l], v1 = m[l + 1], v2 = m[l + 2], v3 = m[l + 3]; m[l] = v0 + v2; m[l + 1] = v1 + v3; m[l + 2] = v0 - v2; m[l + 3] = v1 - v3; }
		for ( int l = 0; l < 4; l++ ) { const float v0 = m[l], v1 = m[l + 4]; m[l] = v0 + v1; m[l + 4] = v0 - v1; }

		// ...and back into the lines with the input
		for ( int l = 0; l < NUM_LINES; l++ )
		{
			Delay_Write( &sR.line[l], m[l] * hadamard + a * sInputSign[l] * 0.35f );
		}

		pOutLeft[s] = erLeft * erLevel + tailLeft * tailLevel * 0.5f;
		pOutRight[s] = erRight * erLevel + tailRight * tailLevel * 0.5f;
	}

	sR.lastPreSamples = preSamples;
	sR.lastErFirst = erFirst;
	sR.lastErLevel = erLevelEnd;
	sR.lastTailLevel = tailLevelEnd;
}
