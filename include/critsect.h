/*
 * Copyright (C) 2015 Frantisek Mensik
 * critsect.h is part of the 4nix.org project.
 *
 * This file is licensed under the GNU Lesser General Public License.
 */

#ifndef __CRITSECT_H__
#define __CRITSECT_H__

#include <pthread.h>

typedef pthread_mutex_t CRITICAL_SECTION, *PCRITICAL_SECTION, *LPCRITICAL_SECTION;

#endif //__CRITSECT_H__
