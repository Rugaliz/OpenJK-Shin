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

// jobs.h -- a small pool of worker threads for work that splits into independent pieces (docs/parallel-work.md).
//
// The engine itself is single threaded and its data (the zone memory, the cvars, the file system) is not safe to touch
// from other threads. This is for the opposite kind of work: loops over plain arrays that the calling thread has set
// up beforehand, like making mip levels or converting the sample rate of a sound. The caller splits the range, hands it
// over, takes a share itself and returns when all of it is done, so to the code around it nothing changes but the time.
//
// Header only: every module (engine, renderer) that includes it has a pool of its own, started on first use.

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace Jobs
{

// fn( begin, end, context ) is called for pieces [begin, end) of [0, count), on this thread and on the workers
typedef void (*RangeFunction)( int begin, int end, void *context );

class Pool
{
public:
	Pool() : m_stop( false ), m_generation( 0 ), m_limit( -1 ), m_count( 0 ), m_next( 0 ), m_grain( 1 ), m_fn( nullptr ), m_context( nullptr ), m_active( 0 )
	{
	}

	~Pool()
	{
		{
			std::lock_guard<std::mutex> lock( m_mutex );
			m_stop = true;
		}
		m_wake.notify_all();
		for ( size_t i = 0; i < m_threads.size(); i++ )
		{
			m_threads[i].join();
		}
	}

	// how many threads may work on one call besides the caller: 0 turns the pool off, -1 is automatic
	void SetLimit( int workers )
	{
		m_limit = workers;
	}

	// how many worker threads the pool uses (or will use), without starting them
	int Wanted()
	{
		unsigned int cores = std::thread::hardware_concurrency();
		int workers = cores < 2 ? 0 : (int)( cores - 1 > 7 ? 7 : cores - 1 );
		return m_limit < 0 ? workers : ( m_limit < workers ? m_limit : workers );
	}

	int Workers()
	{
		Start();
		return m_limit < 0 ? (int)m_threads.size() : ( m_limit < (int)m_threads.size() ? m_limit : (int)m_threads.size() );
	}

	// Runs fn over [0, count) in pieces of at least grain. Small jobs, where waking the workers would cost more than it
	// saves, run on the calling thread alone. Not to be called from within fn, or from two threads at once.
	void ParallelFor( int count, int grain, RangeFunction fn, void *context )
	{
		if ( count <= 0 )
		{
			return;
		}
		if ( grain < 1 )
		{
			grain = 1;
		}
		const int workers = count > grain ? Workers() : 0;
		if ( workers <= 0 )
		{
			fn( 0, count, context );
			return;
		}

		// the number of pieces: enough to share out evenly, not smaller than grain
		int pieces = ( count + grain - 1 ) / grain;
		if ( pieces > ( workers + 1 ) * 4 )
		{
			pieces = ( workers + 1 ) * 4;
		}
		const int size = ( count + pieces - 1 ) / pieces;

		{
			std::lock_guard<std::mutex> lock( m_mutex );
			m_fn = fn;
			m_context = context;
			m_count = count;
			m_grain = size;
			m_next.store( 0 );
			m_workersWanted = workers;
			m_workersTaken = 0;
			m_generation++;
		}
		m_wake.notify_all();

		Work();	// this thread takes pieces too

		// wait until the workers that took part are finished
		std::unique_lock<std::mutex> lock( m_mutex );
		m_done.wait( lock, [this] { return m_active == 0; } );
		m_fn = nullptr;
	}

private:
	void Start()
	{
		if ( !m_threads.empty() )
		{
			return;
		}
		unsigned int cores = std::thread::hardware_concurrency();
		if ( cores < 2 )
		{
			return;
		}
		unsigned int workers = cores - 1;
		if ( workers > 7 )
		{
			workers = 7;	// (past that the pieces get too small to gain anything)
		}
		for ( unsigned int i = 0; i < workers; i++ )
		{
			m_threads.push_back( std::thread( &Pool::Loop, this ) );
		}
	}

	// takes pieces until there are none left
	void Work()
	{
		for ( ;; )
		{
			const int begin = m_next.fetch_add( m_grain );
			if ( begin >= m_count )
			{
				return;
			}
			const int end = begin + m_grain < m_count ? begin + m_grain : m_count;
			m_fn( begin, end, m_context );
		}
	}

	void Loop()
	{
		unsigned long seen = 0;
		for ( ;; )
		{
			{
				std::unique_lock<std::mutex> lock( m_mutex );
				m_wake.wait( lock, [this, &seen] { return m_stop || ( m_generation != seen && m_fn ); } );
				if ( m_stop )
				{
					return;
				}
				seen = m_generation;
				if ( m_workersTaken >= m_workersWanted )
				{
					continue;	// (the job does not want any more helpers)
				}
				m_workersTaken++;
				m_active++;
			}
			Work();
			{
				std::lock_guard<std::mutex> lock( m_mutex );
				m_active--;
			}
			m_done.notify_all();
		}
	}

	std::vector<std::thread>	m_threads;
	std::mutex					m_mutex;
	std::condition_variable		m_wake, m_done;
	bool						m_stop;
	unsigned long				m_generation;
	int							m_limit;
	int							m_count;
	std::atomic<int>			m_next;
	int							m_grain;
	RangeFunction				m_fn;
	void						*m_context;
	int							m_active;
	int							m_workersWanted;
	int							m_workersTaken;
};

inline Pool &GetPool()
{
	static Pool pool;
	return pool;
}

inline void ParallelFor( int count, int grain, RangeFunction fn, void *context )
{
	GetPool().ParallelFor( count, grain, fn, context );
}

// com_jobThreads: 0 works on the calling thread only, -1 (the default) uses what the processor has
// how many worker threads there are going to be
inline int WantedThreads()
{
	return GetPool().Wanted();
}

inline void SetThreads( int workers )
{
	GetPool().SetLimit( workers );
}

} // namespace Jobs
