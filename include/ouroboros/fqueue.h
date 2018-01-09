/*
 * Ouroboros - Copyright (C) 2016 - 2018
 *
 * Flow queues
 *
 *    Dimitri Staessens <dimitri.staessens@ugent.be>
 *    Sander Vrijders   <sander.vrijders@ugent.be>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., http://www.fsf.org/about/contact/.
 */

#ifndef OUROBOROS_FQUEUE_H
#define OUROBOROS_FQUEUE_H

#include <ouroboros/cdefs.h>

#include <stdbool.h>
#include <time.h>

struct flow_set;

struct fqueue;

typedef struct flow_set fset_t;
typedef struct fqueue fqueue_t;

__BEGIN_DECLS

fset_t *   fset_create(void);

void       fset_destroy(fset_t * set);

fqueue_t * fqueue_create(void);

void       fqueue_destroy(struct fqueue * fq);

void       fset_zero(fset_t * set);

int        fset_add(fset_t * set,
                    int      fd);

bool       fset_has(const fset_t * set,
                    int            fd);

void       fset_del(fset_t * set,
                    int      fd);

int        fqueue_next(fqueue_t * fq);

int        fevent(fset_t *                set,
                  fqueue_t *              fq,
                  const struct timespec * timeo);

__END_DECLS

#endif /* OUROBOROS_FQUEUE_H */
