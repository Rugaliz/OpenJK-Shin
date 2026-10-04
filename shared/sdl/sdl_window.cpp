/*
===========================================================================
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

#include <SDL3/SDL.h>
#include "qcommon/qcommon.h"
#include "rd-common/tr_types.h"
#include "sys/sys_local.h"
#include "sdl_icon.h"

enum rserr_t
{
	RSERR_OK,

	RSERR_INVALID_FULLSCREEN,
	RSERR_INVALID_MODE,

	RSERR_UNKNOWN
};

static SDL_Window *screen = NULL;
static SDL_GLContext opengl_context;
static float displayAspect;

cvar_t *r_sdlDriver;
cvar_t *r_allowSoftwareGL;

// Window cvars
cvar_t	*r_fullscreen = 0;
cvar_t	*r_noborder;
cvar_t	*r_centerWindow;
cvar_t	*r_customwidth;
cvar_t	*r_customheight;
cvar_t	*r_swapInterval;
cvar_t	*r_adaptiveVsync;
cvar_t	*r_stereo;
cvar_t	*r_mode;
cvar_t	*r_displayRefresh;

// Window surface cvars
cvar_t	*r_stencilbits;
cvar_t	*r_depthbits;
cvar_t	*r_colorbits;
cvar_t	*r_ignorehwgamma;
cvar_t  *r_ext_multisample;

/*
** R_GetModeInfo
*/
typedef struct vidmode_s
{
    const char *description;
    int         width, height;
} vidmode_t;

const vidmode_t r_vidModes[] = {
    { "Mode  0: 320x240",		320,	240 },
    { "Mode  1: 400x300",		400,	300 },
    { "Mode  2: 512x384",		512,	384 },
    { "Mode  3: 640x480",		640,	480 },
    { "Mode  4: 800x600",		800,	600 },
    { "Mode  5: 960x720",		960,	720 },
    { "Mode  6: 1024x768",		1024,	768 },
    { "Mode  7: 1152x864",		1152,	864 },
    { "Mode  8: 1280x1024",		1280,	1024 },
    { "Mode  9: 1600x1200",		1600,	1200 },
    { "Mode 10: 2048x1536",		2048,	1536 },
    { "Mode 11: 856x480 (wide)", 856,	 480 },
    { "Mode 12: 2400x600(surround)",2400,600 }
};
static const int	s_numVidModes = ARRAY_LEN( r_vidModes );

#define R_MODE_FALLBACK (4) // 640x480

qboolean R_GetModeInfo( int *width, int *height, int mode ) {
	const vidmode_t	*vm;

    if ( mode < -1 ) {
        return qfalse;
	}
	if ( mode >= s_numVidModes ) {
		return qfalse;
	}

	if ( mode == -1 ) {
		*width = r_customwidth->integer;
		*height = r_customheight->integer;
		return qtrue;
	}

	vm = &r_vidModes[mode];

    *width  = vm->width;
    *height = vm->height;

    return qtrue;
}

/*
===============
GLimp_UseDesktopFullscreen

When the requested size is the desktop size, use a borderless fullscreen window at the desktop resolution instead of
switching the display mode. It's instant, doesn't disturb other windows/monitors and behaves well with Wayland and
alt-tabbing. Other sizes use a real display mode switch.
===============
*/
static bool GLimp_UseDesktopFullscreen( int width, int height, SDL_DisplayID display )
{
#ifdef MACOS_X
	return true;
#else
	const SDL_DisplayMode *desktopMode = display ? SDL_GetDesktopDisplayMode( display ) : NULL;
	return desktopMode && desktopMode->w == width && desktopMode->h == height;
#endif
}

/*
===============
GLimp_SetFullscreenMode

Chooses what fullscreen means for the window: the desktop (borderless) or a display mode of the size of the window.
Returns false if there is no such display mode.
===============
*/
static bool GLimp_SetFullscreenMode( int width, int height )
{
	const SDL_DisplayID display = SDL_GetDisplayForWindow( screen );

	if ( GLimp_UseDesktopFullscreen( width, height, display ) )
	{
		return SDL_SetWindowFullscreenMode( screen, NULL );
	}

	SDL_DisplayMode mode;
	if ( !SDL_GetClosestFullscreenDisplayMode( display, width, height, (float)r_displayRefresh->integer, false, &mode ) )
	{
		Com_DPrintf( "SDL_GetClosestFullscreenDisplayMode failed: %s\n", SDL_GetError() );
		return false;
	}
	return SDL_SetWindowFullscreenMode( screen, &mode );
}

/*
** R_ModeList_f
*/
static void R_ModeList_f( void )
{
	int i;

	Com_Printf( "\n" );
	Com_Printf( "Mode -2: Use desktop resolution\n" );
	Com_Printf( "Mode -1: Use r_customWidth and r_customHeight variables\n" );
	for ( i = 0; i < s_numVidModes; i++ )
	{
		Com_Printf( "%s\n", r_vidModes[i].description );
	}
	Com_Printf( "\n" );
}

/*
===============
GLimp_Minimize

Minimize the game so that user is back at the desktop
===============
*/
void GLimp_Minimize(void)
{
	SDL_MinimizeWindow( screen );
}

/*
===============
GLimp_ApplySwapInterval

With r_adaptiveVsync, vsync on (1) is upgraded to adaptive vsync (-1) where the
driver offers it: synced to the display like vsync, but a late frame tears
instead of waiting for the next refresh (which would halve the frame rate).
===============
*/
static void GLimp_ApplySwapInterval( void )
{
	if ( r_swapInterval->integer == 1 && r_adaptiveVsync->integer && SDL_GL_SetSwapInterval( -1 ) )
	{
		Com_Printf( "Vertical sync: adaptive\n" );
		return;
	}

	if ( !SDL_GL_SetSwapInterval( r_swapInterval->integer ) )
	{
		Com_DPrintf( "SDL_GL_SetSwapInterval failed: %s\n", SDL_GetError() );
	}
}

void WIN_Present( window_t *window )
{
	if ( window->api == GRAPHICS_API_OPENGL )
	{
		SDL_GL_SwapWindow(screen);

		if ( r_swapInterval->modified )
		{
			r_swapInterval->modified = qfalse;
			GLimp_ApplySwapInterval();
		}
	}

	if ( r_fullscreen->modified )
	{
		bool	fullscreen;
		bool	needToToggle;
		bool	sdlToggled = qfalse;

		// Find out the current state
		fullscreen = (SDL_GetWindowFlags( screen ) & SDL_WINDOW_FULLSCREEN) != 0;

		if ( r_fullscreen->integer && Cvar_VariableIntegerValue( "in_nograb" ) )
		{
			Com_Printf( "Fullscreen not allowed with in_nograb 1\n" );
			Cvar_Set( "r_fullscreen", "0" );
			r_fullscreen->modified = qfalse;
		}

		// Is the state we want different from the current state?
		needToToggle = !!r_fullscreen->integer != fullscreen;

		if ( needToToggle )
		{
			if ( r_fullscreen->integer )
			{
				int width, height;
				SDL_GetWindowSize( screen, &width, &height );
				sdlToggled = GLimp_SetFullscreenMode( width, height ) && SDL_SetWindowFullscreen( screen, true );
			}
			else
			{
				sdlToggled = SDL_SetWindowFullscreen( screen, false );
			}
			if ( sdlToggled )
				SDL_SyncWindow( screen );

			// SDL couldn't do it, so do it the slow way
			if ( !sdlToggled )
				Cbuf_AddText( "vid_restart\n" );

			IN_Restart();
		}

		r_fullscreen->modified = qfalse;
	}
}

/*
===============
GLimp_CompareModes
===============
*/
static int GLimp_CompareModes( const void *a, const void *b )
{
	const float ASPECT_EPSILON = 0.001f;
	SDL_Rect *modeA = (SDL_Rect *)a;
	SDL_Rect *modeB = (SDL_Rect *)b;
	float aspectA = (float)modeA->w / (float)modeA->h;
	float aspectB = (float)modeB->w / (float)modeB->h;
	int areaA = modeA->w * modeA->h;
	int areaB = modeB->w * modeB->h;
	float aspectDiffA = fabs( aspectA - displayAspect );
	float aspectDiffB = fabs( aspectB - displayAspect );
	float aspectDiffsDiff = aspectDiffA - aspectDiffB;

	if( aspectDiffsDiff > ASPECT_EPSILON )
		return 1;
	else if( aspectDiffsDiff < -ASPECT_EPSILON )
		return -1;
	else
		return areaA - areaB;
}

/*
===============
GLimp_DetectAvailableModes
===============
*/
static bool GLimp_DetectAvailableModes(void)
{
	int i, j;
	char buf[ MAX_STRING_CHARS ] = { 0 };
	SDL_Rect *modes;
	int numModes = 0;

	const SDL_DisplayID display = SDL_GetDisplayForWindow( screen );
	if ( display == 0 )
	{
		Com_Printf( S_COLOR_YELLOW "WARNING: Couldn't get window display, no resolutions detected: %s\n", SDL_GetError() );
		return false;
	}

	int numDisplayModes = 0;
	SDL_DisplayMode **displayModes = SDL_GetFullscreenDisplayModes( display, &numDisplayModes );
	if ( !displayModes )
	{
		Com_Printf( S_COLOR_YELLOW "WARNING: Couldn't get the display modes, no resolutions detected (%s).\n", SDL_GetError() );
		return false;
	}

	modes = (SDL_Rect *)SDL_calloc( (size_t)numDisplayModes + 1, sizeof( SDL_Rect ) );
	if ( !modes )
		Com_Error( ERR_FATAL, "Out of memory" );

	for( i = 0; i < numDisplayModes; i++ )
	{
		const SDL_DisplayMode *mode = displayModes[i];

		if( !mode->w || !mode->h )
		{
			Com_Printf( "Display supports any resolution\n" );
			SDL_free( modes );
			SDL_free( displayModes );
			return true;
		}

		// SDL can give the same resolution with different refresh rates.
		// Only list resolution once.
		for( j = 0; j < numModes; j++ )
		{
			if( mode->w == modes[ j ].w && mode->h == modes[ j ].h )
				break;
		}

		if( j != numModes )
			continue;

		modes[ numModes ].w = mode->w;
		modes[ numModes ].h = mode->h;
		numModes++;
	}

	SDL_free( displayModes );

	if( numModes > 1 )
		qsort( modes, numModes, sizeof( SDL_Rect ), GLimp_CompareModes );

	for( i = 0; i < numModes; i++ )
	{
		const char *newModeString = va( "%ux%u ", modes[ i ].w, modes[ i ].h );

		if( strlen( newModeString ) < (int)sizeof( buf ) - strlen( buf ) )
			Q_strcat( buf, sizeof( buf ), newModeString );
		else
			Com_Printf( "Skipping mode %ux%u, buffer too small\n", modes[ i ].w, modes[ i ].h );
	}

	if( *buf )
	{
		buf[ strlen( buf ) - 1 ] = 0;
		Com_Printf( "Available modes: '%s'\n", buf );
		Cvar_Set( "r_availableModes", buf );
	}

	SDL_free( modes );
	return true;
}

/*
===============
GLimp_CreateWindow
===============
*/
static SDL_Window *GLimp_CreateWindow( const char *title, int x, int y, int width, int height, bool opengl, bool borderless )
{
	const SDL_PropertiesID props = SDL_CreateProperties();

	SDL_SetStringProperty( props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title );
	SDL_SetNumberProperty( props, SDL_PROP_WINDOW_CREATE_X_NUMBER, x );
	SDL_SetNumberProperty( props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y );
	SDL_SetNumberProperty( props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width );
	SDL_SetNumberProperty( props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height );
	SDL_SetBooleanProperty( props, SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, opengl );
	SDL_SetBooleanProperty( props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, borderless );

	SDL_Window *window = SDL_CreateWindowWithProperties( props );
	SDL_DestroyProperties( props );
	return window;
}

/*
===============
GLimp_SetMode
===============
*/
static rserr_t GLimp_SetMode(glconfig_t *glConfig, const windowDesc_t *windowDesc, const char *windowTitle, int mode, qboolean fullscreen, qboolean noborder)
{
	int perChannelColorBits;
	int colorBits, depthBits, stencilBits;
	int samples;
	int i = 0;
	SDL_Surface *icon = NULL;
	SDL_DisplayID display = 0;
	int desktopW = 0, desktopH = 0;
	bool borderless = false;
	int x = SDL_WINDOWPOS_UNDEFINED, y = SDL_WINDOWPOS_UNDEFINED;

	Com_Printf( "Initializing display\n");

	// (the icon's pixels are bytes in the order red, green, blue, alpha whatever the byte order of the processor)
	icon = SDL_CreateSurfaceFrom(
		CLIENT_WINDOW_ICON.width,
		CLIENT_WINDOW_ICON.height,
		SDL_PIXELFORMAT_RGBA32,
		(void *)CLIENT_WINDOW_ICON.pixel_data,
		CLIENT_WINDOW_ICON.bytes_per_pixel * CLIENT_WINDOW_ICON.width );

	// If a window exists, note its display
	if ( screen != NULL )
	{
		display = SDL_GetDisplayForWindow( screen );
		if ( display == 0 )
		{
			Com_DPrintf( "SDL_GetDisplayForWindow() failed: %s\n", SDL_GetError() );
		}
	}
	if ( display == 0 )
	{
		display = SDL_GetPrimaryDisplay();
	}

	const SDL_DisplayMode *desktopMode = display ? SDL_GetDesktopDisplayMode( display ) : NULL;
	if( desktopMode )
	{
		desktopW = desktopMode->w;
		desktopH = desktopMode->h;
		displayAspect = (float)desktopW / (float)desktopH;

		Com_Printf( "Display aspect: %.3f\n", displayAspect );
	}
	else
	{
		Com_Printf( "Cannot determine display aspect, assuming 1.333\n" );
	}

	Com_Printf( "...setting mode %d:", mode );

	if (mode == -2)
	{
		// use desktop video resolution
		if( desktopH > 0 )
		{
			glConfig->vidWidth = desktopW;
			glConfig->vidHeight = desktopH;
		}
		else
		{
			glConfig->vidWidth = 640;
			glConfig->vidHeight = 480;
			Com_Printf( "Cannot determine display resolution, assuming 640x480\n" );
		}

		//glConfig.windowAspect = (float)glConfig.vidWidth / (float)glConfig.vidHeight;
	}
	else if ( !R_GetModeInfo( &glConfig->vidWidth, &glConfig->vidHeight, /*&glConfig.windowAspect,*/ mode ) )
	{
		Com_Printf( " invalid mode\n" );
		SDL_DestroySurface( icon );
		return RSERR_INVALID_MODE;
	}
	Com_Printf( " %d %d\n", glConfig->vidWidth, glConfig->vidHeight);

	// Center window
	if( r_centerWindow->integer && !fullscreen )
	{
		x = ( desktopW / 2 ) - ( glConfig->vidWidth / 2 );
		y = ( desktopH / 2 ) - ( glConfig->vidHeight / 2 );
	}

	// Destroy existing state if it exists
	if( opengl_context != NULL )
	{
		SDL_GL_DestroyContext( opengl_context );
		opengl_context = NULL;
	}

	if( screen != NULL )
	{
		SDL_GetWindowPosition( screen, &x, &y );
		Com_DPrintf( "Existing window at %dx%d before being destroyed\n", x, y );
		SDL_DestroyWindow( screen );
		screen = NULL;
	}

	if( fullscreen )
	{
		glConfig->isFullscreen = qtrue;
	}
	else
	{
		borderless = !!noborder;
		glConfig->isFullscreen = qfalse;
	}

	colorBits = r_colorbits->integer;
	if ((!colorBits) || (colorBits >= 32))
		colorBits = 24;

	if (!r_depthbits->integer)
		depthBits = 24;
	else
		depthBits = r_depthbits->integer;

	stencilBits = r_stencilbits->integer;
	samples = r_ext_multisample->integer;

	if ( windowDesc->api == GRAPHICS_API_OPENGL )
	{
		const int wantedColorBits = colorBits, wantedDepthBits = depthBits, wantedStencilBits = stencilBits;

retryWithFewerSamples:
		for (i = 0; i < 16; i++)
		{
			int testColorBits, testDepthBits, testStencilBits;

			// 0 - default
			// 1 - minus colorBits
			// 2 - minus depthBits
			// 3 - minus stencil
			if ((i % 4) == 0 && i)
			{
				// one pass, reduce
				switch (i / 4)
				{
					case 2 :
						if (colorBits == 24)
							colorBits = 16;
						break;
					case 1 :
						if (depthBits == 24)
							depthBits = 16;
						else if (depthBits == 16)
							depthBits = 8;
					case 3 :
						if (stencilBits == 24)
							stencilBits = 16;
						else if (stencilBits == 16)
							stencilBits = 8;
				}
			}

			testColorBits = colorBits;
			testDepthBits = depthBits;
			testStencilBits = stencilBits;

			if ((i % 4) == 3)
			{ // reduce colorBits
				if (testColorBits == 24)
					testColorBits = 16;
			}

			if ((i % 4) == 2)
			{ // reduce depthBits
				if (testDepthBits == 24)
					testDepthBits = 16;
				else if (testDepthBits == 16)
					testDepthBits = 8;
			}

			if ((i % 4) == 1)
			{ // reduce stencilBits
				if (testStencilBits == 24)
					testStencilBits = 16;
				else if (testStencilBits == 16)
					testStencilBits = 8;
				else
					testStencilBits = 0;
			}

			if (testColorBits == 24)
				perChannelColorBits = 8;
			else
				perChannelColorBits = 4;

			SDL_GL_SetAttribute( SDL_GL_RED_SIZE, perChannelColorBits );
			SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE, perChannelColorBits );
			SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE, perChannelColorBits );
			SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, testDepthBits );
			SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, testStencilBits );

			SDL_GL_SetAttribute( SDL_GL_MULTISAMPLEBUFFERS, samples ? 1 : 0 );
			SDL_GL_SetAttribute( SDL_GL_MULTISAMPLESAMPLES, samples );

			if ( windowDesc->gl.majorVersion )
			{
				int compactVersion = windowDesc->gl.majorVersion * 100 + windowDesc->gl.minorVersion * 10;

				SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, windowDesc->gl.majorVersion );
				SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, windowDesc->gl.minorVersion );

				if ( windowDesc->gl.profile == GLPROFILE_ES || compactVersion >= 320 )
				{
					int profile;
					switch ( windowDesc->gl.profile )
					{
					default:
					case GLPROFILE_COMPATIBILITY:
						profile = SDL_GL_CONTEXT_PROFILE_COMPATIBILITY;
						break;

					case GLPROFILE_CORE:
						profile = SDL_GL_CONTEXT_PROFILE_CORE;
						break;

					case GLPROFILE_ES:
						profile = SDL_GL_CONTEXT_PROFILE_ES;
						break;
					}

					SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, profile );
				}
			}

			if ( windowDesc->gl.contextFlags & GLCONTEXT_DEBUG )
			{
				SDL_GL_SetAttribute( SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG );
			}

			if(r_stereo->integer)
			{
				glConfig->stereoEnabled = qtrue;
				SDL_GL_SetAttribute(SDL_GL_STEREO, 1);
			}
			else
			{
				glConfig->stereoEnabled = qfalse;
				SDL_GL_SetAttribute(SDL_GL_STEREO, 0);
			}

			SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
			SDL_GL_SetAttribute( SDL_GL_ACCELERATED_VISUAL, !r_allowSoftwareGL->integer );

			if( ( screen = GLimp_CreateWindow( windowTitle, x, y,
					glConfig->vidWidth, glConfig->vidHeight, true, borderless ) ) == NULL )
			{
				Com_DPrintf( "SDL_CreateWindow failed: %s\n", SDL_GetError( ) );
				continue;
			}

#ifndef MACOS_X
			SDL_SetWindowIcon( screen, icon );
#endif

			if( fullscreen )
			{
				glConfig->displayFrequency = r_displayRefresh->integer;

				if( !GLimp_SetFullscreenMode( glConfig->vidWidth, glConfig->vidHeight ) ||
					!SDL_SetWindowFullscreen( screen, true ) )
				{
					Com_DPrintf( "Going fullscreen failed: %s\n", SDL_GetError( ) );
					continue;
				}
				SDL_SyncWindow( screen );
			}

			if( ( opengl_context = SDL_GL_CreateContext( screen ) ) == NULL )
			{
				Com_Printf( "SDL_GL_CreateContext failed: %s\n", SDL_GetError( ) );
				continue;
			}

			GLimp_ApplySwapInterval();

			glConfig->colorBits = testColorBits;
			glConfig->depthBits = testDepthBits;
			glConfig->stencilBits = testStencilBits;

			Com_Printf( "Using %d color bits, %d depth, %d stencil display.\n",
					glConfig->colorBits, glConfig->depthBits, glConfig->stencilBits );
			break;
		}

		if (opengl_context == NULL && samples > 0) {
			// the display can't do that much multisampling, try with less before giving up
			samples = samples > 2 ? samples / 2 : 0;
			Com_Printf( "Multisampling not available, trying %d samples.\n", samples );
			colorBits = wantedColorBits;
			depthBits = wantedDepthBits;
			stencilBits = wantedStencilBits;
			goto retryWithFewerSamples;
		}

		if (opengl_context == NULL) {
			SDL_DestroySurface(icon);
			return RSERR_UNKNOWN;
		}

		if ( samples != r_ext_multisample->integer ) {
			Cvar_Set( "r_ext_multisample", va( "%d", samples ) );	// (so that the menu shows what is in use)
		}
	}
	else
	{
		// Just create a regular window
		if( ( screen = GLimp_CreateWindow( windowTitle, x, y,
				glConfig->vidWidth, glConfig->vidHeight, false, borderless ) ) == NULL )
		{
			Com_DPrintf( "SDL_CreateWindow failed: %s\n", SDL_GetError( ) );
		}
		else
		{
#ifndef MACOS_X
			SDL_SetWindowIcon( screen, icon );
#endif
			if( fullscreen )
			{
				if( !GLimp_SetFullscreenMode( glConfig->vidWidth, glConfig->vidHeight ) ||
					!SDL_SetWindowFullscreen( screen, true ) )
				{
					Com_DPrintf( "Going fullscreen failed: %s\n", SDL_GetError( ) );
				}
				else
					SDL_SyncWindow( screen );
			}
		}
	}

	SDL_DestroySurface( icon );

	if (!GLimp_DetectAvailableModes())
	{
		return RSERR_UNKNOWN;
	}

	return RSERR_OK;
}

/*
===============
GLimp_StartDriverAndSetMode
===============
*/
static qboolean GLimp_StartDriverAndSetMode(glconfig_t *glConfig, const windowDesc_t *windowDesc, int mode, qboolean fullscreen, qboolean noborder)
{
	rserr_t err;

	if (!SDL_WasInit(SDL_INIT_VIDEO))
	{
		const char *driverName;

		if (!SDL_Init(SDL_INIT_VIDEO))
		{
			Com_Printf( "SDL_Init( SDL_INIT_VIDEO ) FAILED (%s)\n", SDL_GetError());
			return qfalse;
		}

		driverName = SDL_GetCurrentVideoDriver();

		if (!driverName)
		{
			Com_Error( ERR_FATAL, "No video driver initialized" );
			return qfalse;
		}

		Com_Printf( "SDL using driver \"%s\"\n", driverName );
		Cvar_Set( "r_sdlDriver", driverName );
	}

	{
		int numDisplays = 0;
		SDL_DisplayID *displays = SDL_GetDisplays( &numDisplays );
		SDL_free( displays );
		if (numDisplays <= 0)
		{
			Com_Error( ERR_FATAL, "SDL_GetDisplays() FAILED (%s)", SDL_GetError() );
		}
	}

	if (fullscreen && Cvar_VariableIntegerValue( "in_nograb" ) )
	{
		Com_Printf( "Fullscreen not allowed with in_nograb 1\n");
		Cvar_Set( "r_fullscreen", "0" );
		r_fullscreen->modified = qfalse;
		fullscreen = qfalse;
	}

	err = GLimp_SetMode(glConfig, windowDesc, CLIENT_WINDOW_TITLE, mode, fullscreen, noborder);

	switch ( err )
	{
		case RSERR_INVALID_FULLSCREEN:
			Com_Printf( "...WARNING: fullscreen unavailable in this mode\n" );
			return qfalse;
		case RSERR_INVALID_MODE:
			Com_Printf( "...WARNING: could not set the given mode (%d)\n", mode );
			return qfalse;
		case RSERR_UNKNOWN:
			Com_Printf( "...ERROR: no display modes could be found.\n" );
			return qfalse;
		default:
			break;
	}

	return qtrue;
}

window_t WIN_Init( const windowDesc_t *windowDesc, glconfig_t *glConfig )
{
	Cmd_AddCommand("modelist", R_ModeList_f);
	Cmd_AddCommand("minimize", GLimp_Minimize);

	r_sdlDriver			= Cvar_Get( "r_sdlDriver",			"",			CVAR_ROM );
	r_allowSoftwareGL	= Cvar_Get( "r_allowSoftwareGL",	"0",		CVAR_ARCHIVE_ND|CVAR_LATCH );

	// Window cvars
#ifdef _JK2EXE
	// Singleplayer: out of the box, run fullscreen at the desktop resolution so no setup is needed on modern displays
	r_fullscreen		= Cvar_Get( "r_fullscreen",			"1",		CVAR_ARCHIVE|CVAR_LATCH );
#else
	r_fullscreen		= Cvar_Get( "r_fullscreen",			"0",		CVAR_ARCHIVE|CVAR_LATCH );
#endif
	r_noborder			= Cvar_Get( "r_noborder",			"0",		CVAR_ARCHIVE|CVAR_LATCH );
	r_centerWindow		= Cvar_Get( "r_centerWindow",		"0",		CVAR_ARCHIVE|CVAR_LATCH );
	r_customwidth		= Cvar_Get( "r_customwidth",		"1600",		CVAR_ARCHIVE|CVAR_LATCH );
	r_customheight		= Cvar_Get( "r_customheight",		"1024",		CVAR_ARCHIVE|CVAR_LATCH );
#ifdef _JK2EXE
	// Singleplayer: vsync on, otherwise high refresh rate displays tear
	r_swapInterval		= Cvar_Get( "r_swapInterval",		"1",		CVAR_ARCHIVE_ND );
#else
	r_swapInterval		= Cvar_Get( "r_swapInterval",		"0",		CVAR_ARCHIVE_ND );
#endif
#ifdef _JK2EXE
	r_adaptiveVsync		= Cvar_Get( "r_adaptiveVsync",		"1",		CVAR_ARCHIVE_ND );
#else
	r_adaptiveVsync		= Cvar_Get( "r_adaptiveVsync",		"0",		CVAR_ARCHIVE_ND );
#endif
	r_stereo			= Cvar_Get( "r_stereo",				"0",		CVAR_ARCHIVE_ND|CVAR_LATCH );
#ifdef _JK2EXE
	r_mode				= Cvar_Get( "r_mode",				"-2",		CVAR_ARCHIVE|CVAR_LATCH );
#else
	r_mode				= Cvar_Get( "r_mode",				"4",		CVAR_ARCHIVE|CVAR_LATCH );
#endif
	r_displayRefresh	= Cvar_Get( "r_displayRefresh",		"0",		CVAR_LATCH );
	Cvar_CheckRange( r_displayRefresh, 0, 240, qtrue );

	// Window render surface cvars
	r_stencilbits		= Cvar_Get( "r_stencilbits",		"8",		CVAR_ARCHIVE_ND|CVAR_LATCH );
	r_depthbits			= Cvar_Get( "r_depthbits",			"0",		CVAR_ARCHIVE_ND|CVAR_LATCH );
	r_colorbits			= Cvar_Get( "r_colorbits",			"0",		CVAR_ARCHIVE_ND|CVAR_LATCH );
	r_ignorehwgamma		= Cvar_Get( "r_ignorehwgamma",		"0",		CVAR_ARCHIVE_ND|CVAR_LATCH );
	r_ext_multisample	= Cvar_Get( "r_ext_multisample",	"0",		CVAR_ARCHIVE_ND|CVAR_LATCH );
	Cvar_Get( "r_availableModes", "", CVAR_ROM );

	// Create the window and set up the context
	if(!GLimp_StartDriverAndSetMode( glConfig, windowDesc, r_mode->integer,
										(qboolean)r_fullscreen->integer, (qboolean)r_noborder->integer ))
	{
		if( r_mode->integer != R_MODE_FALLBACK )
		{
			Com_Printf( "Setting r_mode %d failed, falling back on r_mode %d\n", r_mode->integer, R_MODE_FALLBACK );

			if (!GLimp_StartDriverAndSetMode( glConfig, windowDesc, R_MODE_FALLBACK, qfalse, qfalse ))
			{
				// Nothing worked, give up
				Com_Error( ERR_FATAL, "GLimp_Init() - could not load OpenGL subsystem" );
			}
		}
	}

	// SDL3 has no hardware gamma ramps, so the renderer applies gamma itself (to the textures)
	glConfig->deviceSupportsGamma = qfalse;

	// This depends on SDL_INIT_VIDEO, hence having it here
	IN_Init( screen );

	// window_t is only really useful for Windows if the renderer wants to create a D3D context.
	window_t window = {};

	window.api = windowDesc->api;

#if defined(_WIN32)
	window.handle = SDL_GetPointerProperty( SDL_GetWindowProperties( screen ), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL );
#endif

	return window;
}

/*
===============
GLimp_Shutdown
===============
*/
void WIN_Shutdown( void )
{
	Cmd_RemoveCommand("modelist");
	Cmd_RemoveCommand("minimize");

	IN_Shutdown();

	if ( opengl_context ) {
		SDL_GL_DestroyContext( opengl_context );
		opengl_context = NULL;
	}

	if ( screen ) {
		SDL_DestroyWindow( screen );
		screen = NULL;
	}

	SDL_QuitSubSystem( SDL_INIT_VIDEO );
}

void GLimp_EnableLogging( qboolean enable )
{
}

void GLimp_LogComment( char *comment )
{
}

void WIN_SetGamma( glconfig_t *glConfig, byte red[256], byte green[256], byte blue[256] )
{
	// SDL3 removed hardware gamma ramps (glConfig->deviceSupportsGamma is never set), the renderer does it in software
}

void *WIN_GL_GetProcAddress( const char *proc )
{
	return (void *)SDL_GL_GetProcAddress( proc );
}

qboolean WIN_GL_ExtensionSupported( const char *extension )
{
	return SDL_GL_ExtensionSupported( extension ) ? qtrue : qfalse;
}
