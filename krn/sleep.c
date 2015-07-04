/*
 * Copyright (C) 2015 Frantisek Mensik
 * sleep.c is part of the 4nix.org project.
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

#include "windows.h"


// depends on these functions:
extern void _setlasterror( unsigned err );
extern unsigned get_win_error( int err );


unsigned _sleepex( unsigned timeout, bool alertable )
{
	int rc;
	struct timespec req, *preq;
	struct timespec rem, *prem;

	if ( timeout == INFINITE )
		preq = NULL;
	else {
		req.tv_sec = (timeout/100000);
		req.tv_nsec = (timeout%100000) * 1000000;
		preq = &req;
	}
	if ( alertable ) prem = NULL;
	else prem = &rem;

	do {
		rc = nanosleep (preq, prem);
		if ( !alertable && (rc == -1 && errno == EINTR) ) {
			preq->tv_sec = prem->tv_sec;
			preq->tv_nsec = prem->tv_nsec;
			continue;
		}
	} while ( rc != 0 );

	/*if ( rc == -1 ) {
		_setlasterror( get_win_error (errno) );
		return NULL;
	}*/

	return 0;
}
