/*
 * Ouroboros - Copyright (C) 2016
 *
 * The IPC Resource Manager
 *
 *    Sander Vrijders <sander.vrijders@intec.ugent.be>
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

#define OUROBOROS_PREFIX "irmd"

#include <ouroboros/config.h>
#include <ouroboros/logs.h>
#include <ouroboros/sockets.h>
#include <ouroboros/ipcp.h>
#include <ouroboros/nsm.h>
#include <ouroboros/list.h>
#include <ouroboros/utils.h>
#include <ouroboros/irm_config.h>
#include <ouroboros/shm_ap_rbuff.h>
#include <ouroboros/shm_du_map.h>
#include <ouroboros/bitmap.h>
#include <ouroboros/flow.h>
#include <ouroboros/qos.h>
#include <ouroboros/time_utils.h>

#include "utils.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>

#define API_INVALID 0

#define IRMD_MAX_FLOWS 4096

#define IRMD_THREADPOOL_SIZE 3

#define IRMD_FLOW_TIMEOUT 5000 /* ms */

#define IRMD_CLEANUP_TIMER ((IRMD_FLOW_TIMEOUT / 20) * MILLION) /* ns */

#define reg_entry_has_api(e, id) (reg_entry_get_reg_instance(e, id) != NULL)
#define reg_entry_has_ap_name(e, name) (reg_entry_get_ap_name(e, name) != NULL)
#define reg_entry_has_ap_auto(e, name) (reg_entry_get_reg_auto(e, name) != NULL)

struct ipcp_entry {
        struct list_head  next;
        char *            name;
        pid_t             api;
        char *            dif_name;
};

enum irm_state {
        IRMD_NULL = 0,
        IRMD_RUNNING,
        IRMD_SHUTDOWN
};

enum reg_name_state {
        REG_NAME_NULL = 0,
        REG_NAME_IDLE,
        REG_NAME_AUTO_ACCEPT,
        REG_NAME_AUTO_EXEC,
        REG_NAME_FLOW_ACCEPT,
        REG_NAME_FLOW_ARRIVED
};

enum reg_i_state {
        REG_I_NULL = 0,
        REG_I_SLEEP,
        REG_I_WAKE
};

struct reg_instance {
        struct list_head next;
        pid_t            api;

        /* the api will block on this */
        enum reg_i_state state;
        pthread_cond_t   wakeup;
        pthread_mutex_t  mutex;
};

static struct reg_instance * reg_instance_create(pid_t api)
{
        struct reg_instance * i;
        i = malloc(sizeof(*i));
        if (i == NULL)
                return NULL;

        i->api   = api;
        i->state = REG_I_WAKE;

        pthread_mutex_init(&i->mutex, NULL);
        pthread_cond_init(&i->wakeup, NULL);

        INIT_LIST_HEAD(&i->next);

        return i;
}

static void reg_instance_sleep(struct reg_instance * i)
{
        pthread_mutex_lock(&i->mutex);
        if (i->state != REG_I_WAKE) {
                pthread_mutex_unlock(&i->mutex);
                return;
        }

        i->state = REG_I_SLEEP;

        pthread_cleanup_push((void(*)(void *))pthread_mutex_unlock,
                             (void *) &i->mutex);

        while (i->state == REG_I_SLEEP)
                pthread_cond_wait(&i->wakeup, &i->mutex);

        pthread_cleanup_pop(true);
}

static void reg_instance_wake(struct reg_instance * i)
{
        pthread_mutex_lock(&i->mutex);

        if (i->state == REG_I_NULL) {
                pthread_mutex_unlock(&i->mutex);
                return;
        }

        i->state = REG_I_WAKE;

        pthread_cond_signal(&i->wakeup);
        pthread_mutex_unlock(&i->mutex);
}

static void reg_instance_destroy(struct reg_instance * i)
{
        bool wait = true;
        pthread_mutex_lock(&i->mutex);
        i->state = REG_I_NULL;

        pthread_cond_broadcast(&i->wakeup);
        pthread_mutex_unlock(&i->mutex);

        while (wait) {
                pthread_mutex_lock(&i->mutex);
                if (pthread_cond_destroy(&i->wakeup))
                        pthread_cond_broadcast(&i->wakeup);
                else
                        wait = false;
                pthread_mutex_unlock(&i->mutex);
        }

        pthread_mutex_destroy(&i->mutex);

        free(i);
}

struct reg_auto {
        struct list_head next;
        char * ap_name;
        char ** argv;
};

struct reg_ap_name {
        struct list_head next;
        char * ap_name;
};

/* an entry in the registry */
struct reg_entry {
        struct list_head next;

        /* generic name */
        char * name;

        /* names of the aps that can listen to this name */
        struct list_head ap_names;

        enum reg_name_state state;

        uint32_t flags;

        /* auto execution info */
        struct list_head auto_ap_info;

        /* known instances */
        struct list_head ap_instances;

        char * req_ae_name;
        int    response;

        pthread_cond_t  acc_signal;
        pthread_mutex_t state_lock;
};

/* keeps track of port_id's between N and N - 1 */
struct port_map_entry {
        struct list_head next;

        int port_id;

        pid_t n_api;
        pid_t n_1_api;

        pthread_cond_t  res_signal;
        pthread_mutex_t res_lock;

        enum flow_state state;

        struct timespec t0;
};

struct irm {
        /* FIXME: list of ipcps could be merged into the registry */
        struct list_head ipcps;

        struct list_head registry;
        pthread_rwlock_t reg_lock;

        /* keep track of all flows in this processing system */
        struct bmp * port_ids;
        /* maps port_ids to api pair */
        struct list_head port_map;
        pthread_rwlock_t  flows_lock;

        enum irm_state      state;
        struct shm_du_map * dum;
        pthread_t *         threadpool;
        int                 sockfd;
        pthread_rwlock_t    state_lock;

        pthread_t cleanup_flows;
        pthread_t shm_sanitize;
} * instance = NULL;

static struct port_map_entry * port_map_entry_create()
{
        struct port_map_entry * e = malloc(sizeof(*e));
        if (e == NULL)
                return NULL;

        e->n_api   = 0;
        e->n_1_api = 0;
        e->port_id = 0;
        e->state   = FLOW_NULL;

        if (pthread_cond_init(&e->res_signal, NULL)) {
                free(e);
                return NULL;
        }

        if (pthread_mutex_init(&e->res_lock, NULL)) {
                free(e);
                return NULL;
        }

        e->t0.tv_sec  = 0;
        e->t0.tv_nsec = 0;

        return e;
}

static void port_map_entry_destroy(struct port_map_entry * e)
{
        bool wait = true;
        pthread_mutex_lock(&e->res_lock);
        e->state = FLOW_NULL;

        pthread_cond_broadcast(&e->res_signal);
        pthread_mutex_unlock(&e->res_lock);

        while (wait) {
                pthread_mutex_lock(&e->res_lock);
                if (pthread_cond_destroy(&e->res_signal))
                        pthread_cond_broadcast(&e->res_signal);
                else
                        wait = false;
                pthread_mutex_unlock(&e->res_lock);
        }

        pthread_mutex_destroy(&e->res_lock);

        free(e);
}

static struct port_map_entry * get_port_map_entry(int port_id)
{
        struct list_head * pos = NULL;

        list_for_each(pos, &instance->port_map) {
                struct port_map_entry * e =
                        list_entry(pos, struct port_map_entry, next);

                if (e->port_id == port_id)
                        return e;
        }

        return NULL;
}

static struct port_map_entry * get_port_map_entry_n(pid_t n_api)
{
        struct list_head * pos = NULL;

        list_for_each(pos, &instance->port_map) {
                struct port_map_entry * e =
                        list_entry(pos, struct port_map_entry, next);

                if (e->n_api == n_api)
                        return e;
        }

        return NULL;
}

static struct ipcp_entry * ipcp_entry_create()
{
        struct ipcp_entry * e = malloc(sizeof(*e));
        if (e == NULL)
                return NULL;

        e->name = NULL;
        e->dif_name = NULL;

        INIT_LIST_HEAD(&e->next);

        return e;
}

static void ipcp_entry_destroy(struct ipcp_entry * e)
{
        if (e == NULL)
                return;

        if (e->name != NULL)
                free(e->name);

        if (e->dif_name != NULL)
                free(e->dif_name);

        free(e);
}

static struct ipcp_entry * get_ipcp_entry_by_api(pid_t api)
{
        struct list_head * pos = NULL;

        list_for_each(pos, &instance->ipcps) {
                struct ipcp_entry * tmp =
                        list_entry(pos, struct ipcp_entry, next);

                if (api == tmp->api)
                        return tmp;
        }

        return NULL;
}

/*
 * FIXME: this just returns the first IPCP that
 * matches the requested DIF name for now
 */
static pid_t get_ipcp_by_dst_name(char * dst_name,
                                  char * dif_name)
{
        struct list_head * pos = NULL;

        list_for_each(pos, &instance->ipcps) {
                struct ipcp_entry * e =
                        list_entry(pos, struct ipcp_entry, next);

                if (e->dif_name == NULL)
                        continue;

                if (dif_name != NULL) {
                        if (wildcard_match(dif_name, e->dif_name) == 0) {
                                return e->api;
                        }
                } else {
                        return e->api;
                }
        }

        return 0;
}

static struct reg_entry * reg_entry_create()
{
        struct reg_entry * e = malloc(sizeof(*e));
        if (e == NULL)
                return NULL;

        e->name         = NULL;
        e->state        = REG_NAME_NULL;
        e->flags        = 0;

        e->req_ae_name  = NULL;
        e->response     = -1;

        return e;
}

static struct reg_entry * reg_entry_init(struct reg_entry * e,
                                         char *             name,
                                         char *             ap_name,
                                         uint32_t           flags)
{
        if (e == NULL || name == NULL || ap_name == NULL)
                return NULL;

        struct reg_ap_name * n = malloc(sizeof(*n));
        if (n == NULL)
                return NULL;

        INIT_LIST_HEAD(&e->next);
        INIT_LIST_HEAD(&e->ap_names);
        INIT_LIST_HEAD(&e->auto_ap_info);
        INIT_LIST_HEAD(&e->ap_instances);

        e->name    = name;
        e->flags   = flags;
        n->ap_name = ap_name;

        list_add(&n->next, &e->ap_names);

        if (pthread_cond_init(&e->acc_signal, NULL)) {
                free(e);
                return NULL;
        }

        if (pthread_mutex_init(&e->state_lock, NULL)) {
                free(e);
                return NULL;
        }

        e->state = REG_NAME_IDLE;

        return e;
}

static void reg_entry_destroy(struct reg_entry * e)
{
        struct list_head * pos = NULL;
        struct list_head * n   = NULL;

        bool wait = true;

        if (e == NULL)
                return;

        pthread_mutex_lock(&e->state_lock);

        e->state = REG_NAME_NULL;

        pthread_cond_broadcast(&e->acc_signal);
        pthread_mutex_unlock(&e->state_lock);

        while (wait) {
                pthread_mutex_lock(&e->state_lock);
                if (pthread_cond_destroy(&e->acc_signal))
                        pthread_cond_broadcast(&e->acc_signal);
                else
                        wait = false;
                pthread_mutex_unlock(&e->state_lock);
        }

        pthread_mutex_destroy(&e->state_lock);

        if (e->name != NULL)
                free(e->name);

        if (e->req_ae_name != NULL)
                free(e->req_ae_name);

        list_for_each_safe(pos, n, &e->ap_instances) {
                struct reg_instance * i =
                        list_entry(pos, struct reg_instance, next);
                reg_instance_destroy(i);
        }

        list_for_each_safe(pos, n, &e->auto_ap_info) {
                struct reg_auto * a =
                        list_entry(pos, struct reg_auto, next);

                if (a->argv != NULL) {
                        char ** t = a->argv;
                        while (*a->argv != NULL)
                                free(*(a->argv++));
                        free(t);
                }

                free(a->ap_name);
                free(a);
        }

        list_for_each_safe(pos, n, &e->ap_names) {
                struct reg_ap_name * n =
                        list_entry(pos, struct reg_ap_name, next);

                free(n->ap_name);
                free(n);
        }

        free(e);
}

static struct reg_ap_name * reg_entry_get_ap_name(struct reg_entry * e,
                                                  char *             ap_name)
{
        struct list_head * pos = NULL;

        list_for_each(pos, &e->ap_names) {
                struct reg_ap_name * n =
                        list_entry(pos, struct reg_ap_name, next);

                if (strcmp(ap_name, n->ap_name) == 0)
                        return n;
        }

        return NULL;
}

static struct reg_instance * reg_entry_get_reg_instance(struct reg_entry * e,
                                                        pid_t              api)
{
        struct list_head * pos = NULL;

        list_for_each(pos, &e->ap_instances) {
                struct reg_instance * r =
                        list_entry(pos, struct reg_instance, next);

                if (r->api == api)
                        return r;
        }

        return NULL;
}

static struct reg_auto * reg_entry_get_reg_auto(struct reg_entry * e,
                                                char *             ap_name)
{
        struct list_head * pos = NULL;

        list_for_each(pos, &e->auto_ap_info) {
                struct reg_auto * a =
                        list_entry(pos, struct reg_auto, next);

                if (strcmp(ap_name, a->ap_name) == 0)
                        return a;
        }

        return NULL;
}

static struct reg_entry * get_reg_entry_by_name(char * name)
{
        struct list_head * pos = NULL;

        list_for_each(pos, &instance->registry) {
                struct reg_entry * e =
                        list_entry(pos, struct reg_entry, next);

                if (strcmp(name, e->name) == 0)
                        return e;
        }

        return NULL;
}

static struct reg_entry * get_reg_entry_by_ap_name(char * ap_name)
{
        struct list_head * pos = NULL;

        list_for_each(pos, &instance->registry) {
                struct list_head * p = NULL;
                struct reg_entry * e =
                        list_entry(pos, struct reg_entry, next);

                list_for_each(p, &e->ap_names) {
                        struct reg_ap_name * n =
                                list_entry(p, struct reg_ap_name, next);

                        if (strcmp(n->ap_name, ap_name) == 0)
                                return e;
                }
        }

        return NULL;
}

static struct reg_entry * get_reg_entry_by_ap_id(pid_t api)
{
        struct list_head * pos = NULL;

        list_for_each(pos, &instance->registry) {
                struct list_head * p = NULL;
                struct reg_entry * e =
                        list_entry(pos, struct reg_entry, next);

                list_for_each(p, &e->ap_instances) {
                        struct reg_instance * r =
                                list_entry(p, struct reg_instance, next);

                        if (r->api == api)
                                return e;
                }
        }

        return NULL;
}

static int registry_add_entry(char * name, char * ap_name, uint16_t flags)
{
        struct reg_entry * e = NULL;

        if (name == NULL || ap_name == NULL)
                return -EINVAL;

        e = get_reg_entry_by_name(name);
        if (e != NULL) {
                LOG_DBG("Name %s already registered.", name);
                return -1;
        }

        e = reg_entry_create();
        if (e == NULL) {
                LOG_DBG("Could not create registry entry.");
                return -1;
        }

        e = reg_entry_init(e, name, ap_name, flags);
        if (e == NULL) {
                LOG_DBG("Could not initialize registry entry.");
                reg_entry_destroy(e);
                return -1;
        }

        list_add(&e->next, &instance->registry);

        return 0;
}

static int registry_add_ap_auto(char *  name,
                                char *  ap_name,
                                char ** argv)
{
        struct reg_entry * e;
        struct reg_auto * a;

        if (name == NULL || ap_name == NULL)
                return -EINVAL;

        e = get_reg_entry_by_name(name);
        if (e == NULL) {
                LOG_DBG("Name %s not found in registry.", name);
                return -1;
        }

        if (!(e->flags & BIND_AP_AUTO)) {
                LOG_DBG("%s does not allow auto-instantiation.", name);
                return -1;
        }

        if (!reg_entry_has_ap_name(e, ap_name)) {
                LOG_DBG("AP name %s not associated with %s.", ap_name, name);
                return -1;
        }

        if (e->state == REG_NAME_NULL) {
                LOG_DBG("Tried to add instantiation info in NULL state.");
                return -1;
        }

        a = reg_entry_get_reg_auto(e, ap_name);
        if (a != NULL) {
                LOG_DBG("Updating auto-instantiation info for %s.", ap_name);
                list_del(&a->next);
                free(a->ap_name);
                if (a->argv != NULL) {
                        while (*a->argv != NULL)
                                free(*a->argv++);
                }
        } else {
                a = malloc(sizeof(*a));
                if (a == NULL)
                        return -1;
        }

        a->ap_name = ap_name;
        a->argv    = argv;

        if (e->state == REG_NAME_IDLE)
                e->state = REG_NAME_AUTO_ACCEPT;

        list_add(&a->next, &e->auto_ap_info);

        return 0;
}

#if 0
static int registry_remove_ap_auto(char * name,
                                   char * ap_name)
{
        struct reg_entry * e;
        struct reg_auto * a;

        if (name == NULL || ap_name == NULL)
                return -EINVAL;

        e = get_reg_entry_by_name(name);
        if (e == NULL) {
                LOG_DBG("Name %s not found in registry.", name);
                return -1;
        }

        a = reg_entry_get_reg_auto(e, ap_name);
        if (a == NULL) {
                LOG_DBG("Auto-instantiation info for %s not found.", ap_name);
                return -1;
        }

        list_del(&a->next);

        if (e->state == REG_NAME_AUTO_ACCEPT && list_empty(&e->auto_ap_info))
                e->state = REG_NAME_IDLE;

        return 0;
}
#endif

static struct reg_instance * registry_add_ap_instance(char * name,
                                                      pid_t api)
{
        struct reg_entry * e    = NULL;
        struct reg_instance * i = NULL;

        if (name == NULL || api == 0)
                return NULL;

        e = get_reg_entry_by_name(name);
        if (e == NULL) {
                LOG_DBG("Name %s not found in registry.", name);
                return NULL;
        }

        if (api == API_INVALID) {
                LOG_DBG("Invalid api.");
                return NULL;
        }

        if (reg_entry_has_api(e, api)) {
                LOG_DBG("Instance already registered with this name.");
                return NULL;
        }

        if (e->state == REG_NAME_NULL) {
                LOG_DBG("Tried to add instance in NULL state.");
                return NULL;
        }

        i = reg_instance_create(api);
        if (i == NULL) {
                LOG_DBG("Failed to create reg_instance");
                return NULL;
        }

        if (e->state == REG_NAME_IDLE || e->state == REG_NAME_AUTO_ACCEPT
           || e->state == REG_NAME_AUTO_EXEC) {
                e->state = REG_NAME_FLOW_ACCEPT;
                pthread_cond_signal(&e->acc_signal);
        }

        list_add(&i->next, &e->ap_instances);

        return i;
}

static int registry_remove_ap_instance(char * name, pid_t api)
{
        struct reg_entry * e    = NULL;
        struct reg_instance * i = NULL;

        if (name == NULL || api == 0)
                return -1;

        e = get_reg_entry_by_name(name);
        if (e == NULL) {
                LOG_DBG("Name %s is not registered.", name);
                return -1;
        }

        i = reg_entry_get_reg_instance(e, api);
        if (i == NULL) {
                LOG_DBG("Instance %d is not accepting flows for %s.",
                         api, name);
                return -1;
        }

        list_del(&i->next);

        reg_instance_destroy(i);

        if (list_empty(&e->ap_instances)) {
                if ((e->flags & BIND_AP_AUTO) &&
                        !list_empty(&e->auto_ap_info))
                        e->state = REG_NAME_AUTO_ACCEPT;
                else
                        e->state = REG_NAME_IDLE;
        } else {
                e->state = REG_NAME_FLOW_ACCEPT;
        }

        return 0;
}

static pid_t registry_resolve_api(struct reg_entry * e)
{
        struct list_head * pos = NULL;

        /* FIXME: now just returns the first accepting instance */
        list_for_each(pos, &e->ap_instances) {
                struct reg_instance * r =
                        list_entry(pos, struct reg_instance, next);
                return r->api;
        }

        return 0;
}

static char ** registry_resolve_auto(struct reg_entry * e)
{
        struct list_head * pos = NULL;

        /* FIXME: now just returns the first accepting instance */
        list_for_each(pos, &e->auto_ap_info) {
                struct reg_auto * r =
                        list_entry(pos, struct reg_auto, next);
                return r->argv;
        }

        return NULL;
}

static void registry_del_name(char * name)
{
        struct reg_entry * e = get_reg_entry_by_name(name);
        if (e == NULL)
                return;

        list_del(&e->next);
        reg_entry_destroy(e);

        return;
}

static pid_t create_ipcp(char *         name,
                         enum ipcp_type ipcp_type)
{
        pid_t api;
        struct ipcp_entry * tmp = NULL;

        pthread_rwlock_rdlock(&instance->state_lock);

        if (instance->state != IRMD_RUNNING) {
                pthread_rwlock_unlock(&instance->state_lock);
                return 0;
        }

        api = ipcp_create(ipcp_type);
        if (api == -1) {
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("Failed to create IPCP.");
                return -1;
        }

        tmp = ipcp_entry_create();
        if (tmp == NULL) {
                pthread_rwlock_unlock(&instance->state_lock);
                return -1;
        }

        INIT_LIST_HEAD(&tmp->next);

        tmp->api = api;
        tmp->name = strdup(name);
        if (tmp->name  == NULL) {
                ipcp_entry_destroy(tmp);
                pthread_rwlock_unlock(&instance->state_lock);
                return -1;
        }

        tmp->dif_name = NULL;

        pthread_rwlock_wrlock(&instance->reg_lock);

        list_add(&tmp->next, &instance->ipcps);

        pthread_rwlock_unlock(&instance->reg_lock);
        pthread_rwlock_unlock(&instance->state_lock);

        LOG_INFO("Created IPCP %d.", api);

        return api;
}

static int destroy_ipcp(pid_t api)
{
        struct list_head * pos = NULL;
        struct list_head * n   = NULL;

        pthread_rwlock_rdlock(&instance->state_lock);
        pthread_rwlock_wrlock(&instance->reg_lock);

        list_for_each_safe(pos, n, &(instance->ipcps)) {
                struct ipcp_entry * tmp =
                        list_entry(pos, struct ipcp_entry, next);

                if (api == tmp->api) {
                        if (ipcp_destroy(api))
                                LOG_ERR("Could not destroy IPCP.");
                        list_del(&tmp->next);
                        ipcp_entry_destroy(tmp);

                        LOG_INFO("Destroyed IPCP %d.", api);
                }
        }

        pthread_rwlock_unlock(&instance->reg_lock);
        pthread_rwlock_unlock(&instance->state_lock);

        return 0;
}

static int bootstrap_ipcp(pid_t              api,
                          dif_config_msg_t * conf)
{
        struct ipcp_entry * entry = NULL;

        pthread_rwlock_rdlock(&instance->state_lock);

        if (instance->state != IRMD_RUNNING) {
                pthread_rwlock_unlock(&instance->state_lock);
                return -1;
        }

        pthread_rwlock_wrlock(&instance->reg_lock);

        entry = get_ipcp_entry_by_api(api);
        if (entry == NULL) {
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("No such IPCP.");
                return -1;
        }

        entry->dif_name = strdup(conf->dif_name);
        if (entry->dif_name == NULL) {
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("Failed to strdup.");
                return -1;
        }

        if (ipcp_bootstrap(entry->api, conf)) {
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("Could not bootstrap IPCP.");
                free(entry->dif_name);
                entry->dif_name = NULL;
                return -1;
        }

        pthread_rwlock_unlock(&instance->reg_lock);
        pthread_rwlock_unlock(&instance->state_lock);

        LOG_INFO("Bootstrapped IPCP %d in DIF %s.",
                 entry->api, conf->dif_name);

        return 0;
}

static int enroll_ipcp(pid_t  api,
                       char * dif_name)
{
        char ** n_1_difs = NULL;
        ssize_t n_1_difs_size = 0;
        struct ipcp_entry * entry = NULL;

        pthread_rwlock_rdlock(&instance->state_lock);

        if (instance->state != IRMD_RUNNING) {
                pthread_rwlock_unlock(&instance->state_lock);
                return -1;
        }

        pthread_rwlock_rdlock(&instance->reg_lock);

        entry = get_ipcp_entry_by_api(api);
        if (entry == NULL) {
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("No such IPCP.");
                return -1;
        }

        entry->dif_name = strdup(dif_name);
        if (entry->dif_name == NULL) {
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("Failed to strdup.");
                return -1;
        }

        n_1_difs_size = nsm_resolve(dif_name, n_1_difs);
        if (n_1_difs_size < 1) {
                free(entry->dif_name);
                entry->dif_name = NULL;
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("Could not find N-1 DIFs.");
                return -1;
        }

        if (ipcp_enroll(api, dif_name, n_1_difs[0])) {
                free(entry->dif_name);
                entry->dif_name = NULL;
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("Could not enroll IPCP.");
                return -1;
        }

        pthread_rwlock_unlock(&instance->reg_lock);
        pthread_rwlock_unlock(&instance->state_lock);

        LOG_INFO("Enrolled IPCP %d in DIF %s.",
                 entry->api, dif_name);

        return 0;
}

static int bind_name(char *   name,
                     char *   ap_name,
                     uint16_t opts,
                     int      argc,
                     char **  argv)
{
        int i;

        char ** argv_dup = NULL;
        char * apn = path_strip(ap_name);

        pthread_rwlock_rdlock(&instance->state_lock);

        if (instance->state != IRMD_RUNNING) {
                pthread_rwlock_unlock(&instance->state_lock);
                return -1;
        }

        pthread_rwlock_wrlock(&instance->reg_lock);

        if (registry_add_entry(strdup(name), strdup(apn), opts) < 0) {
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("Failed to register %s.", name);
                return -1;
        }

        if (opts & BIND_AP_AUTO) {
                /* we need to duplicate argv */
                if (argc != 0) {
                        argv_dup = malloc((argc + 2) * sizeof(*argv_dup));
                        argv_dup[0] = strdup(ap_name);
                        for (i = 1; i <= argc; ++i)
                                argv_dup[i] = strdup(argv[i - 1]);
                        argv_dup[argc + 1] = NULL;
                }

                registry_add_ap_auto(name, strdup(apn), argv_dup);
        }

        pthread_rwlock_unlock(&instance->reg_lock);
        pthread_rwlock_unlock(&instance->state_lock);

        return 0;
}

static int unbind_name(char * name,
                       char * ap_name,
                       uint16_t opts)

{
        struct reg_entry * rne = NULL;

        if (name == NULL || ap_name  == NULL)
                return -1;

        pthread_rwlock_rdlock(&instance->state_lock);

        if (instance->state != IRMD_RUNNING) {
                pthread_rwlock_unlock(&instance->state_lock);
                return -1;
        }

        pthread_rwlock_wrlock(&instance->reg_lock);

        rne = get_reg_entry_by_name(name);
        if (rne == NULL) {
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("Tried to unbind a name that is not bound.");
                return -1;
        }

        /*
         * FIXME: Remove the mapping of name to ap_name.
         * Remove the name only if it was the last mapping.
         */
        registry_del_name(rne->name);

        pthread_rwlock_unlock(&instance->reg_lock);
        pthread_rwlock_unlock(&instance->state_lock);

        return 0;
}

static ssize_t list_ipcps(char * name,
                          pid_t ** apis)
{
        struct list_head * pos = NULL;
        ssize_t count = 0;
        int i = 0;

        list_for_each(pos, &instance->ipcps) {
                struct ipcp_entry * tmp =
                        list_entry(pos, struct ipcp_entry, next);

                if (wildcard_match(name, tmp->name) == 0) {
                        count++;
                }
        }

        *apis = malloc(count * sizeof(pid_t));
        if (*apis == NULL) {
                return -1;
        }

        list_for_each(pos, &instance->ipcps) {
                struct ipcp_entry * tmp =
                        list_entry(pos, struct ipcp_entry, next);

                if (wildcard_match(name, tmp->name) == 0) {
                        (*apis)[i++] = tmp->api;
                }
        }

        return count;
}

static int ap_reg(char *  name,
                  char ** difs,
                  size_t  len)
{
        int i;
        int ret = 0;
        struct reg_entry * reg = NULL;
        struct list_head * pos = NULL;

        pthread_rwlock_rdlock(&instance->state_lock);

        if (instance->state != IRMD_RUNNING) {
                pthread_rwlock_unlock(&instance->state_lock);
                return -1;
        }

        pthread_rwlock_wrlock(&instance->reg_lock);

        if (list_empty(&instance->ipcps)) {
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                return -1;
        }

        reg = get_reg_entry_by_name(name);
        if (reg == NULL) {
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("Tried to register a name that is not bound.");
                return -1;
        }

        list_for_each(pos, &instance->ipcps) {
                struct ipcp_entry * e =
                        list_entry(pos, struct ipcp_entry, next);

                if (e->dif_name == NULL)
                        continue;

                for (i = 0; i < len; ++i) {
                        if (wildcard_match(difs[i], e->dif_name) == 0) {
                                if (ipcp_name_reg(e->api, name)) {
                                        LOG_ERR("Could not register "
                                                "%s in DIF %s.",
                                                name, e->dif_name);
                                } else {
                                        LOG_INFO("Registered %s in %s",
                                                 name, e->dif_name);
                                        ++ret;
                                }
                        }
                }
        }

        if (ret == 0) {
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                return -1;
        }

        pthread_rwlock_unlock(&instance->reg_lock);
        pthread_rwlock_unlock(&instance->state_lock);

        return ret;
}

static int ap_unreg(char *  name,
                    char ** difs,
                    size_t  len)
{
        int i;
        int ret = 0;
        struct reg_entry * rne = NULL;
        struct list_head * pos = NULL;

        if (name == NULL || len == 0 || difs == NULL || difs[0] == NULL)
                return -1;

        pthread_rwlock_rdlock(&instance->state_lock);

        if (instance->state != IRMD_RUNNING) {
                pthread_rwlock_unlock(&instance->state_lock);
                return -1;
        }

        pthread_rwlock_wrlock(&instance->reg_lock);

        list_for_each(pos, &instance->ipcps) {
                struct ipcp_entry * e =
                        list_entry(pos, struct ipcp_entry, next);

                if (e->dif_name == NULL)
                        continue;

                for (i = 0; i < len; ++i) {
                        if (wildcard_match(difs[i], e->dif_name) == 0) {
                                if (ipcp_name_unreg(e->api,
                                                    rne->name)) {
                                        LOG_ERR("Could not unregister "
                                                "%s in DIF %s.",
                                                rne->name, e->dif_name);
                                        --ret;
                                }
                        }
                }
        }

        pthread_rwlock_unlock(&instance->reg_lock);
        pthread_rwlock_unlock(&instance->state_lock);

        return ret;
}

static struct port_map_entry * flow_accept(pid_t   api,
                                           char *  srv_ap_name,
                                           char ** dst_ae_name)
{
        struct port_map_entry * pme = NULL;
        struct reg_entry * rne      = NULL;
        struct reg_instance * rgi   = NULL;

        pthread_rwlock_rdlock(&instance->state_lock);

        if (instance->state != IRMD_RUNNING) {
                pthread_rwlock_unlock(&instance->state_lock);
                return NULL;
        }

        pthread_rwlock_wrlock(&instance->reg_lock);

        rne = get_reg_entry_by_ap_name(srv_ap_name);
        if (rne == NULL) {
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("AP %s is unknown.", srv_ap_name);
                return NULL;
        }

        if (!reg_entry_has_api(rne, api)) {
                rgi = registry_add_ap_instance(rne->name, api);
                if (rgi == NULL) {
                        pthread_rwlock_unlock(&instance->reg_lock);
                        pthread_rwlock_unlock(&instance->state_lock);
                        LOG_ERR("Failed to register instance %d with %s.",
                                api,srv_ap_name);
                        return NULL;
                }
                LOG_INFO("New instance (%d) of %s added.", api, srv_ap_name);
        }

        pthread_rwlock_unlock(&instance->reg_lock);
        pthread_rwlock_unlock(&instance->state_lock);

        reg_instance_sleep(rgi);

        pthread_rwlock_rdlock(&instance->state_lock);
        pthread_rwlock_rdlock(&instance->reg_lock);
        pthread_mutex_lock(&rne->state_lock);

        if (rne->state != REG_NAME_FLOW_ARRIVED) {
                pthread_mutex_unlock(&rne->state_lock);
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                return NULL;
        }

        pthread_mutex_unlock(&rne->state_lock);
        pthread_rwlock_unlock(&instance->reg_lock);

        pthread_rwlock_rdlock(&instance->flows_lock);

        pme = get_port_map_entry_n(api);
        if (pme == NULL) {
                pthread_rwlock_unlock(&instance->flows_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("Port_id was not created yet.");
                return NULL;
        }

        rne->req_ae_name = NULL;

        if (dst_ae_name != NULL)
                *dst_ae_name = rne->req_ae_name;

        pthread_rwlock_unlock(&instance->flows_lock);
        pthread_rwlock_unlock(&instance->state_lock);

        return pme;
}

static int flow_alloc_resp(pid_t n_api,
                           int   port_id,
                           int   response)
{
        struct port_map_entry * pme = NULL;
        struct reg_entry * rne      = NULL;
        int ret = -1;

        pthread_rwlock_rdlock(&instance->state_lock);

        if (instance->state != IRMD_RUNNING) {
                pthread_rwlock_unlock(&instance->state_lock);
                return -1;
        }

        pthread_rwlock_wrlock(&instance->reg_lock);

        rne = get_reg_entry_by_ap_id(n_api);
        if (rne == NULL) {
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                return -1;
        }

        if (rne->state != REG_NAME_FLOW_ARRIVED) {
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("Process not listening for this name.");
                return -1;
        }

        pthread_mutex_lock(&rne->state_lock);

        registry_remove_ap_instance(rne->name, n_api);

        pthread_mutex_unlock(&rne->state_lock);

        pthread_rwlock_unlock(&instance->reg_lock);

        if (!response) {
                pthread_rwlock_wrlock(&instance->flows_lock);

                pme = get_port_map_entry(port_id);
                if (pme == NULL) {
                        pthread_rwlock_unlock(&instance->flows_lock);
                        pthread_rwlock_unlock(&instance->state_lock);
                        return -1;
                }

                pme->state = FLOW_ALLOCATED;
                pthread_rwlock_unlock(&instance->flows_lock);

                ret = ipcp_flow_alloc_resp(pme->n_1_api,
                                           port_id,
                                           pme->n_api,
                                           response);
        }

        pthread_rwlock_unlock(&instance->state_lock);

        return ret;
}

static struct port_map_entry * flow_alloc(pid_t  api,
                                          char * dst_name,
                                          char * src_ae_name,
                                          struct qos_spec * qos)
{
        struct port_map_entry * pme;
        pid_t ipcp;
        char * dif_name = NULL;

        /* FIXME: Map qos_spec to qos_cube */

        pthread_rwlock_rdlock(&instance->state_lock);

        if (instance->state != IRMD_RUNNING) {
                pthread_rwlock_unlock(&instance->state_lock);
                return NULL;
        }

        pme = port_map_entry_create();
        if (pme == NULL) {
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("Failed to create port_map_entry.");
                return NULL;
        }

        pme->n_api = api;
        pme->state = FLOW_PENDING;
        if (clock_gettime(CLOCK_MONOTONIC, &pme->t0) < 0)
                LOG_WARN("Failed to set timestamp.");

        pthread_rwlock_rdlock(&instance->reg_lock);

        if (qos != NULL)
                dif_name = qos->dif_name;

        ipcp = get_ipcp_by_dst_name(dst_name, dif_name);
        if (ipcp == 0) {
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("Unknown DIF name.");
                return NULL;
        }

        pthread_rwlock_unlock(&instance->reg_lock);
        pthread_rwlock_wrlock(&instance->flows_lock);

        pme->port_id = bmp_allocate(instance->port_ids);
        pme->n_1_api = ipcp;

        list_add(&pme->next, &instance->port_map);

        pthread_rwlock_unlock(&instance->flows_lock);
        pthread_rwlock_unlock(&instance->state_lock);

        if (ipcp_flow_alloc(ipcp,
                            pme->port_id,
                            pme->n_api,
                            dst_name,
                            src_ae_name,
                            QOS_CUBE_BE) < 0) {
                pthread_rwlock_rdlock(&instance->state_lock);
                pthread_rwlock_wrlock(&instance->flows_lock);
                list_del(&pme->next);
                bmp_release(instance->port_ids, pme->port_id);
                pthread_rwlock_unlock(&instance->flows_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                free(pme);
                return NULL;
        }

        return pme;
}

static int flow_alloc_res(int port_id)
{
        struct port_map_entry * e;

        pthread_rwlock_rdlock(&instance->state_lock);

        if (instance->state != IRMD_RUNNING) {
                pthread_rwlock_unlock(&instance->state_lock);
                return -1;
        }
        pthread_rwlock_rdlock(&instance->flows_lock);

        e = get_port_map_entry(port_id);
        if (e == NULL) {
                LOG_ERR("Could not find port %d.", port_id);
                pthread_rwlock_unlock(&instance->flows_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                return -1;
        }

        if (e->state == FLOW_NULL) {
                LOG_INFO("Port %d is deprecated.", port_id);
                pthread_rwlock_unlock(&instance->flows_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                return -1;
        }

        if (e->state == FLOW_ALLOCATED) {
                pthread_rwlock_unlock(&instance->flows_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                return 0;
        }

        pthread_rwlock_unlock(&instance->flows_lock);
        pthread_rwlock_unlock(&instance->state_lock);

        while (true) {
                pthread_mutex_lock(&e->res_lock);
                pthread_cleanup_push((void(*)(void *))pthread_mutex_unlock,
                                     (void*) &e->res_lock);

                pthread_cond_wait(&e->res_signal, &e->res_lock);

                pthread_cleanup_pop(true);

                pthread_rwlock_rdlock(&instance->state_lock);
                pthread_rwlock_wrlock(&instance->flows_lock);

                e = get_port_map_entry(port_id);
                if (e == NULL) {
                        pthread_rwlock_unlock(&instance->flows_lock);
                        pthread_rwlock_unlock(&instance->state_lock);
                        return -1;
                }
                if (e->state == FLOW_ALLOCATED) {
                        pthread_rwlock_unlock(&instance->flows_lock);
                        pthread_rwlock_unlock(&instance->state_lock);
                        return 0;
                }
                if (e->state == FLOW_NULL) {
                        /* don't release the port_id, AP has to call dealloc */
                        list_del(&e->next);
                        pthread_rwlock_unlock(&instance->flows_lock);
                        pthread_rwlock_unlock(&instance->state_lock);
                        free(e);
                        return -1;

                }
                /* still pending, spurious wake */
                pthread_rwlock_unlock(&instance->flows_lock);
                pthread_rwlock_unlock(&instance->state_lock);
        }

        return 0;
}

static int flow_dealloc(int port_id)
{
        pid_t n_1_api;
        int   ret = 0;

        struct port_map_entry * e = NULL;

        pthread_rwlock_rdlock(&instance->state_lock);
        pthread_rwlock_wrlock(&instance->flows_lock);
        bmp_release(instance->port_ids, port_id);

        e = get_port_map_entry(port_id);
        if (e == NULL) {
                pthread_rwlock_unlock(&instance->flows_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                return 0;
        }

        n_1_api = e->n_1_api;

        list_del(&e->next);

        pthread_rwlock_unlock(&instance->flows_lock);

        ret = ipcp_flow_dealloc(n_1_api, port_id);

        pthread_rwlock_unlock(&instance->state_lock);

        free(e);

        return ret;
}

static int auto_execute(char ** argv)
{
        pid_t api;
        LOG_INFO("Executing %s.", argv[0]);
        api = fork();
        if (api == -1) {
                LOG_ERR("Failed to fork");
                return api;
        }

        if (api != 0) {
                return api;
        }

        execv(argv[0], argv);

        LOG_ERR("Failed to execute %s.", argv[0]);

        exit(EXIT_FAILURE);
}

static struct port_map_entry * flow_req_arr(pid_t  api,
                                            char * dst_name,
                                            char * ae_name)
{
        struct reg_entry *      rne = NULL;
        struct port_map_entry * pme = NULL;

        bool acc_wait = true;
        enum reg_name_state state;

        pme = port_map_entry_create();
        if (pme == NULL) {
                LOG_ERR("Failed to create port_map_entry.");
                return NULL;
        }

        pme->state   = FLOW_PENDING;
        pme->n_1_api = api;
        if (clock_gettime(CLOCK_MONOTONIC, &pme->t0) < 0)
                LOG_WARN("Failed to set timestamp.");

        pthread_rwlock_rdlock(&instance->state_lock);
        pthread_rwlock_rdlock(&instance->reg_lock);

        rne = get_reg_entry_by_name(dst_name);
        if (rne == NULL) {
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("Unknown name: %s.", dst_name);
                free(pme);
                return NULL;
        }

        pthread_mutex_lock(&rne->state_lock);
        state = rne->state;
        pthread_mutex_unlock(&rne->state_lock);

        switch (state) {
        case REG_NAME_IDLE:
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("No AP's for %s.", dst_name);
                free(pme);
                return NULL;
        case REG_NAME_AUTO_ACCEPT:
                pthread_mutex_lock(&rne->state_lock);
                rne->state = REG_NAME_AUTO_EXEC;
                pthread_mutex_unlock(&rne->state_lock);

                if (auto_execute(registry_resolve_auto(rne)) < 0) {
                        pthread_rwlock_unlock(&instance->reg_lock);
                        pthread_rwlock_unlock(&instance->state_lock);
                        free(pme);
                        return NULL;
                }

                pthread_rwlock_unlock(&instance->reg_lock);

                pthread_mutex_lock(&rne->state_lock);
                pthread_cleanup_push((void(*)(void *)) pthread_mutex_unlock,
                                     (void *) &rne->state_lock);

                while (rne->state == REG_NAME_AUTO_EXEC)
                        pthread_cond_wait(&rne->acc_signal,
                                          &rne->state_lock);

                pthread_cleanup_pop(true);

                pthread_rwlock_rdlock(&instance->reg_lock);

        case REG_NAME_FLOW_ACCEPT:
                pme->n_api = registry_resolve_api(rne);
                if (pme->n_api == 0) {
                        pthread_rwlock_unlock(&instance->reg_lock);
                        pthread_rwlock_unlock(&instance->state_lock);
                        LOG_ERR("Invalid api returned.");
                        return NULL;
                }

                break;
        default:
                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                LOG_ERR("IRMd in wrong state.");
                free(pme);
                return NULL;
        }

        pthread_rwlock_unlock(&instance->reg_lock);

        pthread_rwlock_wrlock(&instance->flows_lock);
        pme->port_id = bmp_allocate(instance->port_ids);

        list_add(&pme->next, &instance->port_map);

        pthread_rwlock_unlock(&instance->flows_lock);

        pthread_mutex_lock(&rne->state_lock);

        rne->req_ae_name = ae_name;

        rne->state = REG_NAME_FLOW_ARRIVED;

        reg_instance_wake(reg_entry_get_reg_instance(rne, pme->n_api));

        pthread_mutex_unlock(&rne->state_lock);

        while (acc_wait) {
                pthread_mutex_lock(&rne->state_lock);
                acc_wait = (rne->state == REG_NAME_FLOW_ARRIVED);
                pthread_mutex_unlock(&rne->state_lock);
        }

        pthread_rwlock_unlock(&instance->state_lock);

        return pme;
}

static int flow_alloc_reply(int port_id,
                            int response)
{
        struct port_map_entry * e;

        pthread_rwlock_rdlock(&instance->state_lock);
        pthread_rwlock_rdlock(&instance->flows_lock);

        e = get_port_map_entry(port_id);
        if (e == NULL) {
                pthread_rwlock_unlock(&instance->flows_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                return -1;
        }

        pthread_mutex_lock(&e->res_lock);

        if (!response)
                e->state = FLOW_ALLOCATED;

        else
                e->state = FLOW_NULL;

        if (pthread_cond_signal(&e->res_signal))
                LOG_ERR("Failed to send signal.");

        pthread_mutex_unlock(&e->res_lock);

        pthread_rwlock_unlock(&instance->flows_lock);
        pthread_rwlock_unlock(&instance->state_lock);

        return 0;
}

static int flow_dealloc_ipcp(int port_id)
{
        struct port_map_entry * e = NULL;

        pthread_rwlock_rdlock(&instance->state_lock);
        pthread_rwlock_wrlock(&instance->flows_lock);

        e = get_port_map_entry(port_id);
        if (e == NULL) {
                pthread_rwlock_unlock(&instance->flows_lock);
                pthread_rwlock_unlock(&instance->state_lock);
                return 0;
        }

        list_del(&e->next);

        pthread_rwlock_unlock(&instance->flows_lock);
        pthread_rwlock_unlock(&instance->state_lock);

        free(e);

        return 0;
}

static void irm_destroy()
{
        struct list_head * h;
        struct list_head * t;


        pthread_rwlock_rdlock(&instance->state_lock);

        if (instance->state != IRMD_NULL)
                LOG_WARN("Unsafe destroy.");

        if (instance->threadpool != NULL)
                free(instance->threadpool);

        pthread_rwlock_wrlock(&instance->reg_lock);
        /* clear the lists */
        list_for_each_safe(h, t, &instance->ipcps) {
                struct ipcp_entry * e = list_entry(h, struct ipcp_entry, next);
                list_del(&e->next);
                ipcp_destroy(e->api);
                ipcp_entry_destroy(e);
        }

        list_for_each_safe(h, t, &instance->registry) {
                struct reg_entry * e = list_entry(h, struct reg_entry, next);
                list_del(&e->next);
                reg_entry_destroy(e);
        }

        pthread_rwlock_unlock(&instance->reg_lock);

        pthread_rwlock_wrlock(&instance->flows_lock);

        list_for_each_safe(h, t, &instance->port_map) {
                struct port_map_entry * e = list_entry(h,
                                                       struct port_map_entry,
                                                       next);

                list_del(&e->next);
                port_map_entry_destroy(e);

        }

        if (instance->port_ids != NULL)
                bmp_destroy(instance->port_ids);

        pthread_rwlock_unlock(&instance->flows_lock);

        if (instance->dum != NULL)
                shm_du_map_destroy(instance->dum);

        close(instance->sockfd);

        pthread_rwlock_unlock(&instance->state_lock);

        free(instance);
}

void irmd_sig_handler(int sig, siginfo_t * info, void * c)
{
        int i;

        switch(sig) {
        case SIGINT:
        case SIGTERM:
        case SIGHUP:
                pthread_rwlock_wrlock(&instance->state_lock);

                instance->state = IRMD_NULL;

                pthread_rwlock_unlock(&instance->state_lock);

                if (instance->threadpool != NULL) {
                        for (i = 0; i < IRMD_THREADPOOL_SIZE; i++)
                                pthread_cancel(instance->threadpool[i]);

                }

                pthread_cancel(instance->shm_sanitize);
                pthread_cancel(instance->cleanup_flows);
                break;
        case SIGPIPE:
                LOG_DBG("Ignored SIGPIPE.");
        default:
                return;
        }
}

void * irm_flow_cleaner()
{
        struct timespec now;
        struct list_head * pos = NULL;
        struct list_head * n   = NULL;

        struct list_head * pos2 = NULL;
        struct list_head * n2   = NULL;

        struct timespec timeout = {IRMD_CLEANUP_TIMER / BILLION,
                                   IRMD_CLEANUP_TIMER % BILLION};

        while (true) {
                if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
                        LOG_WARN("Failed to get time.");
                /* cleanup stale PENDING flows */

                pthread_rwlock_rdlock(&instance->state_lock);

                if (&instance->state == IRMD_NULL) {
                        pthread_rwlock_unlock(&instance->state_lock);
                        return (void *) 0;
                }

                pthread_rwlock_wrlock(&instance->flows_lock);

                list_for_each_safe(pos, n, &(instance->port_map)) {
                        struct port_map_entry * e =
                                list_entry(pos, struct port_map_entry, next);

                        pthread_mutex_lock(&e->res_lock);

                        if (e->state == FLOW_PENDING &&
                            ts_diff_ms(&e->t0, &now) > IRMD_FLOW_TIMEOUT) {
                                LOG_INFO("Pending port_id %d timed out.",
                                         e->port_id);
                                e->state = FLOW_NULL;
                                pthread_cond_signal(&e->res_signal);
                                pthread_mutex_unlock(&e->res_lock);
                                continue;
                        }

                        pthread_mutex_unlock(&e->res_lock);

                        if (kill(e->n_api, 0) < 0) {
                                struct shm_ap_rbuff * n_rb =
                                        shm_ap_rbuff_open(e->n_api);
                                bmp_release(instance->port_ids, e->port_id);

                                list_del(&e->next);
                                LOG_INFO("Process %d gone, %d deallocated.",
                                         e->n_api, e->port_id);
                                ipcp_flow_dealloc(e->n_1_api, e->port_id);
                                if (n_rb != NULL)
                                        shm_ap_rbuff_destroy(n_rb);
                                port_map_entry_destroy(e);
                        }
                        if (kill(e->n_1_api, 0) < 0) {
                                struct shm_ap_rbuff * n_1_rb =
                                        shm_ap_rbuff_open(e->n_1_api);
                                list_del(&e->next);
                                LOG_ERR("IPCP %d gone, flow %d removed.",
                                        e->n_1_api, e->port_id);
                                if (n_1_rb != NULL)
                                        shm_ap_rbuff_destroy(n_1_rb);
                                port_map_entry_destroy(e);
                        }
                }

                pthread_rwlock_unlock(&instance->flows_lock);
                pthread_rwlock_wrlock(&instance->reg_lock);

                list_for_each_safe(pos, n, &(instance->registry)) {
                        struct reg_entry * e =
                                list_entry(pos, struct reg_entry, next);

                        list_for_each_safe(pos2, n2, &e->ap_instances) {
                                struct reg_instance * r =
                                        list_entry(pos2,
                                                   struct reg_instance,
                                                   next);
                                if (kill(r->api, 0) < 0) {
                                        LOG_INFO("Process %d gone, "
                                                 "instance deleted.",
                                                 r->api);
                                        registry_remove_ap_instance(e->name,
                                                                    r->api);
                                }
                        }
                }

                pthread_rwlock_unlock(&instance->reg_lock);
                pthread_rwlock_unlock(&instance->state_lock);

                nanosleep(&timeout, NULL);
        }
}

void clean_msg(void * msg)
{
        irm_msg__free_unpacked(msg, NULL);
}

void * mainloop()
{
        uint8_t buf[IRM_MSG_BUF_SIZE];

        while (true) {
                int cli_sockfd;
                irm_msg_t * msg;
                ssize_t count;
                buffer_t buffer;
                irm_msg_t ret_msg = IRM_MSG__INIT;
                struct port_map_entry * e = NULL;
                pid_t * apis = NULL;

                ret_msg.code = IRM_MSG_CODE__IRM_REPLY;

                cli_sockfd = accept(instance->sockfd, 0, 0);
                if (cli_sockfd < 0) {
                        LOG_ERR("Cannot accept new connection.");
                        continue;
                }

                count = read(cli_sockfd, buf, IRM_MSG_BUF_SIZE);
                if (count <= 0) {
                        LOG_ERR("Failed to read from socket.");
                        close(cli_sockfd);
                        continue;
                }

                msg = irm_msg__unpack(NULL, count, buf);
                if (msg == NULL) {
                        close(cli_sockfd);
                        continue;
                }

                pthread_cleanup_push(clean_msg, (void *) msg);

                switch (msg->code) {
                case IRM_MSG_CODE__IRM_CREATE_IPCP:
                        ret_msg.has_result = true;
                        ret_msg.result = create_ipcp(msg->dst_name,
                                                     msg->ipcp_type);
                        break;
                case IRM_MSG_CODE__IRM_DESTROY_IPCP:
                        ret_msg.has_result = true;
                        ret_msg.result = destroy_ipcp(msg->api);
                        break;
                case IRM_MSG_CODE__IRM_BOOTSTRAP_IPCP:
                        ret_msg.has_result = true;
                        ret_msg.result = bootstrap_ipcp(msg->api,
                                                        msg->conf);
                        break;
                case IRM_MSG_CODE__IRM_ENROLL_IPCP:
                        ret_msg.has_result = true;
                        ret_msg.result = enroll_ipcp(msg->api,
                                                     msg->dif_name[0]);
                        break;
                case IRM_MSG_CODE__IRM_BIND:
                        ret_msg.has_result = true;
                        ret_msg.result = bind_name(msg->dst_name,
                                                   msg->ap_name,
                                                   msg->opts,
                                                   msg->n_args,
                                                   msg->args);
                        break;
                case IRM_MSG_CODE__IRM_UNBIND:
                        ret_msg.has_result = true;
                        ret_msg.result = unbind_name(msg->dst_name,
                                                     msg->ap_name,
                                                     msg->opts);
                        break;
                case IRM_MSG_CODE__IRM_LIST_IPCPS:
                        ret_msg.n_apis = list_ipcps(msg->dst_name,
                                                    &apis);
                        ret_msg.apis = apis;
                        ret_msg.has_result = true;
                        break;
                case IRM_MSG_CODE__IRM_REG:
                        ret_msg.has_result = true;
                        ret_msg.result = ap_reg(msg->dst_name,
                                                msg->dif_name,
                                                msg->n_dif_name);
                        break;
                case IRM_MSG_CODE__IRM_UNREG:
                        ret_msg.has_result = true;
                        ret_msg.result = ap_unreg(msg->dst_name,
                                                  msg->dif_name,
                                                  msg->n_dif_name);
                        break;
                case IRM_MSG_CODE__IRM_FLOW_ACCEPT:
                        e = flow_accept(msg->api,
                                        msg->ap_name,
                                        &ret_msg.ae_name);

                        if (e == NULL)
                                break;

                        ret_msg.has_port_id = true;
                        ret_msg.port_id     = e->port_id;
                        ret_msg.has_api     = true;
                        ret_msg.api         = e->n_1_api;
                        break;
                case IRM_MSG_CODE__IRM_FLOW_ALLOC_RESP:
                        ret_msg.has_result = true;
                        ret_msg.result = flow_alloc_resp(msg->api,
                                                         msg->port_id,
                                                         msg->response);
                        break;
                case IRM_MSG_CODE__IRM_FLOW_ALLOC:
                        e = flow_alloc(msg->api,
                                       msg->dst_name,
                                       msg->ae_name,
                                       NULL);
                        if (e == NULL)
                                break;

                        ret_msg.has_port_id = true;
                        ret_msg.port_id     = e->port_id;
                        ret_msg.has_api     = true;
                        ret_msg.api         = e->n_1_api;
                        break;
                case IRM_MSG_CODE__IRM_FLOW_ALLOC_RES:
                        ret_msg.has_result = true;
                        ret_msg.result = flow_alloc_res(msg->port_id);
                        break;
                case IRM_MSG_CODE__IRM_FLOW_DEALLOC:
                        ret_msg.has_result = true;
                        ret_msg.result = flow_dealloc(msg->port_id);
                        break;
                case IRM_MSG_CODE__IPCP_FLOW_REQ_ARR:
                        e = flow_req_arr(msg->api,
                                         msg->dst_name,
                                         msg->ae_name);
                        if (e == NULL)
                                break;

                        ret_msg.has_port_id = true;
                        ret_msg.port_id     = e->port_id;
                        ret_msg.has_api     = true;
                        ret_msg.api         = e->n_api;
                        break;
                case IRM_MSG_CODE__IPCP_FLOW_ALLOC_REPLY:
                        ret_msg.has_result = true;
                        ret_msg.result = flow_alloc_reply(msg->port_id,
                                                          msg->response);
                        break;
                case IRM_MSG_CODE__IPCP_FLOW_DEALLOC:
                        ret_msg.has_result = true;
                        ret_msg.result = flow_dealloc_ipcp(msg->port_id);
                        break;
                default:
                        LOG_ERR("Don't know that message code.");
                        break;
                }

                pthread_cleanup_pop(true);

                buffer.size = irm_msg__get_packed_size(&ret_msg);
                if (buffer.size == 0) {
                        LOG_ERR("Failed to send reply message.");
                        if (apis != NULL)
                                free(apis);
                        close(cli_sockfd);
                        continue;
                }

                buffer.data = malloc(buffer.size);
                if (buffer.data == NULL) {
                        if (apis != NULL)
                                free(apis);
                        close(cli_sockfd);
                        continue;
                }

                irm_msg__pack(&ret_msg, buffer.data);

                if (write(cli_sockfd, buffer.data, buffer.size) == -1) {
                        free(buffer.data);
                        if (apis != NULL)
                                free(apis);
                        close(cli_sockfd);
                        continue;
                }

                if (apis != NULL)
                        free(apis);

                free(buffer.data);
                close(cli_sockfd);
        }
}

static struct irm * irm_create()
{
        struct stat st = {0};

        instance = malloc(sizeof(*instance));
        if (instance == NULL)
                return NULL;

        if (access("/dev/shm/" SHM_DU_MAP_FILENAME, F_OK) != -1) {
                struct shm_du_map * dum = shm_du_map_open();

                if (dum == NULL) {
                        LOG_ERR("Could not examine existing shm file.");
                        free(instance);
                        exit(EXIT_FAILURE);
                }

                if (kill(shm_du_map_owner(dum), 0) < 0) {
                        LOG_INFO("IRMd didn't properly shut down last time.");
                        shm_du_map_destroy(dum);
                        LOG_INFO("Stale shm file removed.");
                } else {
                        LOG_INFO("IRMd already running (%d), exiting.",
                                 shm_du_map_owner(dum));
                        free(instance);
                        exit(EXIT_SUCCESS);
                }
        }

        if (pthread_rwlock_init(&instance->state_lock, NULL)) {
                LOG_ERR("Failed to initialize rwlock.");
                free(instance);
                return NULL;
        }

        if (pthread_rwlock_init(&instance->reg_lock, NULL)) {
                LOG_ERR("Failed to initialize rwlock.");
                free(instance);
                return NULL;
        }

        if (pthread_rwlock_init(&instance->flows_lock, NULL)) {
                LOG_ERR("Failed to initialize rwlock.");
                free(instance);
                return NULL;
        }

        instance->threadpool = malloc(sizeof(pthread_t) * IRMD_THREADPOOL_SIZE);
        if (instance->threadpool == NULL) {
                irm_destroy();
                return NULL;
        }

        if ((instance->dum = shm_du_map_create()) == NULL) {
                irm_destroy();
                return NULL;
        }

        INIT_LIST_HEAD(&instance->ipcps);
        INIT_LIST_HEAD(&instance->registry);
        INIT_LIST_HEAD(&instance->port_map);

        instance->port_ids = bmp_create(IRMD_MAX_FLOWS, 0);
        if (instance->port_ids == NULL) {
                irm_destroy();
                return NULL;
        }

        if (stat(SOCK_PATH, &st) == -1) {
                if (mkdir(SOCK_PATH, 0777)) {
                        LOG_ERR("Failed to create sockets directory.");
                        irm_destroy();
                        return NULL;
                }
        }

        instance->sockfd = server_socket_open(IRM_SOCK_PATH);
        if (instance->sockfd < 0) {
                irm_destroy();
                return NULL;
        }

        if (chmod(IRM_SOCK_PATH, 0666)) {
                LOG_ERR("Failed to chmod socket.");
                irm_destroy();
                return NULL;
        }

        instance->state = IRMD_RUNNING;

        return instance;
}

int main()
{
        struct sigaction sig_act;

        int t = 0;

        if (geteuid() != 0) {
                LOG_ERR("IPC Resource Manager must be run as root.");
                exit(EXIT_FAILURE);
        }

        /* init sig_act */
        memset(&sig_act, 0, sizeof sig_act);

        /* install signal traps */
        sig_act.sa_sigaction = &irmd_sig_handler;
        sig_act.sa_flags     = SA_SIGINFO;

        if (sigaction(SIGINT,  &sig_act, NULL) < 0)
                exit(EXIT_FAILURE);
        if (sigaction(SIGTERM, &sig_act, NULL) < 0)
                exit(EXIT_FAILURE);
        if (sigaction(SIGHUP,  &sig_act, NULL) < 0)
                exit(EXIT_FAILURE);
        if (sigaction(SIGPIPE, &sig_act, NULL) < 0)
                exit(EXIT_FAILURE);

        instance = irm_create();
        if (instance == NULL)
                exit(EXIT_FAILURE);

        for (t = 0; t < IRMD_THREADPOOL_SIZE; ++t)
                pthread_create(&instance->threadpool[t], NULL, mainloop, NULL);

        pthread_create(&instance->cleanup_flows, NULL, irm_flow_cleaner, NULL);
        pthread_create(&instance->shm_sanitize, NULL,
                       shm_du_map_sanitize, instance->dum);

        /* wait for (all of them) to return */
        for (t = 0; t < IRMD_THREADPOOL_SIZE; ++t)
                pthread_join(instance->threadpool[t], NULL);

        pthread_join(instance->shm_sanitize, NULL);
        pthread_join(instance->cleanup_flows, NULL);

        irm_destroy();

        exit(EXIT_SUCCESS);
}
