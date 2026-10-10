/*
===========================================================================
Copyright (C) 2013 - 2018, OpenJK contributors

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

// tr_image_prefetch.cpp -- images are read ahead and decoded on worker threads while a level loads (docs/parallel-work.md).
//
// Decoding the textures of a level (JPEG and TGA) is about half of the time it takes to load, and every image is
// independent of the others. Which images a level needs is only known once the shaders have been parsed, so the list
// is the one of the last time the level was loaded: when a level starts to load, those files are read (by this thread,
// the file system belongs to it) and handed to the workers, and by the time the renderer asks for an image it is
// usually decoded. An image that is not in the list, or not done, or not wanted any more costs nothing but what
// loading it always cost, so the lists can be wrong without harm. They are kept in loadlists/<map>.txt next to the
// config and grow to cover everything the level has asked for.

#include "../server/exe_headers.h"
#include "tr_common.h"
#include "jobs/jobs.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define PREFETCH_MAX_ENTRIES	4096
#define PREFETCH_MAX_BYTES		( 640ull * 1024 * 1024 )	// what may be read and decoded at once (an estimate for what is not done yet)

namespace
{

enum { STATE_NOT_QUEUED, STATE_QUEUED, STATE_DONE, STATE_FAILED, STATE_TAKEN };

struct Entry
{
	std::string	file;		// as it is in the file system, with the extension
	std::string	key;		// lower case, without the extension
	int			state;
	bool		jpeg;
	byte		*raw;		// the file while it waits for a worker
	size_t		rawLength;
	byte		*pixels;	// RGBA, made with malloc
	int			width, height;
	size_t		estimate;	// bytes the decoded image is going to take (until it is known)
};

std::vector<Entry *>				entries;
std::unordered_map<std::string, int>	byKey;
std::unordered_set<std::string>		noted;			// files loaded during this level load, in the order of
std::vector<std::string>			notedOrder;
std::string							mapName;
bool								active;
bool								stopWorkers;
int									busyWorkers;
size_t								bytesInUse;

std::mutex							lock;
std::condition_variable				workReady, entryDone;
std::deque<int>						queue;
std::vector<std::thread>			workers;

bool Wanted( void )
{
	return Jobs::WantedThreads() > 0;
}

// the key of an image name: lower case without the extension
std::string KeyOf( const char *name )
{
	std::string key( name );
	const size_t dot = key.find_last_of( '.' );
	const size_t slash = key.find_last_of( "/\\" );
	if ( dot != std::string::npos && ( slash == std::string::npos || dot > slash ) )
	{
		key.resize( dot );
	}
	for ( size_t i = 0; i < key.size(); i++ )
	{
		if ( key[i] == '\\' ) key[i] = '/';
		else if ( key[i] >= 'A' && key[i] <= 'Z' ) key[i] = (char)( key[i] - 'A' + 'a' );
	}
	return key;
}

std::string ListName( const char *map )
{
	std::string name( "loadlists/" );
	for ( const char *c = map; *c; c++ )
	{
		const char ch = *c;
		name += ( ( ch >= 'a' && ch <= 'z' ) || ( ch >= 'A' && ch <= 'Z' ) || ( ch >= '0' && ch <= '9' ) || ch == '_' || ch == '-' ) ? ch : '_';
	}
	return name + ".txt";
}

void Worker( void )
{
	std::unique_lock<std::mutex> guard( lock );
	for ( ;; )
	{
		workReady.wait( guard, [] { return stopWorkers || !queue.empty(); } );
		if ( stopWorkers )
		{
			return;
		}
		const int index = queue.front();
		queue.pop_front();
		Entry *entry = entries[index];
		byte *raw = entry->raw;
		const size_t length = entry->rawLength;
		const bool jpeg = entry->jpeg;
		busyWorkers++;
		guard.unlock();

		byte *pixels = NULL;
		int width = 0, height = 0;
		const bool ok = jpeg ? R_DecodeJPGFromMemory( raw, length, &pixels, &width, &height )
			: R_DecodeTGAFromMemory( raw, length, &pixels, &width, &height );
		free( raw );

		guard.lock();
		busyWorkers--;
		entry->raw = NULL;
		if ( ok && pixels )
		{
			entry->pixels = pixels;
			entry->width = width;
			entry->height = height;
			entry->state = STATE_DONE;
			bytesInUse = bytesInUse - entry->estimate + (size_t)width * height * 4;
			entry->estimate = (size_t)width * height * 4;
		}
		else
		{
			free( pixels );
			entry->state = STATE_FAILED;
			bytesInUse -= entry->estimate;
			entry->estimate = 0;
		}
		entryDone.notify_all();
	}
}

void StartWorkers( void )
{
	if ( !workers.empty() )
	{
		return;
	}
	stopWorkers = false;
	const int count = Jobs::WantedThreads();
	for ( int i = 0; i < count; i++ )
	{
		workers.push_back( std::thread( Worker ) );
	}
}

// forgets the entries, waiting for the workers to finish with them
void Clear( void )
{
	std::unique_lock<std::mutex> guard( lock );
	while ( !queue.empty() )
	{
		Entry *entry = entries[queue.front()];
		queue.pop_front();
		free( entry->raw );
		entry->raw = NULL;
		entry->state = STATE_FAILED;
	}
	entryDone.wait( guard, [] { return busyWorkers == 0; } );
	for ( size_t i = 0; i < entries.size(); i++ )
	{
		free( entries[i]->raw );
		free( entries[i]->pixels );
		delete entries[i];
	}
	entries.clear();
	byKey.clear();
	bytesInUse = 0;
}

bool EndsWith( const std::string &text, const char *suffix )
{
	const size_t length = strlen( suffix );
	return text.size() >= length && !Q_stricmp( text.c_str() + text.size() - length, suffix );
}

} // namespace

/*
=================
R_ImagePrefetch_Begin

A level starts to load: read the images its list names.
=================
*/
void R_ImagePrefetch_Begin( const char *mapName_ )
{
	if ( active )
	{
		R_ImagePrefetch_End();
	}
	if ( !mapName_ || !mapName_[0] || !Wanted() )
	{
		return;
	}
	mapName = mapName_;
	active = true;
	noted.clear();
	notedOrder.clear();

	void *text = NULL;
	const long length = ri.FS_ReadFile( ListName( mapName_ ).c_str(), &text );
	if ( !text || length <= 0 )
	{
		if ( text ) ri.FS_FreeFile( text );
		return;
	}
	std::vector<std::string> names;
	{
		const char *p = (const char *)text;
		const char *end = p + length;
		while ( p < end && names.size() < PREFETCH_MAX_ENTRIES )
		{
			const char *eol = p;
			while ( eol < end && *eol != '\n' && *eol != '\r' ) eol++;
			if ( eol > p )
			{
				names.push_back( std::string( p, eol ) );
			}
			p = eol + 1;
		}
	}
	ri.FS_FreeFile( text );

	StartWorkers();
	for ( size_t i = 0; i < names.size(); i++ )
	{
		const std::string &file = names[i];
		const bool jpeg = EndsWith( file, ".jpg" ) || EndsWith( file, ".jpeg" );
		if ( !jpeg && !EndsWith( file, ".tga" ) )
		{
			continue;	// (PNG and the like are loaded when they are asked for)
		}
		{
			std::lock_guard<std::mutex> guard( lock );
			if ( bytesInUse >= PREFETCH_MAX_BYTES )
			{
				break;	// enough in flight, the rest is loaded when it is asked for
			}
		}
		const std::string key = KeyOf( file.c_str() );
		if ( byKey.find( key ) != byKey.end() )
		{
			continue;
		}

		void *data = NULL;
		const long size = ri.FS_ReadFile( file.c_str(), &data );
		if ( !data || size <= 0 )
		{
			if ( data ) ri.FS_FreeFile( data );
			continue;
		}
		byte *raw = (byte *)malloc( (size_t)size );
		if ( !raw )
		{
			ri.FS_FreeFile( data );
			continue;
		}
		memcpy( raw, data, (size_t)size );
		ri.FS_FreeFile( data );

		Entry *entry = new Entry();
		entry->file = file;
		entry->key = key;
		entry->jpeg = jpeg;
		entry->raw = raw;
		entry->rawLength = (size_t)size;
		entry->pixels = NULL;
		entry->width = entry->height = 0;
		entry->estimate = jpeg ? (size_t)size * 10 : (size_t)size;
		entry->state = STATE_QUEUED;
		{
			std::lock_guard<std::mutex> guard( lock );
			entries.push_back( entry );
			byKey[key] = (int)entries.size() - 1;
			bytesInUse += entry->estimate;
			queue.push_back( (int)entries.size() - 1 );
		}
		workReady.notify_one();
	}
}

/*
=================
R_ImagePrefetch_Take

The image the renderer asks for, if it was read ahead. Waits for the worker if it is not finished.
=================
*/
qboolean R_ImagePrefetch_Take( const char *shortname, byte **pic, int *width, int *height )
{
	if ( !active || entries.empty() )
	{
		return qfalse;
	}
	const std::string key = KeyOf( shortname );
	std::unordered_map<std::string, int>::iterator found = byKey.find( key );
	if ( found == byKey.end() )
	{
		return qfalse;
	}
	Entry *entry = entries[found->second];

	std::unique_lock<std::mutex> guard( lock );
	entryDone.wait( guard, [entry] { return entry->state != STATE_QUEUED; } );
	if ( entry->state != STATE_DONE )
	{
		return qfalse;
	}
	byte *pixels = entry->pixels;
	entry->pixels = NULL;
	entry->state = STATE_TAKEN;
	bytesInUse -= entry->estimate;
	entry->estimate = 0;
	guard.unlock();

	// (the renderer frees what it is given with R_Free: zone memory)
	const int count = entry->width * entry->height * 4;
	byte *out = (byte *)R_Malloc( count, TAG_TEMP_WORKSPACE, qfalse );
	memcpy( out, pixels, (size_t)count );
	free( pixels );
	*pic = out;
	*width = entry->width;
	*height = entry->height;
	R_ImagePrefetch_Note( entry->file.c_str() );
	return qtrue;
}

void R_ImagePrefetch_Note( const char *fileName )
{
	if ( !active )
	{
		return;
	}
	const std::string file( fileName );
	if ( noted.insert( file ).second )
	{
		notedOrder.push_back( file );
	}
}

/*
=================
R_ImagePrefetch_End

The level is loaded: the images it asked for join the list, and what was read ahead for nothing is let go.
=================
*/
void R_ImagePrefetch_End( void )
{
	if ( !active )
	{
		return;
	}
	active = false;
	Clear();

	if ( !notedOrder.empty() )
	{
		// the list so far, then what is new
		std::vector<std::string> list;
		std::unordered_set<std::string> have;
		const std::string path = ListName( mapName.c_str() );
		void *text = NULL;
		const long length = ri.FS_ReadFile( path.c_str(), &text );
		if ( text && length > 0 )
		{
			const char *p = (const char *)text;
			const char *end = p + length;
			while ( p < end )
			{
				const char *eol = p;
				while ( eol < end && *eol != '\n' && *eol != '\r' ) eol++;
				if ( eol > p )
				{
					std::string line( p, eol );
					if ( have.insert( line ).second ) list.push_back( line );
				}
				p = eol + 1;
			}
		}
		if ( text ) ri.FS_FreeFile( text );

		bool changed = false;
		for ( size_t i = 0; i < notedOrder.size() && list.size() < PREFETCH_MAX_ENTRIES; i++ )
		{
			if ( have.insert( notedOrder[i] ).second )
			{
				list.push_back( notedOrder[i] );
				changed = true;
			}
		}
		if ( changed )
		{
			std::string out;
			for ( size_t i = 0; i < list.size(); i++ )
			{
				out += list[i];
				out += '\n';
			}
			ri.FS_WriteFile( path.c_str(), out.data(), (int)out.size() );
		}
	}
	noted.clear();
	notedOrder.clear();
}

void R_ImagePrefetch_Shutdown( void )
{
	if ( active )
	{
		active = false;
		Clear();
	}
	{
		std::lock_guard<std::mutex> guard( lock );
		stopWorkers = true;
	}
	workReady.notify_all();
	for ( size_t i = 0; i < workers.size(); i++ )
	{
		workers[i].join();
	}
	workers.clear();
}
