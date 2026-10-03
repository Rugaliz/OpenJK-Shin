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

// cl_ui.cpp -- client system interaction with the user interface module, for Jedi Outcast (JK2_MODE)
//
// The ui module of Jedi Outcast is a QVM file of the game (vm/ui.qvm), which talks to the engine only by numbered
// system calls. This file is used in place of cl_ui.cpp and cl_uiapi.cpp of Jedi Academy. Derived from the file of
// the same name of JK2MV (GPLv2), which is derived from the Jedi Outcast source code released by Raven Software.

#include "client/client.h"
#include "client/cl_uiapi.h"
#include "client/cl_lan.h"
#include "client/snd_public.h"
#include "botlib/botlib.h"
#include "qcommon/strip.h"
#include "qcommon/g2_vmhandles.h"
#include "qcommon/vm_local.h"
#include "cl_jk2vm.h"

extern botlib_export_t *botlib_export;

vm_t *uivm;

/*
====================
GetClientState
====================
*/
static void GetClientState( uiClientState_t *state ) {
	state->connectPacketCount = clc.connectPacketCount;
	state->connState = cls.state;
	Q_strncpyz( state->servername, cls.servername, sizeof( state->servername ) );
	Q_strncpyz( state->updateInfoString, cls.updateInfoString, sizeof( state->updateInfoString ) );
	Q_strncpyz( state->messageString, clc.serverMessage, sizeof( state->messageString ) );
	state->clientNum = cl.snap.ps.clientNum;
}

/*
====================
GetClipboardData
====================
*/
static void GetClipboardData( char *buf, int buflen ) {
	char	*cbd;

	cbd = Sys_GetClipboardData();

	if ( !cbd ) {
		*buf = 0;
		return;
	}

	Q_strncpyz( buf, cbd, buflen );

	Z_Free( cbd );
}

/*
====================
Key_KeynumToStringBuf
====================
*/
// only ever called by binding-display code, therefore returns non-technical "friendly" names
//	in any language that don't necessarily match those in the config file...
//
static void Key_KeynumToStringBuf( int keynum, char *buf, int buflen )
{
	const char *psKeyName = Key_KeynumToString( keynum/*, qtrue */);

	// see if there's a more friendly (or localised) name...
	//
	const char *psKeyNameFriendly = SP_GetStringTextString( va("KEYNAMES_KEYNAME_%s",psKeyName) );

	Q_strncpyz( buf, (psKeyNameFriendly && psKeyNameFriendly[0]) ? psKeyNameFriendly : psKeyName, buflen );
}

/*
====================
Key_GetBindingBuf
====================
*/
static void Key_GetBindingBuf( int keynum, char *buf, int buflen ) {
	const char	*value;

	value = Key_GetBinding( keynum );
	if ( value ) {
		Q_strncpyz( buf, value, buflen );
	}
	else {
		*buf = 0;
	}
}

/*
====================
GetConfigString
====================
*/
static int GetConfigString(int index, char *buf, int size)
{
	int		offset;

	if (index < 0 || index >= MAX_CONFIGSTRINGS)
		return qfalse;

	offset = cl.gameState.stringOffsets[index];
	if (!offset) {
		if( size ) {
			buf[0] = 0;
		}
		return qfalse;
	}

	Q_strncpyz( buf, cl.gameState.stringData+offset, size);

	return qtrue;
}

/*
====================
UIVM_ wrappers: the calls into the ui module (vmMain)
====================
*/

void UIVM_Init( qboolean inGameLoad ) {
	VM_Call( uivm, UI_INIT, inGameLoad );
}

void UIVM_Shutdown( void ) {
	VM_Call( uivm, UI_SHUTDOWN );
}

void UIVM_KeyEvent( int key, qboolean down ) {
	// the key codes of Jedi Outcast 1.04 are the ones of this engine (the fakeAscii_t of ui/keycodes.h)
	VM_Call( uivm, UI_KEY_EVENT, key, down );
}

void UIVM_MouseEvent( int dx, int dy ) {
	VM_Call( uivm, UI_MOUSE_EVENT, dx, dy );
}

void UIVM_Refresh( int realtime ) {
	VM_Call( uivm, UI_REFRESH, realtime );
}

qboolean UIVM_IsFullscreen( void ) {
	return (qboolean)!!VM_Call( uivm, UI_IS_FULLSCREEN );
}

void UIVM_SetActiveMenu( uiMenuCommand_t menu ) {
	if ( uivm ) {
		VM_Call( uivm, UI_SET_ACTIVE_MENU, menu );
	}
}

qboolean UIVM_ConsoleCommand( int realTime ) {
	return (qboolean)!!VM_Call( uivm, UI_CONSOLE_COMMAND, realTime );
}

void UIVM_DrawConnectScreen( qboolean overlay ) {
	VM_Call( uivm, UI_DRAW_CONNECT_SCREEN, overlay );
}

/*
====================
CL_UISystemCalls

The ui module is making a system call
====================
*/
static intptr_t CL_UISystemCalls( intptr_t *args ) {
	switch( args[0] ) {
	case UI_ERROR:
		Com_Error( ERR_DROP, "%s", VMAS(1) );
		return 0;

	case UI_PRINT:
		Com_Printf( "%s", VMAS(1) );
		return 0;

	case UI_MILLISECONDS:
		return Sys_Milliseconds();

	case UI_CVAR_REGISTER:
		Cvar_Register( VMAV(1, vmCvar_t), VMAS(2), VMAS(3), args[4] );
		return 0;

	case UI_CVAR_UPDATE:
		Cvar_Update( VMAV(1, vmCvar_t) );
		return 0;

	case UI_CVAR_SET:
		Cvar_VM_Set( VMAS(1), VMAS(2), VM_UI );
		return 0;

	case UI_CVAR_VARIABLEVALUE:
		return FloatAsInt( Cvar_VariableValue( VMAS(1) ) );

	case UI_CVAR_VARIABLESTRINGBUFFER:
		Cvar_VariableStringBuffer( VMAS(1), VMAP(2, char, args[3]), args[3] );
		return 0;

	case UI_CVAR_SETVALUE:
		Cvar_VM_SetValue( VMAS(1), VMF(2), VM_UI );
		return 0;

	case UI_CVAR_RESET:
		Cvar_Reset( VMAS(1) );
		return 0;

	case UI_CVAR_CREATE:
		Cvar_Register( NULL, VMAS(1), VMAS(2), args[3] );
		return 0;

	case UI_CVAR_INFOSTRINGBUFFER:
		Cvar_InfoStringBuffer( args[1], VMAP(2, char, args[3]), args[3] );
		return 0;

	case UI_ARGC:
		return Cmd_Argc();

	case UI_ARGV:
		Cmd_ArgvBuffer( args[1], VMAP(2, char, args[3]), args[3] );
		return 0;

	case UI_CMD_EXECUTETEXT:
		Cbuf_ExecuteText( (cbufExec_t)args[1], VMAS(2) );
		return 0;

	case UI_FS_FOPENFILE:
		return FS_FOpenFileByMode( VMAS(1), VMAV(2, fileHandle_t), (fsMode_t)args[3] );

	case UI_FS_READ:
		FS_Read( VMAP(1, char, args[2]), args[2], args[3] );
		return 0;

	case UI_FS_WRITE:
		FS_Write( VMAP(1, char, args[2]), args[2], args[3] );
		return 0;

	case UI_FS_FCLOSEFILE:
		FS_FCloseFile( args[1] );
		return 0;

	case UI_FS_GETFILELIST:
		return FS_GetFileList( VMAS(1), VMAS(2), VMAP(3, char, args[4]), args[4] );

	case UI_R_REGISTERMODEL:
		return re->RegisterModel( VMAS(1) );

	case UI_R_REGISTERSKIN:
		return re->RegisterSkin( VMAS(1) );

	case UI_R_REGISTERSHADERNOMIP:
		return re->RegisterShaderNoMip( VMAS(1) );

	case UI_R_CLEARSCENE:
		re->ClearScene();
		return 0;

	case UI_R_ADDREFENTITYTOSCENE:
		CL_AddVMRefEntityToScene( VMAV(1, const vmRefEntity_t) );
		return 0;

	case UI_R_ADDPOLYTOSCENE:
		re->AddPolyToScene( args[1], args[2], VMAA(3, const polyVert_t, args[2]), 1 );
		return 0;

	case UI_R_ADDLIGHTTOSCENE:
		re->AddLightToScene( VMAP(1, const vec_t, 3), VMF(2), VMF(3), VMF(4), VMF(5) );
		return 0;

	case UI_R_RENDERSCENE:
		CL_RenderVMScene( VMAV(1, const vmRefdef_t) );
		return 0;

	case UI_R_SETCOLOR:
		re->SetColor( VMAP(1, const vec_t, 4) );
		return 0;

	case UI_R_DRAWSTRETCHPIC:
		re->DrawStretchPic( VMF(1), VMF(2), VMF(3), VMF(4), VMF(5), VMF(6), VMF(7), VMF(8), args[9] );
		return 0;

	case UI_R_MODELBOUNDS:
		re->ModelBounds( args[1], VMAP(2, vec_t, 3), VMAP(3, vec_t, 3) );
		return 0;

	case UI_UPDATESCREEN:
		SCR_UpdateScreen();
		return 0;

	case UI_CM_LERPTAG:
		re->LerpTag( VMAV(1, orientation_t), args[2], args[3], args[4], VMF(5), VMAS(6) );
		return 0;

	case UI_S_REGISTERSOUND:
		return S_RegisterSound( VMAS(1) );

	case UI_S_STARTLOCALSOUND:
		S_StartLocalSound( args[1], args[2] );
		return 0;

	case UI_KEY_KEYNUMTOSTRINGBUF:
		Key_KeynumToStringBuf( args[1], VMAP(2, char, args[3]), args[3] );
		return 0;

	case UI_KEY_GETBINDINGBUF:
		Key_GetBindingBuf( args[1], VMAP(2, char, args[3]), args[3] );
		return 0;

	case UI_KEY_SETBINDING:
		Key_SetBinding( args[1], VMAS(2) );
		return 0;

	case UI_KEY_ISDOWN:
		return Key_IsDown( args[1] );

	case UI_KEY_GETOVERSTRIKEMODE:
		return Key_GetOverstrikeMode();

	case UI_KEY_SETOVERSTRIKEMODE:
		Key_SetOverstrikeMode( (qboolean)!!args[1] );
		return 0;

	case UI_KEY_CLEARSTATES:
		Key_ClearStates();
		return 0;

	case UI_KEY_GETCATCHER:
		return Key_GetCatcher();

	case UI_KEY_SETCATCHER:
		// don't allow the ui module to close the console
		Key_SetCatcher( args[1] | ( Key_GetCatcher() & KEYCATCH_CONSOLE ) );
		return 0;

	case UI_GETCLIPBOARDDATA:
		GetClipboardData( VMAP(1, char, args[2]), args[2] );
		return 0;

	case UI_GETCLIENTSTATE:
		GetClientState( VMAV(1, uiClientState_t) );
		return 0;

	case UI_GETGLCONFIG:
		CL_GetVMGLConfig( VMAV(1, vmglconfig_t) );
		return 0;

	case UI_GETCONFIGSTRING:
		return GetConfigString( args[1], VMAP(2, char, args[3]), args[3] );

	case UI_LAN_LOADCACHEDSERVERS:
		LAN_LoadCachedServers();
		return 0;

	case UI_LAN_SAVECACHEDSERVERS:
		LAN_SaveServersToCache();
		return 0;

	case UI_LAN_ADDSERVER:
		return LAN_AddServer( args[1], VMAS(2), VMAS(3) );

	case UI_LAN_REMOVESERVER:
		LAN_RemoveServer( args[1], VMAS(2) );
		return 0;

	case UI_LAN_GETPINGQUEUECOUNT:
		return LAN_GetPingQueueCount();

	case UI_LAN_CLEARPING:
		LAN_ClearPing( args[1] );
		return 0;

	case UI_LAN_GETPING:
		LAN_GetPing( args[1], VMAP(2, char, args[3]), args[3], VMAV(4, int) );
		return 0;

	case UI_LAN_GETPINGINFO:
		LAN_GetPingInfo( args[1], VMAP(2, char, args[3]), args[3] );
		return 0;

	case UI_LAN_GETSERVERCOUNT:
		return LAN_GetServerCount( args[1] );

	case UI_LAN_GETSERVERADDRESSSTRING:
		LAN_GetServerAddressString( args[1], args[2], VMAP(3, char, args[4]), args[4] );
		return 0;

	case UI_LAN_GETSERVERINFO:
		LAN_GetServerInfo( args[1], args[2], VMAP(3, char, args[4]), args[4] );
		return 0;

	case UI_LAN_GETSERVERPING:
		return LAN_GetServerPing( args[1], args[2] );

	case UI_LAN_MARKSERVERVISIBLE:
		LAN_MarkServerVisible( args[1], args[2], (qboolean)!!args[3] );
		return 0;

	case UI_LAN_SERVERISVISIBLE:
		return LAN_ServerIsVisible( args[1], args[2] );

	case UI_LAN_UPDATEVISIBLEPINGS:
		return LAN_UpdateVisiblePings( args[1] );

	case UI_LAN_RESETPINGS:
		LAN_ResetPings( args[1] );
		return 0;

	case UI_LAN_SERVERSTATUS:
		return LAN_GetServerStatus( VMAS(1), VMAP(2, char, args[3]), args[3] );

	case UI_LAN_COMPARESERVERS:
		return LAN_CompareServers( args[1], args[2], args[3], args[4], args[5] );

	case UI_MEMORY_REMAINING:
		return Hunk_MemoryRemaining();

	case UI_GET_CDKEY:
		// there is no CD key
		VMAP(1, char, args[2])[0] = '\0';
		return 0;

	case UI_SET_CDKEY:
		return 0;

	case UI_VERIFY_CDKEY:
		return qtrue;

	case UI_R_REGISTERFONT:
		return re->RegisterFont( VMAS(1) );

	case UI_R_FONT_STRLENPIXELS:
		return re->Font_StrLenPixels( VMAS(1), args[2], VMF(3) );

	case UI_R_FONT_STRLENCHARS:
		return re->Font_StrLenChars( VMAS(1) );

	case UI_R_FONT_STRHEIGHTPIXELS:
		return re->Font_HeightPixels( args[1], VMF(2) );

	case UI_R_FONT_DRAWSTRING:
		re->Font_DrawString( args[1], args[2], VMAS(3), VMAP(4, const vec_t, 4), args[5], args[6], VMF(7) );
		return 0;

	case UI_LANGUAGE_ISASIAN:
		return re->Language_IsAsian();

	case UI_LANGUAGE_USESSPACES:
		return re->Language_UsesSpaces();

	case UI_ANYLANGUAGE_READCHARFROMSTRING:
		return re->AnyLanguage_ReadCharFromString( VMAS(1), VMAV(2, int), VMAV(3, qboolean) );

	case UI_MEMSET:
		Com_Memset( VMAP(1, char, args[3]), args[2], args[3] );
		return 0;

	case UI_MEMCPY:
		Com_Memcpy( VMAP(1, char, args[3]), VMAP(2, char, args[3]), args[3] );
		return 0;

	case UI_STRNCPY:
		return VM_strncpy( args[1], args[2], args[3] );

	case UI_SIN:
		return FloatAsInt( sinf( VMF(1) ) );

	case UI_COS:
		return FloatAsInt( cosf( VMF(1) ) );

	case UI_ATAN2:
		return FloatAsInt( atan2f( VMF(1), VMF(2) ) );

	case UI_SQRT:
		return FloatAsInt( VMF(1) < 0 ? 0 : sqrtf( VMF(1) ) );

	case UI_FLOOR:
		return FloatAsInt( floorf( VMF(1) ) );

	case UI_CEIL:
		return FloatAsInt( ceilf( VMF(1) ) );

	case UI_ACOS:
		return FloatAsInt( Q_acos( VMF(1) ) );

	case UI_ASIN:
		return FloatAsInt( Q_asin( VMF(1) ) );

	case UI_MATRIXMULTIPLY:
		MatrixMultiply( VMAP(1, vec3_t, 3), VMAP(2, vec3_t, 3), VMAP(3, vec3_t, 3) );
		return 0;

	case UI_ANGLEVECTORS:
		AngleVectors( VMAP(1, const vec_t, 3), args[2] ? VMAP(2, vec_t, 3) : NULL, args[3] ? VMAP(3, vec_t, 3) : NULL, args[4] ? VMAP(4, vec_t, 3) : NULL );
		return 0;

	case UI_PERPENDICULARVECTOR:
		PerpendicularVector( VMAP(1, vec_t, 3), VMAP(2, const vec_t, 3) );
		return 0;

	case UI_PC_ADD_GLOBAL_DEFINE:
		return botlib_export->PC_AddGlobalDefine( VMAS(1) );
	case UI_PC_LOAD_SOURCE:
		return botlib_export->PC_LoadSourceHandle( VMAS(1) );
	case UI_PC_FREE_SOURCE:
		return botlib_export->PC_FreeSourceHandle( args[1] );
	case UI_PC_READ_TOKEN:
		return botlib_export->PC_ReadTokenHandle( args[1], VMAV(2, pc_token_t) );
	case UI_PC_SOURCE_FILE_AND_LINE:
		return botlib_export->PC_SourceFileAndLine( args[1], VMAP(2, char, MAX_QPATH), VMAV(3, int) );
	case UI_PC_LOAD_GLOBAL_DEFINES:
		return botlib_export->PC_LoadGlobalDefines( VMAS(1) );
	case UI_PC_REMOVE_ALL_GLOBAL_DEFINES:
		botlib_export->PC_RemoveAllGlobalDefines();
		return 0;

	case UI_S_STOPBACKGROUNDTRACK:
		S_StopBackgroundTrack();
		return 0;
	case UI_S_STARTBACKGROUNDTRACK:
		S_StartBackgroundTrack( VMAS(1), VMAS(2), qfalse );
		return 0;

	case UI_REAL_TIME:
		return Com_RealTime( VMAV(1, qtime_t) );

	case UI_CIN_PLAYCINEMATIC:
		Com_DPrintf( "UI_CIN_PlayCinematic\n" );
		return CIN_PlayCinematic( VMAS(1), args[2], args[3], args[4], args[5], args[6] );

	case UI_CIN_STOPCINEMATIC:
		return CIN_StopCinematic( args[1] );

	case UI_CIN_RUNCINEMATIC:
		return CIN_RunCinematic( args[1] );

	case UI_CIN_DRAWCINEMATIC:
		CIN_DrawCinematic( args[1] );
		return 0;

	case UI_CIN_SETEXTENTS:
		CIN_SetExtents( args[1], args[2], args[3], args[4], args[5] );
		return 0;

	case UI_R_REMAP_SHADER:
		re->RemapShader( VMAS(1), VMAS(2), VMAS(3) );
		return 0;

	case UI_SP_REGISTER:
		return !!SP_Register( VMAS(1), SP_REGISTER_MENU );

	case UI_SP_GETSTRINGTEXTSTRING:
		{
			const char *text = SP_GetStringTextString( VMAS(1) );

			Q_strncpyz( VMAP(2, char, args[3]), text, args[3] );
		}
		return qtrue;

	case UI_G2_ANGLEOVERRIDE:
		return G2VM_SetBoneAngles( args[1], args[2], VMAS(3), VMAP(4, const vec_t, 3), args[5], args[6], args[7], args[8],
			VMAA(9, qhandle_t, args[2] + 1), args[10], args[11] );
	}

	Com_Error( ERR_DROP, "Bad UI system trap: %lli", (long long int)args[0] );
	return 0;
}

/*
====================
CL_ShutdownUI
====================
*/
void CL_ShutdownUI( void ) {
	Key_SetCatcher( Key_GetCatcher( ) & ~KEYCATCH_UI );

	cls.uiStarted = qfalse;

	if ( !uivm ) {
		return;
	}

	VM_Call( uivm, UI_SHUTDOWN );
	VM_Free( uivm );
	uivm = NULL;
}

/*
====================
CL_InitUI
====================
*/
void CL_InitUI( void ) {
	vmInterpret_t	interpret;
	int				v;

	// load the dll or bytecode
	if ( cl_connectedToPureServer != 0 ) {
		// if sv_pure is set we only allow qvms to be loaded
		interpret = VMI_BYTECODE;
	} else {
		interpret = (vmInterpret_t)(int)Cvar_VariableValue( "vm_ui" );
	}
	uivm = VM_Create( "ui", CL_UISystemCalls, interpret );
	if ( !uivm ) {
		Com_Error( ERR_FATAL, "VM_Create on UI failed" );
	}

	// sanity check
	v = VM_Call( uivm, UI_GETAPIVERSION );
	if ( v != UI_API_VERSION ) {
		CL_ShutdownUI();
		Com_Error( ERR_DROP, "User Interface is version %d, expected %d", v, UI_API_VERSION );
	}

	cls.uiStarted = qtrue;

	// init for this gamestate
	//rww - changed to <= CA_ACTIVE, because that is the state when we did a vid_restart
	//ingame (was just < CA_ACTIVE before, resulting in ingame menus getting wiped and
	//not reloaded on vid restart from ingame menu)
	UIVM_Init( (qboolean)(cls.state >= CA_AUTHORIZING && cls.state <= CA_ACTIVE) );
}

/*
====================
UI_GameCommand

See if the current console command is claimed by the ui
====================
*/
qboolean UI_GameCommand( void ) {
	if ( !uivm ) {
		return qfalse;
	}

	return UIVM_ConsoleCommand( cls.realtime );
}
