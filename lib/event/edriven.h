/*
 * intr.h
 *
 *  Created on: Mar 26, 2020
 *      Author: root
 */

#ifndef LIB_EVENT_EDRIVEN_H_
#define LIB_EVENT_EDRIVEN_H_

/* key functions to non-polling mode ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

/* Create epfd and add eventfd for each reactor.
 * 		Called in spdk_reactors_init()
 */
int spdk_reactors_edriven_init(int num_lcores);

/* epoll_wait for reactor epfd, and call related callback_fn if any event occurs.
 *
 * called in _spdk_reactor_run as a replacement for reactor_run
 * function reactor_run() should be split and replaced.
 */
int spdk_reactor_edriven_mainloop(struct spdk_reactor *reactor);


/* Create epfd and add eventfd for a new created spdk_thread.
 * the thd epfd will be aded into reactor epfd.
 *
 * called in _schedule_thread<-reactor_thread_operation<-spdk_thread_lib_init_ext(thread_op_fn)
 */
int spdk_reactor_edriven_schedule_thread(void *arg1, void *arg2);


/* Notify reactor by sending an event to reactor epfd.
 * event may be a spdk_thread_create
 *
 *
 * called in spdk_event_call
 */
int spdk_reactor_event_notify(int lcore_idx);

/* Notify thread by sending an event to thread epfd.
 * many spdk operations are depend on thread msg.
 *
 *
 * called in spdk_thread_send_msg
 */
int spdk_thread_msg_notify(struct spdk_thread *thread);


/* register a timerfd to thread epfd.
 *
 * A replacement to spdk_poller_register(,,period_microseconds,);
 */

int spdk_thread_edriven_interval_register(struct spdk_thread *thread,
		spdk_poller_fn fn, void *arg,
	      uint64_t period_microseconds,
	      const char *name);

/* create one eventfd, for example for one aio poll_group, it is added into thread epfd
 *
 * A replacement to spdk_poller_register in bdev_aio, can achieved by set fn to be bdev_aio_group_poll
 */
int spdk_thread_edriven_event_register(struct spdk_thread *thread,
		spdk_poller_fn fn, void *arg, const char *name);

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~`
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

struct spdk_edriven_event_source *spdk_thread_edriven_register(spdk_poller_fn fn,
		     void *arg);

int spdk_thread_edriven_unregister(struct spdk_edriven_event_source **esrc);


#endif /* LIB_EVENT_EDRIVEN_H_ */
