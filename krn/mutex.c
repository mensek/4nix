/*
 * Copyright (C) 2015 Frantisek Mensik
 * mutex.c is part of the 4nix.org project.
 *
 * This file is licensed under the GNU Lesser General Public License.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif	//HAVE_CONFIG_H

#include <stdlib.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#endif

#include "windows.h"
#include "object.h"


// depends on these functions:
extern void _setlasterror( unsigned err );
extern unsigned get_win_error( int err );


#ifdef __MACH__
//own implementation of pthread_mutex_timedlock function
int pthread_mutex_timedlock (pthread_mutex_t * mutex, const struct timespec * abs_timeout)
{
	int rv;
	long ellapsed_time = 0;
	struct timespec ts;

	do
	{
		// the mutex could not be acquired because it was already locked by an other thread
		rv = pthread_mutex_trylock (mutex);

		ts.tv_sec = 0;
		ts.tv_nsec = 2.5e7; // 25 ms

		do {
			nanosleep (&ts, &ts);
		} while (EINTR == errno);

		ellapsed_time += ts.tv_nsec;
	}
	while (/* locked */ (EBUSY == rv) && /* !timeout */ (ellapsed_time < ((long)(abs_timeout->tv_sec * 1e9) + abs_timeout->tv_nsec)));

	return (EBUSY == rv) ? ETIMEDOUT : rv;
}
#endif


#define MUTEXOBJID		8

static bool _closemutexhandle( uintptr_t hndl );
static bool _duplicatemutexhandle( uintptr_t srchndl, uintptr_t *targethndl, unsigned access, bool inherit, unsigned options );
/*static bool _gethandleinformation( uintptr_t hndl, unsigned *flags );
static bool _sethandleinformation( uintptr_t hndl, unsigned mask, unsigned flags );*/
static unsigned _waitforsinglemutexobject( uintptr_t hndl, unsigned milliseconds );

static TYPEOBJITEM mutexkrnlobj =
{
	MUTEXOBJID, true,
	_closemutexhandle,
	_duplicatemutexhandle,
	NULL,   //_gethandleinformation,
	NULL,   //_sethandleinformation,
	_waitforsinglemutexobject
};

typedef struct MUTEXOBJDATA_ {
	int fdmutex;
	char name[];
} MUTEXOBJDATA;


uintptr_t _createmutexex( SECURITY_ATTRIBUTES *sa, const char *name, unsigned flags, unsigned access )
{
	//NOTE: Windows critical sections are recursive
	//NOTE: posix recursive mutexes don't return error code

	int rc;
	pthread_mutex_t *pm;
	pthread_mutexattr_t mattr;
	int fd_mutex = -1;
	char shmname[MAX_PATH+1];
	size_t namelen = 0;
	bool opened = false;
	uintptr_t hndl;

	if ( name && *name ) {
		namelen = strlen (name);

		if ( 1+namelen > NAME_MAX ) {
			_setlasterror( ERROR_BAD_ARGUMENTS );
			return (uintptr_t)NULL;
		}

		shmname[0] = '/'; strcpy (shmname+1, name);

		//try to open mutex first
		fd_mutex = shm_open (shmname, O_RDWR, S_IRUSR|S_IWUSR);
		if ( fd_mutex != -1 ) {
			struct flock fl;
			fl.l_type   = F_WRLCK;	//note: check only
			fl.l_whence = SEEK_SET;
			fl.l_start  = 0;
			fl.l_len    = 0;
			fl.l_pid    = getpid ();

			rc = fcntl (fd_mutex, F_GETLK, &fl);
			if (!(rc == 0 && fl.l_type == F_RDLCK)) {
				//semaphore is unused or other error
				ftruncate (fd_mutex, sizeof(pthread_mutex_t));
			} else {
				_setlasterror( ERROR_ALREADY_EXISTS );
				opened = true;
			}
		} else {
			//create new semaphore
			fd_mutex = shm_open (shmname, O_CREAT|O_RDWR, S_IRUSR|S_IWUSR);
			if ( fd_mutex == -1 ) {
				_setlasterror( get_win_error (errno) );
				return (uintptr_t)NULL;
			}
			ftruncate (fd_mutex, sizeof(pthread_mutex_t));
		}

		void *addr_mutex = mmap (0, sizeof(pthread_mutex_t), PROT_READ|PROT_WRITE, MAP_SHARED, fd_mutex, 0);
		if (addr_mutex == MAP_FAILED) {
			_setlasterror( get_win_error (errno) );
			close (fd_mutex);
			if (!opened) shm_unlink (shmname);
			return (uintptr_t)NULL;
		}

		//add file open check
		struct flock fl;
		fl.l_type   = F_RDLCK;
		fl.l_whence = SEEK_SET;
		fl.l_start  = 0;
		fl.l_len    = 0;
		fl.l_pid    = getpid ();
		fcntl (fd_mutex, F_SETLK, &fl);

		pm = (pthread_mutex_t *)addr_mutex;
	}
	else
	{
		pm = (pthread_mutex_t *) malloc (sizeof(pthread_mutex_t));
		if ( !pm ) {
			_setlasterror( get_win_error (errno) );	//ERROR_NOT_ENOUGH_MEMORY
			return (uintptr_t)NULL;
		}
	}

	if (!opened) {
	//initialize mutex
	pthread_mutexattr_init (&mattr);
	if ( name && *name )
		pthread_mutexattr_setpshared (&mattr, PTHREAD_PROCESS_SHARED);
	else
		pthread_mutexattr_setpshared (&mattr, PTHREAD_PROCESS_PRIVATE);
	#if defined (__linux__) || defined (__bsd__)
	pthread_mutexattr_setkind_np (&mattr, PTHREAD_MUTEX_RECURSIVE_NP);
	#else
	pthread_mutexattr_settype (&mattr, PTHREAD_MUTEX_RECURSIVE);
	#endif
	rc = pthread_mutex_init (pm, &mattr);
	pthread_mutexattr_destroy (&mattr);

	if ( rc != 0 ) {
		_setlasterror( get_win_error (rc) );
		if ( name && *name ) {
			munmap ((void*)pm, sizeof(pthread_mutex_t));
			shm_unlink (shmname);
			close (fd_mutex);
		} else
			free (pm);
		return (uintptr_t)NULL;
	}
	}

	size_t datalen = sizeof(MUTEXOBJDATA) + (name ? namelen+2 : 0);
	MUTEXOBJDATA* data;
	data = (MUTEXOBJDATA*)malloc (datalen);
	if (!data) {
		_setlasterror( get_win_error (rc) );
		if ( name && *name ) {
			munmap ((void*)pm, sizeof(pthread_mutex_t));
			if (!opened) shm_unlink (shmname);
			close (fd_mutex);
		} else
			free (pm);
		return (uintptr_t)NULL;
	}
	data->fdmutex = fd_mutex;
	strcpy (data->name, (name && *name ? shmname : ""));

	//(flags && CREATE_MUTEX_INITIAL_OWNER)
	hndl = _createhandle (-1, 0, &mutexkrnlobj, &pm, sizeof(void*), data, datalen);
	if ( !hndl ) {
		_setlasterror( get_win_error (errno) );
		pthread_mutex_destroy (pm);		//destroy created mutex
		if ( name && *name ) {
			munmap ((void*)pm, sizeof(pthread_mutex_t));
			if (!opened) shm_unlink (shmname);
			close (fd_mutex);
		} else
			free (pm);

		free (data);
		return (uintptr_t)NULL;
	}

	free (data);
	return hndl;
}

uintptr_t _openmutex( unsigned access, bool inherit, const char *name )
{
	int rc;
	pthread_mutex_t *pm;
	char shmname[MAX_PATH+1];
	size_t namelen = 0;
	uintptr_t hndl;

	if (name) namelen = strlen (name);

	if ( !(name && *name) || 1+namelen > MAX_PATH ) {
		_setlasterror( ERROR_BAD_ARGUMENTS );
		return (uintptr_t)NULL;
	}

	shmname[0] = '/'; strcpy (shmname+1, name);

    //first try to find existing mutex
	int fd_mutex = shm_open (shmname, O_RDWR, S_IRUSR|S_IWUSR);
	if ( fd_mutex == -1 ) {
		_setlasterror( get_win_error (errno) );
		return (uintptr_t)NULL;
	}

	struct flock fl;
	fl.l_type   = F_WRLCK;	//note: check only
	fl.l_whence = SEEK_SET;
	fl.l_start  = 0;
	fl.l_len    = 0;
	fl.l_pid    = getpid ();
	rc = fcntl (fd_mutex, F_GETLK, &fl);
	if (!(rc == 0 && fl.l_type == F_RDLCK)) {
		//mutex is unused or other error
		close (fd_mutex);
		return (uintptr_t)NULL;
	}

	//semaphore exists, try to set read lock
	fl.l_type   = F_RDLCK;
	rc = fcntl (fd_mutex, F_SETLK, &fl);
	if (rc != 0) {
		close (fd_mutex);
		return (uintptr_t)NULL;
	}

	void *addr_mutex = mmap (0, sizeof(pthread_mutex_t), PROT_READ|PROT_WRITE, MAP_SHARED, fd_mutex, 0);
	if (addr_mutex == MAP_FAILED) {
		_setlasterror( get_win_error (errno) );
		close (fd_mutex);
		return (uintptr_t)NULL;
	}

	pm = (pthread_mutex_t *)addr_mutex;

	size_t datalen = sizeof(MUTEXOBJDATA) + (name ? namelen+2 : 0);
	MUTEXOBJDATA *data;
	data = (MUTEXOBJDATA*)malloc (datalen);
	if (!data) {
		_setlasterror( get_win_error (errno) );
		munmap ((void*)pm, sizeof(pthread_mutex_t));
		close (fd_mutex);
		return (uintptr_t)NULL;
	}
	data->fdmutex = fd_mutex;
	strcpy (data->name, (name ? name : ""));

	hndl = _createhandle (-1, 0, &mutexkrnlobj, &pm, sizeof(void*), data, datalen);
	if ( !hndl ) {
		_setlasterror( get_win_error (errno) );
		munmap ((void*)pm, sizeof(pthread_mutex_t));
		close (fd_mutex);
		return (uintptr_t)NULL;
	}

	return hndl;
}

static
unsigned _waitforsinglemutexobject( uintptr_t hndl, unsigned milliseconds )
{
	int rc;
	pthread_mutex_t *pm;

	pm = *(pthread_mutex_t**)hndl;

	if ( milliseconds != INFINITE ) {
		struct timespec abstime;

#ifdef __MACH__
		clock_serv_t cclock;
		mach_timespec_t mts;

		host_get_clock_service (mach_host_self(), CALENDAR_CLOCK, &cclock);
		clock_get_time (cclock, &mts);
		mach_port_deallocate (mach_task_self(), cclock);
		abstime.tv_sec = mts.tv_sec;
		abstime.tv_nsec = mts.tv_nsec;
#else
		clock_gettime (CLOCK_REALTIME, &abstime);
#endif
		abstime.tv_sec += (milliseconds / 1000);
		abstime.tv_nsec += (milliseconds % 1000) * 1000000;

		rc = pthread_mutex_timedlock (pm, &abstime);
	} else
		rc = pthread_mutex_lock (pm);

	if ( rc != 0 ) {
		_setlasterror( get_win_error (rc) );
		return (rc == ETIMEDOUT ? WAIT_TIMEOUT : WAIT_FAILED);
	}

	return WAIT_OBJECT_0;
}

static
bool _releasemutex( uintptr_t hndl )
{
	int rc;
	pthread_mutex_t* pm;

	pm = *(pthread_mutex_t**)hndl;

	rc = pthread_mutex_unlock (pm);
	if ( rc != 0 ) {
		_setlasterror( get_win_error (rc) );
		return false;
	}

	return true;
}

static
bool _closemutexhandle( uintptr_t hndl )
{
	/*if (!hndl) {
		_setlasterror( ERROR_BAD_ARGUMENTS );
		return false;
	}*/

	MUTEXOBJDATA *data;
	unsigned int *refcount;
	data = (MUTEXOBJDATA*)_gethandledata (hndl, &refcount);
	if (*refcount <= 1) {
		int rc;
		pthread_mutex_t *pm;
		const char *name;
		int fdmutex;
		DWORD flags;

		pm = *(pthread_mutex_t**)hndl;
		fdmutex = data->fdmutex;
		name = data->name;

		if ( *name ) {
			//check the file is opened
			struct flock fl;
			fl.l_type   = F_UNLCK;
			fl.l_whence = SEEK_SET;
			fl.l_start  = 0;
			fl.l_len    = 0;
			fl.l_pid    = getpid ();
			rc = fcntl (fdmutex, F_SETLK, &fl);      //release lock

			fl.l_type   = F_WRLCK;
			rc = fcntl (fdmutex, F_GETLK, &fl);
			if (rc == 0 && fl.l_type == F_UNLCK && fl.l_pid == getpid ()) {
				shm_unlink (name);	//unlink created SHM

				pthread_mutex_destroy (pm);
			}

			munmap ((void*)pm, sizeof(pthread_mutex_t));
			close (fdmutex);
		} else {
			pthread_mutex_destroy (pm);

			free (pm);
		}
	}

	return true;
}

static
bool _duplicatemutexhandle( uintptr_t srchndl, uintptr_t *targethndl, unsigned access, bool inherit, unsigned options )
{
	return true;
}
