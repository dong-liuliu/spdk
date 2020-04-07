/*
 * edriven.h
 *
 *  Created on: Apr 7, 2020
 *      Author: root
 */

#ifndef INCLUDE_SPDK_EDRIVEN_THREAD_H_
#define INCLUDE_SPDK_EDRIVEN_THREAD_H_

#include "spdk/stdinc.h"
#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif


/* Notify thread by sending an event to thread epfd.
 * many spdk operations are depend on thread msg.
 *
 *
 * called in spdk_thread_send_msg
 */
int spdk_thread_msg_notify(struct spdk_thread *thread);

int spdk_thread_msg_level_clear(struct spdk_thread *thread);


/* Create epfd and add eventfd for a new created spdk_thread.
 * the thd epfd will be aded into reactor epfd.
 *
 * called in _schedule_thread<-reactor_thread_operation<-spdk_thread_lib_init_ext(thread_op_fn)
 */

int spdk_reactor_edriven_create_thread(struct spdk_thread *thread);

int spdk_reactor_edriven_add_thread(uint32_t current_core, struct spdk_thread *thread);

int spdk_reactor_edriven_remove_thread(uint32_t current_core, struct spdk_thread *thread);

int spdk_reactor_edriven_destroy_thread(struct spdk_thread *thread);



//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
struct spdk_edriven_event_source;

static inline struct spdk_poller *spdk_edriven_source_to_poller(struct spdk_edriven_event_source *esrc)
{
	return (struct spdk_poller *)esrc;
}

static inline struct spdk_edriven_event_source *spdk_poller_to_edriven_source(struct spdk_poller *poller)
{
	return (struct spdk_edriven_event_source *)poller;
}

int spdk_edriven_source_get_efd(struct spdk_edriven_event_source *esrc);

/* create one eventfd, for example for one aio poll_group, it is added into thread epfd
 *
 * A replacement to spdk_poller_register in bdev_aio, can achieved by set fn to be bdev_aio_group_poll
 */
struct spdk_edriven_event_source *spdk_thread_edriven_register(spdk_poller_fn fn, void *arg,
		const char *name);

/* register a timerfd to thread epfd.
 * A replacement to spdk_poller_register(,,period_microseconds,);
 */
struct spdk_edriven_event_source *spdk_thread_edriven_interval_register(spdk_poller_fn fn, void *arg,
	      uint64_t period_microseconds, const char *name);

int spdk_thread_edriven_unregister(struct spdk_edriven_event_source **pesrc);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

/* aio related operations
 * 1. create a eventfd for aio poll group, add add it to epfd.
 * 2. in add io_set_eventfd(&iocbp->iocb, efd); io_set_callback(&iocbp->iocb, aio_callback);
 * 3. if epoll_wait return efd event, then call *bdev_aio_group_poll for next operations.
 * */



#ifdef __cplusplus
}
#endif


#endif /* INCLUDE_SPDK_EDRIVEN_THREAD_H_ */
