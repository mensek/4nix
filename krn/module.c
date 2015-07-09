/*
 * Copyright (C) 2015 Frantisek Mensik
 * module.c is part of the 4nix.org project.
 *
 * This file is licensed under the GNU Lesser General Public License.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif	//HAVE_CONFIG_H

#if (defined __linux__ || defined __CYGWIN__) && !defined(_GNU_SOURCE)
# define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#if defined(__linux__) || defined(__bsd__)
# include <link.h>
#endif
#if defined(__MACH__)
# include <mach-o/dyld.h>
#endif
//#include <proc/readproc.h>

#include <limits.h>
#include <fcntl.h>
#include <unistd.h>

#include "windows.h"


// depends on these functions:
extern void _setlasterror( unsigned err );
extern unsigned get_win_error( int err );
extern uintptr_t _getcurrentprocess( );


#ifdef __cplusplus
extern "C" {
#endif

//NOTE: a global variable initialized when starting
static const char *_pgmptr = NULL;

static const char* get_progpath ()
{
	static char buf[PATH_MAX];	//TODO: dynamic buffer for _pgmptr variable

	if (!_pgmptr) {
		ssize_t rc;

		#if defined __linux__ || defined __CYGWIN__
		char epathbuf[512];
		const char *epath;
		rc = readlink ("/proc/self/exe", epathbuf, sizeof(epathbuf));
		if (rc != -1) {
			epathbuf[rc] = '\0';
			epath = epathbuf;
		}
		#ifndef __CYGWIN__
		else {
			Dl_info info;
			rc = dladdr (get_progpath, &info);
			if (rc == 0) { /*printf ("error\n");*/ return NULL; }
			epath = info.dli_fname;
		}
		#endif

		#elif defined __MACH__
		char epath[512];
		uint32_t size = sizeof(epath);
		rc = _NSGetExecutablePath (epath, &size);
		if (rc != 0) { /*printf ("error\n");*/ return NULL; }

		#elif defined __freebsd__
		int mib[4];
		mib[0] = CTL_KERN;
		mib[1] = KERN_PROC;
		mib[2] = KERN_PROC_PATHNAME;
		mib[3] = -1;
		//char buf[1024];
		char epath[512];
		size_t cb = sizeof(epath);
		sysctl (mib, 4, epath, &cb, NULL, 0);

		#else
		#error get_progpath function not implemented for your platform

		#endif

		buf[0] = '\0';
		realpath (epath, buf);
		buf[sizeof(buf)-1] = '\0';

		__sync_val_compare_and_swap (&_pgmptr, NULL, buf);
	}

	return _pgmptr;
}

static
unsigned _getmodulefilename( uintptr_t hModule, char* filename, unsigned size )
{
	// NOTES:
	// This function always returns the long path of hModule
	// The function doesn't write a terminating '\0' if the buffer is too small.

	const char *modulename;
	size_t len;
	unsigned rc;

#if !defined __MACH__ && !defined __CYGWIN__
	if ( hModule == (uintptr_t)NULL ) {
#endif
		modulename = get_progpath ();
#if !defined __MACH__ && !defined __CYGWIN__
	} else {
		// get path to a library requested by hModule
		void *addr = (void*)hModule;
		modulename = ((struct link_map*)addr)->l_name;
	}
#endif

	if ( modulename == NULL || modulename[0] == '\0' ) {
		_setlasterror( ERROR_INVALID_DATA );
		return 0;
	}

	len = strlen (modulename);
	if ( len >= size ) {
		_setlasterror( ERROR_INSUFFICIENT_BUFFER );
		rc = size;
		len = size - 1;
	} else
		rc = (unsigned)len;

	if (filename) { strncpy (filename, modulename, len); filename[len] = '\0'; }
	return rc;
}

unsigned _getmodulefilenameex( uintptr_t hProcess, uintptr_t hModule, char* filename, unsigned size )
{
	unsigned cnt;

	if ( hProcess == _getcurrentprocess( ) )		//todo: check this
		cnt = _getmodulefilename( hModule, filename, size );
	else
	{
		/*PROCTAB *proct;
		proc_t *proc_info;
		pid_t pid;

		pid = getpid ();		//TODO: extract from hProcess
		//NOTE: I don't know how to get opened shared library name when using 'procps'

		proct = openproc (PROC_FILLCOM);
		if (proct == NULL) {
			_setlasterror( get_win_error (errno) );
			return 0;
		}

		while ( (proc_info = readproc (proct, NULL)) ) {
			if (proc_info->tid == pid) {
				strncpy (lpFilename, proc_info->cmdline[0], nSize); lpFilename[nSize-1] = '\0';
				break;
			}
		}

		closeproc (proct);

		cnt = strlen (lpFilename);*/

		//TODO: enable this function
		_setlasterror( ERROR_CALL_NOT_IMPLEMENTED );
		cnt = 0;
	}

	return cnt;
}

unsigned _getmodulebasename( uintptr_t hProcess, uintptr_t hModule, char* basename, unsigned size )
{
	unsigned cnt;
	char szFilename[PATH_MAX];

	cnt = _getmodulefilenameex( hProcess, hModule, szFilename, sizeof(szFilename) );
	if (cnt > 0) {
		size_t len;

		char *lptr = strrchr (szFilename, '/');
		lptr = (lptr ? lptr+1 : szFilename);

		len = strlen (lptr);
		if ( len >= size ) {
			_setlasterror( ERROR_INSUFFICIENT_BUFFER );
			return 0;
		}

		strcpy (basename, lptr);
		cnt = (unsigned)len;
	}
	return cnt;
}

bool _getmodulehandleex( unsigned flags, const char* name, uintptr_t *module )
{
	//TODO: implement
	_setlasterror( ERROR_CALL_NOT_IMPLEMENTED );
	if (module) *module = (uintptr_t)NULL;
	return false;
}

bool _enumprocessmodulesex( uintptr_t hProcess, uintptr_t* lphModule, unsigned cb, unsigned *lpcbNeeded, unsigned dwFilterFlag )
{
	//TODO: implement the function
	_setlasterror( ERROR_CALL_NOT_IMPLEMENTED );
	return false;
}

uintptr_t DECLSPEC_HOTPATCH _loadlibraryex( const char* libname, uintptr_t hFile, unsigned dwFlags )
{
	// load a dll file into the process address space
	void *lib;

	if ( !libname || hFile != 0 ) {
		// hFile is reserved and must be 0
		_setlasterror( ERROR_INVALID_PARAMETER );
		return (uintptr_t)NULL;
	}

	lib = dlopen (libname, RTLD_NOW);
	if ( lib == NULL ) {
		//TODO: implement error codes
		_setlasterror( ERROR_MOD_NOT_FOUND );
		return (uintptr_t)NULL;
	}

	//TODO: implemnt library HANDLEs
	return (uintptr_t)lib;
}

bool DECLSPEC_HOTPATCH _freelibrary( uintptr_t hModule )
{
	// free a dll loaded into the process address space
	int rc;
	void *lib;

	if ( !hModule ) {
		_setlasterror( ERROR_INVALID_HANDLE );
		return false;
	}

	lib = (void*) hModule;
	rc = dlclose (lib);
	if ( rc != 0 ) {
		//todo: implent error codes
		_setlasterror( ERROR_INVALID_PARAMETER );
		return false;
	}
	return true;
}

FARPROC _getprocaddress( uintptr_t hModule, const char* function )
{
	// find the address of an exported symbol in loaded dll
	void *lib, *fnc;

	lib = (void*) hModule;
	fnc = dlsym (lib, function);
	if ( !fnc ) {
		_setlasterror( ERROR_CALL_NOT_IMPLEMENTED );
		return (FARPROC)NULL;
	}
	return (FARPROC)fnc;
}

#ifdef __cplusplus
}
#endif
