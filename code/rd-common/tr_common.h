/*
===========================================================================
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2005 - 2015, ioquake3 contributors
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

#pragma once

#include "../rd-common/tr_public.h"
#include "../rd-common/tr_font.h"

extern refimport_t ri;

/*
================================================================================
Noise Generation
================================================================================
*/
// Initialize the noise generator.
void R_NoiseInit( void );

// Get random 4-component vector.
float R_NoiseGet4f( float x, float y, float z, float t );

// Get the noise time.
float GetNoiseTime( int t );

/*
================================================================================
 Image Loading
================================================================================
*/
// Initialize the image loader.
void R_ImageLoader_Init();

typedef void (*ImageLoaderFn)( const char *filename, byte **pic, int *width, int *height );

// Adds a new image loader to handle a new image type. The extension should not
// begin with a period (a full stop).
qboolean R_ImageLoader_Add( const char *extension, ImageLoaderFn imageLoader );

// Load an image from file.
void R_LoadImage( const char *shortname, byte **pic, int *width, int *height );

// Image prefetch (tr_image_prefetch.cpp): while a level loads, the images it needs are read ahead and decoded on
// worker threads. The list of the images comes from the last time the level was loaded.
void		R_ImagePrefetch_Begin( const char *mapName );
void		R_ImagePrefetch_End( void );
void		R_ImagePrefetch_Shutdown( void );
qboolean	R_ImagePrefetch_Take( const char *shortname, byte **pic, int *width, int *height );	// zone memory, like R_LoadImage
void		R_ImagePrefetch_Note( const char *fileName );	// this file was loaded
bool		R_DecodeTGAFromMemory( byte *buffer, size_t length, byte **pic, int *width, int *height );
bool		R_DecodeJPGFromMemory( const byte *buffer, size_t length, byte **pic, int *width, int *height );

// Load raw image data from TGA image.
void LoadTGA( const char *name, byte **pic, int *width, int *height );

// Load raw image data from JPEG image.
void LoadJPG( const char *filename, byte **pic, int *width, int *height );

// Load raw image data from PNG image.
void LoadPNG( const char *filename, byte **data, int *width, int *height );

#ifdef JK2_MODE
//Load raw image data from JPEG input.
void LoadJPGFromBuffer( byte *inputBuffer, size_t len, byte **pic, int *width, int *height );
#endif

/*
================================================================================
 Image saving
================================================================================
*/
// Convert raw image data to JPEG format and store in buffer.
size_t RE_SaveJPGToBuffer( byte *buffer, size_t bufSize, int quality, int image_width, int image_height, byte *image_buffer, int padding, bool flip_vertical );

// Save raw image data as JPEG image file.
void RE_SaveJPG( const char * filename, int quality, int image_width, int image_height, byte *image_buffer, int padding );

// Save raw image data as PNG image file.
int RE_SavePNG( const char *filename, byte *buf, size_t width, size_t height, int byteDepth );

void *R_Malloc( int iSize, memtag_t eTag, qboolean bZeroit=qfalse );
void R_Free( void *ptr );
int R_MemSize( memtag_t eTag );
void R_MorphMallocTag( void *pvBuffer, memtag_t eDesiredTag );
void *R_Hunk_Alloc( int iSize, qboolean bZeroit=qtrue );
