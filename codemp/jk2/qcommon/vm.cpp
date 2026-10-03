/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// vm.cpp -- virtual machine for the game modules of Jedi Outcast
//
// The game modules of Jedi Outcast multiplayer (game, cgame and ui) come as QVM files in the pk3 files of the game,
// they are run by an interpreter. A module can also be a native library, built for the platform (vm_game 0 and so on).
//
// Derived from the Quake III Arena source code, by way of JK2MV.

#include "vm_local.h"

#include "sys/sys_public.h"

vm_t	*currentVM = NULL;
vm_t	*lastVM	= NULL;
int		vm_debugLevel;

qboolean vm_profileInclusive;
static vmSymbol_t nullSymbol;

// used by Com_Error to get rid of running vm's before longjmp
static int forced_unload;

#ifndef MIN
#define MIN(a,b)	((a) < (b) ? (a) : (b))
#endif

const char *vmStrs[MAX_VM] = {
	"GameVM",
	"CGameVM",
	"UIVM",
};

static vm_t	vmTable[MAX_VM];

// the renderer reads the slot through vmView_t
static_assert( offsetof( vm_t, slot ) == offsetof( vmView_t, slot ) && offsetof( vm_t, systemCall ) == offsetof( vmView_t, systemCall ), "vm_t and vmView_t do not match" );

static cvar_t	*vm_game, *vm_cgame, *vm_ui;

static void VM_VmInfo_f( void );

void VM_Debug( int level ) {
	vm_debugLevel = level;
}

/*
==============
VM_Init
==============
*/
void VM_Init( void ) {
	// 0 = a native library if there is one, 1 = the QVM in the interpreter (there is no compiler)
	vm_cgame = Cvar_Get( "vm_cgame", "1", CVAR_ARCHIVE );
	vm_game = Cvar_Get( "vm_game", "1", CVAR_ARCHIVE );
	vm_ui = Cvar_Get( "vm_ui", "1", CVAR_ARCHIVE );

	Cmd_AddCommand( "vminfo", VM_VmInfo_f );

	Com_Memset( vmTable, 0, sizeof( vmTable ) );
}

/*
===============
VM_ValueToSymbol

Assumes a program counter value
===============
*/
const char *VM_ValueToSymbol( vm_t *vm, int value ) {
	static char		text[MAX_TOKEN_CHARS];

	Com_sprintf( text, sizeof( text ), "%s(%#x)", vm->name, value );
	return text;
}

/*
===============
VM_ValueToFunctionSymbol

For profiling, find the symbol behind this value (the symbol files of the retail modules are not used)
===============
*/
vmSymbol_t *VM_ValueToFunctionSymbol( vm_t *vm, int value ) {
	return &nullSymbol;
}

int VM_SymbolToValue( vm_t *vm, const char *symbol ) {
	return 0;
}

/*
============
VM_DllSyscall

Native modules call this directly. The arguments are copied to an array so that the handler gets the same
(intptr_t sized) values as it does from the interpreter.
============
*/
intptr_t QDECL VM_DllSyscall( intptr_t arg, ... ) {
	intptr_t args[MAX_VMSYSCALL_ARGS];
	size_t i;
	va_list ap;

	args[0] = arg;

	va_start( ap, arg );
	for ( i = 1; i < ARRAY_LEN( args ); i++ )
		args[i] = va_arg( ap, intptr_t );
	va_end( ap );

	return currentVM->systemCall( args );
}

/*
=================
VM_LoadQVM

Load a .qvm file
=================
*/
static vmHeader_t *VM_LoadQVM( vm_t *vm, qboolean alloc )
{
	int					dataLength;
	int					i;
	char				filename[MAX_QPATH];
	union {
		vmHeader_t	*h;
		void		*v;
	} header;

	// load the image
	Com_sprintf( filename, sizeof(filename), "vm/%s.qvm", vm->name );
	Com_Printf( "Loading vm file %s...\n", filename );

	FS_ReadFile( filename, &header.v );

	if ( !header.h ) {
		Com_Printf( "Failed.\n" );
		VM_Free( vm );

		Com_Printf( S_COLOR_YELLOW "Warning: Couldn't open VM file %s\n", filename );

		return NULL;
	}

	if ( LittleLong( header.h->vmMagic ) == VM_MAGIC ) {
		// byte swap the header
		// sizeof( vmHeader_t ) - sizeof( int ) is the 1.32b vm header size
		for ( size_t j = 0 ; j < ( sizeof( vmHeader_t ) - sizeof( int ) ) / 4 ; j++ ) {
			((int *)header.h)[j] = LittleLong( ((int *)header.h)[j] );
		}

		// validate
		if ( header.h->bssLength < 0
			|| header.h->dataLength < 0
			|| header.h->litLength < 0
			|| header.h->codeLength <= 0 )
		{
			VM_Free( vm );
			FS_FreeFile( header.v );

			Com_Printf( S_COLOR_YELLOW "Warning: %s has bad header\n", filename );
			return NULL;
		}
	} else {
		VM_Free( vm );
		FS_FreeFile( header.v );

		Com_Printf( S_COLOR_YELLOW "Warning: %s does not have a recognisable "
				"magic number in its header\n", filename );
		return NULL;
	}

	// round up to next power of 2 so all data operations can
	// be mask protected
	dataLength = header.h->dataLength + header.h->litLength +
		header.h->bssLength;
	for ( i = 0 ; dataLength > ( 1 << i ) ; i++ ) {
	}
	dataLength = 1 << i;

	if ( alloc )
	{
		// allocate zero filled space for initialized and uninitialized data
		vm->dataBase = (byte *)Hunk_Alloc( dataLength, h_high );
		vm->dataMask = dataLength - 1;
	}
	else
	{
		// clear the data, but make sure we're not clearing more than allocated
		if ( vm->dataMask + 1 != dataLength )
		{
			VM_Free( vm );
			FS_FreeFile( header.v );

			Com_Printf( S_COLOR_YELLOW "Warning: Data region size of %s not matching after "
					"VM_Restart()\n", filename );
			return NULL;
		}

		Com_Memset( vm->dataBase, 0, dataLength );
	}

	// copy the intialized data
	Com_Memcpy( vm->dataBase, (byte *)header.h + header.h->dataOffset,
		header.h->dataLength + header.h->litLength );

	// byte swap the longs
	for ( i = 0 ; i < header.h->dataLength ; i += 4 ) {
		*(int *)(vm->dataBase + i) = LittleLong( *(int *)(vm->dataBase + i ) );
	}

	return header.h;
}

/*
=================
VM_Restart

Reload the data, but leave everything else in place
This allows a server to do a map_restart without changing memory allocation
=================
*/
vm_t *VM_Restart( vm_t *vm )
{
	vmHeader_t	*header;

	// DLL's can't be restarted in place
	if ( vm->dllHandle ) {
		char	name[MAX_QPATH];
		intptr_t	(*systemCall)( intptr_t *parms );

		systemCall = vm->systemCall;
		Q_strncpyz( name, vm->name, sizeof( name ) );

		VM_Free( vm );

		vm = VM_Create( name, systemCall, VMI_NATIVE );
		return vm;
	}

	// load the image
	Com_Printf( "VM_Restart()\n" );

	if ( !( header = VM_LoadQVM( vm, qfalse ) ) )
	{
		Com_Error( ERR_DROP, "VM_Restart failed" );
		return NULL;
	}

	// free the original file
	FS_FreeFile( header );

	return vm;
}

/*
================
VM_Create

With VMI_NATIVE a native library is looked for first, then the QVM.
================
*/
vm_t *VM_Create( const char *module, intptr_t (*systemCalls)(intptr_t *), vmInterpret_t interpret ) {
	vm_t		*vm;
	vmHeader_t	*header;
	int			i, remaining;

	if ( !module || !module[0] || !systemCalls ) {
		Com_Error( ERR_FATAL, "VM_Create: bad parms" );
	}

	remaining = Hunk_MemoryRemaining();

	// see if we already have the VM
	for ( i = 0 ; i < MAX_VM ; i++ ) {
		if ( !Q_stricmp( vmTable[i].name, module ) ) {
			vm = &vmTable[i];
			return vm;
		}
	}

	// find a free vm
	for ( i = 0 ; i < MAX_VM ; i++ ) {
		if ( !vmTable[i].name[0] ) {
			break;
		}
	}

	if ( i == MAX_VM ) {
		Com_Error( ERR_FATAL, "VM_Create: no free vm_t" );
	}

	vm = &vmTable[i];

	Com_Memset( vm, 0, sizeof( *vm ) );
	Q_strncpyz( vm->name, module, sizeof( vm->name ) );
	vm->slot = !Q_stricmp( module, "cgame" ) ? VM_CGAME : !Q_stricmp( module, "ui" ) ? VM_UI : VM_GAME;

	if ( interpret == VMI_NATIVE ) {
		// try to load as a system dll
		Com_Printf( "Loading library file %s.\n", vm->name );
		vm->dllHandle = Sys_LoadLegacyGameDll( module, &vm->entryPoint, VM_DllSyscall );
		if ( vm->dllHandle ) {
			vm->systemCall = systemCalls;
			return vm;
		}

		Com_Printf( "Failed to load library, looking for qvm.\n" );
		interpret = VMI_BYTECODE;
	}

	if ( ( header = VM_LoadQVM( vm, qtrue ) ) == NULL ) {
		return NULL;
	}

	vm->systemCall = systemCalls;

	// allocate space for the jump targets, which will be filled in by the prep function
	vm->instructionCount = header->instructionCount;
	vm->instructionPointers = (intptr_t *)Hunk_Alloc( vm->instructionCount * sizeof( *vm->instructionPointers ), h_high );

	// copy the instructions
	vm->codeLength = header->codeLength;

	vm->compiled = qfalse;
	VM_PrepareInterpreter( vm, header );

	// free the original file
	FS_FreeFile( header );

	// the stack is implicitly at the end of the image
	vm->programStack = vm->dataMask + 1;
	vm->stackBottom = vm->programStack - PROGRAM_STACK_SIZE;

	Com_Printf( "%s loaded in %d bytes on the hunk\n", module, remaining - Hunk_MemoryRemaining() );

	return vm;
}

/*
==============
VM_Free
==============
*/
void VM_Free( vm_t *vm ) {

	if ( !vm ) {
		return;
	}

	if ( vm->callLevel ) {
		if ( !forced_unload ) {
			Com_Error( ERR_FATAL, "VM_Free(%s) on running vm", vm->name );
			return;
		} else {
			Com_Printf( "forcefully unloading %s vm\n", vm->name );
		}
	}

	if ( vm->dllHandle ) {
		Sys_UnloadDll( vm->dllHandle );
	}

	// (the memory of a QVM is on the hunk and is freed with it)
	Com_Memset( vm, 0, sizeof( *vm ) );

	currentVM = NULL;
	lastVM = NULL;
}

void VM_Clear( void ) {
	int i;
	for ( i = 0 ; i < MAX_VM; i++ ) {
		VM_Free( &vmTable[i] );
	}
}

void VM_Forced_Unload_Start( void ) {
	forced_unload = 1;
}

void VM_Forced_Unload_Done( void ) {
	forced_unload = 0;
}

/*
==============
The pointers that a QVM passes are offsets into its data: they are masked to the data of the module, and
the size of what they point to is checked against the end of the data.
==============
*/
void *VM_ArgPtr( int syscall, intptr_t intValue, int32_t size ) {
	if ( currentVM->entryPoint ) {
		return (void *) intValue;
	}
	if ( !intValue ) {
		return NULL;
	}

	// don't drop on overflow for compatibility reasons
	intValue &= currentVM->dataMask;

	// Disregarding integer overflows:
	// if ( size < 0 || currentVM->dataMask < intValue + size - 1 )
	if ( (uint32_t)size > (uint32_t)(currentVM->dataMask - intValue + 1) ) {
		Com_Error( ERR_DROP, "VM_ArgPtr: memory overflow in syscall %d (%s)", syscall, currentVM->name );
	}

	return (void *)(currentVM->dataBase + intValue);
}

void *VM_ArgArray( int syscall, intptr_t intValue, uint32_t size, int32_t num ) {
	if ( currentVM->entryPoint ) {
		return (void *) intValue;
	}
	if ( !intValue ) {
		return NULL;
	}

	// don't drop on overflow for compatibility reasons
	intValue &= currentVM->dataMask;

	int64_t bytes = (int64_t)size * (int64_t)num;
	if ( (uint64_t)bytes > (uint64_t)(currentVM->dataMask - intValue + 1) ) {
		Com_Error( ERR_DROP, "VM_ArgArray: memory overflow in syscall %d (%s)", syscall, currentVM->name );
	}

	return (void *)(currentVM->dataBase + intValue);
}

char *VM_ArgString( int syscall, intptr_t intValue ) {
	if ( currentVM->entryPoint ) {
		return (char *) intValue;
	}
	if ( !intValue ) {
		return NULL;
	}

	intptr_t	len;
	char		*p;
	const int	dataMask = currentVM->dataMask;

	// don't drop on overflow for compatibility reasons
	intValue &= dataMask;

	p = (char *) currentVM->dataBase + intValue;
	len = (intptr_t) strnlen( p, dataMask + 1 - intValue );

	if ( intValue + len > dataMask ) {
		Com_Error( ERR_DROP, "VM_ArgString: memory overflow in syscall %d (%s)", syscall, currentVM->name );
	}

	return p;
}

char *VM_ExplicitArgString( vm_t *vm, intptr_t intValue ) {
	if ( !vm ) {
		return NULL;
	}
	if ( vm->entryPoint ) {
		return (char *) intValue;
	}
	if ( !intValue ) {
		return NULL;
	}

	intptr_t	len;
	char		*p;
	const int	dataMask = vm->dataMask;

	// don't drop on overflow for compatibility reasons
	intValue &= dataMask;

	p = (char *) vm->dataBase + intValue;
	len = (intptr_t) strnlen( p, dataMask + 1 - intValue );

	if ( intValue + len > dataMask ) {
		Com_Error( ERR_DROP, "VM_ExplicitArgString: memory overflow in %s", vm->name );
	}

	return p;
}

intptr_t VM_strncpy( intptr_t dest, intptr_t src, intptr_t size ) {
	if ( currentVM->entryPoint ) {
		return (intptr_t) strncpy( (char *)dest, (const char *)src, size );
	}

	char *dataBase = (char *)currentVM->dataBase;
	int dataMask = currentVM->dataMask;

	// don't drop on overflow for compatibility reasons
	dest &= dataMask;
	src &= dataMask;

	size_t destSize = dataMask - dest;
	size_t srcSize = dataMask - src;
	destSize = MIN( (size_t) size, destSize );
	size_t n = MIN( destSize, srcSize );

	strncpy( dataBase + dest, dataBase + src, n );

	if ( n < destSize ) {
		memset( dataBase + dest + n, 0, destSize - n );
	}

	return dest;
}

// the game data that the module tells the server about (G_LOCATE_GAME_DATA) has to fit in its memory
void VM_LocateGameDataCheck( const void *data, int entitySize, int num_entities ) {
	if ( !data ) {
		return;
	}

	if ( !currentVM->entryPoint ) {
		uintptr_t dataEnd = (uintptr_t) currentVM->dataBase + (uintptr_t)currentVM->dataMask + 1;
		uintptr_t maxSize = dataEnd - (uintptr_t)data;

		if ( entitySize <= 0 || num_entities <= 0 ) {
			Com_Error( ERR_DROP, "LOCATE_GAME_DATA: bad size" );
		}

		if ( (uint64_t) entitySize * (uint64_t) num_entities > maxSize ) {
			Com_Error( ERR_DROP, "LOCATE_GAME_DATA: memory overflow" );
		}
	}
}

/*
==============
VM_BlockCopy

The block copy of the module (OP_BLOCK_COPY), checked against the data of the module
==============
*/
void VM_BlockCopy( unsigned int dest, unsigned int src, size_t n ) {
	unsigned int dataMask = currentVM->dataMask;

	if ( ( dest & dataMask ) != dest
		|| ( src & dataMask ) != src
		|| ( ( dest + n ) & dataMask ) != dest + n
		|| ( ( src + n ) & dataMask ) != src + n )
	{
		Com_Error( ERR_DROP, "OP_BLOCK_COPY out of range!" );
	}

	Com_Memcpy( currentVM->dataBase + dest, currentVM->dataBase + src, n );
}

/*
==============
VM_Call

Upon a system call, the stack will look like:

sp+32	parm1
sp+28	parm0
sp+24	return value
sp+20	return address
sp+16	local1
sp+14	local0
sp+12	arg1
sp+8	arg0
sp+4	return stack
sp		return address

An interpreted function will immediately execute
an OP_ENTER instruction, which will subtract space for
locals from sp
==============
*/
intptr_t QDECL VM_Call( vm_t *vm, int callnum, ... )
{
	vm_t	*oldVM;
	intptr_t r;
	size_t i;

	if ( !vm || !vm->name[0] )
		Com_Error( ERR_FATAL, "VM_Call with NULL vm" );

	oldVM = currentVM;
	currentVM = vm;
	lastVM = vm;

	if ( vm_debugLevel ) {
		Com_Printf( "VM_Call( %d )\n", callnum );
	}

	++vm->callLevel;
	// if we have a dll loaded, call it directly
	if ( vm->entryPoint ) {
		int args[MAX_VMMAIN_ARGS-1];
		va_list ap;
		va_start( ap, callnum );
		for ( i = 0; i < ARRAY_LEN( args ); i++ ) {
			args[i] = va_arg( ap, int );
		}
		va_end( ap );

		r = vm->entryPoint( callnum,  args[0],  args[1],  args[2], args[3],
							args[4],  args[5],  args[6], args[7],
							args[8],  args[9], args[10], args[11] );
	} else {
		struct {
			int callnum;
			int args[MAX_VMMAIN_ARGS-1];
		} a;
		va_list ap;

		a.callnum = callnum;
		va_start( ap, callnum );
		for ( i = 0; i < ARRAY_LEN( a.args ); i++ ) {
			a.args[i] = va_arg( ap, int );
		}
		va_end( ap );

		r = VM_CallInterpreted( vm, &a.callnum );
	}
	--vm->callLevel;

	if ( oldVM != NULL )
		currentVM = oldVM;
	return r;
}

/*
==============
VM_VmInfo_f
==============
*/
static void VM_VmInfo_f( void ) {
	vm_t	*vm;
	int		i;

	Com_Printf( "Registered virtual machines:\n" );
	for ( i = 0 ; i < MAX_VM ; i++ ) {
		vm = &vmTable[i];
		if ( !vm->name[0] ) {
			break;
		}
		Com_Printf( "%s : ", vm->name );
		if ( vm->dllHandle ) {
			Com_Printf( "native\n" );
			continue;
		}
		Com_Printf( "interpreted\n" );
		Com_Printf( "    code length : %7i\n", vm->codeLength );
		Com_Printf( "    table length: %7i\n", vm->instructionCount*4 );
		Com_Printf( "    data length : %7i\n", vm->dataMask + 1 );
	}
}
