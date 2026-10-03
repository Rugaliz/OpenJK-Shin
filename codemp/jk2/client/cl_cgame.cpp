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

// cl_cgame.cpp -- client system interaction with the client game module, for Jedi Outcast (JK2_MODE)
//
// The cgame module of Jedi Outcast is a QVM file of the game (vm/cgame.qvm), which talks to the engine only by
// numbered system calls and which cannot hold a pointer. This file is used in place of cl_cgame.cpp and
// cl_cgameapi.cpp of Jedi Academy. Derived from the file of the same name of JK2MV (GPLv2), which is derived from the
// Jedi Outcast source code released by Raven Software.
//
// The renderer of this engine is the one of Jedi Academy, so what is passed to it is converted here from the layout of
// the module (refEntity_t, refdef_t, glconfig_t); Ghoul2 instances are handles in the module (see g2_vmhandles.h).

#include "qcommon/cm_public.h"
#include "client/client.h"
#include "client/cl_cgameapi.h"
#include "client/cl_uiapi.h"
#include "botlib/botlib.h"
#include "client/FXExport.h"
#include "client/FxUtil.h"
#include "qcommon/RoffSystem.h"
#include "qcommon/strip.h"
#include "qcommon/g2_vmhandles.h"
#include "qcommon/vm_local.h"
#include "cl_jk2vm.h"

extern IHeapAllocator *G2VertSpaceClient;
extern botlib_export_t *botlib_export;

void FX_FeedTrail( effectTrailArgStruct_t *a );	// FxPrimitives.cpp


vm_t *cgvm;

// the shared memory the cgame module gives us (CG_SET_SHARED_BUFFER), as we see it (cl.mSharedMemory) and as the module sees it
static intptr_t cgSharedMemoryVM;

/*
====================
CL_GetVMGLConfig

The glconfig in the layout the modules have (and a 32 bit module can read)
====================
*/
void CL_GetVMGLConfig( vmglconfig_t *vmconfig ) {
	const glconfig_t *config = &cls.glconfig;

	Com_Memset( vmconfig, 0, sizeof( *vmconfig ) );

	if ( config->renderer_string )		Q_strncpyz( vmconfig->renderer_string, config->renderer_string, sizeof( vmconfig->renderer_string ) );
	if ( config->vendor_string )		Q_strncpyz( vmconfig->vendor_string, config->vendor_string, sizeof( vmconfig->vendor_string ) );
	if ( config->version_string )		Q_strncpyz( vmconfig->version_string, config->version_string, sizeof( vmconfig->version_string ) );
	if ( config->extensions_string )	Q_strncpyz( vmconfig->extensions_string, config->extensions_string, sizeof( vmconfig->extensions_string ) );

	vmconfig->maxTextureSize = config->maxTextureSize;
	vmconfig->maxActiveTextures = config->maxActiveTextures;
	vmconfig->colorBits = config->colorBits;
	vmconfig->depthBits = config->depthBits;
	vmconfig->stencilBits = config->stencilBits;
	vmconfig->deviceSupportsGamma = config->deviceSupportsGamma;
	vmconfig->textureCompression = config->textureCompression == TC_NONE ? TC_NONE : TC_S3TC;
	vmconfig->textureEnvAddAvailable = config->textureEnvAddAvailable;
	vmconfig->textureFilterAnisotropicAvailable = (qboolean)( config->maxTextureFilterAnisotropy > 1.0f );
	vmconfig->clampToEdgeAvailable = config->clampToEdgeAvailable;
	vmconfig->vidWidth = config->vidWidth;
	vmconfig->vidHeight = config->vidHeight;
	// a screen of any shape has square pixels, and the module (like the 640x480 virtual screen) is better off without
	// a distorted view
	vmconfig->windowAspect = config->vidHeight ? (float)config->vidWidth / (float)config->vidHeight : 1.0f;
	vmconfig->displayFrequency = config->displayFrequency;
	vmconfig->isFullscreen = config->isFullscreen;
	vmconfig->stereoEnabled = config->stereoEnabled;
	vmconfig->smpActive = qfalse;
}

/*
====================
CL_GetGameState
====================
*/
static void CL_GetGameState( gameState_t *gs ) {
	*gs = cl.gameState;
}

/*
====================
CL_GetUserCmd
====================
*/
qboolean CL_GetUserCmd( int cmdNumber, usercmd_t *ucmd ) {
	// cmds[cmdNumber] is the last properly generated command

	// can't return anything that we haven't created yet
	if ( cmdNumber > cl.cmdNumber ) {
		Com_Error( ERR_DROP, "CL_GetUserCmd: %i >= %i", cmdNumber, cl.cmdNumber );
	}

	// the usercmd has been overwritten in the wrapping
	// buffer because it is too far out of date
	if ( cmdNumber <= cl.cmdNumber - CMD_BACKUP ) {
		return qfalse;
	}

	*ucmd = cl.cmds[ cmdNumber & CMD_MASK ];

	return qtrue;
}

static int CL_GetCurrentCmdNumber( void ) {
	return cl.cmdNumber;
}

/*
====================
CL_GetParseEntityState
====================
*/
qboolean	CL_GetParseEntityState( int parseEntityNumber, entityState_t *state ) {
	// can't return anything that hasn't been parsed yet
	if ( parseEntityNumber >= cl.parseEntitiesNum ) {
		Com_Error( ERR_DROP, "CL_GetParseEntityState: %i >= %i",
			parseEntityNumber, cl.parseEntitiesNum );
	}

	// can't return anything that has been overwritten in the circular buffer
	if ( parseEntityNumber <= cl.parseEntitiesNum - MAX_PARSE_ENTITIES ) {
		return qfalse;
	}

	*state = cl.parseEntities[ parseEntityNumber & ( MAX_PARSE_ENTITIES - 1 ) ];
	return qtrue;
}

/*
====================
CL_GetCurrentSnapshotNumber
====================
*/
static void CL_GetCurrentSnapshotNumber( int *snapshotNumber, int *serverTime ) {
	*snapshotNumber = cl.snap.messageNum;
	*serverTime = cl.snap.serverTime;
}

/*
====================
CL_GetSnapshot

The snapshot in the layout of Jedi Outcast (no playerstate of a vehicle)
====================
*/
qboolean CL_GetSnapshot( int snapshotNumber, snapshot_t *snapshot ) {
	clSnapshot_t	*clSnap;
	int				i, count;

	if ( snapshotNumber > cl.snap.messageNum ) {
		Com_Error( ERR_DROP, "CL_GetSnapshot: snapshotNumber > cl.snapshot.messageNum" );
	}

	// if the frame has fallen out of the circular buffer, we can't return it
	if ( cl.snap.messageNum - snapshotNumber >= PACKET_BACKUP ) {
		return qfalse;
	}

	// if the frame is not valid, we can't return it
	clSnap = &cl.snapshots[snapshotNumber & PACKET_MASK];
	if ( !clSnap->valid ) {
		return qfalse;
	}

	// if the entities in the frame have fallen out of their
	// circular buffer, we can't return it
	if ( cl.parseEntitiesNum - clSnap->parseEntitiesNum >= MAX_PARSE_ENTITIES ) {
		return qfalse;
	}

	// write the snapshot
	snapshot->snapFlags = clSnap->snapFlags;
	snapshot->serverCommandSequence = clSnap->serverCommandNum;
	snapshot->ping = clSnap->ping;
	snapshot->serverTime = clSnap->serverTime;
	Com_Memcpy( snapshot->areamask, clSnap->areamask, sizeof( snapshot->areamask ) );
	snapshot->ps = clSnap->ps;

	count = clSnap->numEntities;
	if ( count > MAX_ENTITIES_IN_SNAPSHOT ) {
		Com_DPrintf( "CL_GetSnapshot: truncated %i entities to %i\n", count, MAX_ENTITIES_IN_SNAPSHOT );
		count = MAX_ENTITIES_IN_SNAPSHOT;
	}
	snapshot->numEntities = count;

	for ( i = 0 ; i < count ; i++ ) {
		int entNum = ( clSnap->parseEntitiesNum + i ) & (MAX_PARSE_ENTITIES-1);

		snapshot->entities[i] = cl.parseEntities[ entNum ];
	}

	// FIXME: configstring changes and server commands!!!

	return qtrue;
}

/*
=====================
CL_SetUserCmdValue
=====================
*/
extern float cl_mPitchOverride;
extern float cl_mYawOverride;
extern float cl_mSensitivityOverride;
void CL_SetUserCmdValue( int userCmdValue, float sensitivityScale, float mPitchOverride, float mYawOverride, float mSensitivityOverride, int fpSel, int invenSel ) {
	cl.cgameUserCmdValue = userCmdValue;
	cl.cgameSensitivity = sensitivityScale;
	cl_mPitchOverride = mPitchOverride;
	cl_mYawOverride = mYawOverride;
	cl_mSensitivityOverride = mSensitivityOverride;
	cl.cgameForceSelection = fpSel;
	cl.cgameInvenSelection = invenSel;
}

/*
=====================
CL_SetClientForceAngle
=====================
*/
static void CL_SetClientForceAngle( int time, const vec3_t angle ) {
	cl.cgameViewAngleForceTime = time;
	VectorCopy( angle, cl.cgameViewAngleForce );
}

/*
=====================
CL_AddCgameCommand
=====================
*/
static void CL_AddCgameCommand( const char *cmdName ) {
	Cmd_AddCommand( cmdName, NULL );
}

int gCLTotalClientNum = 0;
//keep track of the total number of clients
extern cvar_t	*cl_autolodscale;
//if we want to do autolodscaling

void CL_DoAutoLODScale(void)
{
	float finalLODScaleFactor = 0;

	if ( gCLTotalClientNum >= 8 )
	{
		finalLODScaleFactor = (gCLTotalClientNum/-8.0f);
	}


	Cvar_Set( "r_autolodscalevalue", va("%f", finalLODScaleFactor) );
}

/*
=====================
CL_ConfigstringModified
=====================
*/
void CL_ConfigstringModified( void ) {
	char		*old, *s;
	int			i, index;
	char		*dup;
	gameState_t	oldGs;
	int			len;

	index = atoi( Cmd_Argv(1) );
	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		Com_Error( ERR_DROP, "CL_ConfigstringModified: bad index %i", index );
	}
	// get everything after "cs <num>"
	s = Cmd_ArgsFrom(2);

	old = cl.gameState.stringData + cl.gameState.stringOffsets[ index ];
	if ( !strcmp( old, s ) ) {
		return;		// unchanged
	}

	// build the new gameState_t
	oldGs = cl.gameState;

	Com_Memset( &cl.gameState, 0, sizeof( cl.gameState ) );

	// leave the first 0 for uninitialized strings
	cl.gameState.dataCount = 1;

	for ( i = 0 ; i < MAX_CONFIGSTRINGS ; i++ ) {
		if ( i == index ) {
			dup = s;
		} else {
			dup = oldGs.stringData + oldGs.stringOffsets[ i ];
		}
		if ( !dup[0] ) {
			continue;		// leave with the default empty string
		}

		len = (int)strlen( dup );

		if ( len + 1 + cl.gameState.dataCount > MAX_GAMESTATE_CHARS ) {
			Com_Error( ERR_DROP, "MAX_GAMESTATE_CHARS exceeded" );
		}

		// append it to the gameState string buffer
		cl.gameState.stringOffsets[ i ] = cl.gameState.dataCount;
		Com_Memcpy( cl.gameState.stringData + cl.gameState.dataCount, dup, len + 1 );
		cl.gameState.dataCount += len + 1;
	}

	if (cl_autolodscale && cl_autolodscale->integer)
	{
		if (index >= CS_PLAYERS &&
			index < CS_CHARSKINS)
		{ //this means that a client was updated in some way. Go through and count the clients.
			int clientCount = 0;
			i = CS_PLAYERS;

			while (i < CS_CHARSKINS)
			{
				s = cl.gameState.stringData + cl.gameState.stringOffsets[ i ];

				if (s && s[0])
				{
					clientCount++;
				}

				i++;
			}

			gCLTotalClientNum = clientCount;

#ifdef _DEBUG
			Com_DPrintf("%i clients\n", gCLTotalClientNum);
#endif

			CL_DoAutoLODScale();
		}
	}

	if ( index == CS_SYSTEMINFO ) {
		// parse serverId and other cvars
		CL_SystemInfoChanged();
	}
}

/*
===================
CL_GetServerCommand

Set up argc/argv for the given command
===================
*/
qboolean CL_GetServerCommand( int serverCommandNumber ) {
	char	*s;
	char	*cmd;
	static char bigConfigString[BIG_INFO_STRING];

	// if we have irretrievably lost a reliable command, drop the connection
	if ( serverCommandNumber <= clc.serverCommandSequence - MAX_RELIABLE_COMMANDS ) {
		// when a demo record was started after the client got a whole bunch of
		// reliable commands then the client never got those first reliable commands
		if ( clc.demoplaying )
			return qfalse;
		Com_Error( ERR_DROP, "CL_GetServerCommand: a reliable command was cycled out" );
		return qfalse;
	}

	if ( serverCommandNumber > clc.serverCommandSequence ) {
		Com_Error( ERR_DROP, "CL_GetServerCommand: requested a command not received" );
		return qfalse;
	}

	s = clc.serverCommands[ serverCommandNumber & ( MAX_RELIABLE_COMMANDS - 1 ) ];
	clc.lastExecutedServerCommand = serverCommandNumber;

	Com_DPrintf( "serverCommand: %i : %s\n", serverCommandNumber, s );

rescan:
	Cmd_TokenizeString( s );
	cmd = Cmd_Argv(0);

	if ( !strcmp( cmd, "disconnect" ) ) {
		Com_Error (ERR_SERVERDISCONNECT, "%s", SP_GetStringTextString("SVINGAME_SERVER_DISCONNECTED"));//"Server disconnected");
	}

	if ( !strcmp( cmd, "bcs0" ) ) {
		Com_sprintf( bigConfigString, BIG_INFO_STRING, "cs %s \"%s", Cmd_Argv(1), Cmd_Argv(2) );
		return qfalse;
	}

	if ( !strcmp( cmd, "bcs1" ) ) {
		s = Cmd_Argv(2);
		if( strlen(bigConfigString) + strlen(s) >= BIG_INFO_STRING ) {
			Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
		}
		strcat( bigConfigString, s );
		return qfalse;
	}

	if ( !strcmp( cmd, "bcs2" ) ) {
		s = Cmd_Argv(2);
		if( strlen(bigConfigString) + strlen(s) + 1 >= BIG_INFO_STRING ) {
			Com_Error( ERR_DROP, "bcs exceeded BIG_INFO_STRING" );
		}
		strcat( bigConfigString, s );
		strcat( bigConfigString, "\"" );
		s = bigConfigString;
		goto rescan;
	}

	if ( !strcmp( cmd, "cs" ) ) {
		CL_ConfigstringModified();
		// reparse the string, because CL_ConfigstringModified may have done another Cmd_TokenizeString()
		Cmd_TokenizeString( s );
		return qtrue;
	}

	if ( !strcmp( cmd, "map_restart" ) ) {
		// clear notify lines and outgoing commands before passing
		// the restart to the cgame
		Con_ClearNotify();
		// reparse the string, because Con_ClearNotify() may have done another Cmd_TokenizeString()
		Cmd_TokenizeString( s );
		Com_Memset( cl.cmds, 0, sizeof( cl.cmds ) );
		return qtrue;
	}

	// the clientLevelShot command is used during development
	// to generate 128*128 screenshots from the intermission
	// point of levels for the menu system to use
	// we pass it along to the cgame to make apropriate adjustments,
	// but we also clear the console and notify lines here
	if ( !strcmp( cmd, "clientLevelShot" ) ) {
		// don't do it if we aren't running the server locally,
		// otherwise malicious remote servers could overwrite
		// the existing thumbnails
		if ( !com_sv_running->integer ) {
			return qfalse;
		}
		// close the console
		Con_Close();
		// take a special screenshot next frame
		Cbuf_AddText( "wait ; wait ; wait ; wait ; screenshot levelshot\n" );
		return qtrue;
	}

	// we may want to put a "connect to other server" command here

	// cgame can now act on the command
	return qtrue;
}

/*
====================
CL_CM_LoadMap

Just adds default parameters that cgame doesn't need to know about
====================
*/
static void CL_CM_LoadMap( const char *mapname ) {
	CM_LoadMap( mapname, qtrue, NULL );
}

/*
====================
CL_SP_Print

The cgame module has the id of a string of a string package (CG_SP_REGISTER gave the package) and an argument
(a string or an integer) for the one %s or %d the string may have
====================
*/
static void CL_SP_Print( const word ID, intptr_t Data ) {
	cStringsSingle	*String;
	unsigned int	Flags;
	char			temp[1024], *Text;

	String = SP_GetString( ID );
	if ( String ) {
		Text = String->GetText();
		if ( Data ) {
			// replacement for unsafe printf - supports %d, %i and %s
			const char	*p, *tail;
			char		head[1024];
			qboolean	done = qfalse;

			Q_strncpyz( head, Text, sizeof( head ) );
			Q_strncpyz( temp, Text, sizeof( temp ) );

			while ( ( p = strchr( Text, '%' ) ) && !done ) {
				switch ( p[1] ) {
				case 's':
					head[p - Text] = '\0';
					tail = p + 2;
					Com_sprintf( temp, sizeof( temp ), "%s%s%s", head, (char *)VM_ArgString( CG_SP_PRINT, Data ), tail );
					done = qtrue;
					break;
				case 'd':
				case 'i':
					head[p - Text] = '\0';
					tail = p + 2;
					Com_sprintf( temp, sizeof( temp ), "%s%d%s", head, *(int *)VM_ArgPtr( CG_SP_PRINT, Data, sizeof( int ) ), tail );
					done = qtrue;
					break;
				case '\0':
					done = qtrue;
					break;
				default:
					p += 2;
					break;
				}
			}

			Text = temp;
		}

		Flags = String->GetFlags();

		if ( Flags & SP_FLAG1 ) {
			// the string is to be centered on the screen, which the engine of Jedi Outcast did not do any more: it
			// echoed it to the console and did not show it in the lines of the notify area
			Com_Printf( "\n%s\n\n", Text );
			Con_ClearNotify();
		} else {
			Com_Printf( "%s", Text );
		}
	}
}

/*
====================
CL_ShutdownCGame
====================
*/
void CL_ShutdownCGame( void ) {
	Key_SetCatcher( Key_GetCatcher( ) & ~KEYCATCH_CGAME );

	cls.cgameStarted = qfalse;

	if ( !cgvm ) {
		return;
	}

	VM_Call( cgvm, CG_SHUTDOWN );
	VM_Free( cgvm );
	cgvm = NULL;
	cl.mSharedMemory = NULL;
	cgSharedMemoryVM = 0;
}

/*
====================
CGVM_ wrappers: the calls into the cgame module (vmMain)
====================
*/

void CGVM_Init( int serverMessageNum, int serverCommandSequence, int clientNum ) {
	VM_Call( cgvm, CG_INIT, serverMessageNum, serverCommandSequence, clientNum );
}

void CGVM_Shutdown( void ) {
	VM_Call( cgvm, CG_SHUTDOWN );
}

qboolean CGVM_ConsoleCommand( void ) {
	return (qboolean)!!VM_Call( cgvm, CG_CONSOLE_COMMAND );
}

void CGVM_DrawActiveFrame( int serverTime, stereoFrame_t stereoView, qboolean demoPlayback ) {
	VM_Call( cgvm, CG_DRAW_ACTIVE_FRAME, serverTime, stereoView, demoPlayback );
	VM_Debug( 0 );
}

int CGVM_CrosshairPlayer( void ) {
	return VM_Call( cgvm, CG_CROSSHAIR_PLAYER );
}

int CGVM_LastAttacker( void ) {
	return VM_Call( cgvm, CG_LAST_ATTACKER );
}

void CGVM_KeyEvent( int key, qboolean down ) {
	// the key codes of Jedi Outcast 1.04 are the ones of this engine (the fakeAscii_t of ui/keycodes.h)
	VM_Call( cgvm, CG_KEY_EVENT, key, down );
}

void CGVM_MouseEvent( int x, int y ) {
	VM_Call( cgvm, CG_MOUSE_EVENT, x, y );
}

void CGVM_EventHandling( int type ) {
	VM_Call( cgvm, CG_EVENT_HANDLING, type );
}

int CGVM_PointContents( void ) {
	return VM_Call( cgvm, CG_POINT_CONTENTS );
}

void CGVM_GetLerpOrigin( void ) {
	VM_Call( cgvm, CG_GET_LERP_ORIGIN );
}

// the position data of an entity for the effects system: the three calls of Jedi Outcast in place of the one of
// Jedi Academy (the entity number comes in the shared memory and the data go out in it)
void CGVM_GetLerpData( void ) {
	TCGGetBoltData	*data = (TCGGetBoltData *)cl.mSharedMemory;
	TCGVectorData	*vec = (TCGVectorData *)cl.mSharedMemory;
	vec3_t			origin, angles, scale;
	const int		entityNum = data->mEntityNum;

	vec->mEntityNum = entityNum;
	VM_Call( cgvm, CG_GET_LERP_ORIGIN );
	VectorCopy( vec->mPoint, origin );

	vec->mEntityNum = entityNum;
	VM_Call( cgvm, CG_GET_LERP_ANGLES );
	VectorCopy( vec->mPoint, angles );

	vec->mEntityNum = entityNum;
	VM_Call( cgvm, CG_GET_MODEL_SCALE );
	VectorCopy( vec->mPoint, scale );

	VectorCopy( origin, data->mOrigin );
	VectorCopy( angles, data->mAngles );
	VectorCopy( scale, data->mScale );
	data->mEntityNum = entityNum;
}

void CGVM_Trace( void ) {
	VM_Call( cgvm, CG_TRACE );
}

// Jedi Outcast has no trace that goes through the Ghoul2 models
void CGVM_G2Trace( void ) {
	VM_Call( cgvm, CG_TRACE );
}

// Jedi Outcast has no mark on Ghoul2 models
void CGVM_G2Mark( void ) {
}

int CGVM_RagCallback( int callType ) {
	return 0;
}

// Jedi Outcast has no call for it (the line goes to the server)
qboolean CGVM_IncomingConsoleCommand( void ) {
	return qfalse;
}

qboolean CGVM_NoUseableForce( void ) {
	return qfalse;
}

// the address of a structure of the cgame module, as it is in the memory of the engine
static void *CL_CGameAddressToPointer( intptr_t address, int size ) {
	vm_t *oldVM = currentVM;
	void *pointer;

	currentVM = cgvm;
	pointer = VM_ArgPtr( CG_GET_ORIGIN_TRAJECTORY, address, size );
	currentVM = oldVM;

	return pointer;
}

// the list of the models of the module that the renderer is given with a bolt matrix is indexed by the number of the model on
// the Ghoul2 instance (a few)
#define MAX_G2_MODELS_VM	8

// the second half of the shared memory is for the arguments of the calls that take an address of the module
#define CG_SHARED_ARGUMENTS		( MAX_CG_SHARED_BUFFER_SIZE / 2 )

void CGVM_GetOrigin( int entID, vec3_t out ) {
	if ( !cl.mSharedMemory ) {
		VectorClear( out );
		return;
	}

	VM_Call( cgvm, CG_GET_ORIGIN, entID, cgSharedMemoryVM + CG_SHARED_ARGUMENTS );
	VectorCopy( (const float *)( cl.mSharedMemory + CG_SHARED_ARGUMENTS ), out );
}

void CGVM_GetAngles( int entID, vec3_t out ) {
	if ( !cl.mSharedMemory ) {
		VectorClear( out );
		return;
	}

	VM_Call( cgvm, CG_GET_ANGLES, entID, cgSharedMemoryVM + CG_SHARED_ARGUMENTS );
	VectorCopy( (const float *)( cl.mSharedMemory + CG_SHARED_ARGUMENTS ), out );
}

trajectory_t *CGVM_GetOriginTrajectory( int entID ) {
	return (trajectory_t *)CL_CGameAddressToPointer( VM_Call( cgvm, CG_GET_ORIGIN_TRAJECTORY, entID ), sizeof( trajectory_t ) );
}

trajectory_t *CGVM_GetAngleTrajectory( int entID ) {
	return (trajectory_t *)CL_CGameAddressToPointer( VM_Call( cgvm, CG_GET_ANGLE_TRAJECTORY, entID ), sizeof( trajectory_t ) );
}

void CGVM_ROFF_NotetrackCallback( int entID, const char *notetrack ) {
	if ( !cl.mSharedMemory ) {
		return;
	}

	Q_strncpyz( cl.mSharedMemory + CG_SHARED_ARGUMENTS, notetrack, MAX_CG_SHARED_BUFFER_SIZE - CG_SHARED_ARGUMENTS );
	VM_Call( cgvm, CG_ROFF_NOTETRACK_CALLBACK, entID, cgSharedMemoryVM + CG_SHARED_ARGUMENTS );
}

void CGVM_MapChange( void ) {
	if ( cgvm ) {
		VM_Call( cgvm, CG_MAP_CHANGE );
	}
}

// Jedi Outcast has no automap, no miscellaneous model entities and no camera shake
void CGVM_AutomapInput( void ) {
}

void CGVM_MiscEnt( void ) {
}

void CGVM_CameraShake( void ) {
}

/*
====================
conversions for the renderer
====================
*/

void CL_AddVMRefEntityToScene( const vmRefEntity_t *vmEnt ) {
	refEntity_t ent;

	Com_Memset( &ent, 0, sizeof( ent ) );
	Com_Memcpy( &ent, vmEnt->prefix, sizeof( vmEnt->prefix ) );
	ent.ghoul2 = G2VM_Get( vmEnt->ghoul2 );

	re->AddRefEntityToScene( &ent );
}

// the angles for the axis (the renderer does not use them, but the effects system does)
static void CL_AxisToAngles( const vec3_t axis[3], vec3_t angles ) {
	vec3_t	forward, right, up;

	vectoangles( axis[0], angles );
	angles[ROLL] = 0;
	AngleVectors( angles, forward, right, up );
	angles[ROLL] = RAD2DEG( atan2f( -DotProduct( axis[1], up ), DotProduct( axis[1], right ) ) );
}

// the effects system wants to know where the view is: the module tells with each frame (the position and the axis)
// and the field of view comes with the scene of the world
static refdef_t fxRefdef;

void CL_RenderVMScene( const vmRefdef_t *vmFd ) {
	refdef_t fd;

	Com_Memset( &fd, 0, sizeof( fd ) );
	fd.x = vmFd->x;
	fd.y = vmFd->y;
	fd.width = vmFd->width;
	fd.height = vmFd->height;
	fd.fov_x = vmFd->fov_x;
	fd.fov_y = vmFd->fov_y;
	VectorCopy( vmFd->vieworg, fd.vieworg );
	Com_Memcpy( fd.viewaxis, vmFd->viewaxis, sizeof( fd.viewaxis ) );
	CL_AxisToAngles( fd.viewaxis, fd.viewangles );
	fd.time = vmFd->time;
	fd.rdflags = vmFd->rdflags;
	Com_Memcpy( fd.areamask, vmFd->areamask, sizeof( fd.areamask ) );
	Com_Memcpy( fd.text, vmFd->text, sizeof( fd.text ) );

	if ( !( fd.rdflags & RDF_NOWORLDMODEL ) ) {
		fxRefdef.fov_x = fd.fov_x;
		fxRefdef.fov_y = fd.fov_y;
	}

	re->RenderScene( &fd );
}

// an effect that follows an entity (the bolt info the module gives is not used in Jedi Outcast)
extern int g_fxFollowEntityNum;	// FxScheduler.cpp

static void CL_PlayEntityEffect( int id, vec3_t org, vec3_t axis[3], int entNum ) {
	g_fxFollowEntityNum = entNum;
	FX_PlayEntityEffectID( id, org, axis, -1, entNum, -1, -1 );
	g_fxFollowEntityNum = -1;
}


/*
====================
CL_CgameSystemCalls

The cgame module is making a system call
====================
*/
static intptr_t CL_CgameSystemCalls( intptr_t *args ) {
	switch( args[0] ) {
	case CG_PRINT:
		Com_Printf( "%s", VMAS(1) );
		return 0;
	case CG_ERROR:
		Com_Error( ERR_DROP, "%s", VMAS(1) );
		return 0;
	case CG_MILLISECONDS:
		return Sys_Milliseconds();
	case CG_CVAR_REGISTER:
		Cvar_Register( VMAV(1, vmCvar_t), VMAS(2), VMAS(3), args[4] );
		return 0;
	case CG_CVAR_UPDATE:
		Cvar_Update( VMAV(1, vmCvar_t) );
		return 0;
	case CG_CVAR_SET:
		Cvar_VM_Set( VMAS(1), VMAS(2), VM_CGAME );
		return 0;
	case CG_CVAR_VARIABLESTRINGBUFFER:
		Cvar_VariableStringBuffer( VMAS(1), VMAP(2, char, args[3]), args[3] );
		return 0;
	case CG_ARGC:
		return Cmd_Argc();
	case CG_ARGV:
		Cmd_ArgvBuffer( args[1], VMAP(2, char, args[3]), args[3] );
		return 0;
	case CG_ARGS:
		Cmd_ArgsBuffer( VMAP(1, char, args[2]), args[2] );
		return 0;
	case CG_FS_FOPENFILE:
		return FS_FOpenFileByMode( VMAS(1), VMAV(2, fileHandle_t), (fsMode_t)args[3] );
	case CG_FS_READ:
		FS_Read( VMAP(1, char, args[2]), args[2], args[3] );
		return 0;
	case CG_FS_WRITE:
		FS_Write( VMAP(1, const char, args[2]), args[2], args[3] );
		return 0;
	case CG_FS_FCLOSEFILE:
		FS_FCloseFile( args[1] );
		return 0;
	case CG_SENDCONSOLECOMMAND:
		Cbuf_AddText( VMAS(1) );
		return 0;
	case CG_ADDCOMMAND:
		CL_AddCgameCommand( VMAS(1) );
		return 0;
	case CG_REMOVECOMMAND:
		Cmd_VM_RemoveCommand( VMAS(1), VM_CGAME );
		return 0;
	case CG_SENDCLIENTCOMMAND:
		CL_AddReliableCommand( VMAS(1), qfalse );
		return 0;
	case CG_UPDATESCREEN:
		// this is used during lengthy level loading, so pump message loop
//		Com_EventLoop();	// FIXME: if a server restarts here, BAD THINGS HAPPEN!
// We can't call Com_EventLoop here, a restart will crash and this _does_ happen
// if there is a map change while we are downloading at pk3.
// ZOID
		SCR_UpdateScreen();
		return 0;
	case CG_CM_LOADMAP:
		CL_CM_LoadMap( VMAS(1) );
		return 0;
	case CG_CM_NUMINLINEMODELS:
		return CM_NumInlineModels();
	case CG_CM_INLINEMODEL:
		return CM_InlineModel( args[1] );
	case CG_CM_TEMPBOXMODEL:
		return CM_TempBoxModel( VMAP(1, const vec_t, 3), VMAP(2, const vec_t, 3), qfalse );
	case CG_CM_TEMPCAPSULEMODEL:
		return CM_TempBoxModel( VMAP(1, const vec_t, 3), VMAP(2, const vec_t, 3), qtrue );
	case CG_CM_POINTCONTENTS:
		return CM_PointContents( VMAP(1, const vec_t, 3), args[2] );
	case CG_CM_TRANSFORMEDPOINTCONTENTS:
		return CM_TransformedPointContents( VMAP(1, const vec_t, 3), args[2], VMAP(3, const vec_t, 3), VMAP(4, const vec_t, 3) );
	case CG_CM_BOXTRACE:
		CM_BoxTrace( VMAV(1, trace_t), VMAP(2, const vec_t, 3), VMAP(3, const vec_t, 3), VMAP(4, const vec_t, 3), VMAP(5, const vec_t, 3), args[6], args[7], qfalse );
		return 0;
	case CG_CM_CAPSULETRACE:
		CM_BoxTrace( VMAV(1, trace_t), VMAP(2, const vec_t, 3), VMAP(3, const vec_t, 3), VMAP(4, const vec_t, 3), VMAP(5, const vec_t, 3), args[6], args[7], qtrue );
		return 0;
	case CG_CM_TRANSFORMEDBOXTRACE:
		CM_TransformedBoxTrace( VMAV(1, trace_t), VMAP(2, const vec_t, 3), VMAP(3, const vec_t, 3), VMAP(4, const vec_t, 3), VMAP(5, const vec_t, 3), args[6], args[7], VMAP(8, const vec_t, 3), VMAP(9, const vec_t, 3), qfalse );
		return 0;
	case CG_CM_TRANSFORMEDCAPSULETRACE:
		CM_TransformedBoxTrace( VMAV(1, trace_t), VMAP(2, const vec_t, 3), VMAP(3, const vec_t, 3), VMAP(4, const vec_t, 3), VMAP(5, const vec_t, 3), args[6], args[7], VMAP(8, const vec_t, 3), VMAP(9, const vec_t, 3), qtrue );
		return 0;
	case CG_CM_MARKFRAGMENTS:
		return re->MarkFragments( args[1], VMAA(2, const vec3_t, args[1]), VMAP(3, const vec_t, 3), args[4], VMAA(5, vec_t, args[4] * 3), args[6], VMAA(7, markFragment_t, args[6]) );
	case CG_S_MUTESOUND:
		S_MuteSound( args[1], args[2] );
		return 0;
	case CG_S_STARTSOUND:
		S_StartSound( args[1] ? VMAP(1, const vec_t, 3) : NULL, args[2], args[3], args[4] );
		return 0;
	case CG_S_STARTLOCALSOUND:
		S_StartLocalSound( args[1], args[2] );
		return 0;
	case CG_S_CLEARLOOPINGSOUNDS:
		S_ClearLoopingSounds();
		return 0;
	case CG_S_ADDLOOPINGSOUND:
		S_AddLoopingSound( args[1], VMAP(2, const vec_t, 3), VMAP(3, const vec_t, 3), args[4] );
		return 0;
	case CG_S_ADDREALLOOPINGSOUND:
		S_AddLoopingSound( args[1], VMAP(2, const vec_t, 3), VMAP(3, const vec_t, 3), args[4] );
		return 0;
	case CG_S_STOPLOOPINGSOUND:
		S_StopLoopingSound( args[1] );
		return 0;
	case CG_S_UPDATEENTITYPOSITION:
		S_UpdateEntityPosition( args[1], VMAP(2, const vec_t, 3) );
		return 0;
	case CG_S_RESPATIALIZE:
		S_Respatialize( args[1], VMAP(2, const vec_t, 3), VMAP(3, vec3_t, 3), args[4] );
		return 0;
	case CG_S_REGISTERSOUND:
		return S_RegisterSound( VMAS(1) );
	case CG_S_STARTBACKGROUNDTRACK:
		S_StartBackgroundTrack( VMAS(1), VMAS(2), (qboolean)!!args[3] );
		return 0;
	case CG_S_STOPBACKGROUNDTRACK:
		S_StopBackgroundTrack();
		return 0;
	case CG_R_LOADWORLDMAP:
		re->LoadWorld( VMAS(1) );
		return 0;
	case CG_R_REGISTERMODEL:
		return re->RegisterModel( VMAS(1) );
	case CG_R_REGISTERSKIN:
		return re->RegisterSkin( VMAS(1) );
	case CG_R_REGISTERSHADER:
		return re->RegisterShader( VMAS(1) );
	case CG_R_REGISTERSHADERNOMIP:
		return re->RegisterShaderNoMip( VMAS(1) );
	case CG_R_REGISTERFONT:
		return re->RegisterFont( VMAS(1) );
	case CG_R_FONT_STRLENPIXELS:
		return re->Font_StrLenPixels( VMAS(1), args[2], VMF(3) );
	case CG_R_FONT_STRLENCHARS:
		return re->Font_StrLenChars( VMAS(1) );
	case CG_R_FONT_STRHEIGHTPIXELS:
		return re->Font_HeightPixels( args[1], VMF(2) );
	case CG_R_FONT_DRAWSTRING:
		re->Font_DrawString( args[1], args[2], VMAS(3), VMAP(4, const vec_t, 4), args[5], args[6], VMF(7) );
		return 0;
	case CG_LANGUAGE_ISASIAN:
		return re->Language_IsAsian();
	case CG_LANGUAGE_USESSPACES:
		return re->Language_UsesSpaces();
	case CG_ANYLANGUAGE_READCHARFROMSTRING:
		return re->AnyLanguage_ReadCharFromString( VMAS(1), VMAV(2, int), VMAV(3, qboolean) );
	case CG_R_CLEARSCENE:
		re->ClearScene();
		return 0;
	case CG_R_ADDREFENTITYTOSCENE:
		CL_AddVMRefEntityToScene( VMAV(1, const vmRefEntity_t) );
		return 0;
	case CG_R_ADDPOLYTOSCENE:
		re->AddPolyToScene( args[1], args[2], VMAA(3, const polyVert_t, args[2]), 1 );
		return 0;
	case CG_R_ADDPOLYSTOSCENE:
		// args[2] * args[4] > INT_MAX
		if ( args[4] > 0 && args[2] > INT_MAX / args[4] ) {
			Com_Error( ERR_DROP, "CG_R_ADDPOLYSTOSCENE: too many vertices" );
		}
		re->AddPolyToScene( args[1], args[2], VMAA(3, const polyVert_t, args[2] * args[4]), args[4] );
		return 0;
	case CG_R_LIGHTFORPOINT:
		return re->LightForPoint( VMAP(1, vec_t, 3), VMAP(2, vec_t, 3), VMAP(3, vec_t, 3), VMAP(4, vec_t, 3) );
	case CG_R_ADDLIGHTTOSCENE:
		re->AddLightToScene( VMAP(1, const vec_t, 3), VMF(2), VMF(3), VMF(4), VMF(5) );
		return 0;
	case CG_R_ADDADDITIVELIGHTTOSCENE:
		re->AddAdditiveLightToScene( VMAP(1, const vec_t, 3), VMF(2), VMF(3), VMF(4), VMF(5) );
		return 0;
	case CG_R_RENDERSCENE:
		CL_RenderVMScene( VMAV(1, const vmRefdef_t) );
		return 0;
	case CG_R_SETCOLOR:
		re->SetColor( args[1] ? VMAP(1, vec_t, 4) : NULL );
		return 0;
	case CG_R_DRAWSTRETCHPIC:
		re->DrawStretchPic( VMF(1), VMF(2), VMF(3), VMF(4), VMF(5), VMF(6), VMF(7), VMF(8), args[9] );
		return 0;
	case CG_R_MODELBOUNDS:
		re->ModelBounds( args[1], VMAP(2, vec_t, 3), VMAP(3, vec_t, 3) );
		return 0;
	case CG_R_LERPTAG:
		return re->LerpTag( VMAV(1, orientation_t), args[2], args[3], args[4], VMF(5), VMAS(6) );
	case CG_R_DRAWROTATEPIC:
		re->DrawRotatePic( VMF(1), VMF(2), VMF(3), VMF(4), VMF(5), VMF(6), VMF(7), VMF(8), VMF(9), args[10] );
		return 0;
	case CG_R_DRAWROTATEPIC2:
		re->DrawRotatePic2( VMF(1), VMF(2), VMF(3), VMF(4), VMF(5), VMF(6), VMF(7), VMF(8), VMF(9), args[10] );
		return 0;
	case CG_R_REMAP_SHADER:
		re->RemapShader( VMAS(1), VMAS(2), VMAS(3) );
		return 0;
	case CG_R_GET_LIGHT_STYLE:
		re->GetLightStyle( args[1], VMAP(2, byte, 4) );
		return 0;
	case CG_R_SET_LIGHT_STYLE:
		re->SetLightStyle( args[1], args[2] );
		return 0;
	case CG_R_GET_BMODEL_VERTS:
		re->GetBModelVerts( args[1], VMAA(2, vec3_t, 4), VMAP(3, vec_t, 3) );
		return 0;
	case CG_R_INPVS:
		return re->inPVS( VMAP(1, const vec_t, 3), VMAP(2, const vec_t, 3), NULL );
	case CG_GET_ENTITY_TOKEN:
		return re->GetEntityToken( VMAP(1, char, args[2]), args[2] );

	case CG_GETGLCONFIG:
		CL_GetVMGLConfig( VMAV(1, vmglconfig_t) );
		return 0;
	case CG_GETGAMESTATE:
		CL_GetGameState( VMAV(1, gameState_t) );
		return 0;
	case CG_GETCURRENTSNAPSHOTNUMBER:
		CL_GetCurrentSnapshotNumber( VMAV(1, int), VMAV(2, int) );
		return 0;
	case CG_GETSNAPSHOT:
		return CL_GetSnapshot( args[1], VMAV(2, snapshot_t) );
	case CG_GETSERVERCOMMAND:
		return CL_GetServerCommand( args[1] );
	case CG_GETCURRENTCMDNUMBER:
		return CL_GetCurrentCmdNumber();
	case CG_GETUSERCMD:
		return CL_GetUserCmd( args[1], VMAV(2, usercmd_t) );
	case CG_SETUSERCMDVALUE:
		// no override of the mouse speed for the pitch, the yaw and the sensitivity in Jedi Outcast
		CL_SetUserCmdValue( args[1], VMF(2), 0.0f, 0.0f, 0.0f, args[3], args[4] );
		return 0;
	case CG_SETCLIENTFORCEANGLE:
		CL_SetClientForceAngle( args[1], VMAP(2, const vec_t, 3) );
		return 0;
	case CG_SETCLIENTTURNEXTENT:
		cl.cgameTurnExtentAdd = VMF(1);
		cl.cgameTurnExtentSub = VMF(2);
		cl.cgameTurnExtentTime = args[3];
		return 0;

	case CG_OPENUIMENU:
		UIVM_SetActiveMenu( (uiMenuCommand_t)args[1] );
		return 0;

	case CG_MEMORY_REMAINING:
		return Hunk_MemoryRemaining();
	case CG_KEY_ISDOWN:
		return Key_IsDown( args[1] );
	case CG_KEY_GETCATCHER:
		return Key_GetCatcher();
	case CG_KEY_SETCATCHER:
		// don't allow the cgame module to close the console
		Key_SetCatcher( args[1] | ( Key_GetCatcher() & KEYCATCH_CONSOLE ) );
		return 0;
	case CG_KEY_GETKEY:
		return Key_GetKey( VMAS(1) );

	case CGAME_MEMSET:
		Com_Memset( VMAP(1, char, args[3]), args[2], args[3] );
		return 0;
	case CGAME_MEMCPY:
		Com_Memcpy( VMAP(1, char, args[3]), VMAP(2, char, args[3]), args[3] );
		return 0;
	case CGAME_STRNCPY:
		return VM_strncpy( args[1], args[2], args[3] );
	case CGAME_SIN:
		return FloatAsInt( sinf( VMF(1) ) );
	case CGAME_COS:
		return FloatAsInt( cosf( VMF(1) ) );
	case CGAME_ATAN2:
		return FloatAsInt( atan2f( VMF(1), VMF(2) ) );
	case CGAME_SQRT:
		return FloatAsInt( VMF(1) < 0 ? 0 : sqrtf( VMF(1) ) );
	case CGAME_FLOOR:
		return FloatAsInt( floorf( VMF(1) ) );
	case CGAME_CEIL:
		return FloatAsInt( ceilf( VMF(1) ) );
	case CGAME_ACOS:
		return FloatAsInt( Q_acos( VMF(1) ) );
	case CGAME_ASIN:
		return FloatAsInt( Q_asin( VMF(1) ) );
	case CGAME_MATRIXMULTIPLY:
		MatrixMultiply( VMAP(1, vec3_t, 3), VMAP(2, vec3_t, 3), VMAP(3, vec3_t, 3) );
		return 0;
	case CGAME_ANGLEVECTORS:
		AngleVectors( VMAP(1, const vec_t, 3), args[2] ? VMAP(2, vec_t, 3) : NULL, args[3] ? VMAP(3, vec_t, 3) : NULL, args[4] ? VMAP(4, vec_t, 3) : NULL );
		return 0;
	case CGAME_PERPENDICULARVECTOR:
		PerpendicularVector( VMAP(1, vec_t, 3), VMAP(2, const vec_t, 3) );
		return 0;

	case CG_PC_ADD_GLOBAL_DEFINE:
		return botlib_export->PC_AddGlobalDefine( VMAS(1) );
	case CG_PC_LOAD_SOURCE:
		return botlib_export->PC_LoadSourceHandle( VMAS(1) );
	case CG_PC_FREE_SOURCE:
		return botlib_export->PC_FreeSourceHandle( args[1] );
	case CG_PC_READ_TOKEN:
		return botlib_export->PC_ReadTokenHandle( args[1], VMAV(2, pc_token_t) );
	case CG_PC_SOURCE_FILE_AND_LINE:
		return botlib_export->PC_SourceFileAndLine( args[1], VMAP(2, char, MAX_QPATH), VMAV(3, int) );
	case CG_PC_LOAD_GLOBAL_DEFINES:
		return botlib_export->PC_LoadGlobalDefines( VMAS(1) );
	case CG_PC_REMOVE_ALL_GLOBAL_DEFINES:
		botlib_export->PC_RemoveAllGlobalDefines();
		return 0;

	case CG_REAL_TIME:
		return Com_RealTime( VMAV(1, qtime_t) );
	case CG_SNAPVECTOR:
		Sys_SnapVector( VMAP(1, vec_t, 3) );
		return 0;

	case CG_CIN_PLAYCINEMATIC:
		return CIN_PlayCinematic( VMAS(1), args[2], args[3], args[4], args[5], args[6] );
	case CG_CIN_STOPCINEMATIC:
		return CIN_StopCinematic( args[1] );
	case CG_CIN_RUNCINEMATIC:
		return CIN_RunCinematic( args[1] );
	case CG_CIN_DRAWCINEMATIC:
		CIN_DrawCinematic( args[1] );
		return 0;
	case CG_CIN_SETEXTENTS:
		CIN_SetExtents( args[1], args[2], args[3], args[4], args[5] );
		return 0;

	case CG_FX_ADDLINE:
		FX_AddLine( VMAP(1, vec_t, 3), VMAP(2, vec_t, 3), VMF(3), VMF(4), VMF(5),
					VMF(6), VMF(7), VMF(8),
					VMAP(9, vec_t, 3), VMAP(10, vec_t, 3), VMF(11),
					args[12], args[13], args[14] );
		return 0;

	case CG_FX_REGISTER_EFFECT:
		return FX_RegisterEffect( VMAS(1) );

	case CG_FX_PLAY_SIMPLE_EFFECT:
		// an effect with the default direction (up)
		{
			vec3_t up = { 0, 0, 1 };
			FX_PlayEffect( VMAS(1), VMAP(2, vec_t, 3), up, -1, -1 );
		}
		return 0;

	case CG_FX_PLAY_EFFECT:
		FX_PlayEffect( VMAS(1), VMAP(2, vec_t, 3), VMAP(3, vec_t, 3), -1, -1 );
		return 0;

	case CG_FX_PLAY_ENTITY_EFFECT:
		CL_PlayEntityEffect( FX_RegisterEffect( VMAS(1) ), VMAP(2, vec_t, 3), VMAP(3, vec3_t, 3), args[5] );
		return 0;

	case CG_FX_PLAY_SIMPLE_EFFECT_ID:
		{
			vec3_t up = { 0, 0, 1 };
			FX_PlayEffectID( args[1], VMAP(2, vec_t, 3), up, -1, -1 );
		}
		return 0;

	case CG_FX_PLAY_EFFECT_ID:
		FX_PlayEffectID( args[1], VMAP(2, vec_t, 3), VMAP(3, vec_t, 3), -1, -1 );
		return 0;

	case CG_FX_PLAY_ENTITY_EFFECT_ID:
		CL_PlayEntityEffect( args[1], VMAP(2, vec_t, 3), VMAP(3, vec3_t, 3), args[5] );
		return 0;

	case CG_FX_PLAY_BOLTED_EFFECT_ID:
		{
			const sharedBoltInterface_t *bolt = VMAV(2, const sharedBoltInterface_t);
			CGhoul2Info_v *ghoul2 = G2VM_Get( bolt->ghoul2 );
			int boltInfo = 0;

			if ( ghoul2 && re->G2API_AttachEnt( &boltInfo, *ghoul2, bolt->modelNum, bolt->boltNum, bolt->entNum, bolt->modelNum ) ) {
				vec3_t origin;

				VectorCopy( bolt->origin, origin );
				FX_PlayBoltedEffectID( args[1], origin, boltInfo, ghoul2, 0, qfalse );
			}
		}
		return 0;

	case CG_FX_ADD_SCHEDULED_EFFECTS:
		FX_AddScheduledEffects( qfalse );
		return 0;

	case CG_FX_INIT_SYSTEM:
		Com_Memset( &fxRefdef, 0, sizeof( fxRefdef ) );
		fxRefdef.fov_x = fxRefdef.fov_y = 90.0f;
		return FX_InitSystem( &fxRefdef );

	case CG_FX_FREE_SYSTEM:
		return FX_FreeSystem();

	case CG_FX_ADJUST_TIME:
		// the time and where the view is
		VectorCopy( VMAP(2, const vec_t, 3), fxRefdef.vieworg );
		Com_Memcpy( fxRefdef.viewaxis, VMAP(3, const vec3_t, 3), sizeof( fxRefdef.viewaxis ) );
		CL_AxisToAngles( fxRefdef.viewaxis, fxRefdef.viewangles );
		FX_AdjustTime( args[1] );
		return 0;

	case CG_FX_ADDPOLY:
		{
			const addpolyArgStruct_t *p = VMAV(1, const addpolyArgStruct_t);

			if ( p ) {
				FX_AddPoly( (vec3_t *)p->p, (vec2_t *)p->ev, p->numVerts, (float *)p->vel, (float *)p->accel, p->alpha1, p->alpha2,
					p->alphaParm, (float *)p->rgb1, (float *)p->rgb2, p->rgbParm, (float *)p->rotationDelta, p->bounce, p->motionDelay,
					p->killTime, p->shader, p->flags );
			}
		}
		return 0;

	case CG_FX_ADDBEZIER:
		{
			const addbezierArgStruct_t *b = VMAV(1, const addbezierArgStruct_t);

			if ( b ) {
				FX_AddBezier( (float *)b->start, (float *)b->end, (float *)b->control1, (float *)b->control1Vel, (float *)b->control2, (float *)b->control2Vel,
					b->size1, b->size2, b->sizeParm, b->alpha1, b->alpha2, b->alphaParm, (float *)b->sRGB,
					(float *)b->eRGB, b->rgbParm, b->killTime, b->shader, b->flags );
			}
		}
		return 0;

	case CG_FX_ADDPRIMITIVE:
		{
			const effectTrailArgStruct_t *a = VMAV(1, const effectTrailArgStruct_t);

			if ( a ) {
				FX_FeedTrail( (effectTrailArgStruct_t *)a );
			}
		}
		return 0;

	case CG_FX_ADDSPRITE:
		{
			const addspriteArgStruct_t *s = VMAV(1, const addspriteArgStruct_t);

			if ( s ) {
				static vec3_t rgb = { 1, 1, 1 };

				FX_AddParticle( (float *)s->origin, (float *)s->vel, (float *)s->accel, s->scale, s->dscale, 0, s->sAlpha, s->eAlpha, 0,
					rgb, rgb, 0, s->rotation, 0, vec3_origin, vec3_origin, s->bounce, 0, 0, s->life,
					s->shader, s->flags );
			}
		}
		return 0;

	case CG_SP_PRINT:
		CL_SP_Print( args[1], args[2] );
		return 0;

	case CG_SP_GETSTRINGTEXTSTRING:
		return SP_VMGetStringText( VMAS(1), VMAP(2, char, args[3]), args[3] );

	case CG_SP_REGISTER:
		return !!SP_Register( VMAS(1), SP_REGISTER_CLIENT );

	case CG_ROFF_CLEAN:
		return theROFFSystem.Clean( qtrue );

	case CG_ROFF_UPDATE_ENTITIES:
		theROFFSystem.UpdateEntities( qtrue );
		return 0;

	case CG_ROFF_CACHE:
		return theROFFSystem.Cache( VMAS(1), qtrue );

	case CG_ROFF_PLAY:
		return theROFFSystem.Play( args[1], args[2], (qboolean)!!args[3], qtrue );

	case CG_ROFF_PURGE_ENT:
		return theROFFSystem.PurgeEnt( args[1], qtrue );

	case CG_G2_LISTSURFACES:
		G2VM_ListSurfaces( args[1], args[2] );
		return 0;

	case CG_G2_LISTBONES:
		G2VM_ListBones( args[1], args[2], args[3] );
		return 0;

	case CG_G2_HAVEWEGHOULMODELS:
		return G2VM_HaveWeGhoul2Models( args[1] );

	case CG_G2_SETMODELS:
		// the renderer does not need the list of the models and the skins of the module
		return 0;

	case CG_G2_GETBOLT:
		return G2VM_GetBoltMatrix( args[1], args[2], args[3], VMAV(4, mdxaBone_t), VMAP(5, const vec_t, 3), VMAP(6, const vec_t, 3), args[7],
			VMAA(8, qhandle_t, MAX_G2_MODELS_VM), VMAP(9, vec_t, 3), qtrue, qfalse );

	case CG_G2_GETBOLT_NOREC:
		return G2VM_GetBoltMatrix( args[1], args[2], args[3], VMAV(4, mdxaBone_t), VMAP(5, const vec_t, 3), VMAP(6, const vec_t, 3), args[7],
			VMAA(8, qhandle_t, MAX_G2_MODELS_VM), VMAP(9, vec_t, 3), qfalse, qfalse );

	case CG_G2_GETBOLT_NOREC_NOROT:
		return G2VM_GetBoltMatrix( args[1], args[2], args[3], VMAV(4, mdxaBone_t), VMAP(5, const vec_t, 3), VMAP(6, const vec_t, 3), args[7],
			VMAA(8, qhandle_t, MAX_G2_MODELS_VM), VMAP(9, vec_t, 3), qfalse, qtrue );

	case CG_G2_INITGHOUL2MODEL:
		return G2VM_InitGhoul2Model( VMAV(1, g2handle_t), VMAS(2), args[3], (qhandle_t)args[4], (qhandle_t)args[5], args[6], args[7] );

	case CG_G2_COLLISIONDETECT:
		G2VM_CollisionDetect( VMAA(1, CollisionRecord_t, MAX_G2_COLLISIONS), args[2], VMAP(3, const vec_t, 3), VMAP(4, const vec_t, 3),
			args[5], args[6], VMAP(7, vec_t, 3), VMAP(8, vec_t, 3), VMAP(9, vec_t, 3), G2VertSpaceClient, args[10], args[11], VMF(12) );
		return 0;

	case CG_G2_ANGLEOVERRIDE:
		return G2VM_SetBoneAngles( args[1], args[2], VMAS(3), VMAP(4, const vec_t, 3), args[5], args[6], args[7], args[8],
			VMAA(9, qhandle_t, args[2] + 1), args[10], args[11] );

	case CG_G2_CLEANMODELS:
		G2VM_CleanGhoul2Models( VMAV(1, g2handle_t) );
		return 0;

	case CG_G2_PLAYANIM:
		return G2VM_SetBoneAnim( args[1], args[2], VMAS(3), args[4], args[5], args[6], VMF(7), args[8], VMF(9), args[10] );

	case CG_G2_GETGLANAME:
		{
			const char *name = G2VM_GetGLAName( args[1], args[2] );

			if ( name ) {
				strcpy( VMAP(3, char, strlen( name ) + 1), name );
			}
		}
		return 0;

	case CG_G2_COPYGHOUL2INSTANCE:
		return G2VM_CopyGhoul2Instance( args[1], args[2], args[3] );

	case CG_G2_COPYSPECIFICGHOUL2MODEL:
		G2VM_CopySpecificG2Model( args[1], args[2], args[3], args[4] );
		return 0;

	case CG_G2_DUPLICATEGHOUL2INSTANCE:
		G2VM_DuplicateGhoul2Instance( args[1], VMAV(2, g2handle_t) );
		return 0;

	case CG_G2_HASGHOUL2MODELONINDEX:
		return G2VM_HasGhoul2ModelOnIndex( VMAV(1, const g2handle_t), args[2] );

	case CG_G2_REMOVEGHOUL2MODEL:
		return G2VM_RemoveGhoul2Model( VMAV(1, g2handle_t), args[2] );

	case CG_G2_ADDBOLT:
		return G2VM_AddBolt( args[1], args[2], VMAS(3) );

	case CG_G2_SETBOLTON:
		G2VM_SetBoltInfo( args[1], args[2], args[3] );
		return 0;

	case CG_G2_GIVEMEVECTORFROMMATRIX:
		re->G2API_GiveMeVectorFromMatrix( VMAV(1, mdxaBone_t), (Eorientations)args[2], VMAP(3, vec_t, 3) );
		return 0;

	case CG_G2_SETROOTSURFACE:
		return G2VM_SetRootSurface( args[1], args[2], VMAS(3) );

	case CG_G2_SETSURFACEONOFF:
		return G2VM_SetSurfaceOnOff( args[1], VMAS(2), args[3] );

	case CG_G2_SETNEWORIGIN:
		return G2VM_SetNewOrigin( args[1], args[2] );

	case CG_SET_SHARED_BUFFER:
		cl.mSharedMemory = VMAP(1, char, MAX_CG_SHARED_BUFFER_SIZE);
		cgSharedMemoryVM = args[1];
		return 0;
	}

	Com_Error( ERR_DROP, "Bad cgame system trap: %lli", (long long int)args[0] );
	return 0;
}

/*
====================
CL_InitCGame

Should only be called by CL_StartHunkUsers
====================
*/
void CL_InitCGame( void ) {
	const char			*info;
	const char			*mapname;
	int					t1, t2;
	vmInterpret_t		interpret;

	t1 = Sys_Milliseconds();

	// put away the console
	Con_Close();

	// find the current mapname
	info = cl.gameState.stringData + cl.gameState.stringOffsets[ CS_SERVERINFO ];
	mapname = Info_ValueForKey( info, "mapname" );
	Com_sprintf( cl.mapname, sizeof( cl.mapname ), "maps/%s.bsp", mapname );

	// load the dll or bytecode
	if ( cl_connectedToPureServer != 0 ) {
		// if sv_pure is set we only allow qvms to be loaded
		interpret = VMI_BYTECODE;
	}
	else {
		interpret = (vmInterpret_t)(int)Cvar_VariableValue( "vm_cgame" );
	}
	cgvm = VM_Create( "cgame", CL_CgameSystemCalls, interpret );
	if ( !cgvm ) {
		Com_Error( ERR_DROP, "VM_Create on cgame failed" );
	}
	cls.state = CA_LOADING;

	// init for this gamestate
	// use the lastExecutedServerCommand instead of the serverCommandSequence
	// otherwise server commands sent just before a gamestate are dropped
	CGVM_Init( clc.serverMessageSequence, clc.lastExecutedServerCommand, clc.clientNum );

	// reset any CVAR_CHEAT cvars registered by cgame
	if ( !clc.demoplaying && !cl_connectedToCheatServer )
		Cvar_SetCheatState();

	// we will send a usercmd this frame, which
	// will cause the server to send us the first snapshot
	cls.state = CA_PRIMED;

	t2 = Sys_Milliseconds();

	Com_Printf( "CL_InitCGame: %5.2f seconds\n", (t2-t1)/1000.0 );

	// have the renderer touch all its images, so they are present
	// on the card even if the driver does deferred loading
	re->EndRegistration();

	// make sure everything is paged in
//	if (!Sys_LowPhysicalMemory())
	{
		Com_TouchMemory();
	}

	// clear anything that got printed
	Con_ClearNotify ();
}

/*
====================
CL_GameCommand

See if the current console command is claimed by the cgame
====================
*/
qboolean CL_GameCommand( void ) {
	if ( !cgvm ) {
		return qfalse;
	}

	return CGVM_ConsoleCommand();
}

/*
=====================
CL_CGameRendering
=====================
*/
void CL_CGameRendering( stereoFrame_t stereo ) {
	if ( !com_sv_running->integer )
	{ //set the server time to match the client time, if we don't have a server going.
		re->G2API_SetTime( cl.serverTime, 0 );
	}
	re->G2API_SetTime( cl.serverTime, 1 );

	CGVM_DrawActiveFrame( cl.serverTime, stereo, clc.demoplaying );
}

/*
=================
CL_AdjustTimeDelta

Adjust the clients view of server time.

We attempt to have cl.serverTime exactly equal the server's view
of time plus the timeNudge, but with variable latencies over
the internet it will often need to drift a bit to match conditions.

Our ideal time would be to have the adjusted time approach, but not pass,
the very latest snapshot.

Adjustments are only made when a new snapshot arrives with a rational
latency, which keeps the adjustment process framerate independent and
prevents massive overadjustment during times of significant packet loss
or bursted delayed packets.
=================
*/

#define	RESET_TIME	500

void CL_AdjustTimeDelta( void ) {
	int		newDelta;
	int		deltaDelta;

	cl.newSnapshots = qfalse;

	// the delta never drifts when replaying a demo
	if ( clc.demoplaying ) {
		return;
	}

	newDelta = cl.snap.serverTime - cls.realtime;
	deltaDelta = abs( newDelta - cl.serverTimeDelta );

	if ( deltaDelta > RESET_TIME ) {
		cl.serverTimeDelta = newDelta;
		cl.oldServerTime = cl.snap.serverTime;	// FIXME: is this a problem for cgame?
		cl.serverTime = cl.snap.serverTime;
		if ( cl_showTimeDelta->integer ) {
			Com_Printf( "<RESET> " );
		}
	} else if ( deltaDelta > 100 ) {
		// fast adjust, cut the difference in half
		if ( cl_showTimeDelta->integer ) {
			Com_Printf( "<FAST> " );
		}
		cl.serverTimeDelta = ( cl.serverTimeDelta + newDelta ) >> 1;
	} else {
		// slow drift adjust, only move 1 or 2 msec

		// if any of the frames between this and the previous snapshot
		// had to be extrapolated, nudge our sense of time back a little
		// the granularity of +1 / -2 is too high for timescale modified frametimes
		if ( com_timescale->value == 0 || com_timescale->value == 1 ) {
			if ( cl.extrapolatedSnapshot ) {
				cl.extrapolatedSnapshot = qfalse;
				cl.serverTimeDelta -= 2;
			} else {
				// otherwise, move our sense of time forward to minimize total latency
				cl.serverTimeDelta++;
			}
		}
	}

	if ( cl_showTimeDelta->integer ) {
		Com_Printf( "%i ", cl.serverTimeDelta );
	}
}

/*
==================
CL_FirstSnapshot
==================
*/
void CL_FirstSnapshot( void ) {
	// ignore snapshots that don't have entities
	if ( cl.snap.snapFlags & SNAPFLAG_NOT_ACTIVE ) {
		return;
	}

	re->RegisterMedia_LevelLoadEnd();

	cls.state = CA_ACTIVE;

	// set the timedelta so we are exactly on this first frame
	cl.serverTimeDelta = cl.snap.serverTime - cls.realtime;
	cl.oldServerTime = cl.snap.serverTime;

	clc.timeDemoBaseTime = cl.snap.serverTime;

	// if this is the first frame of active play,
	// execute the contents of activeAction now
	// this is to allow scripting a timedemo to start right
	// after loading
	if ( cl_activeAction->string[0] ) {
		Cbuf_AddText( cl_activeAction->string );
		Cvar_Set( "activeAction", "" );
	}
}

/*
==================
CL_SetCGameTime
==================
*/
void CL_SetCGameTime( void ) {
	// getting a valid frame message ends the connection process
	if ( cls.state != CA_ACTIVE ) {
		if ( cls.state != CA_PRIMED ) {
			return;
		}
		if ( clc.demoplaying ) {
			// we shouldn't get the first snapshot on the same frame
			// as the gamestate, because it causes a bad time skip
			if ( !clc.firstDemoFrameSkipped ) {
				clc.firstDemoFrameSkipped = qtrue;
				return;
			}
			CL_ReadDemoMessage();
		}
		if ( cl.newSnapshots ) {
			cl.newSnapshots = qfalse;
			CL_FirstSnapshot();
		}
		if ( cls.state != CA_ACTIVE ) {
			return;
		}
	}

	// if we have gotten to this point, cl.snap is guaranteed to be valid
	if ( !cl.snap.valid ) {
		Com_Error( ERR_DROP, "CL_SetCGameTime: !cl.snap.valid" );
	}

	// allow pause in single player
	if ( sv_paused->integer && CL_CheckPaused() && com_sv_running->integer ) {
		// paused
		return;
	}

	if ( cl.snap.serverTime < cl.oldFrameServerTime ) {
		Com_Error( ERR_DROP, "cl.snap.serverTime < cl.oldFrameServerTime" );
	}
	cl.oldFrameServerTime = cl.snap.serverTime;


	// get our current view of time

	if ( clc.demoplaying && cl_freezeDemo->integer ) {
		// cl_freezeDemo is used to lock a demo in place for single frame advances
	} else
	{
		// cl_timeNudge is a user adjustable cvar that allows more
		// or less latency to be added in the interest of better
		// smoothness or better responsiveness.
		int tn;

		tn = cl_timeNudge->integer;
#ifdef _DEBUG
		if (tn<-900) {
			tn = -900;
		} else if (tn>900) {
			tn = 900;
		}
#else
		if (tn<-30) {
			tn = -30;
		} else if (tn>30) {
			tn = 30;
		}
#endif

		cl.serverTime = cls.realtime + cl.serverTimeDelta - tn;

		// guarantee that time will never flow backwards, even if
		// serverTimeDelta made an adjustment or cl_timeNudge was changed
		if ( cl.serverTime < cl.oldServerTime ) {
			cl.serverTime = cl.oldServerTime;
		}
		cl.oldServerTime = cl.serverTime;

		// note if we are almost past the latest frame (without timeNudge),
		// so we will try and adjust back a bit when the next snapshot arrives
		if ( cls.realtime + cl.serverTimeDelta >= cl.snap.serverTime - 5 ) {
			cl.extrapolatedSnapshot = qtrue;
		}
	}

	// if we have gotten new snapshots, drift serverTimeDelta
	// don't do this every frame, or a period of packet loss would
	// make a huge adjustment
	if ( cl.newSnapshots ) {
		CL_AdjustTimeDelta();
	}

	if ( !clc.demoplaying ) {
		return;
	}

	// if we are playing a demo back, we can just keep reading
	// messages from the demo file until the cgame definately
	// has valid snapshots to interpolate between

	// a timedemo will always use a deterministic set of time samples
	// no matter what speed machine it is run on,
	// while a normal demo may have different time samples
	// each time it is played back
	if ( cl_timedemo->integer ) {
		if (!clc.timeDemoStart) {
			clc.timeDemoStart = Sys_Milliseconds();
		}
		clc.timeDemoFrames++;
		cl.serverTime = clc.timeDemoBaseTime + clc.timeDemoFrames * 50;
	}

	while ( cl.serverTime >= cl.snap.serverTime ) {
		// feed another messag, which should change
		// the contents of cl.snap
		CL_ReadDemoMessage();
		if ( cls.state != CA_ACTIVE ) {
			return;		// end of demo
		}
	}
}
