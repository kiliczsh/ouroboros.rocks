/*
 * Ouroboros - Copyright (C) 2016
 *
 * PDU Forwarding Function
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

#ifndef OUROBOROS_IPCPD_NORMAL_PFF_H
#define OUROBOROS_IPCPD_NORMAL_PFF_H

#include <stdint.h>

struct pff;

/*
 * PFF will take a type in the future,
 * to allow different policies.
 * Only 1 fd per next hop for now.
 */
struct pff * pff_create(void);

int          pff_destroy(struct pff * instance);

int          pff_add(struct pff * instance,
                     uint64_t     addr,
                     int          fd);

int          pff_update(struct pff * instance,
                        uint64_t     addr,
                        int          fd);

int          pff_remove(struct pff * instance,
                        uint64_t     addr);

/* Returns fd towards next hop */
int          pff_nhop(struct pff * instance,
                      uint64_t     addr);

#endif /* OUROBOROS_IPCPD_NORMAL_PFF_H */