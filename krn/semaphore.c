/*
 * Copyright (C) 2015 Frantisek Mensik
 * semaphore.c is part of the 4nix.org project.
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
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef __MACH__
#include <mach/mach_time.h>
#endif

#include "windows.h"
#include "object.h"


// depends on these functions:
extern void _setlasterror( unsigned err );
extern unsigned get_win_error( int err );


#define SEMAPHOREOBJID		6

bool _closesemaphorehandle( uintptr_t hndl );
static bool _duplicatesemaphorehandle( uintptr_t srchndl, uintptr_t *targethndl, unsigned access, bool inherit, unsigned options );
/* bool _gethandleinformation( uintptr_t hndl, unsigned *flags );
bool _sethandleinformation( uintptr_t hndl, unsigned mask, unsigned flags );*/
static unsigned _waitforsinglesemaphoreobject( uintptr_t hndl, unsigned milliseconds );

static TYPEOBJITEM semaphorekrnlobj =
{
	SEMAPHOREOBJID, true,
	_closesemaphorehandle,
	_duplicatesemaphorehandle,
	NULL,   //_gethandleinformation,
	NULL,   //_sethandleinformation,
	_waitforsinglesemaphoreobject
};

typedef struct SEMAPHOREOBJDATA_{
	int fdsemaphore;
	char name[];
} SEMAPHOREOBJDATA;


typedef struct mysem_t_ {
	pthread_mutex_t mx;
	pthread_cond_t cv;
	unsigned value;
	unsigned maxvalue;
} mysem_t;


uintptr_t _createsemaphoreex( SECURITY_ATTRIBUTES *sa, LONG initial, LONG max, const char *name, unsigned flags, unsigned access )
{
	int rc;
	mysem_t *ps;
	int fd_semaphore = -1;
	char shmname[NAME_MAX+1];
	size_t namelen = 0;
	bool opened = false;
	uintptr_t hndl;

	if ( initial < 0 || max <= 0 || max < initial ) {
		_setlasterror( ERROR_BAD_ARGUMENTS );
		return (uintptr_t)NULL;
	}

	if ( name && *name )
	{
		namelen = strlen (name);

		if ( 1+namelen > NAME_MAX ) {
			_setlasterror( ERROR_BAD_ARGUMENTS );
			return (uintptr_t)NULL;
		}

		shmname[0] = '/'; strcpy (shmname+1, name);

		//first try to open existing semaphore
		fd_semaphore = shm_open (shmname, O_RDWR, S_IRUSR|S_IWUSR);
		if ( fd_semaphore != -1 ) {
			struct flock fl;
			fl.l_type   = F_WRLCK;	//note: check only
			fl.l_whence = SEEK_SET;
			fl.l_start  = 0;
			fl.l_len    = 0;
			fl.l_pid    = getpid ();

			rc = fcntl (fd_semaphore, F_GETLK, &fl);
			if (!(rc == 0 && fl.l_type == F_RDLCK)) {
				//semaphore is unused or an error
				ftruncate (fd_semaphore, sizeof(mysem_t));
			} else {
				_setlasterror( ERROR_ALREADY_EXISTS );
				opened = true;
			}
		} else {
			//create a new semaphore
			fd_semaphore = shm_open (name, O_CREAT|O_EXCL|O_RDWR, S_IRUSR|S_IWUSR);
			if ( fd_semaphore == -1 ) {
				_setlasterror( get_win_error (errno) );
				return (uintptr_t)NULL;
			}
			ftruncate (fd_semaphore, sizeof(mysem_t));
		}

		void *addr_semaphore = mmap (0, sizeof(mysem_t), PROT_READ|PROT_WRITE, MAP_SHARED, fd_semaphore, 0);
		if (addr_semaphore == MAP_FAILED) {
			_setlasterror( get_win_error (errno) );
			close (fd_semaphore);
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
		fcntl (fd_semaphore, F_SETLK, &fl);

		ps = (mysem_t *)addr_semaphore;
	}
	else
	{
		ps = malloc (sizeof(mysem_t));
		if ( !ps ) {
			_setlasterror( get_win_error (errno) );	//ERROR_NOT_ENOUGH_MEMORY
			return (uintptr_t)NULL;
		}
	}

	if (!opened) {
	// initialize semaphore
	pthread_mutexattr_t pmxattr;
	pthread_mutexattr_init (&pmxattr);
	if (name && *name)
		pthread_mutexattr_setpshared (&pmxattr, PTHREAD_PROCESS_SHARED);
	rc = pthread_mutex_init (&ps->mx, &pmxattr);
	pthread_mutexattr_destroy (&pmxattr);
	if (rc == 0) {
		pthread_condattr_t pcvattr;
		pthread_condattr_init (&pcvattr);
		if (name && *name)
			pthread_condattr_setpshared (&pcvattr, PTHREAD_PROCESS_SHARED);
		rc = pthread_cond_init (&ps->cv, &pcvattr);
		pthread_condattr_destroy (&pcvattr);
		if (rc != 0)
			pthread_mutex_destroy (&ps->mx);
	}
	if (rc == 0) {
		ps->value    = initial;
		ps->maxvalue = max;
	}

	if ( rc != 0 ) {
		_setlasterror( get_win_error (rc) );
		if ( name && *name ) {
			munmap ((void*)ps, sizeof(mysem_t));
			close (fd_semaphore);
			shm_unlink (shmname);
		} else
			free (ps);
		return (uintptr_t)NULL;
	}
	}

	size_t datalen = sizeof(SEMAPHOREOBJDATA) + (name ? namelen+2 : 0);
	SEMAPHOREOBJDATA *data;
	data = (SEMAPHOREOBJDATA*)malloc (datalen);
	if (!data) {
		_setlasterror( get_win_error (errno) );
		if ( name && *name ) {
			munmap ((void*)ps, sizeof(mysem_t));
			if (!opened) shm_unlink (shmname);
			close (fd_semaphore);
		} else
			free (ps);
		return (uintptr_t)NULL;
	}

	data->fdsemaphore = fd_semaphore;
	strcpy (data->name, (name && *name ? shmname : ""));

	hndl = _createhandle (-1, 0, &semaphorekrnlobj, &ps, sizeof(void*), data, datalen);
	if ( !hndl ) {
		_setlasterror( get_win_error (errno) );
		//destroy created semaphore
		pthread_mutex_destroy (&ps->mx);
		pthread_cond_destroy (&ps->cv);
		if ( name && *name ) {
			munmap ((void*)ps, sizeof(mysem_t));
			if (!opened) shm_unlink (shmname);
			close (fd_semaphore);
		} else
			free (ps);

		free (data);
		return (uintptr_t)NULL;
	}

	free (data);
	return hndl;
}

uintptr_t _opensemaphore( unsigned access, bool inherit, const char *name )
{
	int rc;
	mysem_t *ps;
	char shmname[NAME_MAX+1];
	size_t namelen = 0;
	uintptr_t hndl;

	if (name) namelen = strlen (name);

	if ( !(name && *name) || 1+namelen > NAME_MAX ) {
		_setlasterror( ERROR_BAD_ARGUMENTS );
		return (uintptr_t)NULL;
	}

	shmname[0] = '/'; strcpy (shmname+1, name);

	int fd_semaphore = shm_open (shmname, O_RDWR, S_IRUSR|S_IWUSR);
	if ( fd_semaphore == -1 ) {
		_setlasterror( get_win_error (errno) );
		return (uintptr_t)NULL;
	}

	struct flock fl;
	fl.l_type   = F_WRLCK;	//note: check only
	fl.l_whence = SEEK_SET;
	fl.l_start  = 0;
	fl.l_len    = 0;
	fl.l_pid    = getpid ();
	rc = fcntl (fd_semaphore, F_GETLK, &fl);

	if (!(rc == 0 && fl.l_type == F_RDLCK)) {
		//semaphore is unused or other error
		close (fd_semaphore);
		return (uintptr_t)NULL;
	}

	//semaphore exists, try to set read lock
	fl.l_type   = F_RDLCK;
	rc = fcntl (fd_semaphore, F_SETLK, &fl);
	if (rc != 0) {
		close (fd_semaphore);
		return (uintptr_t)NULL;
	}

	void *addr_semaphore = mmap (0, sizeof(mysem_t), PROT_READ|PROT_WRITE, MAP_SHARED, fd_semaphore, 0);
	if (addr_semaphore == MAP_FAILED) {
		_setlasterror( get_win_error (errno) );
		close (fd_semaphore);
		return (uintptr_t)NULL;
	}

	ps = (mysem_t*)addr_semaphore;

	size_t datalen = sizeof(SEMAPHOREOBJDATA) + (name ? namelen+2 : 0);
	SEMAPHOREOBJDATA *data;
	data = (SEMAPHOREOBJDATA*)malloc (datalen);
	if (!data) {
		_setlasterror( get_win_error (errno) );
		munmap ((void*)ps, sizeof(mysem_t));
		close (fd_semaphore);
		return (uintptr_t)NULL;
	}
	data->fdsemaphore = fd_semaphore;
	strcpy (data->name, (name ? shmname : ""));

	hndl = _createhandle (-1, 0, &semaphorekrnlobj, &ps, sizeof(void*), data, datalen);	//TODO: store 'max' 
	if ( !hndl ) {
		_setlasterror( get_win_error (errno) );
		munmap ((void*)ps, sizeof(mysem_t));
		close (fd_semaphore);
		return (uintptr_t)NULL;
	}

	return hndl;
}

static
unsigned _waitforsinglesemaphoreobject( uintptr_t hndl, unsigned milliseconds )
{
	int rc;
	mysem_t *sm;

	sm = *(mysem_t**)hndl;

	rc = pthread_mutex_lock (&sm->mx);
	if ( rc == -1 ) {
		_setlasterror( get_win_error (errno) );
		return false;
	}

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

		while (sm->value == 0 && rc == 0)
			rc = pthread_cond_timedwait (&sm->cv, &sm->mx, &abstime);
	} else {
		while (sm->value == 0 && rc == 0)
			rc = pthread_cond_wait (&sm->cv, &sm->mx);
	}

	if ( rc != 0 ) {
		pthread_mutex_unlock (&sm->mx);
		_setlasterror( get_win_error (rc) );
		return (errno == ETIMEDOUT ? WAIT_TIMEOUT : WAIT_FAILED);
	}

	sm->value--;

	pthread_mutex_unlock (&sm->mx);

	return WAIT_OBJECT_0;
}

bool _releasesemaphore( uintptr_t hndl, LONG count, LONG *previous )
{
	int rc = 0;
	int prev;
	mysem_t *sm;

	if (count <= 0) {
		_setlasterror( ERROR_BAD_ARGUMENTS );
		return false;
	}

	sm = *(mysem_t **)hndl;

	rc = pthread_mutex_lock (&sm->mx);
	if ( rc != 0 ) {
		_setlasterror( get_win_error (rc) );
		return false;
	}

	if ( sm->value + count > sm->maxvalue ) {	//incremented too much
		pthread_mutex_unlock (&sm->mx);
		_setlasterror( ERROR_BAD_ARGUMENTS );
		return false;
	}

	prev = sm->value;

	while (count-- && rc == 0) {
		if (sm->value == 0) {
			rc = pthread_cond_signal (&sm->cv);
			if (rc != 0) {	//can't signal condition variable
				pthread_mutex_unlock (&sm->mx);
				_setlasterror( get_win_error (rc) );
				return false;
			}
		}
		sm->value++;
	}

	pthread_mutex_unlock (&sm->mx);

	if (previous) *previous = prev;
	return true;
}

static
bool _closesemaphorehandle( uintptr_t hndl )
{
	/*if (!hndl) {
		_setlasterror( ERROR_BAD_ARGUMENTS );
		return false;
	}*/

	SEMAPHOREOBJDATA *data;
	unsigned int *refcount;

	data = (SEMAPHOREOBJDATA*)_gethandledata( hndl, &refcount );
	if (*refcount <= 1) {
		int rc;
		mysem_t *ps;
		const char *name;
		int fdsemaphore;

		ps = *(mysem_t**)hndl;
		fdsemaphore = data->fdsemaphore;
		name = data->name;
		//flags = 0;	//oh->flags;		//TODO: enable object's flags

		if ( *name ) {
			//check the file is opened
			struct flock fl;
			fl.l_type   = F_UNLCK;
			fl.l_whence = SEEK_SET;
			fl.l_start  = 0;
			fl.l_len    = 0;
			fl.l_pid    = getpid ();
			rc = fcntl (fdsemaphore, F_SETLK, &fl);		//release lock

			fl.l_type   = F_WRLCK;
			rc = fcntl (fdsemaphore, F_GETLK, &fl);
			if (rc == 0 && fl.l_type == F_UNLCK && fl.l_pid == getpid ()) {
				shm_unlink (name);	//unlink created SHM

				pthread_mutex_destroy (&ps->mx);
				pthread_cond_destroy (&ps->cv);
			}

			munmap ((void*)ps, sizeof(mysem_t));
			close (fdsemaphore);
		} else {
			pthread_mutex_destroy (&ps->mx);
			pthread_cond_destroy (&ps->cv);

			free (ps);
		}
	}

	return true;
}

static
bool _duplicatesemaphorehandle( uintptr_t srchndl, uintptr_t *targethndl, unsigned access, bool inherit, unsigned options )
{
	return true;
}
