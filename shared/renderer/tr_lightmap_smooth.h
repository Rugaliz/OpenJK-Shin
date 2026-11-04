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

// tr_lightmap_smooth.h -- smoother lightmaps (r_smoothLightmaps)
//
// The light of a level is stored in lightmaps of a few texels per metre, 128 x 128 to a page, and drawn with linear
// filtering: between two texels the light changes in a straight line, and at the texels the slope changes at once. So
// the edges of shadows and the pools of light show the texel grid as steps and diamonds. Here every page is made
// LIGHTMAP_SMOOTH_SCALE times larger as the level loads, with a bicubic filter (Catmull-Rom: it goes through the value
// of every texel, so the light stays as it was made, only the way it changes between texels is smooth; see LM_Between), and drawn
// with linear filtering and mipmaps as before. That costs nothing as the game runs, works the same in every renderer,
// and takes 16 times the memory for the lightmaps (a megabyte a page).
//
// A page holds the lightmaps of many surfaces side by side; a bicubic filter reaches a texel further than a linear one
// does, so it would bring in light from the neighbour. Each surface's rectangle (from the surfaces of the BSP) is
// filtered on its own, its edge texels repeated beyond its edge.
//
// Included once by the tr_bsp.cpp of each renderer.

#pragma once

#include <stdlib.h>
#include <math.h>

#define LIGHTMAP_SMOOTH_SCALE	4

typedef struct {
	int		x, y, width, height;
} lightmapRect_t;

// Catmull-Rom weights of the four texels around a point at fraction t from the second to the third
static void LM_CubicWeights( float t, float w[4] ) {
	const float t2 = t * t;
	const float t3 = t2 * t;

	w[0] = 0.5f * ( -t3 + 2.0f * t2 - t );
	w[1] = 0.5f * ( 3.0f * t3 - 5.0f * t2 + 2.0f );
	w[2] = 0.5f * ( -3.0f * t3 + 4.0f * t2 + t );
	w[3] = 0.5f * ( t3 - t2 );
}

// A cubic curve swings past the values on either side of a sharp change (the edge of a shadow), which would draw a
// line brighter or darker than anything in the lightmap along it: what it gives is kept between the two texels it is
// between, so it is smooth where the light changes smoothly and never makes light that wasn't there.
static float LM_Between( float value, float a, float b ) {
	const float low = ( a < b ) ? a : b;
	const float high = ( a < b ) ? b : a;
	return ( value < low ) ? low : ( value > high ) ? high : value;
}

// The texels [x0, x1) x [y0, y1) of in (RGBA, size x size) made larger into their place in out (RGBA, size *
// LIGHTMAP_SMOOTH_SCALE square), with nothing from outside the rectangle; a row at a time, then a column at a time
static void LM_SmoothRect( const byte *in, int size, int x0, int y0, int x1, int y1, byte *out ) {
	const int	scale = LIGHTMAP_SMOOTH_SCALE;
	const int	outSize = size * scale;
	const int	width = x1 - x0;
	const int	height = y1 - y0;
	const int	outWidth = width * scale;
	const int	outHeight = height * scale;
	float		*rows = (float *)malloc( (size_t)height * outWidth * 3 * sizeof( float ) );
	int			ox, oy, k, c;

	if ( !rows ) {
		return;
	}

	for ( ox = 0; ox < outWidth; ox++ ) {
		const float	u = ( ox + 0.5f ) / scale - 0.5f;
		const int	base = (int)floorf( u );
		float		w[4];
		int			sx[4];

		LM_CubicWeights( u - base, w );
		for ( k = 0; k < 4; k++ ) {
			sx[k] = base - 1 + k;
			sx[k] = ( sx[k] < 0 ) ? 0 : ( sx[k] >= width ) ? width - 1 : sx[k];
		}
		for ( oy = 0; oy < height; oy++ ) {
			const byte *row = in + ( ( y0 + oy ) * size + x0 ) * 4;
			float *dst = rows + ( oy * outWidth + ox ) * 3;
			for ( c = 0; c < 3; c++ ) {
				const float value = w[0] * row[sx[0] * 4 + c] + w[1] * row[sx[1] * 4 + c] + w[2] * row[sx[2] * 4 + c] + w[3] * row[sx[3] * 4 + c];
				dst[c] = LM_Between( value, row[sx[1] * 4 + c], row[sx[2] * 4 + c] );
			}
		}
	}

	for ( oy = 0; oy < outHeight; oy++ ) {
		const float	v = ( oy + 0.5f ) / scale - 0.5f;
		const int	base = (int)floorf( v );
		float		w[4];
		const float	*src[4];
		byte		*dst = out + ( ( y0 * scale + oy ) * outSize + x0 * scale ) * 4;

		LM_CubicWeights( v - base, w );
		for ( k = 0; k < 4; k++ ) {
			int sy = base - 1 + k;
			sy = ( sy < 0 ) ? 0 : ( sy >= height ) ? height - 1 : sy;
			src[k] = rows + sy * outWidth * 3;
		}
		for ( ox = 0; ox < outWidth; ox++, dst += 4 ) {
			for ( c = 0; c < 3; c++ ) {
				const float value = LM_Between( w[0] * src[0][ox * 3 + c] + w[1] * src[1][ox * 3 + c] + w[2] * src[2][ox * 3 + c] + w[3] * src[3][ox * 3 + c],
					src[1][ox * 3 + c], src[2][ox * 3 + c] );
				dst[c] = (byte)( ( value <= 0.0f ) ? 0 : ( value >= 255.0f ) ? 255 : (int)( value + 0.5f ) );
			}
			dst[3] = 255;
		}
	}

	free( rows );
}

/*
================
R_SmoothLightmap

in: a page (RGBA, size x size); rects: the rectangles of the surfaces on it; out: the page LIGHTMAP_SMOOTH_SCALE times
larger. The whole page first (for the texels no surface owns), then every rectangle again on its own.
================
*/
static void R_SmoothLightmap( const byte *in, int size, const lightmapRect_t *rects, int numRects, byte *out ) {
	int i;

	LM_SmoothRect( in, size, 0, 0, size, size, out );
	for ( i = 0; i < numRects; i++ ) {
		const int x0 = ( rects[i].x < 0 ) ? 0 : rects[i].x;
		const int y0 = ( rects[i].y < 0 ) ? 0 : rects[i].y;
		const int x1 = ( rects[i].x + rects[i].width > size ) ? size : rects[i].x + rects[i].width;
		const int y1 = ( rects[i].y + rects[i].height > size ) ? size : rects[i].y + rects[i].height;

		if ( x1 > x0 && y1 > y0 ) {
			LM_SmoothRect( in, size, x0, y0, x1, y1, out );
		}
	}
}

/*
================
R_LightmapRects

The rectangles of all the surfaces of a BSP on page lightmap (as many as fit in rects).
================
*/
static int R_LightmapRects( const byte *fileBase, const lump_t *surfs, int lightmap, lightmapRect_t *rects, int maxRects ) {
	const dsurface_t	*surf = (const dsurface_t *)( fileBase + surfs->fileofs );
	const int			count = surfs->filelen / (int)sizeof( dsurface_t );
	int					numRects = 0;
	int					i, s;

	for ( i = 0; i < count; i++, surf++ ) {
		for ( s = 0; s < MAXLIGHTMAPS; s++ ) {
			if ( LittleLong( surf->lightmapNum[s] ) != lightmap || numRects >= maxRects ) {
				continue;
			}
			rects[numRects].x = LittleLong( surf->lightmapX[s] );
			rects[numRects].y = LittleLong( surf->lightmapY[s] );
			rects[numRects].width = LittleLong( surf->lightmapWidth );
			rects[numRects].height = LittleLong( surf->lightmapHeight );
			numRects++;
		}
	}
	return numRects;
}

/*
================
R_CreateLightmapImage

The image of page lightmap of the BSP being loaded (image: RGBA, size x size), larger and smoother unless
r_smoothLightmaps is 0 (then as it is). Mipmapped when larger, since it is then minified more on screen.
================
*/
static image_t *R_CreateLightmapImage( const char *name, byte *image, int size, const byte *fileBase, const lump_t *surfs,
									   int lightmap, qboolean compress ) {
	if ( r_smoothLightmaps->integer && surfs && surfs->filelen > 0 ) {
		const int		maxRects = surfs->filelen / (int)sizeof( dsurface_t ) * MAXLIGHTMAPS;
		lightmapRect_t	*rects = (lightmapRect_t *)malloc( ( maxRects + 1 ) * sizeof( lightmapRect_t ) );
		byte			*smooth = (byte *)malloc( (size_t)size * size * LIGHTMAP_SMOOTH_SCALE * LIGHTMAP_SMOOTH_SCALE * 4 );

		if ( rects && smooth ) {
			image_t *result;

			R_SmoothLightmap( image, size, rects, R_LightmapRects( fileBase, surfs, lightmap, rects, maxRects ), smooth );
			result = R_CreateImage( name, smooth, size * LIGHTMAP_SMOOTH_SCALE, size * LIGHTMAP_SMOOTH_SCALE, GL_RGBA, qtrue, qfalse,
				compress, GL_CLAMP );
			free( rects );
			free( smooth );
			return result;
		}
		free( rects );
		free( smooth );
	}
	return R_CreateImage( name, image, size, size, GL_RGBA, qfalse, qfalse, compress, GL_CLAMP );
}
