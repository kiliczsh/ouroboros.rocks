/*
 * Ouroboros - Copyright (C) 2016 - 2017
 *
 * Logging facilities
 *
 *    Sander Vrijders <sander.vrijders@intec.ugent.be>
 *    Francesco Salvestrini <f.salvestrini@nextworks.it>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#ifndef OUROBOROS_LOGS_H
#define OUROBOROS_LOGS_H

#include <unistd.h>
#include <stdio.h>

#ifndef OUROBOROS_PREFIX
#error You must define OUROBOROS_PREFIX before including this file
#endif

int  set_logfile(char * filename);
void close_logfile(void);

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_RESET   "\x1b[0m"

#define DEBUG_CODE "DB"
#define ERROR_CODE "EE"
#define WARN_CODE  "WW"
#define INFO_CODE  "II"
#define IMPL_CODE  "NI"

extern FILE * logfile;

#define __LOG(CLR, LVL, FMT, ARGS...)                                   \
        do {                                                            \
                if (logfile != NULL) {                                  \
                        fprintf(logfile,                                \
                                OUROBOROS_PREFIX "(" LVL "): "          \
                                FMT ANSI_COLOR_RESET "\n", ##ARGS);     \
                        fflush(logfile);                                \
                } else {                                                \
                        printf(CLR "==%05d== "                          \
                               OUROBOROS_PREFIX "(" LVL "): "           \
                               FMT ANSI_COLOR_RESET "\n", getpid(),     \
                               ##ARGS);                                 \
                }                                                       \
        } while (0)

#define LOG_ERR(FMT, ARGS...) __LOG(ANSI_COLOR_RED,             \
                                    ERROR_CODE, FMT, ##ARGS)
#define LOG_WARN(FMT, ARGS...) __LOG(ANSI_COLOR_YELLOW,         \
                                     WARN_CODE, FMT, ##ARGS)
#define LOG_INFO(FMT, ARGS...) __LOG(ANSI_COLOR_GREEN,          \
                                     INFO_CODE, FMT, ##ARGS)
#define LOG_NI(FMT, ARGS...) __LOG(ANSI_COLOR_BLUE,             \
                                   IMPL_CODE,  FMT, ##ARGS)

#ifdef CONFIG_OUROBOROS_DEBUG
#define LOG_DBG(FMT, ARGS...) __LOG("", DEBUG_CODE, FMT, ##ARGS)
#else
#define LOG_DBG(FMT, ARGS...) do { } while (0)
#endif

#define LOG_DBGF(FMT, ARGS...) LOG_DBG("%s: " FMT, __FUNCTION__, ##ARGS)

#define LOG_MISSING LOG_NI("Missing code in %s:%d",__FILE__, __LINE__)

#endif
