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

// tr_light_shafts.h -- rays of sunlight over the 3D view (r_lightShafts), on levels whose sky gives a sun
//
// The sky near the sun, where it is seen, is blurred along lines towards the sun (the scattering of light as a post
// process of Mitchell, GPU Gems 3, chapter 13): where something stands in front of the sky the rays have gaps, so
// they come through windows, between pillars and around the edges of things. Done at a quarter of the size of the
// view in three passes: the sky near the sun, the blur, and the result added over the view. tr_post.h has the OpenGL
// passes, vk_post.cpp (post_shafts.frag) the Vulkan ones; this has what they share.
//
// Included by tr_post.h (OpenGL) and vk_post.cpp (Vulkan), after tr_local.h.

#pragma once

#define LIGHT_SHAFTS_SCALE		4		// the passes are this many times smaller than the view on each side

/*
================
R_LightShaftsSun

Where the sun is on the view: sun[0], sun[1] from 0 to 1 across it from its bottom left corner (outside that when the
sun is off the side), sun[2] how strong the rays are: r_lightShafts, less as the sun leaves the view, none when it is
behind or the level has no sun (then qfalse).
================
*/
static qboolean R_LightShaftsSun( float sun[3] ) {
	const float	*m = backEnd.viewParms.world.modelMatrix;
	const float	*p = backEnd.viewParms.projectionMatrix;
	const float	*d = tr.sunDirection;
	vec3_t		e;
	int			i;

	if ( r_lightShafts->value <= 0.0f || !tr.levelHasSun || !tr.world ) {
		return qfalse;
	}

	// the direction to the sun in the space of the eye, then projected (a point infinitely far away)
	for ( i = 0; i < 3; i++ ) {
		e[i] = m[i] * d[0] + m[4 + i] * d[1] + m[8 + i] * d[2];
	}
	const float w = p[3] * e[0] + p[7] * e[1] + p[11] * e[2];
	if ( w <= 0.05f ) {
		return qfalse;	// behind, or right beside
	}
	sun[0] = 0.5f + 0.5f * ( p[0] * e[0] + p[4] * e[1] + p[8] * e[2] ) / w;
	sun[1] = 0.5f + 0.5f * ( p[1] * e[0] + p[5] * e[1] + p[9] * e[2] ) / w;

	// full while the sun is on the view, gone half a view beyond its edge
	const float outside = Q_max( Q_max( -sun[0], sun[0] - 1.0f ), Q_max( -sun[1], sun[1] - 1.0f ) );
	sun[2] = Com_Clamp( 0.0f, 2.0f, r_lightShafts->value ) * Com_Clamp( 0.0f, 1.0f, 1.0f - outside / 0.5f );
	return (qboolean)( sun[2] > 0.01f );
}
