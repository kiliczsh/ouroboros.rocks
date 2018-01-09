/*
 * Ouroboros - Copyright (C) 2016 - 2018
 *
 * Utils of the IPC Resource Manager
 *
 *    Dimitri Staessens <dimitri.staessens@ugent.be>
 *    Sander Vrijders   <sander.vrijders@ugent.be>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., http://www.fsf.org/about/contact/.
 */

/*
 * Checks whether the string argument matches the pattern argument,
 * which is a wildcard pattern.
 */

#ifndef OUROBOROS_IRMD_UTILS_H
#define OUROBOROS_IRMD_UTILS_H

#include <sys/types.h>

struct str_el {
        struct list_head next;
        char *           str;
};

struct pid_el {
        struct list_head next;
        pid_t            pid;
};

int     wildcard_match(const char * pattern,
                       const char * string);

/* functions for copying and destroying arguments list */
char ** argvdup(char ** argv);

void    argvfree(char ** argv);

#endif /* OUROBOROS_IRM_UTILS_H */
