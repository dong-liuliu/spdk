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

#ifndef SPDK_EVENTDRIVEN__H_
#define SPDK_EVENTDRIVEN__H_

#include "spdk/stdinc.h"
#include "spdk/thread.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_edriven_esrc;

/**
 * Set the running mode of SPDK APP framework.
 *
 * This function must be called before spdk_reactors_init().
 */
void spdk_set_edriven_mode(void);
bool spdk_is_edriven_mode(void);

int
spdk_edriven_thread_unregister_esrc(struct spdk_edriven_esrc **pesrc);

/* aio related operations
 * 1. create a eventfd for aio poll group, add add it to epfd.
 * 2. in add io_set_eventfd(&iocbp->iocb, efd); io_set_callback(&iocbp->iocb, aio_callback);
 * 3. if epoll_wait return efd event, then call *bdev_aio_group_poll for next operations.
 * */
/* create one eventfd, for example for one aio poll_group, it is added into thread epfd
 *
 * A replacement to spdk_poller_register in bdev_aio, can achieved by set fn to be bdev_aio_group_poll
 */
struct spdk_edriven_esrc *spdk_edriven_thread_register_aio(spdk_poller_fn fn,
		void *arg, const char *name);
int spdk_edriven_esrc_get_efd(struct spdk_edriven_esrc *esrc);

struct spdk_edriven_esrc *spdk_edriven_thread_register_nbd(spdk_poller_fn fn,
		void *arg, const char *name, int datafd);
int spdk_edriven_thread_nbd_change_trigger(struct spdk_edriven_esrc *event_src, bool edge_trigger);


struct spdk_edriven_esrc *spdk_edriven_thread_register_vqkick(spdk_poller_fn fn,
		void *arg, const char *name, int vq_kickfd);

/* register a timerfd to thread epfd.
 * A replacement to spdk_poller_register(,,period_microseconds,);
 */
struct spdk_edriven_esrc *spdk_edriven_thread_register_interval_esrc(spdk_poller_fn fn, void *arg,
		uint64_t period_microseconds, const char *name);


static inline struct spdk_poller *spdk_edriven_source_to_poller(struct spdk_edriven_esrc *esrc)
{
	return (struct spdk_poller *)esrc;
}

static inline struct spdk_edriven_esrc *spdk_poller_to_edriven_source(struct spdk_poller *poller)
{
	return (struct spdk_edriven_esrc *)poller;
}

#ifdef __cplusplus
}
#endif


#endif /* SPDK_EVENTDRIVEN__H_ */
