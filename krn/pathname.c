/* 
 * Copyright (C) 2015 Frantisek Mensik
 * pathname.c is part of the 4nix.org project.
 *
 * This file is licensed under the GNU Lesser General Public License.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif	//HAVE_CONFIG_H

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
//#ifndef HAVE_UNISTD_H
 #include <unistd.h>
//#endif

#include "windows.h"


// depends on these functions
extern void _setlasterror( unsigned err );
extern unsigned get_win_error( int err );
extern unsigned _getfileattributes( const char *name );


unsigned _getfullpathname( const char *name, unsigned len, char *buffer, char **lastpart )
{
	//TODO: original function works also when the file does not exist
	char res_path[PATH_MAX];		//todo: check
	char *path;
	size_t pathlen;

	path = realpath (name, res_path);
	if (!path) {
		_setlasterror( get_win_error (errno) );
		return 0;
	}

	pathlen = strlen (res_path);
	if (pathlen >= len) {
		_setlasterror( get_win_error (ERANGE) );
		return (unsigned)(pathlen+1);
	}

	strncpy (buffer, res_path, pathlen); buffer[pathlen] = '\0';

	if (lastpart) {
		if ( _getfileattributes( buffer ) & FILE_ATTRIBUTE_DIRECTORY ) {
			path = NULL;
		} else {
			path = rindex (buffer, '/');
			if (path) path++;
		}
		*lastpart = path;
	}

	return (unsigned)pathlen;
}

unsigned _getlongpathname( const char* shortpath, char* longpath, unsigned longlen )
{
	//note: implement for Windows like file systems

	if (!shortpath || !longpath) {
		_setlasterror( ERROR_INVALID_PARAMETER );
		return 0;
	}

	size_t len = strlen (shortpath);
	if (!len) {
		_setlasterror( ERROR_PATH_NOT_FOUND );
		return 0;
	}
	if (len >= longlen) {
		_setlasterror( get_win_error(ERANGE) );
		return (unsigned)(len+1);
	}

	//TODO: add file exists check

	strncpy (longpath, shortpath, len); longpath[len] = '\0';
	return (unsigned)len;
}

unsigned _getshortpathname( const char* longpath, char* shortpath, unsigned shortlen )
{
	//note: only for Windows like file systems

	_setlasterror (ERROR_CALL_NOT_IMPLEMENTED);
	return 0;
}

bool _checknamelegaldos8dot3( const char* lpName, char *lpOemName, unsigned dwOemNameSize,
                              bool* pbNameContainsSpaces, bool* pbNameLegal )
{
	register char *lptr;
	int c, n, lastdot = 0;
	bool legal = true, hasspace = false;
	BOOL isdir;

	if (!lpName) {
		if (pbNameContainsSpaces) *pbNameContainsSpaces = false;
		if (pbNameLegal) *pbNameLegal = false;
		_setlasterror( ERROR_INVALID_PARAMETER );
		return FALSE;
	}

	isdir = (GetFileAttributesA( lpName ) & FILE_ATTRIBUTE_DIRECTORY);
	c = 0;
	lptr = strrchr (lpName, '/');
	if (!lptr) lptr = (char*)lpName;
	else lptr++;

	while (*lptr)
	{
		if (legal)
		{
			c++;

			if (*lptr == '.') lastdot = c;
			else {
				//check illegal characters
				if (strchr ("<>:\"/\\|?*", *lptr) != NULL)
					legal = false;
			}

			if (!pbNameContainsSpaces)
				if (c > (isdir ? 8 : 12)) legal = false;
		}

		if (pbNameContainsSpaces) {
			if (!hasspace && *lptr == ' ') hasspace = true;
		} else {
			if (!legal) break;
		}

		lptr++;
	}
	n = c;
	if (!isdir && lastdot)
		c = lastdot - 1;
	if (!c || lpName[c-1] == ' ')	//no filename or illegal space
		legal = false;

	if (legal) {
		if (isdir) {
			if ((c > 0 && c <= 8) &&
			    !lastdot)
				;
			else
				legal = false;
		} else {
			if ((c > 0 && c <= 8) &&
			    (lastdot && (n-lastdot > 0 && n-lastdot < 4)))
				;
			else
				legal = false;
		}
	}

	if (pbNameContainsSpaces) *pbNameContainsSpaces = (hasspace == true);
	if (pbNameLegal) *pbNameLegal = (legal == true);
	return (legal == true);
}
