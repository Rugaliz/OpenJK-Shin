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

// snd_reverb.h -- reverberation of the sound of a room
//
// Sounds in a room are heard directly, then as a few early reflections off the nearest surfaces, then as a dense,
// slowly fading tail of reflections of reflections (reverb) whose length depends on the size and the materials of the
// room. This makes a sound heard in a hangar sound very different from one heard in a small corridor.
//
// The sounds that are to have reverb are mixed together into one signal (mono) which is fed through this; what comes out
// (stereo) is added to the mix. The room is described by a few numbers that can be changed at any time, they are
// followed smoothly.
//
// This file does not depend on the rest of the engine.

#ifndef SND_REVERB_H
#define SND_REVERB_H

typedef struct
{
	float	rt60;			// how long (seconds) the tail takes to fade by 60dB, at low and middle frequencies
	float	hfRatio;		// the same at high frequencies, as a fraction of rt60 (soft walls and air take the highs first)
	float	preDelay;		// seconds before the tail begins
	float	erDelay;		// seconds to the first of the early reflections
	float	erLevel;		// loudness of the early reflections (about 0..1)
	float	lateLevel;		// loudness of the tail (about 0..1)
} reverbParams_t;

// Prepares for an output rate, and silences it
void	S_Reverb_Init( int sampleRate );

// The room to change into, gradually
void	S_Reverb_SetTarget( const reverbParams_t *pParams );

// The room to be in at once (for the start of a level)
void	S_Reverb_SetNow( const reverbParams_t *pParams );

// Remembers everything that is in the reverb now...
void	S_Reverb_Mark( void );

// ...and goes back to that (returns 0 if there is nothing to go back to). The mixer paints ahead of what is played and
// paints the same moments again in its next update, with the sounds that started meanwhile: going back lets the reverb
// be run again over them.
int		S_Reverb_Rewind( void );

// Runs n samples of the mono signal pIn through the reverb. The stereo reverb is written to pOutLeft and pOutRight
// (not added), it does not include the signal itself.
void	S_Reverb_Process( const float *pIn, int n, float *pOutLeft, float *pOutRight );

#endif	// SND_REVERB_H
