/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef SPDK_EVENTDRIVEN_INTERNAL_H_
#define SPDK_EVENTDRIVEN_INTERNAL_H_

#include "spdk/stdinc.h"
#include "spdk/edriven.h"

#define SPDK_MAX_EDRIVEN_NAME_LEN	256

/**
 * Callback function registered for one edriven.
 *
 * \param ctx Context passed as arg to spdk_edriven_register().
 * \return 0 to indicate that edriven took place but no events were found;
 * positive to indicate that edriven took place and some events were processed;
 * negative if the edriven does not provide spin-wait information.
 */
typedef int (*spdk_edriven_fn)(void *ctx);

struct spdk_edriven_group;

//reactor .c use~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
struct spdk_edriven_group *spdk_edriven_get_reactor_egrp(int lcore_idx);

/* Create epfd and add eventfd for each reactor.
 * 		Called in spdk_reactors_init()
 */
int spdk_edriven_reactor_init(int lcore_idx, spdk_edriven_fn equeue_fn, void *fn_arg,
			      const char *name);
int spdk_edriven_reactor_fini(int lcore_idx);

int spdk_edriven_group_wait(void *edriven_ctx, int timeout);

/* Create epfd and add eventfd for a new created spdk_thread.
 * the thd epfd will be aded into reactor epfd.
 *
 * called in _schedule_thread<-reactor_thread_operation<-spdk_thread_lib_init_ext(thread_op_fn)
 */
int spdk_edriven_reactor_add_thread(int current_core, struct spdk_thread *thread);
int spdk_edriven_reactor_remove_thread(int current_core, struct spdk_thread *thread);

/* Notify reactor by sending an event to reactor epfd.
 * event may be a spdk_thread_create
 *
 * called in spdk_event_call
 */
int spdk_edriven_reactor_event_notify(int lcore_idx);
int spdk_edriven_reactor_events_level_clear(int lcore_idx);

// thread.c use~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
int spdk_edriven_create_thread(struct spdk_thread *thread, const char *name);
int spdk_edriven_destroy_thread(struct spdk_thread *thread);

/* Notify thread by sending an event to thread epfd.
 * many spdk operations are depend on thread msg.
 *
 * called in spdk_thread_send_msg
 */
int spdk_edriven_thread_msg_notify(struct spdk_thread *thread);
int spdk_edriven_thread_msg_level_clear(struct spdk_thread *thread);

#endif /* SPDK_EVENTDRIVEN_INTERNAL_H_ */
