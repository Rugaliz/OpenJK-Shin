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

// tr_mip_lanczos.h -- the Lanczos mip level filter (r_simpleMipMaps 2) of the single player and multiplayer renderers.
//
// Header only, included by the tr_image.cpp of each renderer after its tr_local.h (for byte, M_PI and Com_Clampi). The
// caller allocates the work buffer with its own allocator.

#pragma once

#include "jobs/jobs.h"

#include <math.h>

/*
================
R_MipMapLanczos

Halves the size of the texture in place with a windowed sinc (Lanczos, 3 lobes) filter, one dimension at a time.
The box filter of the original averages 2x2 pixels, which smears the detail of every mip level; this keeps it
sharp, so that textures seen at a grazing angle stay clear under anisotropic filtering. The textures repeat, so
the filter wraps around the edges. The colour is filtered as light (the stored values are made linear first, gamma
2.2, and back after): averaged as stored, fine bright detail on a dark texture (lit panels, stripes on metal) went
darker the further it was. Needs power of two sizes of at least 2x2, and temp room for
R_MipMapLanczosTempSize( inWidth, inHeight ) bytes.
================
*/
#define MIP_LANCZOS_LOBES	3
#define MIP_GAMMA			2.2f

// a stored colour value as light
static const float *R_MipLinear( void ) {
	static float	linear[256];
	static bool		ready = false;
	if ( !ready ) {
		for ( int i = 0; i < 256; i++ ) {
			linear[i] = powf( i / 255.0f, MIP_GAMMA );
		}
		ready = true;
	}
	return linear;
}

typedef struct lanczosJob_s {
	byte		*in;
	float		*temp;
	const float	*weights;
	int			inWidth, inHeight, outWidth, outHeight;
} lanczosJob_t;

static inline size_t R_MipMapLanczosTempSize( int inWidth, int inHeight ) {
	return (size_t)( inWidth >> 1 ) * inHeight * 4 * sizeof( float );
}

static void R_MipMapLanczosHorizontal( int begin, int end, void *context ) {
	const lanczosJob_t *job = (const lanczosJob_t *)context;
	const int widthMask = job->inWidth - 1;

	for ( int y = begin; y < end; y++ ) {
		const byte *row = job->in + (size_t)y * job->inWidth * 4;
		float *out = job->temp + (size_t)y * job->outWidth * 4;
		const float *linear = R_MipLinear();
		for ( int x = 0; x < job->outWidth; x++, out += 4 ) {
			for ( int c = 0; c < 4; c++ ) {
				float total = 0.0f;
				for ( int k = 0; k < MIP_LANCZOS_LOBES; k++ ) {
					const byte a = row[( ( x * 2 - k ) & widthMask ) * 4 + c], b = row[( ( x * 2 + 1 + k ) & widthMask ) * 4 + c];
					total += job->weights[k] * ( c < 3 ? linear[a] + linear[b] : (float)a + b );	// (alpha as it is)
				}
				out[c] = total;
			}
		}
	}
}

static void R_MipMapLanczosVertical( int begin, int end, void *context ) {
	const lanczosJob_t *job = (const lanczosJob_t *)context;
	const int heightMask = job->inHeight - 1;
	const int outWidth = job->outWidth;

	for ( int y = begin; y < end; y++ ) {
		byte *out = job->in + (size_t)y * outWidth * 4;
		for ( int x = 0; x < outWidth; x++, out += 4 ) {
			for ( int c = 0; c < 4; c++ ) {
				float total = 0.0f;
				for ( int k = 0; k < MIP_LANCZOS_LOBES; k++ ) {
					total += job->weights[k] * ( job->temp[( ( ( y * 2 - k ) & heightMask ) * outWidth + x ) * 4 + c] + job->temp[( ( ( y * 2 + 1 + k ) & heightMask ) * outWidth + x ) * 4 + c] );
				}
				if ( c < 3 ) {
					total = 255.0f * powf( Q_max( total, 0.0f ), 1.0f / MIP_GAMMA );
				}
				out[c] = (byte) Com_Clampi( 0, 255, (int) ( total + 0.5f ) );
			}
		}
	}
}

static void R_MipMapLanczos( byte *in, int inWidth, int inHeight, float *temp ) {
	static float	weights[MIP_LANCZOS_LOBES];
	static bool		weightsReady = false;
	const int		outWidth = inWidth >> 1;
	const int		outHeight = inHeight >> 1;
	int				k;

	if ( !weightsReady ) {
		// the output pixel sits between two input pixels, the taps are at 0.5, 1.5, 2.5 input pixels away on
		// either side, which are 0.25, 0.75, 1.25 output pixels
		float sum = 0.0f;
		for ( k = 0; k < MIP_LANCZOS_LOBES; k++ ) {
			const float u = ( k + 0.5f ) * 0.5f;
			const float a = M_PI * u;
			const float b = a / MIP_LANCZOS_LOBES;
			weights[k] = ( sinf( a ) / a ) * ( sinf( b ) / b );
			sum += 2.0f * weights[k];
		}
		for ( k = 0; k < MIP_LANCZOS_LOBES; k++ ) {
			weights[k] /= sum;
		}
		weightsReady = true;
	}

	// horizontal pass: inWidth x inHeight -> outWidth x inHeight into temp
	lanczosJob_t job = { in, temp, weights, inWidth, inHeight, outWidth, outHeight };

	// the rows of both passes are independent of each other, so they are shared between the worker threads
	// (small levels are done by this thread alone, waking the workers costs more than they save)
	const int rowsPerPiece = Q_max( 4096 / Q_max( inWidth, 1 ), 1 );
	Jobs::ParallelFor( inHeight, rowsPerPiece, R_MipMapLanczosHorizontal, &job );
	// vertical pass: outWidth x inHeight -> outWidth x outHeight, written back over the input
	Jobs::ParallelFor( outHeight, rowsPerPiece, R_MipMapLanczosVertical, &job );
}

/*
================
R_MipAlphaIsCutout, R_MipAlphaCoverage, R_MipKeepAlphaCoverage

A texture whose alpha is a cut-out (almost all of it fully there or fully not: grates, railings, leaves, made for an
alpha test) loses its shape in the smaller levels, whose alpha is the average: fewer pixels pass the test the further
it is, until grates and leaves fade away at a distance. The alpha of each smaller level is scaled so that the test
keeps the same share of the texture as in the full size one (as in "Computing Alpha Mipmaps", Castano, 2010).
================
*/
#define MIP_ALPHA_TEST	128		// the share is counted at this threshold, the alpha test of most cut-out stages

static bool R_MipAlphaIsCutout( const byte *data, int count ) {
	int clear = 0, solid = 0;
	for ( int i = 0; i < count; i++ ) {
		const byte a = data[i * 4 + 3];
		clear += ( a <= 8 );
		solid += ( a >= 247 );
	}
	return clear + solid >= count * 95 / 100 && clear >= count / 50 && solid >= count / 50;
}

// the share of the texture the alpha test keeps, with its alpha scaled by scale
static float R_MipAlphaCoverage( const byte *data, int count, float scale = 1.0f ) {
	int kept = 0;
	for ( int i = 0; i < count; i++ ) {
		kept += ( data[i * 4 + 3] * scale >= MIP_ALPHA_TEST );
	}
	return count ? (float)kept / count : 0.0f;
}

static void R_MipKeepAlphaCoverage( byte *data, int count, float coverage ) {
	float lo = 0.0f, hi = 8.0f;
	for ( int i = 0; i < 12; i++ ) {
		const float mid = 0.5f * ( lo + hi );
		if ( R_MipAlphaCoverage( data, count, mid ) < coverage ) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	const float scale = 0.5f * ( lo + hi );
	for ( int i = 0; i < count; i++ ) {
		data[i * 4 + 3] = (byte) Com_Clampi( 0, 255, (int)( data[i * 4 + 3] * scale + 0.5f ) );
	}
}
