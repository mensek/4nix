/*
 * Copyright (C) 2015 Frantisek Mensik
 * thread.c is part of the 4nix.org project.
 *
 * This file is licensed under the GNU Lesser General Public License.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif	//HAVE_CONFIG_H

#ifdef __linux__
#define _GNU_SOURCE
#endif

#include <sched.h>
#include <stdlib.h>
//#ifdef HAVE_UNISTD_H
# include <unistd.h>
//#endif	//HAVE_UNISTD_H
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pthread.h>

#ifdef __MACH__
# include <mach/mach.h>
# include <mach/mach_host.h>
# include <mach/mach_time.h>
#endif
#ifdef __linux__
# include <syscall.h>
#endif

#include "windows.h"
#include "timedef.h"
#include "object.h"


// depends on these functions
extern void _setlasterror( unsigned err );
extern unsigned get_win_error( int err );


#define THREADOBJID		4

bool _closethreadhandle( uintptr_t hndl );
/*bool _duplicatethreadhandle( uintptr_t srchndl, uintptr_t *targethndl,
                               unsigned access, bool inherit, unsigned options );
bool _gethandleinformation( uintptr_t hndl, unsigned *flags );
bool _sethandleinformation( uintptr_t hndl, unsigned mask, unsigned flags );*/
unsigned _waitforsinglethreadobject( uintptr_t hndl, unsigned milliseconds );

static TYPEOBJITEM threadkrnlobj =
{
	THREADOBJID, true,
	_closethreadhandle,
	NULL,   //_duplicatethreadhandle,
	NULL,   //_gethandleinformation,
	NULL,   //_sethandleinformation,
	_waitforsinglethreadobject
};


typedef struct THREADOBJDATA_ {
	pid_t tid;
	pthread_mutex_t *firstresumesync;
	int pipefd;

	FILETIME creationtime;
	FILETIME exittime;
	//FILETIME kerneltime;
	FILETIME usertime;
	DWORD exitcode;
} THREADOBJDATA;


// starting address of a thread
//DWORD WINAPI ThreadProc( LPVOID lpParameter );


//wrapped thread function
typedef struct THREADSPRMS_WRAP {
	//unsigned ( __stdcall *origfunc)(void*);
	LPTHREAD_START_ROUTINE origstart;
	LPVOID origprms;
	pthread_mutex_t *firstresumesync;
	int pipefd;
} THREADPRMS_WRAP;

static void pthreadcleanup (void *arg);

static
unsigned prethreadfunc (void *arg)
{
	THREADPRMS_WRAP *pre_prms;

	unsigned long res;
	unsigned (__stdcall *start)( void * );
	LPVOID prms;
	pthread_mutex_t *pm;
	int pipefd;

	//block some signals
	//sigemptyset (&signal_mask);
	//sigaddset (&signal_mask, SIGINT);
	//sigaddset (&signal_mask, SIGTERM);
	//sigaddset (&signal_mask, SIGKILL);
	//pthread_sigmask (SIG_BLOCK, &signal_mask, NULL);

	pre_prms = (THREADPRMS_WRAP*)arg;
	start = pre_prms->origstart;
	prms = pre_prms->origprms;
	pm = pre_prms->firstresumesync;
	pipefd = pre_prms->pipefd;

	free (pre_prms);

	// close the pipe
	pthread_cleanup_push (pthreadcleanup, (void*)&pipefd);

#ifndef __MACH__
	pid_t tid;
	tid = syscall (SYS_gettid);

	ssize_t rc;
	rc = write (pipefd, &tid, sizeof(tid));
	if (rc == -1)
		res = -1;
	else
#endif
	{
		if (pm)
			pthread_mutex_lock (pm);	// suspend a new thread

		//start the original function
		res = (unsigned) start( prms );
	}

	pthread_cleanup_pop (1);

	pthread_exit ((void*)res);
	return res;
}

static
void pthreadcleanup (void *arg)     //cleanup function
{
	if (!arg) return;

	//TODO: set exit code - 0 jako defaultni hodnota
	// set exit and user time

	//HANDLE hThread;
	//hThread = GetCurrentThread( );
	//THREADHANDLE* hndl = (THREADHANDLE*)hThread;
	//hndl->exittime = ;
	//hndl->usertime = ;
	//hndl->exitcode = ;

	int *pipefd = (int*)arg;
	if (pipefd && *pipefd != -1)
		close (*pipefd);
}

static
bool isvalidthreadhandle (uintptr_t hndl)
{
	return (_gethandletypeid( hndl ) == THREADOBJID || hndl == (uintptr_t)CURRENT_THREAD_HANDLE);
}

static
uintptr_t getcurrentthreadrealhandle ()
{
	//TODO: update this code
	uintptr_t hndl;
	pthread_t thr = pthread_self ();

	hndl = _getrealhandle ((uintptr_t)CURRENT_THREAD_HANDLE, THREADOBJID, (void*)&thr, sizeof(thr));
	return hndl;
}


uintptr_t _createthread( SECURITY_ATTRIBUTES *sa, size_t stack,
                         LPTHREAD_START_ROUTINE start, void *param,
                         unsigned flags, unsigned *id )
{
	int rc;
	pthread_t thr_id;
	pthread_attr_t thr_attr;

	struct timeval tv;
	FILETIME ct, nulltime = { 0, 0 };
	THREADPRMS_WRAP *pre_prms;
	pthread_mutex_t *pm = NULL;

	uintptr_t hndl;

	SIZE_T stacksize = stack;
	if ( stacksize > 0 ) {
		// round stack size to page size
		//SYSTEM_INFO sysinfo;
		//GetSystemInfo (&sysinfo);

		int pagesize = getpagesize ();
		stacksize = stack % pagesize;	//sysinfo.dwPageSize;
		if ( stacksize == 0 ) stacksize = stack;
		else stacksize = stack - stacksize + pagesize;	//sysinfo.dwPageSize;
	}

	if ( flags & CREATE_SUSPENDED ) {
		pm = malloc (sizeof (pthread_mutex_t));
		if ( !pm ) {
			SetLastError( get_win_error (errno) );
			return (uintptr_t)NULL;
		}

		pthread_mutexattr_t mattr;
		pthread_mutexattr_init (&mattr);
#ifdef __MACH__
		pthread_mutexattr_settype (&mattr, PTHREAD_MUTEX_ERRORCHECK);
#else
		pthread_mutexattr_settype (&mattr, PTHREAD_MUTEX_ERRORCHECK_NP);
#endif
		rc = pthread_mutex_init (pm, &mattr);
		pthread_mutexattr_destroy (&mattr);
		if ( rc != 0 ) {
			free (pm);
			_setlasterror( get_win_error (rc) );
			return (uintptr_t)NULL;
		}

		pthread_mutex_lock (pm);
	}

	int pipefd[2];
	rc = pipe (pipefd);	//create pipe to enable WaitForSingleObject
	if ( rc == -1 ) {
		_setlasterror( get_win_error (errno) );
		if (pm) {
			pthread_mutex_destroy (pm);
			free (pm);
		}
		return (uintptr_t)NULL;
	}

	pre_prms = malloc (sizeof(THREADPRMS_WRAP));
	if ( !pre_prms ) {
		_setlasterror( get_win_error (errno) );
		if (pm) {
			pthread_mutex_destroy (pm);
			free (pm);
		}
		close (pipefd[0]); close (pipefd[1]);		//close pipe
		return (uintptr_t)NULL;
	}

	pre_prms->origprms = param;
	pre_prms->origstart = start;
	pre_prms->firstresumesync = pm;		//signal suspended thread
	pre_prms->pipefd = pipefd[1];

	pthread_attr_init (&thr_attr);
	if ( stacksize > 0 )
		pthread_attr_setstacksize (&thr_attr, stacksize);
	pthread_attr_setdetachstate (&thr_attr, PTHREAD_CREATE_JOINABLE);	//PTHREAD_CREATE_DETACHED
	rc = pthread_create (&thr_id, &thr_attr, (void*(*)(void*))prethreadfunc, pre_prms);
	//get thread creation time
	gettimeofday (&tv, NULL);
	timeval_to_FILETIME( &tv, &ct );
	pthread_attr_destroy (&thr_attr);

	if ( rc != 0 ) {
		_setlasterror( get_win_error (rc) );
		if (pm) {
			pthread_mutex_destroy (pm);
			free (pm);
		}
		close (pipefd[0]); close (pipefd[1]);		//close pipe
		return (uintptr_t)NULL;
	}

	//get thread ID
	pid_t tid;
#ifdef __MACH__
	unsigned long long myid;
	pthread_threadid_np(thr_id, &myid);
	tid = (pid_t)myid;
#else
	ssize_t rcnt;
	rcnt = read (pipefd[0], &tid, sizeof(pid_t));
	if ( rcnt == -1) {
		SetLastError( get_win_error (errno) );
		if (pm) {
			pthread_mutex_destroy (pm);
			free (pm);
		}
		close (pipefd[0]); close (pipefd[1]);		//close pipe
		return (uintptr_t)NULL;
	}
#endif

	THREADOBJDATA data;
	data.tid             = tid;
	data.firstresumesync = pm;
	data.pipefd          = pipefd[0];
	//thread times
	data.creationtime    = ct;
	data.exittime        = nulltime;
	//data.kerneltime     = nulltime;
	data.usertime        = nulltime;
	data.exitcode        = STILL_ACTIVE;

	hndl = _createhandle (-1, 0, &threadkrnlobj, (void*)thr_id, sizeof(thr_id), (void*)&data, sizeof(data));
	if (!hndl) {
		SetLastError( get_win_error (errno) );

		pthread_cancel (thr_id);

		pthread_mutex_destroy (pm);
		free (pm);
		close (pipefd[0]); close (pipefd[1]);		//close pipe

		return (uintptr_t)NULL;
	}

	if (id) *id = (DWORD)tid;
	return (uintptr_t)hndl;
}

uintptr_t _openthread( unsigned dwDesiredAccess, bool bInheritHandle, unsigned dwThreadId )
{
	//TODO: implement the call
	_setlasterror( ERROR_CALL_NOT_IMPLEMENTED );
	return (uintptr_t)NULL;
}

bool _terminatethread( uintptr_t hndl, unsigned exit_code)
{
	int rc;
	pthread_t thr_id;

	if ( hndl == (uintptr_t)CURRENT_THREAD_HANDLE )
		hndl = getcurrentthreadrealhandle ();

	if ( !isvalidthreadhandle( hndl ) ) {
		_setlasterror( ERROR_INVALID_HANDLE );
		return false;
	}

	thr_id = *(pthread_t*)hndl;

	rc = pthread_cancel (thr_id);
	if( rc != 0 ) {
		_setlasterror( get_win_error (rc) );
		return false;
	}

	THREADOBJDATA *data;
	FILETIME et;
	struct timeval tv;
	gettimeofday (&tv, NULL);
	timeval_to_FILETIME( &tv, &et );

	data = (THREADOBJDATA*)_gethandledata (hndl, NULL);
	data->exitcode = exit_code;
	data->exittime = et;

	return true;
}

DECLSPEC_NORETURN
void _exitthread( uintptr_t hndl, unsigned code )
{
	THREADOBJDATA *data;
	FILETIME et;
	struct timeval tv;

	if ( hndl == (uintptr_t)CURRENT_THREAD_HANDLE )
		hndl = getcurrentthreadrealhandle ();

	if ( !isvalidthreadhandle( hndl ) ) {
		_setlasterror( ERROR_INVALID_HANDLE );
		return ;
	}

	gettimeofday (&tv, NULL);
	timeval_to_FILETIME( &tv, &et );

	data = (THREADOBJDATA*)_gethandledata (hndl, NULL);
	data->exitcode = code;
	data->exittime = et;

	pthread_exit ((void*)code);
}

bool _getexitcodethread( uintptr_t hndl, unsigned *exitcode )
{
	THREADOBJDATA *data;

	if ( hndl == (uintptr_t)CURRENT_THREAD_HANDLE )
		hndl = getcurrentthreadrealhandle ();

	if ( !isvalidthreadhandle( hndl ) ) {
		_setlasterror( ERROR_INVALID_HANDLE );
		return false;
	}

	data = _gethandledata( hndl, NULL );

	if ( exitcode ) *exitcode = data->exitcode;
	return true;
}

unsigned _waitforsinglethreadobject( uintptr_t hndl, unsigned milliseconds )
{
	// TODO: implement retval

	int rc;
	pthread_t thr_id;
	void *retval;

	//TODO: check
	//if ( !hndl ) {
	//	_setlasterror( ERROR_INVALID_HANDLE );
	//	return FALSE;
	//}

	thr_id = *(pthread_t*)hndl;

#ifdef __linux__
	struct timespec *timeout, abstime;

	if ( milliseconds != INFINITE ) {
		clock_gettime (CLOCK_REALTIME, &abstime);

		abstime.tv_sec = (milliseconds / 1000);
		abstime.tv_nsec = (milliseconds % 1000) * 1000000;
		timeout = &abstime;
	} else
		timeout = NULL;

	if (timeout)
		rc = pthread_timedjoin_np (thr_id, &retval, timeout);
	else
		rc = pthread_join (thr_id, &retval);

	if (rc != 0)
		return (rc == ETIMEDOUT ? WAIT_TIMEOUT : WAIT_FAILED);

#else
	THREADOBJDATA *data;
	int pipefd;
	struct timeval *timeout, tv;

	data = (THREADOBJDATA*)_gethandledata(hndl);
	pipefd = data->pipefd;

	fd_set rset;
	FD_ZERO (&rset);
	FD_SET (pipefd, &rset);

	if ( milliseconds != INFINITE ) {
		tv.tv_sec = (milliseconds / 1000);
		tv.tv_usec = (milliseconds % 1000) * 1000;
		timeout = &tv;
	} else
		timeout = NULL;

	rc = select (pipefd+1, &rset, NULL, NULL, timeout);	//TODO: enable signal handling

	if (rc == -1) {
		_setlasterror( get_win_error (errno) );
		return WAIT_FAILED; }
	if (rc == 0 )
		return WAIT_TIMEOUT;
#endif

	return WAIT_OBJECT_0;
}

bool _closethreadhandle( uintptr_t hndl )
{
	//TODO: implement the CloseHandle function
	//_setlasterror( ERROR_CALL_NOT_IMPLEMENTED );
	//return false;

	return true;
}

unsigned _getcurrentthreadid( )
{
#ifdef __MACH__
	int rc;
	unsigned long long thr_id;
	rc = pthread_threadid_np (pthread_self(), &thr_id);
#else
	int thr_id;
	thr_id = syscall (SYS_gettid);
#endif  //__MACH__

	return thr_id;
}

unsigned _suspendthread( uintptr_t hndl )
{
	int rc;
	pthread_t thr_id;

	if ( hndl == (uintptr_t)CURRENT_THREAD_HANDLE )
		hndl = getcurrentthreadrealhandle ();

	if ( !isvalidthreadhandle( hndl ) ) {
		_setlasterror( ERROR_INVALID_HANDLE );
		return ~0U;
	}

	thr_id = *(pthread_t*)hndl;

	rc = pthread_kill (thr_id, SIGTSTP);	//SIGSTOP
	if (rc != 0) {
		_setlasterror( get_win_error (rc) );
		return ~0U;	//Failure: 0xFFFFFFFF
	}

	return 0;
}

unsigned _resumethread( uintptr_t hndl )
{
	int rc;
	THREADOBJDATA *data;
	pthread_t thr_id;
	pthread_mutex_t *pm;

	if ( hndl == (uintptr_t)CURRENT_THREAD_HANDLE )
		hndl = getcurrentthreadrealhandle ();

	if ( !isvalidthreadhandle( hndl ) ) {
		_setlasterror( ERROR_INVALID_HANDLE );
		return ~0U;
	}

	thr_id = *(pthread_t*)hndl;
	data = (THREADOBJDATA*)_gethandledata( hndl, NULL );
	pm = data->firstresumesync;

	if ( pm ) {
		rc = pthread_mutex_unlock (pm);
		if ( rc == 0 ) {
			data->firstresumesync = NULL;
			pthread_mutex_destroy (pm);
		}
		if ( rc != 0 && rc != EPERM ) {
			_setlasterror( get_win_error (rc) );
			return ~0U;
		} else if ( rc == 0 )
			return 0;
	}

	rc = pthread_kill (thr_id, SIGCONT);

	if (rc != 0) {
		_setlasterror( get_win_error (rc) );
		return ~0U;	//Failure: 0xFFFFFFFF
	}

	return 0;
}

int _getthreadpriority( uintptr_t hndl )
{
	int rc, priority, policy;
	struct sched_param schprm;
	pthread_t thr_id;

	if ( hndl == (uintptr_t)CURRENT_THREAD_HANDLE )
		hndl = getcurrentthreadrealhandle ();

	if ( !isvalidthreadhandle( hndl ) ) {
		_setlasterror( ERROR_INVALID_HANDLE );
		return THREAD_PRIORITY_ERROR_RETURN;
	}

	thr_id = *(pthread_t*)hndl;

	rc = pthread_getschedparam( thr_id, &policy, &schprm );
	if ( rc == 0 ) {
		if (policy == SCHED_RR || policy == SCHED_FIFO) {
			int min, max;

			#ifdef __OpenBSD__
			min = 0;	//PTHREAD_MIN_PRIORITY
			max = 31;	//PTHREAD_MAX_PRIORITY
			#else
			min = sched_get_priority_min (policy);
			max = sched_get_priority_max (policy);
			#endif	//__OpenBSD__
			//mid = ( max - min ) / 2 + min;

			//TODO: implement this part
			//case THREAD_PRIORITY_IDLE:          break;	//TODO: implement this
			//case THREAD_PRIORITY_LOWEST:        prio = min; break;
			//case THREAD_PRIORITY_BELOW_NORMAL:  prio = mid - ( (max-min)/4 + (((max-min)%4)/2) ); break;
			//case THREAD_PRIORITY_NORMAL:        prio = mid; break;
			//case THREAD_PRIORITY_ABOVE_NORMAL:  prio = mid + ( (max-min)/4 + (((max-min)%4)/2) ); break;
			//case THREAD_PRIORITY_HIGHEST:       prio = min; break;
			//case THREAD_PRIORITY_TIME_CRITICAL: break;	//TODO: implement this

			/*
			case THREAD_PRIORITY_TIME_CRITICAL: iRealPriority = max; break;
			case THREAD_PRIORITY_HIGHEST:       iRealPriority = max - ((max-min)/3 + ((max-min)%3)/2); break;
			case THREAD_PRIORITY_ABOVE_NORMAL:  iRealPriority = min + ((max-min)/3 + ((max-min)%3)/2); break;
			case THREAD_PRIORITY_NORMAL:        iRealPriority = min; break;
			default:                            iRealPriority = min; break; }
			*/

		} else
			priority = THREAD_PRIORITY_NORMAL;
	}

	if ( rc != 0 ) {
		_setlasterror( get_win_error (rc) );
		return THREAD_PRIORITY_ERROR_RETURN;
	}

	return priority;
}

bool _setthreadpriority( uintptr_t hndl, int priority )
{
	int rc;
	pthread_t thr_id;

	if ( hndl == (uintptr_t)CURRENT_THREAD_HANDLE )
		hndl = getcurrentthreadrealhandle ();

	if ( !isvalidthreadhandle( hndl ) ) {
		_setlasterror( ERROR_INVALID_HANDLE );
		return false;
	}

	thr_id = *(pthread_t*)hndl;

	struct sched_param schprm;
	int policy, iRealPriority;

	rc = pthread_getschedparam( thr_id, &policy, &schprm );
	if ( rc == 0 ) {
		if (policy == SCHED_RR || policy == SCHED_FIFO) {
			int min, max;

			#ifdef __OpenBSD__
			min = 0;	//PTHREAD_MIN_PRIORITY
			max = 31;	//PTHREAD_MAX_PRIORITY
			#else
			min = sched_get_priority_min (policy);
			max = sched_get_priority_max (policy);
			#endif	//__OpenBSD__
			//mid = ( max - min ) / 2 + min;

			switch (priority) {
			//TODO: set proper priorities values bellow (THREAD_PRIORITY_IDLE, THREAD_PRIORITY_TIME_CRITICAL)
			//TODO: implemnt these two priority flags
			//THREAD_MODE_BACKGROUND_BEGIN
			//THREAD_MODE_BACKGROUND_END
			//case THREAD_PRIORITY_IDLE:          break;	//TODO: implement this
			//case THREAD_PRIORITY_LOWEST:        prio = min; break;
			//case THREAD_PRIORITY_BELOW_NORMAL:  prio = mid - ( (max-min)/4 + (((max-min)%4)/2) ); break;
			//case THREAD_PRIORITY_NORMAL:        prio = mid; break;
			//case THREAD_PRIORITY_ABOVE_NORMAL:   prio = mid + ( (max-min)/4 + (((max-min)%4)/2) ); break;
			//case THREAD_PRIORITY_HIGHEST:       prio = min; break;
			//case THREAD_PRIORITY_TIME_CRITICAL: break;	//TODO: implement this
			//default:
			//		_setlasterror( ERROR_INVALID_PARAMETER ); return FALSE;
			//		break;
				
			case THREAD_PRIORITY_TIME_CRITICAL: iRealPriority = max; break;
			case THREAD_PRIORITY_HIGHEST:       iRealPriority = max - ((max-min)/3 + ((max-min)%3)/2); break;
			case THREAD_PRIORITY_ABOVE_NORMAL:  iRealPriority = min + ((max-min)/3 + ((max-min)%3)/2); break;
			case THREAD_PRIORITY_NORMAL:        iRealPriority = min; break;
			default:                            iRealPriority = min; break; }

			schprm.sched_priority = iRealPriority;
			rc = pthread_setschedparam (thr_id, policy, &schprm);
		} else {
			if (priority != THREAD_PRIORITY_NORMAL)
				rc = ENOTSUP;
		}
	}
	if ( rc != 0 ) {
		_setlasterror( get_win_error (rc) );
		return false;
	}

	return true;
}

DWORD_PTR _setthreadaffinitymask( uintptr_t hndl, DWORD_PTR dwThreadAffinityMask )
{
	DWORD_PTR affinitymask_prev = 0x0;
#ifndef __MACH__
	int rc, i;
	int logical_cpus;
	cpu_set_t cs, cs_prev;

	if ( hndl == (uintptr_t)CURRENT_THREAD_HANDLE )
		hndl = getcurrentthreadrealhandle ();

	if ( !isvalidthreadhandle( hndl ) ) {
		_setlasterror( ERROR_INVALID_HANDLE );
		return 0;
	}

	pthread_t thr_id = *(pthread_t*)hndl;

	logical_cpus = sysconf (_SC_NPROCESSORS_ONLN);		//_CONF

	rc = pthread_getaffinity_np (thr_id, sizeof(cs_prev), &cs_prev);
	if (rc != 0) {
		_setlasterror( get_win_error (rc) );
		return 0;
	}

	CPU_ZERO(&cs_prev);
	for (i = 0; i < logical_cpus && i < sizeof(DWORD_PTR); i++)
	{
		if (CPU_ISSET(i, &cs))
			affinitymask_prev |= 0x1 << i;
	}

	CPU_ZERO(&cs);
	for (i = 0; i < logical_cpus && i < sizeof(DWORD_PTR); i++)
	{
		if (dwThreadAffinityMask >> i & 0x1)
			CPU_SET(i, &cs);
	}

	rc = pthread_setaffinity_np (thr_id, sizeof(cs), &cs);
	if (rc != 0) {
		_setlasterror( get_win_error (rc) );
		return 0;
	}
#endif  //__MACH__

	return affinitymask_prev;
}

bool _getthreadtimes( uintptr_t hndl, LPFILETIME creationtime,
                      LPFILETIME exittime, LPFILETIME kerneltime,
                      LPFILETIME usertime)
{
	FILETIME ct, ut, kt, et = {0,0};
	pthread_t thr_id;

	if ( hndl == (uintptr_t)CURRENT_THREAD_HANDLE )
		hndl = getcurrentthreadrealhandle ();

	if ( !isvalidthreadhandle( hndl ) ) {
		_setlasterror( ERROR_INVALID_HANDLE );
		return false;
	}

	thr_id = *(pthread_t*)hndl;

#ifdef __MACH__
	kern_return_t kr;
	mach_port_t tid = pthread_mach_thread_np (thr_id);
	integer_t *thinfo = (integer_t *) malloc (sizeof (integer_t) * THREAD_INFO_MAX);

	mach_msg_type_number_t thread_info_count = THREAD_INFO_MAX;
	kr = thread_info (tid, THREAD_BASIC_INFO, (thread_info_t) thinfo, &thread_info_count);
	if (kr != KERN_SUCCESS) {
		//TODO: check error
		_setlasterror( get_win_error (errno) );
		free (thinfo);
		return false;
	}

	thread_basic_info_t ti = (thread_basic_info_t)thinfo;
	//TODO: upgrade this part
	ut = mach_timevalue_to_FILETIME (ti->user_time);
	kt = mach_timevalue_to_FILETIME (ti->system_time);

	free (thinfo);
#else
	THREADOBJDATA *data;
	data = (THREADOBJDATA*)_gethandledata( hndl, NULL );
	ct = data->creationtime;
	//TODO: finish this part
	if ( data->exitcode == STILL_ACTIVE ) {
		int rc;
		/*clockid_t clk;
		struct timespec ts;

		rc = pthread_getcpuclockid (thr_id, &clk);
		if ( rc == 0 ) {
			rc = clock_gettime (clk, &ts);
			if ( rc == 0 )
				timespec_to_FILETIME( &ts, &ut );
		}*/
		if (pthread_equal (pthread_self(), thr_id)) {
			struct rusage ru;

			//TODO: pouzit i pro aktivni proces
			rc = getrusage (RUSAGE_THREAD, &ru);
			if (rc == 0) {
				timeval_to_FILETIME( &ru.ru_stime, &kt);	//kernel time (seconds + microseconds)
				timeval_to_FILETIME( &ru.ru_utime, &ut);	//user time (seconds + microseconds)
			}
		} else {
			char procFilename[64], buffer[1024];
			pid_t pid, tid;

			pid = getpid ();
			tid = data->tid;	//syscall (SYS_gettid);

			sprintf (procFilename, "/proc/%d/task/%d/stat", pid, tid);
			int i, fd, num_read;
			fd = open (procFilename, O_RDONLY, 0);
			if (fd == -1) {
				_setlasterror( get_win_error (errno) );
				return false;
			}

			num_read = read (fd, buffer, sizeof(buffer-1));
			close (fd);
			buffer[num_read] = '\0';

			char* ptrUsr = strrchr (buffer, ')') + 1;
			for (i = 3 ; i != 14 ; ++i) ptrUsr = strchr (ptrUsr+1, ' ');

			ptrUsr++;
			long jiffies_user = atol (ptrUsr);
			long jiffies_sys = atol (strchr(ptrUsr,' ') + 1);

			struct timespec ts_usr, ts_sys;
			jiffies_to_timespec (jiffies_user, &ts_usr);
			jiffies_to_timespec (jiffies_sys, &ts_sys);

			timespec_to_FILETIME (&ts_usr, &ut);
			timespec_to_FILETIME (&ts_sys, &kt);
		}
	} else {
		et = data->exittime;
	}
#endif

	if (creationtime) *creationtime = ct;
	if (exittime) *exittime = et;
	if (kerneltime) *kerneltime = kt;
	if (usertime) *usertime = ut;

	return true;
}

unsigned _getthreadid( uintptr_t hndl )
{
	if ( hndl == (uintptr_t)CURRENT_THREAD_HANDLE )
		hndl = getcurrentthreadrealhandle ();

	if ( !isvalidthreadhandle( hndl ) ) {
		_setlasterror( ERROR_INVALID_HANDLE );
		return 0;
	}

	THREADOBJDATA *data;
	data = (THREADOBJDATA*)_gethandledata( hndl, NULL );

	return data->tid;
}

bool _switchtothread( void )
{
	int rc;

#ifdef __MACH__
	pthread_yield_np (); rc = 0;
#else
	rc = pthread_yield ();
#endif
	if ( rc != 0 ) {
		_setlasterror( get_win_error( rc ) );
		return false;
	}

	return true;
}
