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

// snd_resample.h -- band-limited (windowed sinc) sample rate conversion shared by the sound code
//
// The game's sound data comes in at several rates (11, 22 and 44.1kHz files, 22kHz video soundtracks) while the
// output device runs at whatever its native rate is (usually 48kHz). Everything is converted with the same filter,
// so that the mixer can run at the device's own rate and the operating system never has to resample behind our back.
//
// This file does not depend on the rest of the engine.

#ifndef SND_RESAMPLE_H
#define SND_RESAMPLE_H

#define RESAMPLE_HALF_TAPS	16		// source samples used either side of a position (when not downsampling)

typedef struct
{
	float	cutoff;		// low pass cutoff as a fraction of the source's Nyquist frequency (1 = keep everything)
	float	support;	// how many source samples either side of a position take part
} resampleFilter_t;

// Builds the filter table. Called on demand by everything below, but can be called early.
void	S_Resample_Init( void );

// Filter for converting by step = (source rate / destination rate) source samples per output sample
void	S_Resample_SetupFilter( resampleFilter_t *pFilter, double dStep );

// The filter's weight for a source sample u (in source samples, scaled by pFilter->cutoff) away from the position
float	S_Resample_Kernel( float u );

// Value of nChan interleaved 16 bit channels at the fractional frame position dPos, written to fOut[0..nChan-1].
// Frames outside 0..nFrames-1 count as silence.
void	S_Resample_Interp( const short *pSrc, int nChan, int nFrames, double dPos, const resampleFilter_t *pFilter, float *fOut );


// Resampling of a continuous stream that arrives in pieces (cinematic soundtracks). The filter's position and the
// samples either side of a piece boundary are carried over so the pieces join up without clicks.
#define RESAMPLE_STREAM_HISTORY	160
#define RESAMPLE_STREAM_MAXIN	4096	// most frames S_ResampleStream_Process will take at once

typedef struct
{
	resampleFilter_t	filter;
	double		dStep;
	double		dPos;		// next output position, in frames from the start of the history
	int			nChan;		// 1 or 2
	int			nHist;
	short		hist[RESAMPLE_STREAM_HISTORY * 2];
} resampleStream_t;

void	S_ResampleStream_Reset( resampleStream_t *pStream, int nChan, double dStep );

// Feeds nIn (<= RESAMPLE_STREAM_MAXIN) frames in and returns the number of output frames (nChan floats each) written
// to fOut, at most nMaxOut. Stops early if fOut is full, so size it for about nIn / dStep + 2 frames.
int		S_ResampleStream_Process( resampleStream_t *pStream, const short *pIn, int nIn, float *fOut, int nMaxOut );

#endif	// SND_RESAMPLE_H
