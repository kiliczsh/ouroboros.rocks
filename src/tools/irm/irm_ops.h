/*
 * Ouroboros - Copyright (C) 2016 - 2021
 *
 * Functions of the IRM tool that are one level deep
 *
 *    Dimitri Staessens <dimitri.staessens@ugent.be>
 *    Sander Vrijders   <sander.vrijders@ugent.be>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following
 * disclaimer in the documentation and/or other materials provided
 * with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived
 * from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

int ipcp_cmd(int     argc,
             char ** argv);

int do_create_ipcp(int     argc,
                   char ** argv);

int do_destroy_ipcp(int     argc,
                    char ** argv);

int do_bootstrap_ipcp(int     argc,
                      char ** argv);

int do_enroll_ipcp(int     argc,
                   char ** argv);

int do_connect_ipcp(int     argc,
                    char ** argv);

int do_disconnect_ipcp(int     argc,
                       char ** argv);

int do_list_ipcp(int     argc,
                 char ** argv);

int bind_cmd(int     argc,
             char ** argv);

int do_bind_program(int     argc,
                    char ** argv);

int do_bind_process(int     argc,
                    char ** argv);

int do_bind_ipcp(int     argc,
                 char ** argv);

int unbind_cmd(int     argc,
               char ** argv);

int do_unbind_program(int     argc,
                      char ** argv);

int do_unbind_process(int     argc,
                      char ** argv);

int do_unbind_ipcp(int     argc,
                   char ** argv);

int name_cmd(int     argc,
             char ** argv);

int do_create_name(int     argc,
                   char ** argv);

int do_destroy_name(int     argc,
                    char ** argv);

int do_reg_name(int     argc,
                char ** argv);

int do_unreg_name(int     argc,
                  char ** argv);

int do_list_name(int     argc,
                 char ** argv);
