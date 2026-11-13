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

// fs_packages.h -- mods as packages, the conflicts between pk3 files, and reading every copy of a file. Included by
// the file systems of the single player engine (code/qcommon/files.cpp) and of the multiplayer one
// (codemp/qcommon/files.cpp), after their FS_LoadZipFile, with FS_PACKAGES_MULTIPLAYER set to 0 or 1.
//
// PACKAGES. The old ways stay as they were: pk3 files in base are all loaded, by the order of their names (the last
// wins), and a mod folder next to base (fs_game) is loaded after everything. New: the "mods" folders (in the game's
// folder and in the home folder) hold packages, which are loaded after the pk3 files of base and before a mod folder:
//   - a .zip as it was downloaded: the pk3 files in it, at any depth, are copied once into <home>/base/modcache and
//     loaded from there (a readme and screenshots stay in the zip); a zip with no pk3 in it but the folders of the game
//     (models, textures, sound...) at its top is loaded as a pk3 itself;
//   - a .pk3;
//   - a folder: the pk3 files in it, and its own files when it has the folders of the game.
// <home>/base/mods.txt lists them in the order they are loaded, top to bottom (a later one wins where two hold the same
// file). A line beginning with - is a package switched off; [sp] or [mp] after the name loads it only in single player
// or only in multiplayer. Packages found that are not on the list are added at its end, switched on; the ones gone are
// taken off. The single player Mods page edits it (FS_Packages_*). The single and multiplayer games of one game share
// their home folder, and so the list.
//
// A package may describe itself in a modinfo.txt (at the top of the zip, folder or pk3), lines of "key: value":
//   name, version, author, description, game (jo or ja: not loaded by the other game), scope (sp, mp or both),
//   requires (the names of other packages, by commas: a warning when one is not loaded before it).
// Without one, a readme in it gives the name (its first line), and "Author:" or "Version:" lines when it has them.
//
// CONFLICTS. fs_conflicts lists the pk3 files that hold files of others, and which wins; fs_mods the packages.
//
// EVERY COPY. FS_ReadFileVersions hands over each copy of a file, the lowest in priority first. The string tables
// (strings/<language>/*.str, strip/*.sp) are read so, one over the other, so that a mod that adds or changes a few lines
// of a table no longer takes away every line another mod (or a translation) gave it.

#pragma once

#include <sys/stat.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

#ifdef JK2_MODE
#define FS_PACKAGES_GAME	"jo"
#else
#define FS_PACKAGES_GAME	"ja"
#endif

#define FS_PKG_SP			1
#define FS_PKG_MP			2
#define FS_PKG_BOTH			( FS_PKG_SP | FS_PKG_MP )
#define FS_PKG_THIS			( FS_PACKAGES_MULTIPLAYER ? FS_PKG_MP : FS_PKG_SP )

enum { FS_PKG_PK3, FS_PKG_ZIP_PAKS, FS_PKG_ZIP_FILES, FS_PKG_FOLDER };

#define FS_MAX_PACKAGES		256

typedef struct {
	char	name[MAX_OSPATH];		// as in mods.txt: its name in a mods folder
	char	folder[MAX_OSPATH];		// the mods folder it is in
	int		kind;					// FS_PKG_PK3...
	bool	enabled;
	int		scope;					// FS_PKG_SP | FS_PKG_MP
	bool	scopeListed;			// the scope was written in mods.txt (else it comes from modinfo.txt, else both)
	bool	loaded;					// its files are in the search path now
	bool	otherGame;				// modinfo.txt says it is for the other game
	int		numPaks;				// pk3 files of it loaded
	char	title[128], version[64], author[128], description[512], needs[256];
} fsPackage_t;

static fsPackage_t	fs_packages[FS_MAX_PACKAGES];
static int			fs_numPackages;
static std::unordered_map<const pack_t *, int>	fs_packagePaks;	// the pk3 files loaded from packages: which package

static const char *const fs_packageAssetDirs[] = {
	"models/", "textures/", "gfx/", "sound/", "music/", "maps/", "ext_data/", "ui/", "strings/", "strip/", "shaders/",
	"scripts/", "effects/", "levelshots/", "video/", "fonts/", "menu/", "botfiles/", "anims/", "skins/", "env/",
	"chars/", "mp/", "cfg/", "demos/", "sprites/", "icons/", "players/",
};

static bool FS_Packages_IsAssetPath( const char *name )
{
	for ( size_t i = 0; i < ARRAY_LEN( fs_packageAssetDirs ); i++ )
	{
		if ( !Q_stricmpn( name, fs_packageAssetDirs[i], (int)strlen( fs_packageAssetDirs[i] ) ) )
			return true;
	}
	return false;
}

static bool FS_Packages_HasExt( const char *name, const char *ext )
{
	const size_t n = strlen( name ), e = strlen( ext );
	return n > e && !Q_stricmp( name + n - e, ext );
}

static const char *FS_Packages_BaseName( const char *path )
{
	const char *slash = strrchr( path, '/' ), *back = strrchr( path, '\\' );
	if ( back && ( !slash || back > slash ) )
		slash = back;
	return slash ? slash + 1 : path;
}

static void FS_Packages_Trim( char *s )
{
	char *p = s;
	while ( *p == ' ' || *p == '\t' )
		p++;
	if ( p != s )
		memmove( s, p, strlen( p ) + 1 );
	size_t n = strlen( s );
	while ( n && ( s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n' ) )
		s[--n] = '\0';
}

// the whole of the file at name in an open zip (NUL terminated), or empty
static std::string FS_Packages_ZipRead( unzFile z, const char *name )
{
	std::string out;
	if ( unzLocateFile( z, name, 2 ) != UNZ_OK )
		return out;
	unz_file_info info;
	if ( unzGetCurrentFileInfo( z, &info, NULL, 0, NULL, 0, NULL, 0 ) != UNZ_OK || info.uncompressed_size > 1024 * 1024 )
		return out;
	if ( unzOpenCurrentFile( z ) != UNZ_OK )
		return out;
	out.resize( info.uncompressed_size );
	const int got = info.uncompressed_size ? unzReadCurrentFile( z, &out[0], (unsigned)info.uncompressed_size ) : 0;
	unzCloseCurrentFile( z );
	if ( got < 0 )
		out.clear();
	else
		out.resize( got );
	return out;
}

static std::string FS_Packages_ReadOSFile( const char *ospath, size_t maxLen )
{
	std::string out;
	FILE *f = fopen( ospath, "rb" );
	if ( !f )
		return out;
	char buf[4096];
	size_t n;
	while ( out.size() < maxLen && ( n = fread( buf, 1, sizeof( buf ), f ) ) > 0 )
		out.append( buf, n );
	fclose( f );
	return out;
}

// modinfo.txt ("key: value" lines)
static void FS_Packages_ParseInfo( fsPackage_t *pkg, const std::string &text )
{
	size_t pos = 0;
	while ( pos < text.size() )
	{
		size_t end = text.find( '\n', pos );
		if ( end == std::string::npos )
			end = text.size();
		char line[1024];
		Q_strncpyz( line, text.substr( pos, end - pos ).c_str(), sizeof( line ) );
		pos = end + 1;
		FS_Packages_Trim( line );
		char *colon = strchr( line, ':' );
		if ( !line[0] || line[0] == '#' || !colon )
			continue;
		*colon = '\0';
		char *value = colon + 1;
		FS_Packages_Trim( line );
		FS_Packages_Trim( value );
		if ( !Q_stricmp( line, "name" ) )
			Q_strncpyz( pkg->title, value, sizeof( pkg->title ) );
		else if ( !Q_stricmp( line, "version" ) )
			Q_strncpyz( pkg->version, value, sizeof( pkg->version ) );
		else if ( !Q_stricmp( line, "author" ) )
			Q_strncpyz( pkg->author, value, sizeof( pkg->author ) );
		else if ( !Q_stricmp( line, "description" ) )
			Q_strncpyz( pkg->description, value, sizeof( pkg->description ) );
		else if ( !Q_stricmp( line, "requires" ) )
			Q_strncpyz( pkg->needs, value, sizeof( pkg->needs ) );
		else if ( !Q_stricmp( line, "game" ) )
			pkg->otherGame = Q_stricmp( value, FS_PACKAGES_GAME ) && Q_stricmp( value, "both" ) &&
				!( !Q_stricmp( FS_PACKAGES_GAME, "jo" ) && ( !Q_stricmp( value, "jk2" ) || !Q_stricmp( value, "jedi outcast" ) ) ) &&
				!( !Q_stricmp( FS_PACKAGES_GAME, "ja" ) && ( !Q_stricmp( value, "jka" ) || !Q_stricmp( value, "jedi academy" ) ) );
		else if ( !Q_stricmp( line, "scope" ) && !pkg->scopeListed )
			pkg->scope = !Q_stricmp( value, "sp" ) ? FS_PKG_SP : !Q_stricmp( value, "mp" ) ? FS_PKG_MP : FS_PKG_BOTH;
	}
}

// a readme: "Title:" / "Name:", "Author:", "Version:" and "Description:" lines; the first line that says something of
// its own (not the game's name, not "readme", not a line of stars) as the description. Without a name the package keeps
// its file name, which says more than the headings readmes begin with.
static bool FS_Packages_IsGenericLine( const char *line )
{
	static const char *const generic[] = { "jedi knight", "jedi academy", "jedi outcast", "readme", "read me", "read_me",
		"modification", "installation", "install", "copyright", "credits", "disclaimer", "contents", "------", "======", "******" };
	char lower[512];
	Q_strncpyz( lower, line, sizeof( lower ) );
	Q_strlwr( lower );
	for ( size_t i = 0; i < ARRAY_LEN( generic ); i++ )
	{
		if ( strstr( lower, generic[i] ) )
			return true;
	}
	int letters = 0;
	for ( const char *c = line; *c; c++ )
		letters += ( *c >= 'a' && *c <= 'z' ) || ( *c >= 'A' && *c <= 'Z' );
	return letters < 4 || line[strlen( line ) - 1] == ':';
}

static void FS_Packages_ParseReadme( fsPackage_t *pkg, const std::string &text )
{
	size_t pos = 0;
	int lines = 0;
	char firstLine[512] = "";
	while ( pos < text.size() && lines < 80 )
	{
		size_t end = text.find_first_of( "\r\n", pos );
		if ( end == std::string::npos )
			end = text.size();
		char line[512];
		Q_strncpyz( line, text.substr( pos, end - pos ).c_str(), sizeof( line ) );
		pos = end + 1;
		lines++;
		// (the stars, slashes and dashes around headings)
		FS_Packages_Trim( line );
		size_t n = strlen( line );
		while ( n && strchr( "*/=-~#|", line[n - 1] ) )
			line[--n] = '\0';
		char *start = line;
		while ( *start && strchr( "*/=-~#| \t", *start ) )
			start++;
		if ( !*start )
			continue;
		const char *colon = strchr( start, ':' );
		const char *value = colon ? colon + 1 : NULL;
		while ( value && ( *value == ' ' || *value == '\t' ) )
			value++;
		const int keyLen = colon ? (int)( colon - start ) : 0;
		if ( value && *value && keyLen < 16 )
		{
			if ( !Q_stricmpn( start, "author", 6 ) || !Q_stricmpn( start, "by", 2 ) || !Q_stricmpn( start, "created by", 10 ) )
			{
				if ( !pkg->author[0] )
					Q_strncpyz( pkg->author, value, sizeof( pkg->author ) );
				continue;
			}
			if ( !Q_stricmpn( start, "version", 7 ) )
			{
				if ( !pkg->version[0] )
					Q_strncpyz( pkg->version, value, sizeof( pkg->version ) );
				continue;
			}
			if ( !Q_stricmpn( start, "title", 5 ) || !Q_stricmpn( start, "name", 4 ) || !Q_stricmpn( start, "mod name", 8 ) ||
				 !Q_stricmpn( start, "file name", 9 ) || !Q_stricmpn( start, "filename", 8 ) )
			{
				if ( !pkg->title[0] && !FS_Packages_HasExt( value, ".pk3" ) && !FS_Packages_HasExt( value, ".zip" ) )
					Q_strncpyz( pkg->title, value, sizeof( pkg->title ) );
				continue;
			}
			if ( !Q_stricmpn( start, "description", 11 ) )
			{
				if ( !pkg->description[0] )
					Q_strncpyz( pkg->description, value, sizeof( pkg->description ) );
				continue;
			}
		}
		if ( !firstLine[0] && !FS_Packages_IsGenericLine( start ) )
			Q_strncpyz( firstLine, start, sizeof( firstLine ) );
	}
	if ( !pkg->description[0] && firstLine[0] )
		Q_strncpyz( pkg->description, firstLine, sizeof( pkg->description ) );
}

static bool FS_Packages_IsReadme( const char *name )
{
	const char *base = FS_Packages_BaseName( name );
	return FS_Packages_HasExt( base, ".txt" ) && ( !Q_stricmpn( base, "readme", 6 ) || !Q_stricmpn( base, "read me", 7 ) ||
		!Q_stricmpn( base, "read_me", 7 ) );
}

// what the zip holds: its pk3 files (names in it), and whether it has the game's folders at its top; and its description
static int FS_Packages_ScanZip( const char *ospath, fsPackage_t *pkg, std::vector<std::string> *paks )
{
	unzFile z = unzOpen( ospath );
	if ( !z )
		return -1;
	bool assets = false;
	std::string readme;
	for ( int r = unzGoToFirstFile( z ); r == UNZ_OK; r = unzGoToNextFile( z ) )
	{
		char name[MAX_OSPATH];
		unz_file_info info;
		if ( unzGetCurrentFileInfo( z, &info, name, sizeof( name ), NULL, 0, NULL, 0 ) != UNZ_OK )
			continue;
		if ( FS_Packages_HasExt( name, ".pk3" ) )
			paks->push_back( name );
		else if ( FS_Packages_IsAssetPath( name ) )
			assets = true;
		else if ( readme.empty() && FS_Packages_IsReadme( name ) )
			readme = name;
	}
	std::string info = FS_Packages_ZipRead( z, "modinfo.txt" );
	if ( !info.empty() )
		FS_Packages_ParseInfo( pkg, info );
	else if ( !readme.empty() )
		FS_Packages_ParseReadme( pkg, FS_Packages_ZipRead( z, readme.c_str() ) );
	unzClose( z );
	return !paks->empty() ? FS_PKG_ZIP_PAKS : assets ? FS_PKG_ZIP_FILES : -1;
}

// the pk3 files of a zip, copied into <home>/base/modcache/<zip>/ unless they are there already (same size and time)
static bool FS_Packages_Extract( const char *zipPath, const char *cacheDir, const std::vector<std::string> &paks,
	std::vector<std::string> *out )
{
	struct stat st;
	if ( stat( zipPath, &st ) != 0 )
		return false;
	char stamp[64];
	Com_sprintf( stamp, sizeof( stamp ), "%lld %lld", (long long)st.st_size, (long long)st.st_mtime );

	char stampPath[MAX_OSPATH];
	Com_sprintf( stampPath, sizeof( stampPath ), "%s%c.stamp", cacheDir, PATH_SEP );
	const bool fresh = FS_Packages_ReadOSFile( stampPath, 64 ) == stamp;

	unzFile z = fresh ? NULL : unzOpen( zipPath );
	if ( !fresh && !z )
		return false;
	for ( size_t i = 0; i < paks.size(); i++ )
	{
		char dest[MAX_OSPATH];
		Com_sprintf( dest, sizeof( dest ), "%s%c%s", cacheDir, PATH_SEP, FS_Packages_BaseName( paks[i].c_str() ) );
		out->push_back( dest );
		if ( fresh )
			continue;
		FS_CreatePath( dest );
		FILE *f = fopen( dest, "wb" );
		bool ok = f && unzLocateFile( z, paks[i].c_str(), 1 ) == UNZ_OK && unzOpenCurrentFile( z ) == UNZ_OK;
		if ( ok )
		{
			char buf[65536];
			int n;
			while ( ( n = unzReadCurrentFile( z, buf, sizeof( buf ) ) ) > 0 )
				ok &= fwrite( buf, 1, n, f ) == (size_t)n;
			ok &= n == 0;
			unzCloseCurrentFile( z );
		}
		if ( f )
			fclose( f );
		if ( !ok )
		{
			Com_Printf( S_COLOR_YELLOW "Mods: could not copy %s out of %s\n", paks[i].c_str(), zipPath );
			unzClose( z );
			return false;
		}
	}
	if ( z )
	{
		unzClose( z );
		FILE *f = fopen( stampPath, "wb" );
		if ( f )
		{
			fputs( stamp, f );
			fclose( f );
		}
	}
	return true;
}

static void FS_Packages_AddPak( const char *ospath, int package )
{
	pack_t *pak = FS_LoadZipFile( ospath, FS_Packages_BaseName( ospath ) );
	if ( !pak )
		return;
	char dir[MAX_OSPATH];
	Q_strncpyz( dir, ospath, sizeof( dir ) );
	char *end = (char *)FS_Packages_BaseName( dir );
	if ( end > dir )
		end[-1] = '\0';
	Q_strncpyz( pak->pakPathname, dir, sizeof( pak->pakPathname ) );
	Q_strncpyz( pak->pakGamename, BASEGAME, sizeof( pak->pakGamename ) );	// (as a pk3 in base, for pure servers)
	fs_packFiles += pak->numfiles;
	searchpath_t *search = (searchpath_t *)Z_Malloc( sizeof( searchpath_t ), TAG_FILESYS, qtrue );
	search->pack = pak;
	search->next = fs_searchpaths;
	fs_searchpaths = search;
	fs_packagePaks[pak] = package;
	fs_packages[package].numPaks++;
}

static void FS_Packages_AddDir( const char *folder, const char *name )
{
	searchpath_t *search = (searchpath_t *)Z_Malloc( sizeof( searchpath_t ), TAG_FILESYS, qtrue );
	search->dir = (directory_t *)Z_Malloc( sizeof( *search->dir ), TAG_FILESYS, qtrue );
	Q_strncpyz( search->dir->path, folder, sizeof( search->dir->path ) );
	Q_strncpyz( search->dir->fullpath, FS_BuildOSPath( folder, name, "" ), sizeof( search->dir->fullpath ) );
	Q_strncpyz( search->dir->gamedir, name, sizeof( search->dir->gamedir ) );
	search->next = fs_searchpaths;
	fs_searchpaths = search;
}

static int FS_Packages_Find( const char *name )
{
	for ( int i = 0; i < fs_numPackages; i++ )
	{
		if ( !Q_stricmp( fs_packages[i].name, name ) )
			return i;
	}
	return -1;
}

static void FS_Packages_ListPath( char *out, size_t size )
{
	Com_sprintf( out, (int)size, "%s", FS_BuildOSPath( fs_homepath->string, BASEGAME, "mods.txt" ) );
}

// mods.txt, as the packages are now
void FS_Packages_Save( void )
{
	char path[MAX_OSPATH];
	FS_Packages_ListPath( path, sizeof( path ) );
	FS_CreatePath( path );
	FILE *f = fopen( path, "wb" );
	if ( !f )
	{
		Com_Printf( S_COLOR_YELLOW "Mods: could not write %s\n", path );
		return;
	}
	fputs( "# The mods in the \"mods\" folders, loaded from top to bottom: where two hold the same file, the lower one wins.\n"
		"# A line beginning with - is switched off. [sp] or [mp] after a name: only in single player, or only in multiplayer.\n"
		"# The Mods page of the Setup menu edits this list; new mods are added at the end, switched on.\n", f );
	for ( int i = 0; i < fs_numPackages; i++ )
	{
		const fsPackage_t *p = &fs_packages[i];
		fprintf( f, "%s%s%s\n", p->enabled ? "" : "-", p->name,
			!p->scopeListed ? "" : p->scope == FS_PKG_SP ? " [sp]" : p->scope == FS_PKG_MP ? " [mp]" : " [sp+mp]" );
	}
	fclose( f );
}

// the packages in a mods folder, not listed yet (at the end, in the order of their names)
static void FS_Packages_Discover( const char *base )
{
	if ( !base || !base[0] )
		return;
	char folder[MAX_OSPATH];
	Q_strncpyz( folder, FS_BuildOSPath( base, "mods", "" ), sizeof( folder ) );
	folder[strlen( folder ) - 1] = '\0';	// (the trailing separator)

	static const char *const exts[] = { ".zip", ".pk3", "/" };
	for ( size_t e = 0; e < ARRAY_LEN( exts ); e++ )
	{
		int count = 0;
		char **list = Sys_ListFiles( folder, exts[e], NULL, &count, qfalse );
		if ( count > 1 )
			qsort( list, count, sizeof( char * ), paksort );
		for ( int i = 0; i < count && fs_numPackages < FS_MAX_PACKAGES; i++ )
		{
			if ( !list[i][0] || list[i][0] == '.' )
				continue;
			int at = FS_Packages_Find( list[i] );
			if ( at < 0 )
			{
				at = fs_numPackages++;
				memset( &fs_packages[at], 0, sizeof( fs_packages[at] ) );
				Q_strncpyz( fs_packages[at].name, list[i], sizeof( fs_packages[at].name ) );
				fs_packages[at].enabled = true;
				fs_packages[at].scope = FS_PKG_BOTH;
			}
			if ( !fs_packages[at].folder[0] )
				Q_strncpyz( fs_packages[at].folder, folder, sizeof( fs_packages[at].folder ) );
		}
		Sys_FreeFileList( list );
	}
}

static void FS_Packages_ReadList( void )
{
	char path[MAX_OSPATH];
	FS_Packages_ListPath( path, sizeof( path ) );
	std::string text = FS_Packages_ReadOSFile( path, 256 * 1024 );
	size_t pos = 0;
	while ( pos < text.size() && fs_numPackages < FS_MAX_PACKAGES )
	{
		size_t end = text.find( '\n', pos );
		if ( end == std::string::npos )
			end = text.size();
		char line[MAX_OSPATH + 32];
		Q_strncpyz( line, text.substr( pos, end - pos ).c_str(), sizeof( line ) );
		pos = end + 1;
		FS_Packages_Trim( line );
		if ( !line[0] || line[0] == '#' )
			continue;
		fsPackage_t *p = &fs_packages[fs_numPackages];
		memset( p, 0, sizeof( *p ) );
		char *name = line;
		p->enabled = true;
		if ( *name == '-' )
		{
			p->enabled = false;
			name++;
		}
		p->scope = FS_PKG_BOTH;
		size_t n = strlen( name );
		if ( n > 4 && name[n - 1] == ']' )
		{
			char *open = strrchr( name, '[' );
			if ( open )
			{
				p->scopeListed = true;
				p->scope = !Q_stricmp( open, "[sp]" ) ? FS_PKG_SP : !Q_stricmp( open, "[mp]" ) ? FS_PKG_MP : FS_PKG_BOTH;
				*open = '\0';
			}
		}
		FS_Packages_Trim( name );
		if ( name[0] && FS_Packages_Find( name ) < 0 )
		{
			Q_strncpyz( p->name, name, sizeof( p->name ) );
			fs_numPackages++;
		}
	}
}

/*
================
FS_Packages_Mount

After the pk3 files of base: the packages of the mods folders, as mods.txt orders them (FS_Startup).
================
*/
static void FS_Packages_Mount( void )
{
	fs_numPackages = 0;
	fs_packagePaks.clear();

	FS_Packages_ReadList();
	const int listed = fs_numPackages;
	FS_Packages_Discover( fs_basepath->string );
	if ( fs_homepath->string[0] && Q_stricmp( fs_homepath->string, fs_basepath->string ) )
		FS_Packages_Discover( fs_homepath->string );

	// the ones listed that are gone
	bool changed = fs_numPackages != listed;
	for ( int i = 0; i < fs_numPackages; )
	{
		if ( !fs_packages[i].folder[0] )
		{
			memmove( &fs_packages[i], &fs_packages[i + 1], ( fs_numPackages - i - 1 ) * sizeof( fs_packages[0] ) );
			fs_numPackages--;
			changed = true;
		}
		else
			i++;
	}
	if ( !fs_numPackages )
		return;

	char cacheRoot[MAX_OSPATH];
	Q_strncpyz( cacheRoot, FS_BuildOSPath( fs_homepath->string, BASEGAME, "modcache" ), sizeof( cacheRoot ) );

	int loaded = 0;
	for ( int i = 0; i < fs_numPackages; i++ )
	{
		fsPackage_t *p = &fs_packages[i];
		char ospath[MAX_OSPATH];
		Com_sprintf( ospath, sizeof( ospath ), "%s%c%s", p->folder, PATH_SEP, p->name );
		std::vector<std::string> paks, files;

		// what it is, and what it says of itself
		if ( FS_Packages_HasExt( p->name, ".zip" ) )
		{
			const int kind = FS_Packages_ScanZip( ospath, p, &paks );
			if ( kind < 0 )
			{
				Com_Printf( S_COLOR_YELLOW "Mods: %s holds no pk3 files and no game folders, it is not loaded\n", p->name );
				continue;
			}
			p->kind = kind;
		}
		else if ( FS_Packages_HasExt( p->name, ".pk3" ) )
		{
			p->kind = FS_PKG_PK3;
			unzFile z = unzOpen( ospath );
			if ( z )
			{
				std::string info = FS_Packages_ZipRead( z, "modinfo.txt" );
				if ( !info.empty() )
					FS_Packages_ParseInfo( p, info );
				unzClose( z );
			}
		}
		else
		{
			p->kind = FS_PKG_FOLDER;
			char infoPath[MAX_OSPATH];
			Com_sprintf( infoPath, sizeof( infoPath ), "%s%cmodinfo.txt", ospath, PATH_SEP );
			std::string info = FS_Packages_ReadOSFile( infoPath, 64 * 1024 );
			if ( !info.empty() )
				FS_Packages_ParseInfo( p, info );
			else
			{
				int count = 0;
				char **list = Sys_ListFiles( ospath, ".txt", NULL, &count, qfalse );
				for ( int k = 0; k < count; k++ )
				{
					if ( FS_Packages_IsReadme( list[k] ) )
					{
						Com_sprintf( infoPath, sizeof( infoPath ), "%s%c%s", ospath, PATH_SEP, list[k] );
						FS_Packages_ParseReadme( p, FS_Packages_ReadOSFile( infoPath, 64 * 1024 ) );
						break;
					}
				}
				Sys_FreeFileList( list );
			}
		}
		if ( !p->title[0] )
		{
			Q_strncpyz( p->title, p->name, sizeof( p->title ) );
			char *dot = strrchr( p->title, '.' );
			if ( dot && p->kind != FS_PKG_FOLDER )
				*dot = '\0';
		}

		if ( !p->enabled || !( p->scope & FS_PKG_THIS ) )
			continue;
		if ( p->otherGame )
		{
			Com_Printf( S_COLOR_YELLOW "Mods: %s is for the other game (modinfo.txt), it is not loaded\n", p->name );
			continue;
		}

		// its files
		switch ( p->kind )
		{
		case FS_PKG_PK3:
			FS_Packages_AddPak( ospath, i );
			break;
		case FS_PKG_ZIP_FILES:
			FS_Packages_AddPak( ospath, i );
			break;
		case FS_PKG_ZIP_PAKS:
		{
			char cacheDir[MAX_OSPATH];
			Com_sprintf( cacheDir, sizeof( cacheDir ), "%s%c%s", cacheRoot, PATH_SEP, p->name );
			std::vector<std::string> copied;
			if ( FS_Packages_Extract( ospath, cacheDir, paks, &copied ) )
			{
				std::vector<const char *> sorted;
				for ( size_t k = 0; k < copied.size(); k++ )
					sorted.push_back( copied[k].c_str() );
				std::sort( sorted.begin(), sorted.end(), []( const char *a, const char *b ) {
					return FS_PathCmp( FS_Packages_BaseName( a ), FS_Packages_BaseName( b ) ) < 0; } );
				for ( size_t k = 0; k < sorted.size(); k++ )
					FS_Packages_AddPak( sorted[k], i );
			}
			break;
		}
		case FS_PKG_FOLDER:
		{
			int count = 0;
			char **list = Sys_ListFiles( ospath, ".pk3", NULL, &count, qfalse );
			if ( count > 1 )
				qsort( list, count, sizeof( char * ), paksort );
			for ( int k = 0; k < count; k++ )
			{
				char pak[MAX_OSPATH];
				Com_sprintf( pak, sizeof( pak ), "%s%c%s", ospath, PATH_SEP, list[k] );
				FS_Packages_AddPak( pak, i );
			}
			Sys_FreeFileList( list );
			// its own files, when it has the game's folders
			list = Sys_ListFiles( ospath, "/", NULL, &count, qfalse );
			bool assets = false;
			for ( int k = 0; k < count && !assets; k++ )
				assets = FS_Packages_IsAssetPath( va( "%s/", list[k] ) );
			Sys_FreeFileList( list );
			if ( assets )
				FS_Packages_AddDir( p->folder, p->name );
			break;
		}
		}
		p->loaded = p->numPaks > 0 || p->kind == FS_PKG_FOLDER;
		loaded += p->loaded ? 1 : 0;
	}

	// what each needs (modinfo.txt "requires"), loaded before it
	for ( int i = 0; i < fs_numPackages; i++ )
	{
		const fsPackage_t *p = &fs_packages[i];
		if ( !p->loaded || !p->needs[0] )
			continue;
		char list[sizeof( p->needs )];
		Q_strncpyz( list, p->needs, sizeof( list ) );
		for ( char *tok = strtok( list, "," ); tok; tok = strtok( NULL, "," ) )
		{
			FS_Packages_Trim( tok );
			bool found = false;
			for ( int k = 0; k < i && !found; k++ )
			{
				const fsPackage_t *q = &fs_packages[k];
				char stem[MAX_OSPATH];
				Q_strncpyz( stem, q->name, sizeof( stem ) );
				char *dot = strrchr( stem, '.' );
				if ( dot && q->kind != FS_PKG_FOLDER )
					*dot = '\0';
				found = q->loaded && ( !Q_stricmp( tok, q->name ) || !Q_stricmp( tok, stem ) || !Q_stricmp( tok, q->title ) );
			}
			if ( tok[0] && !found )
				Com_Printf( S_COLOR_YELLOW "Mods: %s needs %s, which is not loaded before it\n", p->name, tok );
		}
	}

	if ( changed )
		FS_Packages_Save();
	Com_Printf( "Mods: %d of %d in the mods folders loaded (fs_mods lists them, fs_conflicts what they replace)\n", loaded,
		fs_numPackages );
}

// for the Mods page (single player)
int FS_Packages_Count( void )
{
	return fs_numPackages;
}

const fsPackage_t *FS_Packages_Get( int i )
{
	return ( i >= 0 && i < fs_numPackages ) ? &fs_packages[i] : NULL;
}

void FS_Packages_Toggle( int i )
{
	if ( i >= 0 && i < fs_numPackages )
	{
		fs_packages[i].enabled = !fs_packages[i].enabled;
		FS_Packages_Save();
	}
}

// moves it up (-1) or down (1) the list; where it is now
int FS_Packages_Move( int i, int delta )
{
	const int to = i + delta;
	if ( i < 0 || i >= fs_numPackages || to < 0 || to >= fs_numPackages )
		return i;
	fsPackage_t t = fs_packages[i];
	fs_packages[i] = fs_packages[to];
	fs_packages[to] = t;
	FS_Packages_Save();
	return to;
}

// the original game's own pk3 files (assets*.pk3 in base): what replaces their files is normal for a mod
static bool FS_Packages_IsGamePak( const pack_t *pak )
{
	return !Q_stricmp( pak->pakGamename, BASEGAME ) && !Q_stricmpn( pak->pakBasename, "assets", 6 ) &&
		fs_packagePaks.find( pak ) == fs_packagePaks.end();
}

static bool FS_Packages_IsMergedFile( const char *name )
{
	return ( !Q_stricmpn( name, "strings/", 8 ) && FS_Packages_HasExt( name, ".str" ) ) ||
		( !Q_stricmpn( name, "strip/", 6 ) && FS_Packages_HasExt( name, ".sp" ) );
}

static void FS_Packages_PakLabel( const pack_t *pak, char *out, int size )
{
	const auto it = fs_packagePaks.find( pak );
	if ( it != fs_packagePaks.end() )
		Com_sprintf( out, size, "%s (%s)", pak->pakBasename, fs_packages[it->second].name );
	else
		Com_sprintf( out, size, "%s/%s", pak->pakGamename, pak->pakBasename );
}

typedef struct {
	const pack_t	*winner, *loser;
	int				count;
	char			example[MAX_QPATH];
} fsConflict_t;

// which pk3 holds files of which other (the one loaded later wins); text tables are merged, not counted
static std::vector<fsConflict_t> FS_Packages_Conflicts( void )
{
	std::vector<const pack_t *> paks;	// lowest priority first
	for ( searchpath_t *s = fs_searchpaths; s; s = s->next )
	{
		if ( s->pack )
			paks.insert( paks.begin(), s->pack );
	}
	std::unordered_map<std::string, const pack_t *> owner;
	std::vector<fsConflict_t> out;
	for ( size_t i = 0; i < paks.size(); i++ )
	{
		const pack_t *pak = paks[i];
		for ( int f = 0; f < pak->numfiles; f++ )
		{
			const char *name = pak->buildBuffer[f].name;
			if ( !name || !name[0] || name[strlen( name ) - 1] == '/' || FS_Packages_IsMergedFile( name ) )
				continue;
			const pack_t *&prev = owner[name];
			if ( prev && prev != pak && !( FS_Packages_IsGamePak( prev ) && FS_Packages_IsGamePak( pak ) ) )
			{
				size_t k = 0;
				while ( k < out.size() && !( out[k].winner == pak && out[k].loser == prev ) )
					k++;
				if ( k == out.size() )
				{
					fsConflict_t c = { pak, prev, 0, "" };
					Q_strncpyz( c.example, name, sizeof( c.example ) );
					out.push_back( c );
				}
				out[k].count++;
			}
			prev = pak;
		}
	}
	return out;
}

// for the Mods page: what the package replaces, in a line
void FS_Packages_ConflictText( int package, char *buf, int size )
{
	buf[0] = '\0';
	const std::vector<fsConflict_t> all = FS_Packages_Conflicts();
	int ofGame = 0, ofOthers = 0, lost = 0;
	const char *loser = NULL, *winner = NULL;
	for ( size_t i = 0; i < all.size(); i++ )
	{
		const auto w = fs_packagePaks.find( all[i].winner ), l = fs_packagePaks.find( all[i].loser );
		if ( w != fs_packagePaks.end() && w->second == package )
		{
			if ( FS_Packages_IsGamePak( all[i].loser ) )
				ofGame += all[i].count;
			else
			{
				ofOthers += all[i].count;
				loser = all[i].loser->pakBasename;
			}
		}
		if ( l != fs_packagePaks.end() && l->second == package && ( w == fs_packagePaks.end() || w->second != package ) )
		{
			lost += all[i].count;
			winner = all[i].winner->pakBasename;
		}
	}
	if ( ofGame )
		Q_strcat( buf, size, va( "Replaces %d files of the game. ", ofGame ) );
	if ( ofOthers )
		Q_strcat( buf, size, va( "Replaces %d files of other mods (%s%s). ", ofOthers, loser, ofOthers > 1 ? "..." : "" ) );
	if ( lost )
		Q_strcat( buf, size, va( "%d of its files are replaced by %s. ", lost, winner ) );
}

static void FS_Conflicts_f( void )
{
	const std::vector<fsConflict_t> all = FS_Packages_Conflicts();
	int shown = 0;
	for ( size_t i = 0; i < all.size(); i++ )
	{
		if ( FS_Packages_IsGamePak( all[i].loser ) && Cmd_Argc() < 2 )
			continue;	// (replacing the game's own files is what most mods do: "fs_conflicts all" shows those too)
		char winner[MAX_OSPATH * 2], loser[MAX_OSPATH * 2];
		FS_Packages_PakLabel( all[i].winner, winner, sizeof( winner ) );
		FS_Packages_PakLabel( all[i].loser, loser, sizeof( loser ) );
		Com_Printf( "%s replaces %d file%s of %s, e.g. %s\n", winner, all[i].count, all[i].count == 1 ? "" : "s", loser,
			all[i].example );
		shown++;
	}
	Com_Printf( "%d pk3 files replace files of another mod%s; text tables (strings, strip) are merged, not replaced.\n",
		shown, Cmd_Argc() < 2 ? " (fs_conflicts all: also of the game's own pk3 files)" : " or of the game" );
}

static void FS_Mods_f( void )
{
	if ( !fs_numPackages )
	{
		Com_Printf( "No mods in the mods folders (%s/mods, %s/mods).\n", fs_basepath->string, fs_homepath->string );
		return;
	}
	for ( int i = 0; i < fs_numPackages; i++ )
	{
		const fsPackage_t *p = &fs_packages[i];
		char line[1024];
		Com_sprintf( line, sizeof( line ), "%2d %-3s %s%s", i + 1, p->enabled ? "on" : "off", p->name,
			p->scope == FS_PKG_SP ? " [sp]" : p->scope == FS_PKG_MP ? " [mp]" : "" );
		if ( Q_stricmp( p->title, p->name ) )
			Q_strcat( line, sizeof( line ), va( " - %s", p->title ) );
		if ( p->version[0] )
			Q_strcat( line, sizeof( line ), va( " %s", p->version ) );
		if ( p->author[0] )
			Q_strcat( line, sizeof( line ), va( " by %s", p->author ) );
		if ( p->numPaks )
			Q_strcat( line, sizeof( line ), va( " (%d pk3)", p->numPaks ) );
		if ( p->otherGame )
			Q_strcat( line, sizeof( line ), " (for the other game)" );
		else if ( p->enabled && ( p->scope & FS_PKG_THIS ) && !p->loaded )
			Q_strcat( line, sizeof( line ), " (not loaded yet: fs_restart)" );
		else if ( !p->enabled && p->loaded )
			Q_strcat( line, sizeof( line ), " (still loaded: fs_restart)" );
		Com_Printf( "%s\n", line );
	}
	Com_Printf( "The order and what is on: %s\n", FS_BuildOSPath( fs_homepath->string, BASEGAME, "mods.txt" ) );
}

/*
================
FS_ReadFileVersions

Every copy of a file in the search path, the lowest in priority first: callback( data (NUL terminated), length,
where ). How many there were.
================
*/
int FS_ReadFileVersions( const char *qpath, void ( *callback )( const char *data, int len, const char *where, void *user ),
	void *user )
{
	if ( !fs_searchpaths || !qpath || !qpath[0] )
		return 0;
	std::vector<searchpath_t *> found;	// highest priority first
	std::vector<const fileInPack_t *> entries;
	for ( searchpath_t *s = fs_searchpaths; s; s = s->next )
	{
		if ( s->pack )
		{
#if FS_PACKAGES_MULTIPLAYER
			if ( !FS_PakIsPure( s->pack ) )
				continue;
#endif
			const long hash = FS_HashFileName( qpath, s->pack->hashSize );
			for ( fileInPack_t *f = s->pack->hashTable[hash]; f; f = f->next )
			{
				if ( !FS_FilenameCompare( f->name, qpath ) )
				{
					found.push_back( s );
					entries.push_back( f );
					break;
				}
			}
		}
		else if ( s->dir )
		{
			FILE *f = fopen( FS_BuildOSPath( s->dir->path, s->dir->gamedir, qpath ), "rb" );
			if ( f )
			{
				fclose( f );
				found.push_back( s );
				entries.push_back( NULL );
			}
		}
	}
	for ( size_t i = found.size(); i-- > 0; )
	{
		std::string data;
		const char *where;
		if ( found[i]->pack )
		{
			unzFile z = unzOpen( found[i]->pack->pakFilename );
			if ( !z )
				continue;
			// (where the file is in it, as the pack's own table says: no search through it)
			if ( unzSetOffset( z, entries[i]->pos ) == UNZ_OK && unzOpenCurrentFile( z ) == UNZ_OK )
			{
				data.resize( entries[i]->len );
				const int got = entries[i]->len ? unzReadCurrentFile( z, &data[0], (unsigned)entries[i]->len ) : 0;
				data.resize( got > 0 ? got : 0 );
				unzCloseCurrentFile( z );
			}
			unzClose( z );
			where = found[i]->pack->pakBasename;
		}
		else
		{
			data = FS_Packages_ReadOSFile( FS_BuildOSPath( found[i]->dir->path, found[i]->dir->gamedir, qpath ), 16 * 1024 * 1024 );
			where = found[i]->dir->gamedir;
		}
		callback( data.c_str(), (int)data.size(), where, user );
	}
	return (int)found.size();
}

/*
================
FS_FileSource

Where the file is read from (the pk3 or folder name, "" when nowhere); isGame: one of the game's own pk3 files.
================
*/
const char *FS_FileSource( const char *qpath, qboolean *isGame )
{
	*isGame = qfalse;
	for ( searchpath_t *s = fs_searchpaths; s; s = s->next )
	{
		if ( s->pack )
		{
#if FS_PACKAGES_MULTIPLAYER
			if ( !FS_PakIsPure( s->pack ) )
				continue;
#endif
			const long hash = FS_HashFileName( qpath, s->pack->hashSize );
			for ( fileInPack_t *f = s->pack->hashTable[hash]; f; f = f->next )
			{
				if ( !FS_FilenameCompare( f->name, qpath ) )
				{
					*isGame = FS_Packages_IsGamePak( s->pack ) ? qtrue : qfalse;
					return s->pack->pakBasename;
				}
			}
		}
		else if ( s->dir )
		{
			FILE *f = fopen( FS_BuildOSPath( s->dir->path, s->dir->gamedir, qpath ), "rb" );
			if ( f )
			{
				fclose( f );
				return s->dir->gamedir;
			}
		}
	}
	return "";
}

/*
================
FS_Packages_Describe

For the Mods page: the package's line in the list, and what it says when chosen. qfalse past the last one.
================
*/
qboolean FS_Packages_Describe( int i, char *row, int rowSize, char *desc, int descSize )
{
	const fsPackage_t *p = FS_Packages_Get( i );
	if ( !p )
		return qfalse;
	const char *state = !p->enabled ? "Off" : p->otherGame ? "--" : "On";
	Com_sprintf( row, rowSize, "%s\t%s%s%s%s", state, p->title, p->version[0] ? " " : "", p->version,
		p->scope == FS_PKG_SP ? " (SP only)" : p->scope == FS_PKG_MP ? " (MP only)" : "" );

	desc[0] = '\0';
	Q_strcat( desc, descSize, p->title );
	if ( p->author[0] )
		Q_strcat( desc, descSize, va( " by %s", p->author ) );
	Q_strcat( desc, descSize, va( " (%s). ", p->name ) );
	if ( p->description[0] )
		Q_strcat( desc, descSize, va( "%s ", p->description ) );
	if ( p->otherGame )
		Q_strcat( desc, descSize, "Made for the other game: not loaded. " );
	else if ( p->enabled != p->loaded && ( p->scope & FS_PKG_THIS ) )
		Q_strcat( desc, descSize, p->enabled ? "On after Apply. " : "Off after Apply. " );
	if ( p->loaded )
	{
		char conflicts[512];
		FS_Packages_ConflictText( i, conflicts, sizeof( conflicts ) );
		Q_strcat( desc, descSize, conflicts );
	}
	return qtrue;
}
