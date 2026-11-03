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

// cl_cgame_time.h -- the client's view of the server's time: what moment of the game is drawn (between which two
// snapshots), and the time the commands of the player are stamped with. Shared by the client of both games
// (codemp/client/cl_cgame.cpp for Jedi Academy, codemp/jk2/client/cl_cgame.cpp for Jedi Outcast), included once by
// each after client.h.
//
// cl_timeSync 1 (the default) keeps the drawn moment just far enough behind the newest snapshot that the next one is
// there in time to draw between the two (see CL_TimeSync_Snapshot); cl_timeSync 0 is the original timing
// (CL_AdjustTimeDelta), which keeps the drawn moment at the newest snapshot and so often has to draw past it
// (extrapolate) when snapshots come late.

#pragma once

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
=================
CL_TimeSync (cl_timeSync 1)

The moment that is drawn has to lie between two snapshots the client has, so that what is drawn can be interpolated
between them; past the newest one, the cgame has to extrapolate (guess where things went), and other players stutter
and jump when the next snapshot shows where they really are. A snapshot takes as long as the network does to arrive,
and it is only used from the first frame after it came: the latest it can come, measured over the last seconds, says
how far behind the newest snapshot the drawn moment has to be for the next one to be there in time. So:

- for each new snapshot, the offset between its time and the client's clock when it could first be used is
  remembered (serverTime - cls.realtime), and the gap since the one before it (the snapshot interval)
- the drawn moment is to be cls.realtime + (a low percentile of the recent offsets - the usual interval + the
  shortest frame): a snapshot only has to be there for the frame after the last one that drew before it came, so when
  the latest of the recent snapshots came, the frame before had just drawn up to the snapshot before it
- the client's view of the server's time goes to that gently: it runs up to 5% slower (10% while it is past the newest
  snapshot) or 2% faster than the real time, and jumps only after a hitch or a new level, as the original does

On a steady connection this draws about as far behind as the original timing does at its best; on an unsteady one it
draws as much further behind as the connection needs instead of extrapolating every time a snapshot is late. A late
snapshot of the kind that comes once in a while (a few in a hundred) doesn't count, and a lost one is extrapolated
over as before. cl_showTimeDelta 2 prints what it measures once a second, for either timing.
=================
*/

#define TIMESYNC_SAMPLES	256			// snapshots remembered (more than the window holds at 40 a second)
#define TIMESYNC_WINDOW		3000		// msec of snapshots the timing looks at
#define TIMESYNC_SLOWER		0.05		// how much slower than the real time the view of the server's time may run...
#define TIMESYNC_SLOWER_FAST	0.10	// ... while it is past the newest snapshot
#define TIMESYNC_FASTER		0.02		// how much faster

static struct {
	qboolean	valid;
	double		delta;					// the view of the server's time is cls.realtime + delta
	int			target;					// where delta is going
	int			lastRealtime;
	int			lastSnapTime;			// serverTime of the last snapshot looked at
	int			head, count;
	int			realtime[TIMESYNC_SAMPLES];	// when each snapshot could first be used...
	int			offset[TIMESYNC_SAMPLES];	// ... its serverTime - that
	int			gap[TIMESYNC_SAMPLES];		// msec since the snapshot before it (0 if not known)
	int			interval, spread;			// for cl_showTimeDelta 2: the usual interval, the spread of the offsets
	int			minFrame, minFrameNow;		// the shortest frame (msec) of the last second, and of this one so far
	int			nextMinFrame;

	// statistics for cl_showTimeDelta 2, for either timing
	int			frames, extrapolated, snapshots, nextReport;
	double		behind;					// summed msec the drawn moment was behind the newest snapshot
} clTimeSync;

static void CL_TimeSync_Reset( void ) {
	clTimeSync.valid = qtrue;
	clTimeSync.delta = cl.serverTimeDelta;
	clTimeSync.target = cl.serverTimeDelta;
	clTimeSync.lastRealtime = cls.realtime;
	clTimeSync.lastSnapTime = 0;
	clTimeSync.head = clTimeSync.count = 0;
	clTimeSync.minFrame = clTimeSync.minFrameNow = 0;
	clTimeSync.nextMinFrame = cls.realtime + 1000;
}

static int CL_TimeSync_CompareInts( const void *a, const void *b ) {
	return *(const int *)a - *(const int *)b;
}

// A new snapshot is in cl.snap
static void CL_TimeSync_Snapshot( void ) {
	int			offsets[TIMESYNC_SAMPLES], gaps[TIMESYNC_SAMPLES];
	int			numOffsets = 0, numGaps = 0;
	int			i, low, interval;

	cl.newSnapshots = qfalse;

	if ( !clTimeSync.valid ) {
		CL_TimeSync_Reset();
	}

	clTimeSync.realtime[clTimeSync.head] = cls.realtime;
	clTimeSync.offset[clTimeSync.head] = cl.snap.serverTime - cls.realtime;
	clTimeSync.gap[clTimeSync.head] = ( clTimeSync.lastSnapTime && cl.snap.serverTime > clTimeSync.lastSnapTime ) ? cl.snap.serverTime - clTimeSync.lastSnapTime : 0;
	clTimeSync.lastSnapTime = cl.snap.serverTime;
	clTimeSync.head = ( clTimeSync.head + 1 ) % TIMESYNC_SAMPLES;
	if ( clTimeSync.count < TIMESYNC_SAMPLES ) {
		clTimeSync.count++;
	}

	for ( i = 0; i < clTimeSync.count; i++ ) {
		const int k = ( clTimeSync.head - 1 - i + TIMESYNC_SAMPLES ) % TIMESYNC_SAMPLES;
		if ( cls.realtime - clTimeSync.realtime[k] > TIMESYNC_WINDOW ) {
			break;
		}
		offsets[numOffsets++] = clTimeSync.offset[k];
		if ( clTimeSync.gap[k] > 0 ) {
			gaps[numGaps++] = clTimeSync.gap[k];
		}
	}

	qsort( offsets, numOffsets, sizeof( int ), CL_TimeSync_CompareInts );
	low = offsets[numOffsets / 50];			// (the few latest of a hundred don't count)
	clTimeSync.spread = offsets[numOffsets / 2] - low;

	if ( numGaps ) {
		qsort( gaps, numGaps, sizeof( int ), CL_TimeSync_CompareInts );
		interval = gaps[numGaps / 2];		// (a lost snapshot makes one gap twice as long, it doesn't count)
	} else {
		interval = 0;
	}
	clTimeSync.interval = interval;
	clTimeSync.target = low - interval + ( clTimeSync.minFrame > 1 ? clTimeSync.minFrame - 1 : 0 );

	if ( abs( clTimeSync.target - (int)clTimeSync.delta ) > RESET_TIME ) {
		// a new level, a long hitch: be there at once, as the original does
		clTimeSync.delta = clTimeSync.target;
		cl.serverTimeDelta = clTimeSync.target;
		cl.oldServerTime = cls.realtime + clTimeSync.target;	// (time may go back here)
		if ( cl_showTimeDelta->integer ) {
			Com_Printf( "<RESET> " );
		}
	}
}

// Every frame: the view of the server's time moves towards the target, gently
static void CL_TimeSync_Frame( void ) {
	const double diff = clTimeSync.target - clTimeSync.delta;
	int msec = cls.realtime - clTimeSync.lastRealtime;

	clTimeSync.lastRealtime = cls.realtime;
	if ( msec < 0 ) {
		msec = 0;
	} else if ( msec > 100 ) {
		msec = 100;
	}

	if ( msec > 0 && ( !clTimeSync.minFrameNow || msec < clTimeSync.minFrameNow ) ) {
		clTimeSync.minFrameNow = msec;
	}
	if ( cls.realtime >= clTimeSync.nextMinFrame ) {
		clTimeSync.minFrame = clTimeSync.minFrameNow;
		clTimeSync.minFrameNow = 0;
		clTimeSync.nextMinFrame = cls.realtime + 1000;
	}

	if ( fabs( diff ) > 100 ) {
		clTimeSync.delta += diff * 0.5;	// (a large step at once, as the original does)
	} else if ( diff < 0 ) {
		const qboolean past = (qboolean)( cls.realtime + clTimeSync.delta >= cl.snap.serverTime );
		const double step = msec * ( past ? TIMESYNC_SLOWER_FAST : TIMESYNC_SLOWER );
		clTimeSync.delta -= ( -diff < step ) ? -diff : step;
	} else {
		const double step = msec * TIMESYNC_FASTER;
		clTimeSync.delta += ( diff < step ) ? diff : step;
	}

	cl.serverTimeDelta = (int)floor( clTimeSync.delta );
}

// Statistics for cl_showTimeDelta 2: how far behind the newest snapshot the drawn moment is, how often it is past it
static void CL_TimeSync_Statistics( void ) {
	clTimeSync.frames++;
	if ( cl.serverTime >= cl.snap.serverTime ) {
		clTimeSync.extrapolated++;
	}
	clTimeSync.behind += cl.snap.serverTime - cl.serverTime;

	if ( cls.realtime >= clTimeSync.nextReport ) {
		if ( cl_showTimeDelta->integer == 2 && clTimeSync.frames ) {
			Com_Printf( "timesync %s: %.1f msec behind the newest snapshot, %d of %d frames extrapolated (%.1f%%), %d snapshots; interval %d, spread %d\n",
				( cl_timeSync->integer && clTimeSync.valid ) ? "adaptive" : "original",
				clTimeSync.behind / clTimeSync.frames, clTimeSync.extrapolated, clTimeSync.frames,
				100.0 * clTimeSync.extrapolated / clTimeSync.frames, clTimeSync.snapshots, clTimeSync.interval, clTimeSync.spread );
		}
		clTimeSync.frames = clTimeSync.extrapolated = clTimeSync.snapshots = 0;
		clTimeSync.behind = 0;
		clTimeSync.nextReport = cls.realtime + 1000;
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
	clTimeSync.valid = qfalse;	// (and the adaptive timing starts from there)

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

		// (the adaptive timing is for live games at the normal speed, demos and timescale use the original)
		const qboolean adaptive = (qboolean)( cl_timeSync->integer && !clc.demoplaying && ( com_timescale->value == 0 || com_timescale->value == 1 ) );
		if ( adaptive ) {
			if ( !clTimeSync.valid ) {
				CL_TimeSync_Reset();
			}
			CL_TimeSync_Frame();
		} else {
			clTimeSync.valid = qfalse;
		}

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

	if ( !clc.demoplaying ) {
		CL_TimeSync_Statistics();
	}

	// if we have gotten new snapshots, drift serverTimeDelta
	// don't do this every frame, or a period of packet loss would
	// make a huge adjustment
	if ( cl.newSnapshots ) {
		clTimeSync.snapshots++;
		if ( clTimeSync.valid ) {
			CL_TimeSync_Snapshot();
		} else {
			CL_AdjustTimeDelta();
		}
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
