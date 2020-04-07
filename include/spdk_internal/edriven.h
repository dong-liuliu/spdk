/*
 * intr.h
 *
 *  Created on: Mar 26, 2020
 *      Author: root
 */

#ifndef LIB_EVENT_EDRIVEN_H_
#define LIB_EVENT_EDRIVEN_H_


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/* key functions to non-polling mode  */

/* Create epfd and add eventfd for each reactor.
 * 		Called in spdk_reactors_init()
 */

/** Function to be registered for the specific interrupt */
typedef int (*reactor_edriven_callback_fn)(void *cb_arg);

int spdk_reactor_edriven_init(int num_lcores, reactor_edriven_callback_fn cb, void *cb_arg);

int spdk_reactors_edriven_fini(int num_lcores,  reactor_edriven_callback_fn cb);

/* Notify reactor by sending an event to reactor epfd.
 * event may be a spdk_thread_create
 *
 *
 * called in spdk_event_call
 */
int spdk_reactor_event_notify(int lcore_idx);
int spdk_reactor_events_level_clear(int lcore_idx);

//struct reactor_edriven_ctx *ectx;
int spdk_reactor_edriven_epoll_wait(void *edriven_ctx, int timeout);

struct reactor_edriven_ctx *spdk_reactor_edriven_get_ctx(int lcore_idx);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~



#endif /* LIB_EVENT_EDRIVEN_H_ */
