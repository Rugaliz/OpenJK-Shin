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

// tr_eye_adaptation.h -- the 3D view brightens in dark places and dims a little in bright ones, over a second or two,
// as eyes do (r_eyeAdaptation)
//
// The games draw in the 0 to 1 range of the screen, there is no wider range to choose an exposure from as HDR
// rendering would. So the view is measured (the average of the logarithm of its brightness, on the graphics card: the
// view shrunk to EYE_LUM_SIZE square, then to one pixel), the measurement is read back a few frames later without
// waiting for it, the exposure follows it slowly here, and the composite pass of the screen effects applies it with a
// curve that keeps black black and white white: 1 - (1 - c) ^ exposure, which brightens the dark parts about as
// much as the exposure says and the bright ones less, so nothing clips.
//
// Included by tr_post.h (OpenGL) and vk_post.cpp (Vulkan), after tr_local.h.

#pragma once

#define EYE_LUM_SIZE		64			// the view is shrunk to this square first
#define EYE_LOG_MIN			( -10 )		// the range of log2 of the brightness the 8 bits of the measurement hold
#define EYE_LOG_RANGE		12
#define EYE_READ_FRAMES		3			// how many measurements are in flight before the oldest is read

// the brightness the view is brought towards (as the screen shows it, 0 to 1), and how far the exposure may go
#define EYE_TARGET			0.25f
#define EYE_MIN				0.8f
#define EYE_MAX				1.8f

static struct {
	float	exposure;		// as it is now
	int		lastTime;		// when it was last moved (ms), 0 to start again from 1
} eyeAdaptation;

/*
================
R_EyeAdaptation_Exposure

The exposure for this frame: 1 when r_eyeAdaptation is 0. measured is the byte of the last measurement read back
(the encoded average log brightness), or -1 when there is none this frame (it keeps moving towards the last one).
================
*/
static float R_EyeAdaptation_Exposure( int measured ) {
	static float target = 1.0f;
	const int now = ri.Milliseconds();

	if ( r_eyeAdaptation->value <= 0.0f ) {
		eyeAdaptation.lastTime = 0;
		return 1.0f;
	}
	if ( measured >= 0 ) {
		const float average = powf( 2.0f, EYE_LOG_MIN + EYE_LOG_RANGE * ( measured / 255.0f ) );
		const float strength = Com_Clamp( 0.0f, 2.0f, r_eyeAdaptation->value );
		target = Com_Clamp( EYE_MIN, EYE_MAX, powf( EYE_TARGET / Q_max( average, 0.001f ), 0.5f * strength ) );
	}
	if ( !eyeAdaptation.lastTime || now - eyeAdaptation.lastTime > 1000 ) {
		eyeAdaptation.exposure = 1.0f;	// (a new level, or back from the menu: from the original look)
	} else {
		// eyes get used to light faster than to the dark
		const float seconds = ( now - eyeAdaptation.lastTime ) * 0.001f;
		const float speed = ( target < eyeAdaptation.exposure ) ? 1.5f : 0.6f;
		eyeAdaptation.exposure += ( target - eyeAdaptation.exposure ) * ( 1.0f - expf( -seconds * speed ) );
	}
	eyeAdaptation.lastTime = now;
	return eyeAdaptation.exposure;
}
