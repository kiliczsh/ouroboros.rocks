/*
 * Ouroboros - Copyright (C) 2016
 *
 * CDAP - CDAP request management
 *
 *    Sander Vrijders   <sander.vrijders@intec.ugent.be>
 *    Dimitri Staessens <dimitri.staessens@intec.ugent.be>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <ouroboros/config.h>
#include <ouroboros/time_utils.h>
#include <ouroboros/errno.h>

#include "cdap_req.h"

#include <stdlib.h>
#include <assert.h>

struct cdap_req * cdap_req_create(cdap_key_t key)
{
        struct cdap_req * creq = malloc(sizeof(*creq));
        pthread_condattr_t cattr;

        if (creq == NULL)
                return NULL;

        creq->key = key;
        creq->state     = REQ_INIT;

        creq->response = -1;
        creq->data.data = NULL;
        creq->data.len  = 0;

        pthread_condattr_init(&cattr);
#ifndef __APPLE__
        pthread_condattr_setclock(&cattr, PTHREAD_COND_CLOCK);
#endif
        pthread_cond_init(&creq->cond, &cattr);
        pthread_mutex_init(&creq->lock, NULL);

        INIT_LIST_HEAD(&creq->next);

        clock_gettime(PTHREAD_COND_CLOCK, &creq->birth);

        return creq;
}

void cdap_req_destroy(struct cdap_req * creq)
{
        assert(creq);

        pthread_mutex_lock(&creq->lock);

        if (creq->state == REQ_DESTROY) {
                pthread_mutex_unlock(&creq->lock);
                return;
        }

        if (creq->state == REQ_INIT)
                creq->state = REQ_DONE;

        if (creq->state == REQ_PENDING) {
                creq->state = REQ_DESTROY;
                pthread_cond_broadcast(&creq->cond);
        }

        while (creq->state != REQ_DONE)
                pthread_cond_wait(&creq->cond, &creq->lock);

        pthread_mutex_unlock(&creq->lock);

        pthread_cond_destroy(&creq->cond);
        pthread_mutex_destroy(&creq->lock);

        free(creq);
}

int cdap_req_wait(struct cdap_req * creq)
{
        struct timespec timeout = {(CDAP_REPLY_TIMEOUT / 1000),
                                   (CDAP_REPLY_TIMEOUT % 1000) * MILLION};
        struct timespec abstime;
        int ret = -1;

        assert(creq);

        ts_add(&creq->birth, &timeout, &abstime);

        pthread_mutex_lock(&creq->lock);

        if (creq->state != REQ_INIT) {
                pthread_mutex_unlock(&creq->lock);
                return -EINVAL;
        }

        creq->state = REQ_PENDING;

        while (creq->state == REQ_PENDING) {
                if ((ret = -pthread_cond_timedwait(&creq->cond,
                                                   &creq->lock,
                                                   &abstime)) == -ETIMEDOUT)
                        break;
        }

        if (creq->state == REQ_DESTROY)
                ret = -1;

        creq->state = REQ_DONE;
        pthread_cond_broadcast(&creq->cond);

        pthread_mutex_unlock(&creq->lock);

        return ret;
}

void cdap_req_respond(struct cdap_req * creq, int response, buffer_t data)
{
        assert(creq);

        pthread_mutex_lock(&creq->lock);

        if (creq->state != REQ_PENDING) {
                pthread_mutex_unlock(&creq->lock);
                return;
        }

        creq->state    = REQ_RESPONSE;
        creq->response = response;
        creq->data     = data;

        pthread_cond_broadcast(&creq->cond);

        while (creq->state == REQ_RESPONSE)
                pthread_cond_wait(&creq->cond, &creq->lock);

        pthread_mutex_unlock(&creq->lock);
}