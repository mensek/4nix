/* 
 * Copyright (C) 2015 Frantisek Mensik
 * directory.c is part of the 4nix.org project.
 *
 * This file is licensed under the GNU Lesser General Public License.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif	//HAVE_CONFIG_H

#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
//#ifndef HAVE_UNISTD_H
# include <unistd.h>
//#endif

#include "windows.h"


// depends on these functions:
extern void _setlasterror( unsigned err );
extern unsigned get_win_error( int err );


bool _createdirectoryex( const char *template, const char* path, LPSECURITY_ATTRIBUTES sa )
{
	int rc;
	mode_t mode;

	if( !path || !*path ) {
		_setlasterror( ERROR_PATH_NOT_FOUND );
		return false;
	}

	if ( sa == NULL )	// the default
		mode = S_IRWXU | S_IRGRP|S_IXGRP | S_IROTH|S_IXOTH;
	else {
		mode = 0755;	//TODO: update, handle file permissions
	}

	rc = mkdir (path, mode);
	if (rc == -1) {
		DWORD dwErr;
		switch (errno) {
		case ENOENT: dwErr = ERROR_INVALID_NAME; break;
		default: dwErr = get_win_error (errno); break; }
		_setlasterror( dwErr );
		return false;
	}

	return true;
}

bool _removedirectory( const char* path )
{
	int rc;
	struct stat st;

	if ( lstat (path, &st) == 0 && S_ISLNK(st.st_mode) )		//TODO: update the check (whether it points to a directory or a file)
		rc = unlink (path);	//remove a link pointing to the directory
	else
		rc = rmdir (path);
	if (rc == -1) {
		DWORD dwErr;
		switch (errno) {
		case ENOENT: dwErr = ERROR_INVALID_NAME; break;
		case EFAULT: dwErr = ERROR_PATH_NOT_FOUND; break;
		default: dwErr = get_win_error (errno); break; }
		_setlasterror( dwErr );
		return false;
	}

	return true;
}

unsigned _getcurrentdirectory( unsigned buflen, char* buf )
{
	char *cwd;
	size_t len = 0;

	cwd = getcwd (buf, buflen);
	if (!cwd) {
		if (errno == ERANGE ||
		    (errno == EINVAL && buflen == 0))
		{
			#define PATH_BUFLEN	MAX_PATH * 32 * 4	//TODO: update the value
			cwd = (char*) malloc (PATH_BUFLEN);
			if (cwd) {
				char *pcwd;
				memset (cwd, 0, PATH_BUFLEN);
				pcwd = getcwd (cwd, PATH_BUFLEN);
				len = (pcwd ? strlen (pcwd) + 1 : 0);
				free (cwd);
				_setlasterror( pcwd ? NO_ERROR : get_win_error (errno) );
			} else {
				_setlasterror( get_win_error (ENOMEM) );
				return 0;
			}
		} else {
			_setlasterror( get_win_error (errno) );
			return 0;
		}
	} else
		len = strlen (buf);

	return len;
}

bool _setcurrentdirectory( const char* dir )
{
	int rc;

	if (!dir) {
		_setlasterror( ERROR_INVALID_NAME );
		return false;
	}

	rc = chdir (dir);
	if (rc == -1) {
		DWORD dwErr;
		switch (errno) {
		case ENOENT: dwErr = ERROR_INVALID_NAME; break;
		default: dwErr = get_win_error (errno); break; }
		_setlasterror( dwErr );
		return false;
	}

	return true;
}
