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

// snd_local.h -- private sound definations

#ifndef SND_LOCAL_H
#define SND_LOCAL_H

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "snd_public.h"
#include "snd_resample.h"
#include "snd_hrtf.h"
#include "snd_reverb.h"
#include "../mp3code/mp3struct.h"


//from SND_AMBIENT
extern void AS_Init( void );
extern void AS_Free( void );


#define	PAINTBUFFER_SIZE	1024


// !!! if this is changed, the asm code must change !!!
typedef struct {
	int			left;	// the final values will be clamped to +/- 0x00ffff00 and shifted down
	int			right;
} portable_samplepair_t;


// keep this enum in sync with the table "sSoundCompressionMethodStrings"	-ste
//
typedef enum
{
	ct_16 = 0,		// formerly ct_NONE in EF1, now indicates 16-bit samples (the default)
	ct_MP3,
	//
	ct_NUMBEROF		// used only for array sizing

} SoundCompressionMethod_t;


typedef struct sfx_s {
	short			*pSoundData;
	bool			bDefaultSound;			// couldn't be loaded, so use buzz
	bool			bInMemory;				// not in Memory, set qtrue when loaded, and qfalse when its buffers are freed up because of being old, so can be reloaded
	short			iLastLevelUsedOn;		// used for cacheing purposes
	SoundCompressionMethod_t eSoundCompressionMethod;
	MP3STREAM		*pMP3StreamHeader;		// NULL ptr unless this sfx_t is an MP3. Use Z_Malloc and Z_Free
	int 			iSoundLengthInSamples;	// length in samples, always kept as 16bit now so this is #shorts (watch for stereo later for music?)
	char 			sSoundName[MAX_QPATH];
	int				iLastTimeUsed;
	float			fVolRange;				// used to set the highest volume this sample has at load time - used for lipsynching

	struct sfx_s	*next;					// only used because of hash table when registering
} sfx_t;

typedef struct {
	int			channels;
	int			samples;				// mono samples in buffer
	int			submission_chunk;		// don't mix less than this #
	int			samplebits;
	int			speed;
	byte		*buffer;
} dma_t;


#define START_SAMPLE_IMMEDIATE	0x7fffffff


typedef struct
{
// back-indented fields new in TA codebase, will re-format when MP3 code finished -ste
// note: field missing in TA: qboolean	loopSound;		// from an S_AddLoopSound call, cleared each frame
//
	int				startSample;	// START_SAMPLE_IMMEDIATE = set immediately on next mix
	int				entnum;			// to allow overriding a specific sound
	soundChannel_t	entchannel;		// to allow overriding a specific sound
	int				leftvol;		// 0-255 volume after spatialization
	int				rightvol;		// 0-255 volume after spatialization
	int				master_vol;		// 0-255 volume before spatialization


	vec3_t		origin;			// only use if fixed_origin is set

	qboolean	fixed_origin;	// use origin instead of fetching entnum's origin
	sfx_t		*thesfx;		// sfx structure
	qboolean	loopSound;		// from an S_AddLoopSound call, cleared each frame
	//
	MP3STREAM	MP3StreamHeader;
	byte		MP3SlidingDecodeBuffer[50000/*12000*/];	// typical back-request = -3072, so roughly double is 6000 (safety), then doubled again so the 6K pos is in the middle of the buffer)
	int			iMP3SlidingDecodeWritePos;
	int			iMP3SlidingDecodeWindowPos;
	//
	// binaural positioning (s_hrtf): the direction of the sound relative to the listener, set by S_Respatialize,
	// and the filter history of the sound. For a sound that persists (a one shot sound) the state lives in a pool
	// indexed by channel, for a looping sound (whose channel is made afresh every frame) in a table by sfx.
	qboolean	hrtf;
	qboolean	hrtfInit;		// the pool entry of a one shot sound has been cleared for this sound
	qboolean	hrtfKept;		// was among the sounds positioned binaurally after the last S_HrtfLimit
	float		hrtfAzimuth;	// degrees, 0 = ahead, positive to the right
	float		hrtfElevation;	// degrees, positive upwards
	hrtfState_t	*pHrtfState;
	//
	// room effects (s_reverb): how much of the sound is sent to the reverb of the room, how much the sound is held
	// back by something between it and the listener (0 to 1, and where that was heading and when it was last looked at),
	// and the state of the low pass filter that does that
	int			reverbvol;		// 0-255
	float		obstruct;
	float		obstructTarget;
	int			obstructNextTime;
	float		lpState;

} channel_t;


#define	WAV_FORMAT_PCM		1
#define WAV_FORMAT_ADPCM	2	// not actually implemented, but is the value that you get in a header
#define WAV_FORMAT_MP3		3	// not actually used this way, but just ensures we don't match one of the legit formats


typedef struct {
	int			format;
	int			rate;
	int			width;
	int			channels;
	int			samples;
	int			dataofs;		// chunk starts this many bytes from file start
} wavinfo_t;

//====================================================================

// Sounds that can play at once. (The stock 32 could run out in big fights; idle channels cost nothing, and with the
// headphone positioning on only the loudest HRTF_MAX_SOUNDS of them are filtered binaurally, see S_HrtfLimit.)
#define	MAX_CHANNELS			128
extern	channel_t   s_channels[MAX_CHANNELS];

extern	int		s_paintedtime;
extern	int		s_soundtime;		// the play position (sample pairs)
extern	int		s_rawend;
extern	vec3_t	listener_origin;
extern	dma_t	dma;

// Raw sample ring (cinematic audio, streamed music). Must be a power of two. It has to hold more than the first audio
// packet of a cinematic: those are ~0.5 seconds long, which is 23520 samples at 44.1kHz and overflowed the old 16384 ring.
#define	MAX_RAW_SAMPLES	65536
// How far ahead of the current sound time streamed music is buffered. Kept at the original ring size so music
// still reacts quickly to volume and dynamic-music state changes.
#define	MUSIC_RAW_LOOKAHEAD	16384
extern	portable_samplepair_t	s_rawsamples[MAX_RAW_SAMPLES];

extern	cvar_t	*s_reverb;			// 1 = the sound of the room: reverb, sounds held back by walls, muffling underwater
extern	cvar_t	*s_reverbLevel;		// how much reverb there is (1 = as worked out from the room)
extern	qboolean	s_reverbActive;	// the room effects are in use this frame
void	S_Mix_ResetReverb( void );		// (the reverb has been set up again: its time starts over)
extern	float		s_underwater;	// 0 to 1, how far under the surface the listener is
extern	cvar_t	*s_hrtf;			// 1 = position sounds binaurally (head-related transfer functions) for headphones
extern	cvar_t	*s_quality;			// 0 fast, 1 good, 2 best: quality of sample rate conversion when loading sounds
portable_samplepair_t *S_GetRawSamplePointer();	// TA added this, but it just returns the s_rawsamples[] array above. Oh well...

extern cvar_t *s_allowDynamicMusic;
extern cvar_t *s_initsound;
extern cvar_t *s_khz;
extern cvar_t *s_mixahead;
extern cvar_t *s_nosound;
extern cvar_t *s_separation;
extern cvar_t *s_show;
extern cvar_t *s_testsound;
extern cvar_t *s_volume;
extern cvar_t *s_volumeVoice;

wavinfo_t GetWavinfo (const char *name, byte *wav, int wavlength);

qboolean S_LoadSound( sfx_t *sfx );


// Mixes from s_paintedtime to endtime. An update mixes the part ahead of the play position that it mixes again every
// time (bReserve qfalse, which also runs the reverb), then the reserve further ahead that is only mixed once (qtrue)
void S_PaintChannels(int endtime, qboolean bReserve = qfalse);

// picks a channel based on priorities, empty slots, number of channels
channel_t *S_PickChannel(int entnum, int entchannel);

// spatializes a channel
void S_Spatialize(channel_t *ch);


//////////////////////////////////
//
// new stuff from TA codebase

byte	*SND_malloc(int iSize, sfx_t *sfx);
void	 SND_setup();
int		 SND_FreeOldestSound(sfx_t *pButNotThisOne = NULL);
void	 SND_TouchSFX(sfx_t *sfx);

void S_DisplayFreeMemory(void);
void S_memoryLoad(sfx_t *sfx);
//
//////////////////////////////////

#include "cl_mp3.h"

#endif	// #ifndef SND_LOCAL_H

