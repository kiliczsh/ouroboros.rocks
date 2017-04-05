/*
 * Ouroboros - Copyright (C) 2016 - 2017
 *
 * Flow manager of the IPC Process
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define OUROBOROS_PREFIX "flow-manager"

#include <ouroboros/config.h>
#include <ouroboros/logs.h>
#include <ouroboros/dev.h>
#include <ouroboros/list.h>
#include <ouroboros/ipcp-dev.h>
#include <ouroboros/fqueue.h>
#include <ouroboros/errno.h>
#include <ouroboros/cacep.h>
#include <ouroboros/rib.h>

#include "connmgr.h"
#include "fmgr.h"
#include "frct.h"
#include "ipcp.h"
#include "shm_pci.h"
#include "ribconfig.h"
#include "pff.h"
#include "neighbors.h"
#include "gam.h"
#include "routing.h"

#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include <string.h>
#include <inttypes.h>

#include "flow_alloc.pb-c.h"
typedef FlowAllocMsg flow_alloc_msg_t;

#define FD_UPDATE_TIMEOUT 10000 /* nanoseconds */

struct {
        flow_set_t *       np1_set[QOS_CUBE_MAX];
        fqueue_t *         np1_fqs[QOS_CUBE_MAX];
        pthread_rwlock_t   np1_flows_lock;

        cep_id_t           np1_fd_to_cep_id[AP_MAX_FLOWS];
        int                np1_cep_id_to_fd[IPCPD_MAX_CONNS];

        pthread_t          np1_sdu_reader;

        flow_set_t *       nm1_set[QOS_CUBE_MAX];
        fqueue_t *         nm1_fqs[QOS_CUBE_MAX];
        pthread_t          nm1_sdu_reader;

        struct pff *       pff[QOS_CUBE_MAX];
        struct routing_i * routing[QOS_CUBE_MAX];

        struct gam *       gam;
        struct nbs *       nbs;
        struct ae *        ae;

        struct nb_notifier nb_notifier;
} fmgr;

static int fmgr_neighbor_event(enum nb_event event,
                               struct conn   conn)
{
        qoscube_t cube;

        /* We are only interested in neighbors being added and removed. */
        switch (event) {
        case NEIGHBOR_ADDED:
                ipcp_flow_get_qoscube(conn.flow_info.fd, &cube);
                flow_set_add(fmgr.nm1_set[cube], conn.flow_info.fd);
                log_dbg("Added fd %d to flow set.", conn.flow_info.fd);
                break;
        case NEIGHBOR_REMOVED:
                ipcp_flow_get_qoscube(conn.flow_info.fd, &cube);
                flow_set_del(fmgr.nm1_set[cube], conn.flow_info.fd);
                log_dbg("Removed fd %d from flow set.", conn.flow_info.fd);
                break;
        default:
                break;
        }

        return 0;
}

static void * fmgr_np1_sdu_reader(void * o)
{
        struct shm_du_buff * sdb;
        struct timespec      timeout = {0, FD_UPDATE_TIMEOUT};
        int                  fd;
        int                  i = 0;
        int                  ret;

        (void) o;

        while (true) {
                /* FIXME: replace with scheduling policy call */
                i = (i + 1) % QOS_CUBE_MAX;

                ret = flow_event_wait(fmgr.np1_set[i],
                                      fmgr.np1_fqs[i],
                                      &timeout);
                if (ret == -ETIMEDOUT)
                        continue;

                if (ret < 0) {
                        log_warn("Event error: %d.", ret);
                        continue;
                }

                while ((fd = fqueue_next(fmgr.np1_fqs[i])) >= 0) {
                        if (ipcp_flow_read(fd, &sdb)) {
                                log_warn("Failed to read SDU from fd %d.", fd);
                                continue;
                        }

                        pthread_rwlock_rdlock(&fmgr.np1_flows_lock);

                        if (frct_i_write_sdu(fmgr.np1_fd_to_cep_id[fd], sdb)) {
                                pthread_rwlock_unlock(&fmgr.np1_flows_lock);
                                ipcp_flow_del(sdb);
                                log_warn("Failed to hand SDU to FRCT.");
                                continue;
                        }

                        pthread_rwlock_unlock(&fmgr.np1_flows_lock);
                }
        }

        return (void *) 0;
}

void * fmgr_nm1_sdu_reader(void * o)
{
        struct timespec      timeout = {0, FD_UPDATE_TIMEOUT};
        struct shm_du_buff * sdb;
        struct pci           pci;
        int                  fd;
        int                  i = 0;
        int                  ret;

        (void) o;

        memset(&pci, 0, sizeof(pci));

        while (true) {
                /* FIXME: replace with scheduling policy call */
                i = (i + 1) % QOS_CUBE_MAX;

                ret = flow_event_wait(fmgr.nm1_set[i],
                                      fmgr.nm1_fqs[i],
                                      &timeout);
                if (ret == -ETIMEDOUT)
                        continue;

                if (ret < 0) {
                        log_err("Event error: %d.", ret);
                        continue;
                }

                while ((fd = fqueue_next(fmgr.nm1_fqs[i])) >= 0) {
                        if (ipcp_flow_read(fd, &sdb)) {
                                log_err("Failed to read SDU from fd %d.", fd);
                                continue;
                        }

                        shm_pci_des(sdb, &pci);

                        if (pci.dst_addr != ipcpi.dt_addr) {
                                if (pci.ttl == 0) {
                                        log_dbg("TTL was zero.");
                                        ipcp_flow_del(sdb);
                                        continue;
                                }

                                pff_lock(fmgr.pff[i]);
                                fd = pff_nhop(fmgr.pff[i], pci.dst_addr);
                                if (fd < 0) {
                                        pff_unlock(fmgr.pff[i]);
                                        log_err("No next hop for %" PRIu64,
                                                pci.dst_addr);
                                        ipcp_flow_del(sdb);
                                        continue;
                                }
                                pff_unlock(fmgr.pff[i]);

                                if (ipcp_flow_write(fd, sdb)) {
                                        log_err("Failed to write SDU to fd %d.",
                                                fd);
                                        ipcp_flow_del(sdb);
                                        continue;
                                }
                        } else {
                                shm_pci_shrink(sdb);

                                if (frct_nm1_post_sdu(&pci, sdb)) {
                                        log_err("Failed to hand PDU to FRCT.");
                                        continue;
                                }
                        }
                }
        }

        return (void *) 0;
}

static void fmgr_destroy_flows(void)
{
        int i;

        for (i = 0; i < QOS_CUBE_MAX; ++i) {
                flow_set_destroy(fmgr.nm1_set[i]);
                flow_set_destroy(fmgr.np1_set[i]);
                fqueue_destroy(fmgr.nm1_fqs[i]);
                fqueue_destroy(fmgr.np1_fqs[i]);
        }
}

static void fmgr_destroy_routing(void)
{
        int i;

        for (i = 0; i < QOS_CUBE_MAX; ++i)
                routing_i_destroy(fmgr.routing[i]);
}

static void fmgr_destroy_pff(void)
{
        int i;

        for (i = 0; i < QOS_CUBE_MAX; ++i)
                pff_destroy(fmgr.pff[i]);
}

int fmgr_init(void)
{
        int              i;
        int              j;
        struct conn_info info;

        for (i = 0; i < AP_MAX_FLOWS; ++i)
                fmgr.np1_fd_to_cep_id[i] = INVALID_CEP_ID;

        for (i = 0; i < IPCPD_MAX_CONNS; ++i)
                fmgr.np1_cep_id_to_fd[i] = -1;

        for (i = 0; i < QOS_CUBE_MAX; ++i) {
                fmgr.np1_set[i] = flow_set_create();
                if (fmgr.np1_set[i] == NULL) {
                        fmgr_destroy_flows();
                        return -1;
                }

                fmgr.np1_fqs[i] = fqueue_create();
                if (fmgr.np1_fqs[i] == NULL) {
                        fmgr_destroy_flows();
                        return -1;
                }

                fmgr.nm1_set[i] = flow_set_create();
                if (fmgr.nm1_set[i] == NULL) {
                        fmgr_destroy_flows();
                        return -1;
                }

                fmgr.nm1_fqs[i] = fqueue_create();
                if (fmgr.nm1_fqs[i] == NULL) {
                        fmgr_destroy_flows();
                        return -1;
                }
        }

        if (shm_pci_init()) {
                log_err("Failed to init shm pci.");
                fmgr_destroy_flows();
                return -1;
        }

        memset(&info, 0, sizeof(info));

        strcpy(info.ae_name, DT_AE);
        strcpy(info.protocol, FRCT_PROTO);
        info.pref_version = 1;
        info.pref_syntax = PROTO_FIXED;
        info.addr = ipcpi.dt_addr;

        fmgr.ae = connmgr_ae_create(info);
        if (fmgr.ae == NULL) {
                log_err("Failed to create AE struct.");
                fmgr_destroy_flows();
                return -1;
        }

        fmgr.nbs = nbs_create();
        if (fmgr.nbs == NULL) {
                log_err("Failed to create neighbors struct.");
                fmgr_destroy_flows();
                connmgr_ae_destroy(fmgr.ae);
                return -1;
        }

        fmgr.nb_notifier.notify_call = fmgr_neighbor_event;
        if (nbs_reg_notifier(fmgr.nbs, &fmgr.nb_notifier)) {
                log_err("Failed to register notifier.");
                nbs_destroy(fmgr.nbs);
                fmgr_destroy_flows();
                connmgr_ae_destroy(fmgr.ae);
                return -1;
        }

        if (routing_init(fmgr.nbs)) {
                log_err("Failed to init routing.");
                nbs_unreg_notifier(fmgr.nbs, &fmgr.nb_notifier);
                nbs_destroy(fmgr.nbs);
                fmgr_destroy_flows();
                connmgr_ae_destroy(fmgr.ae);
                return -1;
        }

        if (pthread_rwlock_init(&fmgr.np1_flows_lock, NULL)) {
                routing_fini();
                nbs_unreg_notifier(fmgr.nbs, &fmgr.nb_notifier);
                nbs_destroy(fmgr.nbs);
                fmgr_destroy_flows();
                connmgr_ae_destroy(fmgr.ae);
                return -1;
        }

        for (i = 0; i < QOS_CUBE_MAX; ++i) {
                fmgr.pff[i] = pff_create();
                if (fmgr.pff[i] == NULL) {
                        for (j = 0; j < i; ++j)
                                pff_destroy(fmgr.pff[j]);
                        pthread_rwlock_destroy(&fmgr.np1_flows_lock);
                        routing_fini();
                        nbs_unreg_notifier(fmgr.nbs, &fmgr.nb_notifier);
                        nbs_destroy(fmgr.nbs);
                        fmgr_destroy_flows();
                        connmgr_ae_destroy(fmgr.ae);
                        return -1;
                }

                fmgr.routing[i] = routing_i_create(fmgr.pff[i]);
                if (fmgr.routing[i] == NULL) {
                        for (j = 0; j < i; ++j)
                                routing_i_destroy(fmgr.routing[j]);
                        fmgr_destroy_pff();
                        pthread_rwlock_destroy(&fmgr.np1_flows_lock);
                        routing_fini();
                        nbs_unreg_notifier(fmgr.nbs, &fmgr.nb_notifier);
                        nbs_destroy(fmgr.nbs);
                        fmgr_destroy_flows();
                        connmgr_ae_destroy(fmgr.ae);
                        return -1;
                }
        }

        return 0;
}

void fmgr_fini()
{
        nbs_unreg_notifier(fmgr.nbs, &fmgr.nb_notifier);

        fmgr_destroy_routing();

        fmgr_destroy_pff();

        routing_fini();

        fmgr_destroy_flows();

        connmgr_ae_destroy(fmgr.ae);

        nbs_destroy(fmgr.nbs);
}

int fmgr_start(void)
{
        enum pol_gam pg;

        if (rib_read(BOOT_PATH "/dt/gam/type", &pg, sizeof(pg))
            != sizeof(pg)) {
                log_err("Failed to read policy for ribmgr gam.");
                return -1;
        }

        fmgr.gam = gam_create(pg, fmgr.nbs, fmgr.ae);
        if (fmgr.gam == NULL) {
                log_err("Failed to init dt graph adjacency manager.");
                return -1;
        }

        pthread_create(&fmgr.np1_sdu_reader, NULL, fmgr_np1_sdu_reader, NULL);
        pthread_create(&fmgr.nm1_sdu_reader, NULL, fmgr_nm1_sdu_reader, NULL);

        return 0;
}

void fmgr_stop(void)
{
        pthread_cancel(fmgr.np1_sdu_reader);
        pthread_cancel(fmgr.nm1_sdu_reader);

        pthread_join(fmgr.np1_sdu_reader, NULL);
        pthread_join(fmgr.nm1_sdu_reader, NULL);

        gam_destroy(fmgr.gam);
}

int fmgr_np1_alloc(int       fd,
                   char *    dst_ap_name,
                   qoscube_t cube)
{
        cep_id_t         cep_id;
        buffer_t         buf;
        flow_alloc_msg_t msg = FLOW_ALLOC_MSG__INIT;
        char             path[RIB_MAX_PATH_LEN + 1];
        uint64_t         addr;
        ssize_t          ch;
        ssize_t          i;
        char **          children;
        char *           dst_ipcp = NULL;

        assert(strlen(dst_ap_name) + strlen("/" DIR_NAME) + 1
               < RIB_MAX_PATH_LEN);

        strcpy(path, DIR_PATH);

        rib_path_append(path, dst_ap_name);

        ch = rib_children(path, &children);
        if (ch <= 0)
                return -1;

        for (i = 0; i < ch; ++i)
                if (dst_ipcp == NULL && strcmp(children[i], ipcpi.name) != 0)
                        dst_ipcp = children[i];
                else
                        free(children[i]);

        free(children);

        if (dst_ipcp == NULL)
                return -1;

        strcpy(path, "/" MEMBERS_NAME);

        rib_path_append(path, dst_ipcp);

        free(dst_ipcp);

        if (rib_read(path, &addr, sizeof(addr)) < 0)
                return -1;

        msg.code = FLOW_ALLOC_CODE__FLOW_REQ;
        msg.dst_name = dst_ap_name;
        msg.has_qoscube = true;
        msg.qoscube = cube;

        buf.len = flow_alloc_msg__get_packed_size(&msg);
        if (buf.len == 0)
                return -1;

        buf.data = malloc(buf.len);
        if (buf.data == NULL)
                return -1;

        flow_alloc_msg__pack(&msg, buf.data);

        pthread_rwlock_wrlock(&fmgr.np1_flows_lock);

        cep_id = frct_i_create(addr, &buf, cube);
        if (cep_id == INVALID_CEP_ID) {
                pthread_rwlock_unlock(&fmgr.np1_flows_lock);
                free(buf.data);
                return -1;
        }

        free(buf.data);

        fmgr.np1_fd_to_cep_id[fd] = cep_id;
        fmgr.np1_cep_id_to_fd[cep_id] = fd;

        pthread_rwlock_unlock(&fmgr.np1_flows_lock);

        return 0;
}

/* Call under np1_flows lock */
static int np1_flow_dealloc(int fd)
{
        flow_alloc_msg_t msg = FLOW_ALLOC_MSG__INIT;
        buffer_t         buf;
        int              ret;
        qoscube_t        cube;

        ipcp_flow_get_qoscube(fd, &cube);
        flow_set_del(fmgr.np1_set[cube], fd);

        msg.code = FLOW_ALLOC_CODE__FLOW_DEALLOC;

        buf.len = flow_alloc_msg__get_packed_size(&msg);
        if (buf.len == 0)
                return -1;

        buf.data = malloc(buf.len);
        if (buf.data == NULL)
                return -ENOMEM;

        flow_alloc_msg__pack(&msg, buf.data);

        ret = frct_i_destroy(fmgr.np1_fd_to_cep_id[fd], &buf);

        fmgr.np1_cep_id_to_fd[fmgr.np1_fd_to_cep_id[fd]] = INVALID_CEP_ID;
        fmgr.np1_fd_to_cep_id[fd] = -1;

        free(buf.data);

        return ret;
}

int fmgr_np1_alloc_resp(int fd,
                        int response)
{
        flow_alloc_msg_t msg = FLOW_ALLOC_MSG__INIT;
        buffer_t         buf;

        msg.code = FLOW_ALLOC_CODE__FLOW_REPLY;
        msg.response = response;
        msg.has_response = true;

        buf.len = flow_alloc_msg__get_packed_size(&msg);
        if (buf.len == 0)
                return -1;

        buf.data = malloc(buf.len);
        if (buf.data == NULL)
                return -ENOMEM;

        flow_alloc_msg__pack(&msg, buf.data);

        pthread_rwlock_wrlock(&fmgr.np1_flows_lock);

        if (response < 0) {
                frct_i_destroy(fmgr.np1_fd_to_cep_id[fd], &buf);
                free(buf.data);
                fmgr.np1_cep_id_to_fd[fmgr.np1_fd_to_cep_id[fd]]
                        = INVALID_CEP_ID;
                fmgr.np1_fd_to_cep_id[fd] = -1;
        } else {
                qoscube_t cube;
                ipcp_flow_get_qoscube(fd, &cube);
                if (frct_i_accept(fmgr.np1_fd_to_cep_id[fd], &buf, cube)) {
                        pthread_rwlock_unlock(&fmgr.np1_flows_lock);
                        free(buf.data);
                        return -1;
                }
                flow_set_add(fmgr.np1_set[cube], fd);
        }

        pthread_rwlock_unlock(&fmgr.np1_flows_lock);

        free(buf.data);

        return 0;
}

int fmgr_np1_dealloc(int fd)
{
        int ret;

        pthread_rwlock_wrlock(&fmgr.np1_flows_lock);

        ret = np1_flow_dealloc(fd);

        pthread_rwlock_unlock(&fmgr.np1_flows_lock);

        return ret;
}

int fmgr_np1_post_buf(cep_id_t   cep_id,
                      buffer_t * buf)
{
        int ret = 0;
        int fd;
        flow_alloc_msg_t * msg;
        qoscube_t cube;

        /* Depending on the message call the function in ipcp-dev.h */

        msg = flow_alloc_msg__unpack(NULL, buf->len, buf->data);
        if (msg == NULL) {
                log_err("Failed to unpack flow alloc message");
                return -1;
        }

        switch (msg->code) {
        case FLOW_ALLOC_CODE__FLOW_REQ:
                pthread_mutex_lock(&ipcpi.alloc_lock);
                fd = ipcp_flow_req_arr(getpid(),
                                       msg->dst_name,
                                       msg->qoscube);
                if (fd < 0) {
                        pthread_mutex_unlock(&ipcpi.alloc_lock);
                        flow_alloc_msg__free_unpacked(msg, NULL);
                        log_err("Failed to get fd for flow.");
                        return -1;
                }

                pthread_rwlock_wrlock(&fmgr.np1_flows_lock);

                fmgr.np1_fd_to_cep_id[fd] = cep_id;
                fmgr.np1_cep_id_to_fd[cep_id] = fd;

                pthread_rwlock_unlock(&fmgr.np1_flows_lock);
                pthread_mutex_unlock(&ipcpi.alloc_lock);

                break;
        case FLOW_ALLOC_CODE__FLOW_REPLY:
                pthread_rwlock_wrlock(&fmgr.np1_flows_lock);

                fd = fmgr.np1_cep_id_to_fd[cep_id];
                ret = ipcp_flow_alloc_reply(fd, msg->response);
                if (msg->response < 0) {
                        fmgr.np1_fd_to_cep_id[fd] = INVALID_CEP_ID;
                        fmgr.np1_cep_id_to_fd[cep_id] = -1;
                } else {
                        ipcp_flow_get_qoscube(fd, &cube);
                        flow_set_add(fmgr.np1_set[cube],
                                     fmgr.np1_cep_id_to_fd[cep_id]);
                }

                pthread_rwlock_unlock(&fmgr.np1_flows_lock);

                break;
        case FLOW_ALLOC_CODE__FLOW_DEALLOC:
                fd = fmgr.np1_cep_id_to_fd[cep_id];
                ipcp_flow_get_qoscube(fd, &cube);
                flow_set_del(fmgr.np1_set[cube], fd);
                ret = flow_dealloc(fd);
                break;
        default:
                log_err("Got an unknown flow allocation message.");
                ret = -1;
                break;
        }

        flow_alloc_msg__free_unpacked(msg, NULL);

        return ret;
}

int fmgr_np1_post_sdu(cep_id_t             cep_id,
                      struct shm_du_buff * sdb)
{
        int fd;

        pthread_rwlock_rdlock(&fmgr.np1_flows_lock);

        fd = fmgr.np1_cep_id_to_fd[cep_id];
        if (ipcp_flow_write(fd, sdb)) {
                pthread_rwlock_unlock(&fmgr.np1_flows_lock);
                log_err("Failed to hand SDU to N flow.");
                return -1;
        }

        pthread_rwlock_unlock(&fmgr.np1_flows_lock);

        return 0;
}

int fmgr_nm1_write_sdu(struct pci *         pci,
                       struct shm_du_buff * sdb)
{
        int fd;

        if (pci == NULL || sdb == NULL)
                return -EINVAL;

        pff_lock(fmgr.pff[pci->qos_id]);
        fd = pff_nhop(fmgr.pff[pci->qos_id], pci->dst_addr);
        if (fd < 0) {
                pff_unlock(fmgr.pff[pci->qos_id]);
                log_err("Could not get nhop for address %" PRIu64,
                        pci->dst_addr);
                ipcp_flow_del(sdb);
                return -1;
        }
        pff_unlock(fmgr.pff[pci->qos_id]);

        if (shm_pci_ser(sdb, pci)) {
                log_err("Failed to serialize PDU.");
                ipcp_flow_del(sdb);
                return -1;
        }

        if (ipcp_flow_write(fd, sdb)) {
                log_err("Failed to write SDU to fd %d.", fd);
                ipcp_flow_del(sdb);
                return -1;
        }

        return 0;
}

int fmgr_nm1_write_buf(struct pci * pci,
                       buffer_t *   buf)
{
        buffer_t * buffer;
        int        fd;

        if (pci == NULL || buf == NULL || buf->data == NULL)
                return -EINVAL;

        pff_lock(fmgr.pff[pci->qos_id]);
        fd = pff_nhop(fmgr.pff[pci->qos_id], pci->dst_addr);
        if (fd < 0) {
                pff_unlock(fmgr.pff[pci->qos_id]);
                log_err("Could not get nhop for address %" PRIu64,
                        pci->dst_addr);
                return -1;
        }
        pff_unlock(fmgr.pff[pci->qos_id]);

        buffer = shm_pci_ser_buf(buf, pci);
        if (buffer == NULL) {
                log_err("Failed to serialize buffer.");
                return -1;
        }

        if (flow_write(fd, buffer->data, buffer->len) == -1) {
                log_err("Failed to write buffer to fd.");
                free(buffer);
                return -1;
        }

        free(buffer->data);
        free(buffer);
        return 0;
}
