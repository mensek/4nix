/*
 * Copyright (C) 2015 Frantisek Mensik
 * critsect.c is part of the 4nix.org project.
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

#include "windows.h"


// depends on these functions:
extern void _setlasterror( unsigned err );
extern unsigned get_win_error( int err );


bool _initializecriticalsectionex( CRITICAL_SECTION *crit, DWORD spincount, DWORD flags )
{
	//TODO: implement the spin count on multi-processor systems

	//NOTE: spincount is ignored on uni-processor systems
	//NOTE: typedef pthread_mutex_t CRITICAL_SECTION;

	if ( !crit ) {
		_setlasterror( ERROR_BAD_ARGUMENTS );
		return false;
	}

	int rc;
	pthread_mutex_t cs;
	pthread_mutexattr_t attr;

	pthread_mutexattr_init (&attr);
	//pthread_mutexattr_setpshared( &attr, PTHREAD_PROCESS_PRIVATE );
	#if defined (__linux__) || defined (__bsd__)
	pthread_mutexattr_setkind_np (&attr, PTHREAD_MUTEX_RECURSIVE_NP);
	#else
	pthread_mutexattr_settype (&attr, PTHREAD_MUTEX_RECURSIVE);
	#endif
	rc = pthread_mutex_init (&cs, &attr);
	pthread_mutexattr_destroy (&attr);

	if ( rc != 0 ) {
		_setlasterror( get_win_error (errno) );
		return false;
	}

	*crit = cs;
	return true;
}

void _entercriticalsection( CRITICAL_SECTION *crit )
{
    int rc;
    
    rc = pthread_mutex_lock (crit);
    if ( rc != 0 )
        _setlasterror( get_win_error (rc) );
}

bool _tryentercriticalsection( CRITICAL_SECTION *crit )
{
    int rc;
    
    rc = pthread_mutex_trylock (crit);
    if ( rc != 0 ) {
        _setlasterror( get_win_error (rc) );
        return FALSE;
    }
    
    return TRUE;
}

void _leavecriticalsection( CRITICAL_SECTION *crit )
{
    int rc;
    
    rc = pthread_mutex_unlock (crit);
    if ( rc != 0 )
        _setlasterror( get_win_error (rc) );
}

void _uninitializecriticalsection( CRITICAL_SECTION *crit )
{
    int rc;
    
    rc = pthread_mutex_destroy (crit);
    if ( rc != 0)
        _setlasterror( get_win_error (rc) );
}
