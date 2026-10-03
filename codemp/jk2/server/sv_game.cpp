/*
===========================================================================
Copyright (C) 1999 - 2005, Id Software, Inc.
Copyright (C) 2000 - 2013, Raven Software, Inc.
Copyright (C) 2001 - 2013, Activision, Inc.
Copyright (C) 2013 - 2015, OpenJK contributors
Copyright (C) JK2MV contributors

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

// sv_game.cpp -- interface to the game module of Jedi Outcast multiplayer (JK2_MODE)
//
// The retail game logic is bytecode (vm/jk2mpgame.qvm) that runs in the interpreter (jk2/qcommon/vm.cpp). This
// file is what the Jedi Academy build has in server/sv_game.cpp and server/sv_gameapi.cpp: the system calls of the
// module (the numbering of Jedi Outcast 1.04, jk2/game/g_public.h) and the calls the server makes into it.
// Derived from the server of JK2MV (GPLv2).

#include "server/server.h"
#include "botlib/botlib.h"
#include "qcommon/RoffSystem.h"
#include "ghoul2/ghoul2_shared.h"
#include "qcommon/cm_public.h"
#include "qcommon/MiniHeap.h"
#include "jk2/qcommon/strip.h"
#include "jk2/qcommon/vm_local.h"
#include "jk2/qcommon/g2_vmhandles.h"
#include "server/sv_gameapi.h"

botlib_export_t	*botlib_export;

extern vm_t *currentVM;

static vm_t		*gvm = NULL;	// game vm

// the sizes of the structures of the bot library that the module passes by pointer (botlib/be_interface.cpp)
extern const size_t aas_clientmove_size, aas_entityinfo_size, aas_areainfo_size, aas_altroutegoal_size,
	aas_predictroute_size, bot_consolemessage_size, bot_match_size, bot_goal_size, bot_moveresult_size,
	bot_initmove_size, weaponinfo_size;

int SV_BotLibSetup( void );
int SV_BotLibShutdown( void );

#ifndef MAX_ITEMS
#define MAX_ITEMS	256
#endif

// these functions must be used instead of pointer arithmetic, because
// the game allocates gentities with private information after the server shared part
int	SV_NumForGentity( sharedEntity_t *ent ) {
	int		num;

	num = ( (byte *)ent - (byte *)sv.gentities ) / sv.gentitySize;

	return num;
}

sharedEntity_t *SV_GentityNum( int num ) {
	sharedEntity_t *ent;

	if ( (unsigned)num >= (unsigned)sv.num_entities ) {
		Com_Error( ERR_DROP, "SV_GentityNum: bad num" );
	}

	ent = (sharedEntity_t *)((byte *)sv.gentities + sv.gentitySize*(num));

	return ent;
}

playerState_t *SV_GameClientNum( int num ) {
	playerState_t	*ps;

	ps = (playerState_t *)((byte *)sv.gameClients + sv.gameClientSize*(num));

	return ps;
}

svEntity_t	*SV_SvEntityForGentity( sharedEntity_t *gEnt ) {
	if ( !gEnt || gEnt->s.number < 0 || gEnt->s.number >= MAX_GENTITIES ) {
		Com_Error( ERR_DROP, "SV_SvEntityForGentity: bad gEnt" );
	}
	return &sv.svEntities[ gEnt->s.number ];
}

sharedEntity_t *SV_GEntityForSvEntity( svEntity_t *svEnt ) {
	int		num;

	num = svEnt - sv.svEntities;
	return SV_GentityNum( num );
}

/*
===============
SV_GameSendServerCommand

Sends a command string to a client
===============
*/
static void SV_GameSendServerCommand( int clientNum, const char *text ) {
	if ( clientNum == -1 ) {
		SV_SendServerCommand( NULL, "%s", text );
	} else {
		if ( clientNum < 0 || clientNum >= sv_maxclients->integer ) {
			return;
		}
		SV_SendServerCommand( svs.clients + clientNum, "%s", text );
	}
}

/*
===============
SV_GameDropClient

Disconnects the client with a message
===============
*/
static void SV_GameDropClient( int clientNum, const char *reason ) {
	if ( clientNum < 0 || clientNum >= sv_maxclients->integer ) {
		return;
	}
	SV_DropClient( svs.clients + clientNum, reason );
}

/*
=================
SV_SetBrushModel

sets mins and maxs for inline bmodels
=================
*/
static void SV_SetBrushModel( sharedEntity_t *ent, const char *name ) {
	clipHandle_t	h;
	vec3_t			mins, maxs;

	if (!name) {
		Com_Error( ERR_DROP, "SV_SetBrushModel: NULL" );
	}

	if (name[0] != '*') {
		Com_Error( ERR_DROP, "SV_SetBrushModel: %s isn't a brush model", name );
	}

	ent->s.modelindex = atoi( name + 1 );

	h = CM_InlineModel( ent->s.modelindex );
	CM_ModelBounds( h, mins, maxs );
	VectorCopy (mins, ent->r.mins);
	VectorCopy (maxs, ent->r.maxs);
	ent->r.bmodel = qtrue;

	ent->r.contents = -1;		// we don't know exactly what is in the brushes

	SV_LinkEntity( ent );		// FIXME: remove
}

/*
=================
SV_inPVS

Also checks portalareas so that doors block sight
=================
*/
qboolean SV_inPVS (const vec3_t p1, const vec3_t p2)
{
	int		leafnum;
	int		cluster;
	int		area1, area2;
	byte	*mask;

	leafnum = CM_PointLeafnum (p1);
	cluster = CM_LeafCluster (leafnum);
	area1 = CM_LeafArea (leafnum);
	mask = CM_ClusterPVS (cluster);

	leafnum = CM_PointLeafnum (p2);
	cluster = CM_LeafCluster (leafnum);
	area2 = CM_LeafArea (leafnum);
	if ( mask && (!(mask[cluster>>3] & (1<<(cluster&7)) ) ) )
		return qfalse;
	if (!CM_AreasConnected (area1, area2))
		return qfalse;		// a door blocks sight
	return qtrue;
}

/*
=================
SV_inPVSIgnorePortals

Does NOT check portalareas
=================
*/
static qboolean SV_inPVSIgnorePortals( const vec3_t p1, const vec3_t p2 )
{
	int		leafnum;
	int		cluster;
	byte	*mask;

	leafnum = CM_PointLeafnum (p1);
	cluster = CM_LeafCluster (leafnum);
	mask = CM_ClusterPVS (cluster);

	leafnum = CM_PointLeafnum (p2);
	cluster = CM_LeafCluster (leafnum);

	if ( mask && (!(mask[cluster>>3] & (1<<(cluster&7)) ) ) )
		return qfalse;

	return qtrue;
}

/*
========================
SV_AdjustAreaPortalState
========================
*/
static void SV_AdjustAreaPortalState( sharedEntity_t *ent, qboolean open ) {
	svEntity_t	*svEnt;

	svEnt = SV_SvEntityForGentity( ent );
	if ( svEnt->areanum2 == -1 ) {
		return;
	}
	CM_AdjustAreaPortalState( svEnt->areanum, svEnt->areanum2, open );
}

/*
==================
SV_EntityContact
==================
*/
static qboolean SV_EntityContact( const vec3_t mins, const vec3_t maxs, const sharedEntity_t *gEnt, int capsule ) {
	const float	*origin, *angles;
	clipHandle_t	ch;
	trace_t			trace;

	// check for exact collision
	origin = gEnt->r.currentOrigin;
	angles = gEnt->r.currentAngles;

	ch = SV_ClipHandleForEntity( gEnt );
	CM_TransformedBoxTrace ( &trace, vec3_origin, vec3_origin, mins, maxs,
		ch, -1, origin, angles, capsule );

	return (qboolean)trace.startsolid;
}

/*
===============
SV_GetServerinfo
===============
*/
static void SV_GetServerinfo( char *buffer, int bufferSize ) {
	if ( bufferSize < 1 ) {
		Com_Error( ERR_DROP, "SV_GetServerinfo: bufferSize == %i", bufferSize );
	}
	Q_strncpyz( buffer, Cvar_InfoString( CVAR_SERVERINFO ), bufferSize );
}

/*
===============
SV_LocateGameData
===============
*/
static void SV_LocateGameData( sharedEntity_t *gEnts, int numGEntities, int sizeofGEntity_t,
					   playerState_t *clients, int sizeofGameClient ) {
	if ( gEnts && ( sizeofGEntity_t < (int)sizeof(sharedEntity_t) || numGEntities < 0 ) ) {
		Com_Error( ERR_DROP, "SV_LocateGameData: incorrect game entity data" );
	}

	if ( clients && sizeofGameClient < (int)sizeof(playerState_t) ) {
		Com_Error( ERR_DROP, "SV_LocateGameData: incorrect player state data" );
	}

	sv.gentities = gEnts;
	sv.gentitySize = sizeofGEntity_t;
	sv.num_entities = numGEntities;

	sv.gameClients = clients;
	sv.gameClientSize = sizeofGameClient;
}

/*
===============
SV_GetUsercmd
===============
*/
static void SV_GetUsercmd( int clientNum, usercmd_t *cmd ) {
	if ( clientNum < 0 || clientNum >= sv_maxclients->integer ) {
		Com_Error( ERR_DROP, "SV_GetUsercmd: bad clientNum:%i", clientNum );
	}
	*cmd = svs.clients[clientNum].lastUsercmd;
}

/*
===============
SV_BotUserCommand

The bots (the module) hand their commands to the server like a client would
===============
*/
static void SV_BotUserCommand( int clientNum, usercmd_t *ucmd ) {
	if ( clientNum < 0 || clientNum >= sv_maxclients->integer ) {
		Com_Error( ERR_DROP, "SV_BotUserCommand: bad clientNum:%i", clientNum );
	}
	SV_ClientThink( &svs.clients[clientNum], ucmd );
}

/*
===============
SV_GetEntityToken

The entity tokens of the map, for the module at GAME_INIT
===============
*/
static qboolean SV_GetEntityToken( char *buffer, int bufferSize ) {
	const char	*s;

	s = COM_Parse( (const char **)&sv.entityParsePoint );
	Q_strncpyz( buffer, s, bufferSize );
	if ( !sv.entityParsePoint && !s[0] ) {
		return qfalse;
	}
	return qtrue;
}

//==============================================

/*
==============================================================================

GHOUL2

The module holds a Ghoul2 instance as an int (a handle), where the renderer holds a pointer to a CGhoul2Info_v:
the translation is jk2/qcommon/g2_vmhandles.cpp, shared with the cgame module. The renderer is the one of Jedi
Academy, so the calls are made on it.

==============================================================================
*/

// the model list that the module can pass (an array of model handles indexed by the model index of the instance,
// as many as the models there are): the renderer reads only what the instance needs, so what is checked here is
// that the list that fits into the memory of the module (all the models at most) can be addressed
static qhandle_t *SV_G2ModelList( intptr_t *args, int argNum ) {
	int	count;

	if ( !args[argNum] ) {
		return NULL;
	}
	if ( currentVM->entryPoint ) {
		return (qhandle_t *)args[argNum];
	}

	count = ( currentVM->dataMask - ( args[argNum] & currentVM->dataMask ) + 1 ) / sizeof( qhandle_t );
	if ( count > MAX_MODELS ) {
		count = MAX_MODELS;
	}

	return (qhandle_t *)VM_ArgArray( args[0], args[argNum], sizeof( qhandle_t ), count );
}

static intptr_t SV_G2SystemCalls( intptr_t *args ) {
	switch ( args[0] ) {
	case G_G2_LISTBONES:
		G2VM_ListBones( args[1], args[2], args[3] );
		return 0;

	case G_G2_LISTSURFACES:
		G2VM_ListSurfaces( args[1], args[2] );
		return 0;

	case G_G2_HAVEWEGHOULMODELS:
		return G2VM_HaveWeGhoul2Models( args[1] );

	case G_G2_SETMODELS:
		// TODO: the game of Jedi Outcast 1.04 does not use it (JK2MV ignores it as well); the length of the two
		// lists is not known here
		return 0;

	case G_G2_GETBOLT:
		return G2VM_GetBoltMatrix( args[1], args[2], args[3], VMAV(4, mdxaBone_t), VMAP(5, vec_t, 3), VMAP(6, vec_t, 3),
			args[7], SV_G2ModelList( args, 8 ), VMAP(9, vec_t, 3), qtrue, qfalse );

	case G_G2_GETBOLT_NOREC:
		return G2VM_GetBoltMatrix( args[1], args[2], args[3], VMAV(4, mdxaBone_t), VMAP(5, vec_t, 3), VMAP(6, vec_t, 3),
			args[7], SV_G2ModelList( args, 8 ), VMAP(9, vec_t, 3), qfalse, qfalse );

	case G_G2_GETBOLT_NOREC_NOROT:
		return G2VM_GetBoltMatrix( args[1], args[2], args[3], VMAV(4, mdxaBone_t), VMAP(5, vec_t, 3), VMAP(6, vec_t, 3),
			args[7], SV_G2ModelList( args, 8 ), VMAP(9, vec_t, 3), qfalse, qtrue );

	case G_G2_INITGHOUL2MODEL:
		return G2VM_InitGhoul2Model( VMAV(1, g2handle_t), VMAS(2), args[3], (qhandle_t)args[4], (qhandle_t)args[5], args[6], args[7] );

	case G_G2_ADDBOLT:
		return G2VM_AddBolt( args[1], args[2], VMAS(3) );

	case G_G2_SETBOLTINFO:
		G2VM_SetBoltInfo( args[1], args[2], args[3] );
		return 0;

	case G_G2_ANGLEOVERRIDE:
		return G2VM_SetBoneAngles( args[1], args[2], VMAS(3), VMAP(4, vec_t, 3), args[5], args[6], args[7], args[8],
			SV_G2ModelList( args, 9 ), args[10], args[11] );

	case G_G2_PLAYANIM:
		return G2VM_SetBoneAnim( args[1], args[2], VMAS(3), args[4], args[5], args[6], VMF(7), args[8], VMF(9), args[10] );

	case G_G2_GETGLANAME:
		// the module passes a buffer to be filled (a pointer cannot be returned to a VM)
		{
			char *name = G2VM_GetGLAName( args[1], args[2] );

			if ( name ) {
				Q_strncpyz( VMAP(3, char, strlen( name ) + 1), name, strlen( name ) + 1 );
			} else {
				VMAP(3, char, 1)[0] = '\0';
			}
		}
		return 0;

	case G_G2_COPYGHOUL2INSTANCE:
		return G2VM_CopyGhoul2Instance( args[1], args[2], args[3] );

	case G_G2_COPYSPECIFICGHOUL2MODEL:
		G2VM_CopySpecificG2Model( args[1], args[2], args[3], args[4] );
		return 0;

	case G_G2_DUPLICATEGHOUL2INSTANCE:
		G2VM_DuplicateGhoul2Instance( args[1], VMAV(2, g2handle_t) );
		return 0;

	case G_G2_HASGHOUL2MODELONINDEX:
		return G2VM_HasGhoul2ModelOnIndex( VMAV(1, const g2handle_t), args[2] );

	case G_G2_REMOVEGHOUL2MODEL:
		return G2VM_RemoveGhoul2Model( VMAV(1, g2handle_t), args[2] );

	case G_G2_CLEANMODELS:
		G2VM_CleanGhoul2Models( VMAV(1, g2handle_t) );
		return 0;

	case G_G2_COLLISIONDETECT:
		G2VM_CollisionDetect( VMAA(1, CollisionRecord_t, MAX_G2_COLLISIONS), args[2], VMAP(3, vec_t, 3), VMAP(4, vec_t, 3),
			args[5], args[6], VMAP(7, vec_t, 3), VMAP(8, vec_t, 3), VMAP(9, vec_t, 3), G2VertSpaceServer, args[10],
			args[11], VMF(12) );
		return 0;
	}

	return -1;
}

//==============================================

/*
====================
SV_GameSystemCalls

The module is making a system call
====================
*/
static intptr_t SV_GameSystemCalls( intptr_t *args ) {
	switch( args[0] ) {
	case G_PRINT:
		Com_Printf( "%s", VMAS(1) );
		return 0;
	case G_ERROR:
		Com_Error( ERR_DROP, "%s", VMAS(1) );
		return 0;
	case G_MILLISECONDS:
		return Sys_Milliseconds();
	case G_CVAR_REGISTER:
		Cvar_Register( VMAV(1, vmCvar_t), VMAS(2), VMAS(3), args[4] );
		return 0;
	case G_CVAR_UPDATE:
		Cvar_Update( VMAV(1, vmCvar_t) );
		return 0;
	case G_CVAR_SET:
		Cvar_VM_Set( VMAS(1), VMAS(2), VM_GAME );
		return 0;
	case G_CVAR_VARIABLE_INTEGER_VALUE:
		return Cvar_VariableIntegerValue( VMAS(1) );
	case G_CVAR_VARIABLE_STRING_BUFFER:
		Cvar_VariableStringBuffer( VMAS(1), VMAP(2, char, args[3]), args[3] );
		return 0;
	case G_ARGC:
		return Cmd_Argc();
	case G_ARGV:
		Cmd_ArgvBuffer( args[1], VMAP(2, char, args[3]), args[3] );
		return 0;
	case G_SEND_CONSOLE_COMMAND:
		Cbuf_ExecuteText( args[1], VMAS(2) );
		return 0;

	case G_FS_FOPEN_FILE:
		return FS_FOpenFileByMode( VMAS(1), VMAV(2, int), (fsMode_t)args[3] );
	case G_FS_READ:
		FS_Read( VMAP(1, char, args[2]), args[2], args[3] );
		return 0;
	case G_FS_WRITE:
		FS_Write( VMAP(1, char, args[2]), args[2], args[3] );
		return 0;
	case G_FS_FCLOSE_FILE:
		FS_FCloseFile( args[1] );
		return 0;
	case G_FS_GETFILELIST:
		return FS_GetFileList( VMAS(1), VMAS(2), VMAP(3, char, args[4]), args[4] );

	case G_LOCATE_GAME_DATA:
		SV_LocateGameData( (sharedEntity_t *)VM_ArgArray(args[0], args[1], args[3], args[2]), args[2], args[3],
			(playerState_t *)VM_ArgArray(args[0], args[4], args[5], MAX_CLIENTS), args[5] );
		VM_LocateGameDataCheck( sv.gentities, sv.gentitySize, sv.num_entities );
		return 0;
	case G_DROP_CLIENT:
		SV_GameDropClient( args[1], VMAS(2) );
		return 0;
	case G_SEND_SERVER_COMMAND:
		SV_GameSendServerCommand( args[1], VMAS(2) );
		return 0;
	case G_LINKENTITY:
		SV_LinkEntity( VMAIV(1, sharedEntity_t, sv.gentitySize > (int)sizeof(sharedEntity_t) ? sv.gentitySize : (int)sizeof(sharedEntity_t)) );
		return 0;
	case G_UNLINKENTITY:
		SV_UnlinkEntity( VMAV(1, sharedEntity_t) );
		return 0;
	case G_ENTITIES_IN_BOX:
		return SV_AreaEntities( VMAP(1, const vec_t, 3), VMAP(2, const vec_t, 3), VMAA(3, int, args[4]), args[4] );
	case G_ENTITY_CONTACT:
		return SV_EntityContact( VMAP(1, const vec_t, 3), VMAP(2, const vec_t, 3), VMAV(3, const sharedEntity_t), qfalse );
	case G_ENTITY_CONTACTCAPSULE:
		return SV_EntityContact( VMAP(1, const vec_t, 3), VMAP(2, const vec_t, 3), VMAV(3, const sharedEntity_t), qtrue );
	case G_TRACE:
		SV_Trace( VMAV(1, trace_t), VMAP(2, const vec_t, 3), VMAP(3, const vec_t, 3), VMAP(4, const vec_t, 3), VMAP(5, const vec_t, 3), args[6], args[7], qfalse, args[8], args[9] );
		return 0;
	case G_TRACECAPSULE:
		SV_Trace( VMAV(1, trace_t), VMAP(2, const vec_t, 3), VMAP(3, const vec_t, 3), VMAP(4, const vec_t, 3), VMAP(5, const vec_t, 3), args[6], args[7], qtrue, args[8], args[9] );
		return 0;
	case G_POINT_CONTENTS:
		return SV_PointContents( VMAP(1, const vec_t, 3), args[2] );
	case G_SET_BRUSH_MODEL:
		SV_SetBrushModel( VMAV(1, sharedEntity_t), VMAS(2) );
		return 0;
	case G_IN_PVS:
		return SV_inPVS( VMAP(1, const vec_t, 3), VMAP(2, const vec_t, 3) );
	case G_IN_PVS_IGNORE_PORTALS:
		return SV_inPVSIgnorePortals( VMAP(1, const vec_t, 3), VMAP(2, const vec_t, 3) );
	case G_SET_CONFIGSTRING:
		SV_SetConfigstring( args[1], VMAS(2) );
		return 0;
	case G_GET_CONFIGSTRING:
		SV_GetConfigstring( args[1], VMAP(2, char, args[3]), args[3] );
		return 0;
	case G_SET_USERINFO:
		SV_SetUserinfo( args[1], VMAS(2) );
		return 0;
	case G_GET_USERINFO:
		SV_GetUserinfo( args[1], VMAP(2, char, args[3]), args[3] );
		return 0;
	case G_GET_SERVERINFO:
		SV_GetServerinfo( VMAP(1, char, args[2]), args[2] );
		return 0;
	case G_ADJUST_AREA_PORTAL_STATE:
		SV_AdjustAreaPortalState( VMAV(1, sharedEntity_t), (qboolean)!!args[2] );
		return 0;
	case G_AREAS_CONNECTED:
		return CM_AreasConnected( args[1], args[2] );

	case G_BOT_ALLOCATE_CLIENT:
		return SV_BotAllocateClient();
	case G_BOT_FREE_CLIENT:
		SV_BotFreeClient( args[1] );
		return 0;

	case G_GET_USERCMD:
		SV_GetUsercmd( args[1], VMAV(2, usercmd_t) );
		return 0;
	case G_GET_ENTITY_TOKEN:
		return SV_GetEntityToken( VMAP(1, char, args[2]), args[2] );

	case G_DEBUG_POLYGON_CREATE:
		return BotImport_DebugPolygonCreate( args[1], args[2], VMAA(3, vec3_t, args[2]) );
	case G_DEBUG_POLYGON_DELETE:
		BotImport_DebugPolygonDelete( args[1] );
		return 0;
	case G_REAL_TIME:
		return Com_RealTime( VMAV(1, qtime_t) );
	case G_SNAPVECTOR:
		Sys_SnapVector( VMAP(1, vec_t, 3) );
		return 0;

	// the string packages (strip/*.sp): the module asks for the ones it needs, the server sends their names
	// to the clients (CS_STRING_PACKAGES)
	case SP_REGISTER_SERVER_CMD:
		return SP_RegisterServer( VMAS(1) );
	case SP_GETSTRINGTEXTSTRING:
		return SP_VMGetStringText( VMAS(1), VMAP(2, char, args[3]), args[3] );

	case G_ROFF_CLEAN:
		return theROFFSystem.Clean( qfalse );
	case G_ROFF_UPDATE_ENTITIES:
		theROFFSystem.UpdateEntities( qfalse );
		return 0;
	case G_ROFF_CACHE:
		return theROFFSystem.Cache( VMAS(1), qfalse );
	case G_ROFF_PLAY:
		return theROFFSystem.Play( args[1], args[2], (qboolean)!!args[3], qfalse );
	case G_ROFF_PURGE_ENT:
		return theROFFSystem.PurgeEnt( args[1], qfalse );

	//====================================

	case BOTLIB_SETUP:
		return SV_BotLibSetup();
	case BOTLIB_SHUTDOWN:
		return SV_BotLibShutdown();
	case BOTLIB_LIBVAR_SET:
		return botlib_export->BotLibVarSet( VMAS(1), VMAS(2) );
	case BOTLIB_LIBVAR_GET:
		return botlib_export->BotLibVarGet( VMAS(1), VMAP(2, char, args[3]), args[3] );

	case BOTLIB_PC_ADD_GLOBAL_DEFINE:
		return botlib_export->PC_AddGlobalDefine( VMAS(1) );
	case BOTLIB_PC_LOAD_SOURCE:
		return botlib_export->PC_LoadSourceHandle( VMAS(1) );
	case BOTLIB_PC_FREE_SOURCE:
		return botlib_export->PC_FreeSourceHandle( args[1] );
	case BOTLIB_PC_READ_TOKEN:
		return botlib_export->PC_ReadTokenHandle( args[1], VMAV(2, pc_token_t) );
	case BOTLIB_PC_SOURCE_FILE_AND_LINE:
		return botlib_export->PC_SourceFileAndLine( args[1], VMAP(2, char, MAX_QPATH), VMAV(3, int) );

	case BOTLIB_START_FRAME:
		return botlib_export->BotLibStartFrame( VMF(1) );
	case BOTLIB_LOAD_MAP:
		return botlib_export->BotLibLoadMap( VMAS(1) );
	case BOTLIB_UPDATENTITY:
		return botlib_export->BotLibUpdateEntity( args[1], VMAV(2, bot_entitystate_t) );
	case BOTLIB_TEST:
		return botlib_export->Test( args[1], NULL, VMAP(3, vec_t, 3), VMAP(4, vec_t, 3) );

	case BOTLIB_GET_SNAPSHOT_ENTITY:
		return SV_BotGetSnapshotEntity( args[1], args[2] );
	case BOTLIB_GET_CONSOLE_MESSAGE:
		return SV_BotGetConsoleMessage( args[1], VMAP(2, char, args[3]), args[3] );
	case BOTLIB_USER_COMMAND:
		SV_BotUserCommand( args[1], VMAV(2, usercmd_t) );
		return 0;

	case BOTLIB_AAS_BBOX_AREAS:
		return botlib_export->aas.AAS_BBoxAreas( VMAP(1, vec_t, 3), VMAP(2, vec_t, 3), VMAA(3, int, args[4]), args[4] );
	case BOTLIB_AAS_AREA_INFO:
		return botlib_export->aas.AAS_AreaInfo( args[1], VMAIV(2, struct aas_areainfo_s, aas_areainfo_size) );
	case BOTLIB_AAS_ALTERNATIVE_ROUTE_GOAL:
		return botlib_export->aas.AAS_AlternativeRouteGoals( VMAP(1, vec_t, 3), args[2], VMAP(3, vec_t, 3), args[4], args[5], (struct aas_altroutegoal_s *)VM_ArgArray(args[0], args[6], aas_altroutegoal_size, args[7]), args[7], args[8] );
	case BOTLIB_AAS_ENTITY_INFO:
		botlib_export->aas.AAS_EntityInfo( args[1], VMAIV(2, struct aas_entityinfo_s, aas_entityinfo_size) );
		return 0;

	case BOTLIB_AAS_INITIALIZED:
		return botlib_export->aas.AAS_Initialized();
	case BOTLIB_AAS_PRESENCE_TYPE_BOUNDING_BOX:
		botlib_export->aas.AAS_PresenceTypeBoundingBox( args[1], VMAP(2, vec_t, 3), VMAP(3, vec_t, 3) );
		return 0;
	case BOTLIB_AAS_TIME:
		return FloatAsInt( botlib_export->aas.AAS_Time() );

	case BOTLIB_AAS_POINT_AREA_NUM:
		return botlib_export->aas.AAS_PointAreaNum( VMAP(1, vec_t, 3) );
	case BOTLIB_AAS_POINT_REACHABILITY_AREA_INDEX:
		return botlib_export->aas.AAS_PointReachabilityAreaIndex( VMAP(1, vec_t, 3) );
	case BOTLIB_AAS_TRACE_AREAS:
		return botlib_export->aas.AAS_TraceAreas( VMAP(1, vec_t, 3), VMAP(2, vec_t, 3), VMAA(3, int, args[5]), VMAA(4, vec3_t, args[5]), args[5] );

	case BOTLIB_AAS_POINT_CONTENTS:
		return botlib_export->aas.AAS_PointContents( VMAP(1, vec_t, 3) );
	case BOTLIB_AAS_NEXT_BSP_ENTITY:
		return botlib_export->aas.AAS_NextBSPEntity( args[1] );
	case BOTLIB_AAS_VALUE_FOR_BSP_EPAIR_KEY:
		return botlib_export->aas.AAS_ValueForBSPEpairKey( args[1], VMAS(2), VMAP(3, char, args[4]), args[4] );
	case BOTLIB_AAS_VECTOR_FOR_BSP_EPAIR_KEY:
		return botlib_export->aas.AAS_VectorForBSPEpairKey( args[1], VMAS(2), VMAP(3, vec_t, 3) );
	case BOTLIB_AAS_FLOAT_FOR_BSP_EPAIR_KEY:
		return botlib_export->aas.AAS_FloatForBSPEpairKey( args[1], VMAS(2), VMAV(3, float) );
	case BOTLIB_AAS_INT_FOR_BSP_EPAIR_KEY:
		return botlib_export->aas.AAS_IntForBSPEpairKey( args[1], VMAS(2), VMAV(3, int) );

	case BOTLIB_AAS_AREA_REACHABILITY:
		return botlib_export->aas.AAS_AreaReachability( args[1] );

	case BOTLIB_AAS_AREA_TRAVEL_TIME_TO_GOAL_AREA:
		return botlib_export->aas.AAS_AreaTravelTimeToGoalArea( args[1], VMAP(2, vec_t, 3), args[3], args[4] );
	case BOTLIB_AAS_ENABLE_ROUTING_AREA:
		return botlib_export->aas.AAS_EnableRoutingArea( args[1], args[2] );
	case BOTLIB_AAS_PREDICT_ROUTE:
		return botlib_export->aas.AAS_PredictRoute( VMAIV(1, struct aas_predictroute_s, aas_predictroute_size), args[2], VMAP(3, vec_t, 3), args[4], args[5], args[6], args[7], args[8], args[9], args[10], args[11] );

	case BOTLIB_AAS_SWIMMING:
		return botlib_export->aas.AAS_Swimming( VMAP(1, vec_t, 3) );
	case BOTLIB_AAS_PREDICT_CLIENT_MOVEMENT:
		return botlib_export->aas.AAS_PredictClientMovement( VMAIV(1, struct aas_clientmove_s, aas_clientmove_size), args[2], VMAP(3, vec_t, 3), args[4], args[5],
			VMAP(6, vec_t, 3), VMAP(7, vec_t, 3), args[8], args[9], VMF(10), args[11], args[12], args[13] );

	case BOTLIB_EA_SAY:
		botlib_export->ea.EA_Say( args[1], VMAS(2) );
		return 0;
	case BOTLIB_EA_SAY_TEAM:
		botlib_export->ea.EA_SayTeam( args[1], VMAS(2) );
		return 0;
	case BOTLIB_EA_COMMAND:
		botlib_export->ea.EA_Command( args[1], VMAS(2) );
		return 0;

	case BOTLIB_EA_ACTION:
		botlib_export->ea.EA_Action( args[1], args[2] );
		return 0;
	case BOTLIB_EA_GESTURE:
		botlib_export->ea.EA_Gesture( args[1] );
		return 0;
	case BOTLIB_EA_TALK:
		botlib_export->ea.EA_Talk( args[1] );
		return 0;
	case BOTLIB_EA_ATTACK:
		botlib_export->ea.EA_Attack( args[1] );
		return 0;
	case BOTLIB_EA_ALT_ATTACK:
		botlib_export->ea.EA_Alt_Attack( args[1] );
		return 0;
	case BOTLIB_EA_FORCEPOWER:
		botlib_export->ea.EA_ForcePower( args[1] );
		return 0;
	case BOTLIB_EA_USE:
		botlib_export->ea.EA_Use( args[1] );
		return 0;
	case BOTLIB_EA_RESPAWN:
		botlib_export->ea.EA_Respawn( args[1] );
		return 0;
	case BOTLIB_EA_CROUCH:
		botlib_export->ea.EA_Crouch( args[1] );
		return 0;
	case BOTLIB_EA_MOVE_UP:
		botlib_export->ea.EA_MoveUp( args[1] );
		return 0;
	case BOTLIB_EA_MOVE_DOWN:
		botlib_export->ea.EA_MoveDown( args[1] );
		return 0;
	case BOTLIB_EA_MOVE_FORWARD:
		botlib_export->ea.EA_MoveForward( args[1] );
		return 0;
	case BOTLIB_EA_MOVE_BACK:
		botlib_export->ea.EA_MoveBack( args[1] );
		return 0;
	case BOTLIB_EA_MOVE_LEFT:
		botlib_export->ea.EA_MoveLeft( args[1] );
		return 0;
	case BOTLIB_EA_MOVE_RIGHT:
		botlib_export->ea.EA_MoveRight( args[1] );
		return 0;

	case BOTLIB_EA_SELECT_WEAPON:
		botlib_export->ea.EA_SelectWeapon( args[1], args[2] );
		return 0;
	case BOTLIB_EA_JUMP:
		botlib_export->ea.EA_Jump( args[1] );
		return 0;
	case BOTLIB_EA_DELAYED_JUMP:
		botlib_export->ea.EA_DelayedJump( args[1] );
		return 0;
	case BOTLIB_EA_MOVE:
		botlib_export->ea.EA_Move( args[1], VMAP(2, vec_t, 3), VMF(3) );
		return 0;
	case BOTLIB_EA_VIEW:
		botlib_export->ea.EA_View( args[1], VMAP(2, vec_t, 3) );
		return 0;

	case BOTLIB_EA_END_REGULAR:
		botlib_export->ea.EA_EndRegular( args[1], VMF(2) );
		return 0;
	case BOTLIB_EA_GET_INPUT:
		botlib_export->ea.EA_GetInput( args[1], VMF(2), VMAV(3, bot_input_t) );
		return 0;
	case BOTLIB_EA_RESET_INPUT:
		botlib_export->ea.EA_ResetInput( args[1] );
		return 0;

	case BOTLIB_AI_LOAD_CHARACTER:
		return botlib_export->ai.BotLoadCharacter( VMAS(1), VMF(2) );
	case BOTLIB_AI_FREE_CHARACTER:
		botlib_export->ai.BotFreeCharacter( args[1] );
		return 0;
	case BOTLIB_AI_CHARACTERISTIC_FLOAT:
		return FloatAsInt( botlib_export->ai.Characteristic_Float( args[1], args[2] ) );
	case BOTLIB_AI_CHARACTERISTIC_BFLOAT:
		return FloatAsInt( botlib_export->ai.Characteristic_BFloat( args[1], args[2], VMF(3), VMF(4) ) );
	case BOTLIB_AI_CHARACTERISTIC_INTEGER:
		return botlib_export->ai.Characteristic_Integer( args[1], args[2] );
	case BOTLIB_AI_CHARACTERISTIC_BINTEGER:
		return botlib_export->ai.Characteristic_BInteger( args[1], args[2], args[3], args[4] );
	case BOTLIB_AI_CHARACTERISTIC_STRING:
		botlib_export->ai.Characteristic_String( args[1], args[2], VMAP(3, char, args[4]), args[4] );
		return 0;

	case BOTLIB_AI_ALLOC_CHAT_STATE:
		return botlib_export->ai.BotAllocChatState();
	case BOTLIB_AI_FREE_CHAT_STATE:
		botlib_export->ai.BotFreeChatState( args[1] );
		return 0;
	case BOTLIB_AI_QUEUE_CONSOLE_MESSAGE:
		botlib_export->ai.BotQueueConsoleMessage( args[1], args[2], VMAS(3) );
		return 0;
	case BOTLIB_AI_REMOVE_CONSOLE_MESSAGE:
		botlib_export->ai.BotRemoveConsoleMessage( args[1], args[2] );
		return 0;
	case BOTLIB_AI_NEXT_CONSOLE_MESSAGE:
		return botlib_export->ai.BotNextConsoleMessage( args[1], VMAIV(2, struct bot_consolemessage_s, bot_consolemessage_size) );
	case BOTLIB_AI_NUM_CONSOLE_MESSAGE:
		return botlib_export->ai.BotNumConsoleMessages( args[1] );
	case BOTLIB_AI_INITIAL_CHAT:
		botlib_export->ai.BotInitialChat( args[1], VMAS(2), args[3], VMAS(4), VMAS(5), VMAS(6), VMAS(7), VMAS(8), VMAS(9), VMAS(10), VMAS(11) );
		return 0;
	case BOTLIB_AI_NUM_INITIAL_CHATS:
		return botlib_export->ai.BotNumInitialChats( args[1], VMAS(2) );
	case BOTLIB_AI_REPLY_CHAT:
		return botlib_export->ai.BotReplyChat( args[1], VMAS(2), args[3], args[4], VMAS(5), VMAS(6), VMAS(7), VMAS(8), VMAS(9), VMAS(10), VMAS(11), VMAS(12) );
	case BOTLIB_AI_CHAT_LENGTH:
		return botlib_export->ai.BotChatLength( args[1] );
	case BOTLIB_AI_ENTER_CHAT:
		botlib_export->ai.BotEnterChat( args[1], args[2], args[3] );
		return 0;
	case BOTLIB_AI_GET_CHAT_MESSAGE:
		botlib_export->ai.BotGetChatMessage( args[1], VMAP(2, char, args[3]), args[3] );
		return 0;
	case BOTLIB_AI_STRING_CONTAINS:
		return botlib_export->ai.StringContains( VMAS(1), VMAS(2), args[3] );
	case BOTLIB_AI_FIND_MATCH:
		return botlib_export->ai.BotFindMatch( VMAS(1), VMAIV(2, struct bot_match_s, bot_match_size), args[3] );
	case BOTLIB_AI_MATCH_VARIABLE:
		botlib_export->ai.BotMatchVariable( VMAIV(1, struct bot_match_s, bot_match_size), args[2], VMAP(3, char, args[4]), args[4] );
		return 0;
	case BOTLIB_AI_UNIFY_WHITE_SPACES:
		botlib_export->ai.UnifyWhiteSpaces( VMAS(1) );
		return 0;
	case BOTLIB_AI_REPLACE_SYNONYMS:
		botlib_export->ai.BotReplaceSynonyms( VMAS(1), args[2] );
		return 0;
	case BOTLIB_AI_LOAD_CHAT_FILE:
		return botlib_export->ai.BotLoadChatFile( args[1], VMAS(2), VMAS(3) );
	case BOTLIB_AI_SET_CHAT_GENDER:
		botlib_export->ai.BotSetChatGender( args[1], args[2] );
		return 0;
	case BOTLIB_AI_SET_CHAT_NAME:
		botlib_export->ai.BotSetChatName( args[1], VMAS(2), args[3] );
		return 0;

	case BOTLIB_AI_RESET_GOAL_STATE:
		botlib_export->ai.BotResetGoalState( args[1] );
		return 0;
	case BOTLIB_AI_RESET_AVOID_GOALS:
		botlib_export->ai.BotResetAvoidGoals( args[1] );
		return 0;
	case BOTLIB_AI_REMOVE_FROM_AVOID_GOALS:
		botlib_export->ai.BotRemoveFromAvoidGoals( args[1], args[2] );
		return 0;
	case BOTLIB_AI_PUSH_GOAL:
		botlib_export->ai.BotPushGoal( args[1], VMAIV(2, struct bot_goal_s, bot_goal_size) );
		return 0;
	case BOTLIB_AI_POP_GOAL:
		botlib_export->ai.BotPopGoal( args[1] );
		return 0;
	case BOTLIB_AI_EMPTY_GOAL_STACK:
		botlib_export->ai.BotEmptyGoalStack( args[1] );
		return 0;
	case BOTLIB_AI_DUMP_AVOID_GOALS:
		botlib_export->ai.BotDumpAvoidGoals( args[1] );
		return 0;
	case BOTLIB_AI_DUMP_GOAL_STACK:
		botlib_export->ai.BotDumpGoalStack( args[1] );
		return 0;
	case BOTLIB_AI_GOAL_NAME:
		botlib_export->ai.BotGoalName( args[1], VMAP(2, char, args[3]), args[3] );
		return 0;
	case BOTLIB_AI_GET_TOP_GOAL:
		return botlib_export->ai.BotGetTopGoal( args[1], VMAIV(2, struct bot_goal_s, bot_goal_size) );
	case BOTLIB_AI_GET_SECOND_GOAL:
		return botlib_export->ai.BotGetSecondGoal( args[1], VMAIV(2, struct bot_goal_s, bot_goal_size) );
	case BOTLIB_AI_CHOOSE_LTG_ITEM:
		return botlib_export->ai.BotChooseLTGItem( args[1], VMAP(2, vec_t, 3), VMAA(3, int, MAX_ITEMS), args[4] );
	case BOTLIB_AI_CHOOSE_NBG_ITEM:
		return botlib_export->ai.BotChooseNBGItem( args[1], VMAP(2, vec_t, 3), VMAA(3, int, MAX_ITEMS), args[4], VMAIV(5, struct bot_goal_s, bot_goal_size), VMF(6) );
	case BOTLIB_AI_TOUCHING_GOAL:
		return botlib_export->ai.BotTouchingGoal( VMAP(1, vec_t, 3), VMAIV(2, struct bot_goal_s, bot_goal_size) );
	case BOTLIB_AI_ITEM_GOAL_IN_VIS_BUT_NOT_VISIBLE:
		return botlib_export->ai.BotItemGoalInVisButNotVisible( args[1], VMAP(2, vec_t, 3), VMAP(3, vec_t, 3), VMAIV(4, struct bot_goal_s, bot_goal_size) );
	case BOTLIB_AI_GET_LEVEL_ITEM_GOAL:
		return botlib_export->ai.BotGetLevelItemGoal( args[1], VMAS(2), VMAIV(3, struct bot_goal_s, bot_goal_size) );
	case BOTLIB_AI_GET_NEXT_CAMP_SPOT_GOAL:
		return botlib_export->ai.BotGetNextCampSpotGoal( args[1], VMAIV(2, struct bot_goal_s, bot_goal_size) );
	case BOTLIB_AI_GET_MAP_LOCATION_GOAL:
		return botlib_export->ai.BotGetMapLocationGoal( VMAS(1), VMAIV(2, struct bot_goal_s, bot_goal_size) );
	case BOTLIB_AI_AVOID_GOAL_TIME:
		return FloatAsInt( botlib_export->ai.BotAvoidGoalTime( args[1], args[2] ) );
	case BOTLIB_AI_SET_AVOID_GOAL_TIME:
		botlib_export->ai.BotSetAvoidGoalTime( args[1], args[2], VMF(3));
		return 0;
	case BOTLIB_AI_INIT_LEVEL_ITEMS:
		botlib_export->ai.BotInitLevelItems();
		return 0;
	case BOTLIB_AI_UPDATE_ENTITY_ITEMS:
		botlib_export->ai.BotUpdateEntityItems();
		return 0;
	case BOTLIB_AI_LOAD_ITEM_WEIGHTS:
		return botlib_export->ai.BotLoadItemWeights( args[1], VMAS(2) );
	case BOTLIB_AI_FREE_ITEM_WEIGHTS:
		botlib_export->ai.BotFreeItemWeights( args[1] );
		return 0;
	case BOTLIB_AI_INTERBREED_GOAL_FUZZY_LOGIC:
		botlib_export->ai.BotInterbreedGoalFuzzyLogic( args[1], args[2], args[3] );
		return 0;
	case BOTLIB_AI_SAVE_GOAL_FUZZY_LOGIC:
		botlib_export->ai.BotSaveGoalFuzzyLogic( args[1], VMAS(2) );
		return 0;
	case BOTLIB_AI_MUTATE_GOAL_FUZZY_LOGIC:
		botlib_export->ai.BotMutateGoalFuzzyLogic( args[1], VMF(2) );
		return 0;
	case BOTLIB_AI_ALLOC_GOAL_STATE:
		return botlib_export->ai.BotAllocGoalState( args[1] );
	case BOTLIB_AI_FREE_GOAL_STATE:
		botlib_export->ai.BotFreeGoalState( args[1] );
		return 0;

	case BOTLIB_AI_RESET_MOVE_STATE:
		botlib_export->ai.BotResetMoveState( args[1] );
		return 0;
	case BOTLIB_AI_ADD_AVOID_SPOT:
		botlib_export->ai.BotAddAvoidSpot( args[1], VMAP(2, vec_t, 3), VMF(3), args[4] );
		return 0;
	case BOTLIB_AI_MOVE_TO_GOAL:
		botlib_export->ai.BotMoveToGoal( VMAIV(1, struct bot_moveresult_s, bot_moveresult_size), args[2], VMAIV(3, struct bot_goal_s, bot_goal_size), args[4] );
		return 0;
	case BOTLIB_AI_MOVE_IN_DIRECTION:
		return botlib_export->ai.BotMoveInDirection( args[1], VMAP(2, vec_t, 3), VMF(3), args[4] );
	case BOTLIB_AI_RESET_AVOID_REACH:
		botlib_export->ai.BotResetAvoidReach( args[1] );
		return 0;
	case BOTLIB_AI_RESET_LAST_AVOID_REACH:
		botlib_export->ai.BotResetLastAvoidReach( args[1] );
		return 0;
	case BOTLIB_AI_REACHABILITY_AREA:
		return botlib_export->ai.BotReachabilityArea( VMAP(1, vec_t, 3), args[2] );
	case BOTLIB_AI_MOVEMENT_VIEW_TARGET:
		return botlib_export->ai.BotMovementViewTarget( args[1], VMAIV(2, struct bot_goal_s, bot_goal_size), args[3], VMF(4), VMAP(5, vec_t, 3) );
	case BOTLIB_AI_PREDICT_VISIBLE_POSITION:
		return botlib_export->ai.BotPredictVisiblePosition( VMAP(1, vec_t, 3), args[2], VMAIV(3, struct bot_goal_s, bot_goal_size), args[4], VMAP(5, vec_t, 3) );
	case BOTLIB_AI_ALLOC_MOVE_STATE:
		return botlib_export->ai.BotAllocMoveState();
	case BOTLIB_AI_FREE_MOVE_STATE:
		botlib_export->ai.BotFreeMoveState( args[1] );
		return 0;
	case BOTLIB_AI_INIT_MOVE_STATE:
		botlib_export->ai.BotInitMoveState( args[1], VMAIV(2, struct bot_initmove_s, bot_initmove_size) );
		return 0;

	case BOTLIB_AI_CHOOSE_BEST_FIGHT_WEAPON:
		return botlib_export->ai.BotChooseBestFightWeapon( args[1], VMAA(2, int, MAX_ITEMS) );
	case BOTLIB_AI_GET_WEAPON_INFO:
		botlib_export->ai.BotGetWeaponInfo( args[1], args[2], VMAIV(3, struct weaponinfo_s, weaponinfo_size) );
		return 0;
	case BOTLIB_AI_LOAD_WEAPON_WEIGHTS:
		return botlib_export->ai.BotLoadWeaponWeights( args[1], VMAS(2) );
	case BOTLIB_AI_ALLOC_WEAPON_STATE:
		return botlib_export->ai.BotAllocWeaponState();
	case BOTLIB_AI_FREE_WEAPON_STATE:
		botlib_export->ai.BotFreeWeaponState( args[1] );
		return 0;
	case BOTLIB_AI_RESET_WEAPON_STATE:
		botlib_export->ai.BotResetWeaponState( args[1] );
		return 0;

	case BOTLIB_AI_GENETIC_PARENTS_AND_CHILD_SELECTION:
		return botlib_export->ai.GeneticParentsAndChildSelection(args[1], VMAA(2, float, args[1]), VMAV(3, int), VMAV(4, int), VMAV(5, int));

	case TRAP_MEMSET:
		Com_Memset( VMAP(1, char, args[3]), args[2], args[3] );
		return 0;

	case TRAP_MEMCPY:
		Com_Memcpy( VMAP(1, char, args[3]), VMAP(2, char, args[3]), args[3] );
		return 0;

	case TRAP_STRNCPY:
		return VM_strncpy( args[1], args[2], args[3] );

	case TRAP_SIN:
		return FloatAsInt( sinf( VMF(1) ) );

	case TRAP_COS:
		return FloatAsInt( cosf( VMF(1) ) );

	case TRAP_ATAN2:
		return FloatAsInt( atan2f( VMF(1), VMF(2) ) );

	case TRAP_SQRT:
		return FloatAsInt( VMF(1) < 0 ? 0 : sqrtf( VMF(1) ) );

	case TRAP_MATRIXMULTIPLY:
		MatrixMultiply( VMAP(1, vec3_t, 3), VMAP(2, vec3_t, 3), VMAP(3, vec3_t, 3) );
		return 0;

	case TRAP_ANGLEVECTORS:
		AngleVectors( VMAP(1, vec_t, 3), VMAP(2, vec_t, 3), VMAP(3, vec_t, 3), VMAP(4, vec_t, 3) );
		return 0;

	case TRAP_PERPENDICULARVECTOR:
		PerpendicularVector( VMAP(1, vec_t, 3), VMAP(2, vec_t, 3) );
		return 0;

	case TRAP_FLOOR:
		return FloatAsInt( floorf( VMF(1) ) );

	case TRAP_CEIL:
		return FloatAsInt( ceilf( VMF(1) ) );

	case G_ACOS:
		return FloatAsInt( Q_acos( VMF(1) ) );

	case G_ASIN:
		return FloatAsInt( Q_asin( VMF(1) ) );

	case G_TESTPRINTINT:
	case G_TESTPRINTFLOAT:
		return 0;

	default:
		if ( args[0] >= G_G2_LISTBONES && args[0] <= G_G2_COLLISIONDETECT ) {
			return SV_G2SystemCalls( args );
		}
		break;
	}

	Com_Error( ERR_DROP, "Bad game system trap: %lli", (long long int)args[0] );
	return -1;
}

//==============================================

/*
===============
SV_InitGame

Called for both a full init and a restart
===============
*/
void SV_InitGame( qboolean restart ) {
	int		i;

	// clear physics interaction links (the entities are not linked in the cleared world: they could still point
	// into the world sectors of the level that is being restarted)
	SV_ClearWorld();
	for ( i = 0 ; i < MAX_GENTITIES ; i++ ) {
		sv.svEntities[i].worldSector = NULL;
		sv.svEntities[i].nextEntityInWorldSector = NULL;
	}

	// start the entity parsing at the beginning
	sv.entityParsePoint = (char *)CM_EntityString();

	// clear all gentity pointers that might still be set from a previous level
	for ( i = 0 ; i < sv_maxclients->integer ; i++ ) {
		svs.clients[i].gentity = NULL;
	}

	// use the current msec count for a random seed
	GVM_InitGame( sv.time, Com_Milliseconds(), restart );

	if ( !sv.gentities || !sv.gameClients ) {
		Com_Error( ERR_DROP, "SV_InitGame: failed to locate game data" );
	}
}

/*
===============
SV_BindGame

Loads the module (the QVM, or a native library with vm_game 0)
===============
*/
void SV_BindGame( void ) {
	gvm = VM_Create( "jk2mpgame", SV_GameSystemCalls, (vmInterpret_t)(int)Cvar_VariableValue( "vm_game" ) );
	if ( !gvm ) {
		svs.gameStarted = qfalse;
		Com_Error( ERR_FATAL, "VM_Create on game failed" );
	}
}

void SV_UnbindGame( void ) {
	GVM_ShutdownGame( qfalse );
	VM_Free( gvm );
	gvm = NULL;
}

/*
===================
SV_RestartGame

Called on a map_restart, but not on a normal map change
===================
*/
void SV_RestartGame( void ) {
	if ( !gvm ) {
		return;
	}
	GVM_ShutdownGame( qtrue );

	// do a restart instead of a free
	gvm = VM_Restart( gvm );
	if ( !gvm ) {
		svs.gameStarted = qfalse;
		Com_Error( ERR_FATAL, "VM_Restart on game failed" );
		return;
	}

	SV_InitGame( qtrue );
}

/*
===============
SV_ShutdownGameProgs

Called every time a map changes
===============
*/
void SV_ShutdownGameProgs( void ) {
	if ( !gvm ) {
		return;
	}
	SV_UnbindGame();
}

/*
===============
SV_InitGameProgs

Called on a normal map change, not on a map_restart
===============
*/
void SV_InitGameProgs( void ) {
	//FIXME these are temp while I make bots run in vm
	extern int	bot_enable;

	cvar_t *var = Cvar_Get( "bot_enable", "1", CVAR_LATCH );
	bot_enable = var ? var->integer : 0;

	svs.gameStarted = qtrue;
	SV_BindGame();

	SV_InitGame( qfalse );
}

/*
====================
SV_GameCommand

See if the current console command is claimed by the game
====================
*/
qboolean SV_GameCommand( void ) {
	if ( sv.state != SS_GAME ) {
		return qfalse;
	}

	return GVM_ConsoleCommand();
}

/*
==============================================================================

The calls into the module (GAME_*). Nothing here passes a pointer to the module, its memory is not the engine's.

==============================================================================
*/

void GVM_InitGame( int levelTime, int randomSeed, int restart ) {
	VM_Call( gvm, GAME_INIT, levelTime, randomSeed, restart );
}

void GVM_ShutdownGame( int restart ) {
	if ( !gvm ) {
		return;
	}
	VM_Call( gvm, GAME_SHUTDOWN, restart );
}

char *GVM_ClientConnect( int clientNum, qboolean firstTime, qboolean isBot ) {
	intptr_t	ret = VM_Call( gvm, GAME_CLIENT_CONNECT, clientNum, firstTime, isBot );

	// the denial is a string in the memory of the module, which can only be addressed inside a VM_Call
	return VM_ExplicitArgString( gvm, ret );
}

void GVM_ClientBegin( int clientNum ) {
	VM_Call( gvm, GAME_CLIENT_BEGIN, clientNum );
}

qboolean GVM_ClientUserinfoChanged( int clientNum ) {
	VM_Call( gvm, GAME_CLIENT_USERINFO_CHANGED, clientNum );
	return qtrue;
}

void GVM_ClientDisconnect( int clientNum ) {
	VM_Call( gvm, GAME_CLIENT_DISCONNECT, clientNum );
}

void GVM_ClientCommand( int clientNum ) {
	VM_Call( gvm, GAME_CLIENT_COMMAND, clientNum );
}

void GVM_ClientThink( int clientNum, usercmd_t *ucmd ) {
	// the module gets the command with G_GET_USERCMD
	VM_Call( gvm, GAME_CLIENT_THINK, clientNum );
}

void GVM_RunFrame( int levelTime ) {
	VM_Call( gvm, GAME_RUN_FRAME, levelTime );
}

qboolean GVM_ConsoleCommand( void ) {
	return (qboolean)!!VM_Call( gvm, GAME_CONSOLE_COMMAND );
}

int GVM_BotAIStartFrame( int time ) {
	if ( !gvm ) {
		return 0;
	}
	return (int)VM_Call( gvm, BOTAI_START_FRAME, time );
}

void GVM_ROFF_NotetrackCallback( int entID, const char *notetrack ) {
	// TODO: the notetrack is a string of the engine, the module can only read strings in its own memory (the
	// original engine passed the pointer as it is, which only works for a native library). Not called for the QVM.
	if ( gvm && gvm->entryPoint ) {
		VM_Call( gvm, GAME_ROFF_NOTETRACK_CALLBACK, entID, (intptr_t)notetrack );
	}
}
