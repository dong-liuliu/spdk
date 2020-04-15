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

#include "spdk/stdinc.h"
#include "spdk/likely.h"
#include "spdk/env.h"
#include "spdk/util.h"

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

#include "spdk_internal/event.h"
#include "spdk_internal/log.h"
#include "spdk_internal/thread.h"
#include "spdk_internal/edriven.h"

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

struct edriven_callback_ctx {
	spdk_edriven_fn cb_fn;  /**< callback func address */
	void *cb_arg;           /**< parameter for callback */
};

TAILQ_HEAD(reactor_edriven_cb_list, reactor_edriven_callback);

struct spdk_edriven_esrc {
	TAILQ_ENTRY(spdk_edriven_esrc)	next;
	struct edriven_callback_ctx				ctx;  /**< user callback */
	int 									fd;	 /**< interrupt event file descriptor */
	int 									epevent_flag; /**<  event types for registered file descriptor */
	bool timer; /**< it is a timer */
	bool keepfd; /**< the fd is created outside of edriven, so it shouldn't be closed in edriven */
	bool clear_edge; /**< after this event is triggered, the fd should be read out to clear the event edge */

	char name[SPDK_MAX_EDRIVEN_NAME_LEN + 1];
};

TAILQ_HEAD(spdk_edriven_event_source_list, spdk_edriven_esrc);

struct spdk_edriven_group {
	int epfd;
	int num_fds;	// Number of fds registered in this group

	/* Notification to process event_ring
	 * For each spdk_event_call, write 1 into this eventfd,
	 * After triggered, and event_queue_run_batch may not process all events,
	 * so event_queue_run_batch should write 1 into eventfd if event_ring
	 * is not empty, in order to get remained events processed in next loop.
	 */
	struct spdk_edriven_esrc *equeue_esrc;

	/* interrupt sources list */
	struct spdk_edriven_event_source_list edriven_sources;

	/* used to get epoll events */
	struct epoll_event *events_pool;
	int events_poll_size;

	char name[SPDK_MAX_EDRIVEN_NAME_LEN + 1];
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

static struct spdk_edriven_esrc *edriven_group_esrc_register(struct spdk_edriven_group *egrp,
		int efd, int epevent_flag,
		spdk_edriven_fn fn, void *arg, const char *esrc_name);

static int edriven_group_esrc_unregister(struct spdk_edriven_group *egrp,
		struct spdk_edriven_esrc *esrc);

static int edriven_group_equeue_register(struct spdk_edriven_group *egrp,
		spdk_edriven_fn equeue_fn, void *fn_arg, const char *eq_name);

static struct spdk_edriven_group *
spdk_edriven_group_init(spdk_edriven_fn equeue_fn, void *fn_arg, const char *grp_name)
{
	struct spdk_edriven_group *egrp;
	struct spdk_edriven_event_source_list *edriven_sources;
	char equeue_name[SPDK_MAX_EDRIVEN_NAME_LEN + 1];
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_EDRIVEN, "init edriven group %s\n", grp_name);

	egrp = calloc(1, sizeof(*egrp));
	if (egrp == NULL) {
		SPDK_ERRLOG("Failed to allocate spdk_edriven_group\n");
		return NULL;
	}

	/* init the event source head */
	edriven_sources = &egrp->edriven_sources;
	TAILQ_INIT(edriven_sources);

	egrp->epfd = epoll_create1(EPOLL_CLOEXEC);
	assert(egrp->epfd > 0);
	egrp->num_fds = 0;
	if (grp_name) {
		snprintf(egrp->name, sizeof(egrp->name), "%s", grp_name);
	} else {
		snprintf(egrp->name, sizeof(egrp->name), "group-%d", rand());
	}

	snprintf(equeue_name, sizeof(equeue_name), "%s-%s", grp_name, "equeue_esrc");
	rc = edriven_group_equeue_register(egrp, equeue_fn, fn_arg, (const char *)equeue_name);
	assert(rc == 0);
	(void)rc;

	return egrp;
}

static int
spdk_edriven_group_fini(struct spdk_edriven_group *egrp)
{
	int rc;

	if (egrp == NULL) {
		SPDK_ERRLOG("NUll spdk_edriven_group pointer.\n");
		return -1;
	}

	SPDK_DEBUGLOG(SPDK_LOG_EDRIVEN, "fini edriven group (%s)\n", egrp->name);

	/* num_fds check */
	if (egrp->num_fds < 1) {
		SPDK_ERRLOG("there should be at least 1 esrc, but now %d esrc in egrp (%s)\n", egrp->num_fds,
			    egrp->name);
		return -1;
	} else if (egrp->num_fds > 1) {
		SPDK_ERRLOG("there are %d existed esrcs in egrp (%s)\n", egrp->num_fds, egrp->name);
		return -1;
	}

	/* unregister efd for msg queue to thread epfd */
	rc = edriven_group_esrc_unregister(egrp, egrp->equeue_esrc);
	assert(rc == 0);
	(void)rc;

	close(egrp->epfd);
	free(egrp->events_pool);
	free(egrp);

	return 0;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

#define MAX_LCORE_NUM	64
static struct spdk_edriven_group *g_edriven_egrp[MAX_LCORE_NUM];

struct spdk_edriven_group *
spdk_edriven_get_reactor_egrp(int lcore_idx)
{
	assert(lcore_idx < MAX_LCORE_NUM);
	return g_edriven_egrp[lcore_idx];
}

int
spdk_edriven_reactor_init(int lcore_idx, spdk_edriven_fn equeue_fn, void *fn_arg, const char *name)
{
	struct spdk_edriven_group *egrp;

	SPDK_INFOLOG(SPDK_LOG_EDRIVEN, "init edriven reactor %s\n", name);

	assert(lcore_idx < MAX_LCORE_NUM);

	egrp = spdk_edriven_group_init(equeue_fn, fn_arg, name);
	assert(egrp);

	g_edriven_egrp[lcore_idx] = egrp;

	return 0;
}

int
spdk_edriven_reactor_fini(int lcore_idx)
{
	struct spdk_edriven_group *egrp = g_edriven_egrp[lcore_idx];
	int rc;

	assert(lcore_idx < MAX_LCORE_NUM);

	SPDK_INFOLOG(SPDK_LOG_EDRIVEN, "fini edriven reactor %s\n", egrp->name);

	rc = spdk_edriven_group_fini(egrp);
	assert(rc == 0);
	(void)rc;

	return 0;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

static int
_edriven_group_process(struct epoll_event *events, int nfds)
{
	struct spdk_edriven_esrc *event_src;
	int n;

	for (n = 0; n < nfds; n++) {
		/* find the edriven_source */
		event_src = events[n].data.ptr;

		/* clear the edge of interval timer or other specific esrc */
		if (event_src->clear_edge || spdk_unlikely(event_src->timer)) {
			uint64_t exp;
			int rc;

			rc = read(event_src->fd, &exp, sizeof(exp));
			assert(rc == sizeof(exp));
			if (rc != sizeof(exp)) {
				SPDK_ERRLOG("edriven: read error");
			}
		}

		/* call the edriven callback */
		event_src->ctx.cb_fn(event_src->ctx.cb_arg);
	}

	return 0;
}

int
spdk_edriven_group_wait(void *edriven_ctx, int timeout)
{
	struct spdk_edriven_group *ectx = edriven_ctx;
	struct epoll_event *events = ectx->events_pool;
	int totalfds = ectx->num_fds;
	int nfds;

	/* dynamically allocate events */
	if (spdk_unlikely(ectx->events_poll_size < totalfds)) {
		events = realloc(events, totalfds * sizeof(struct epoll_event));
		assert(events);
		memset(events, 0, totalfds * sizeof(struct epoll_event));

		ectx->events_poll_size = totalfds;
		ectx->events_pool = events;
	}

	nfds = epoll_wait(ectx->epfd, events, totalfds, timeout);
	if (nfds < 0) {
		if (errno == EINTR) {
			return -errno;
		}

		SPDK_ERRLOG("egrp epoll_wait returns with fail. errno is %d\n", errno);
		return -errno;
	} else if (nfds == 0) {
		/* epoll_wait timeout, will never happens here */
		assert(false);
		return -1;
	}

	_edriven_group_process(events, nfds);

	return 0;
}

static struct spdk_edriven_esrc *
edriven_group_esrc_register(struct spdk_edriven_group *egrp, int efd, int epevent_flag,
			    spdk_edriven_fn fn, void *arg, const char *esrc_name)
{
	struct edriven_callback_ctx *callback;
	struct spdk_edriven_esrc *event_src;
	struct epoll_event epevent;
	int rc;

	SPDK_DEBUGLOG(SPDK_LOG_EDRIVEN, "egroup (%s) registers esrc (%s) with fd %d.\n", egrp->name,
		      esrc_name, efd);

	/* first do parameter checking */
	if (egrp == NULL || efd <= 0 || fn == NULL) {
		errno = EINVAL;
		return NULL;
	}

	/* check if there is one callback registered for the fd */
	TAILQ_FOREACH(event_src, &egrp->edriven_sources, next) {
		if (event_src->fd == efd) {
			SPDK_ERRLOG("egrp (%s) already registered esrc (%s) with fd %d.\n", egrp->name, event_src->name,
				    event_src->fd);
			errno = EEXIST;
			return NULL;
		}
	}

	/* create a new event src */
	event_src = calloc(1, sizeof(*event_src));
	if (event_src == NULL) {
		SPDK_ERRLOG("Can not allocate memory for event src\n");
		errno = ENOMEM;
		return NULL;
	}

	event_src->fd = efd;
	event_src->epevent_flag = epevent_flag;
	snprintf(event_src->name, sizeof(event_src->name), "%s", esrc_name);

	callback = &event_src->ctx;
	callback->cb_fn = fn;
	callback->cb_arg = arg;

	epevent.events = epevent_flag;
	epevent.data.ptr = event_src;

	rc = epoll_ctl(egrp->epfd, EPOLL_CTL_ADD, efd, &epevent);
	assert(rc == 0);
	(void)rc;

	TAILQ_INSERT_TAIL(&egrp->edriven_sources, event_src, next);
	egrp->num_fds++;

	return event_src;
}

// don't foget to record and then close the fd in event_src
static int
edriven_group_esrc_unregister(struct spdk_edriven_group *egrp,
			      struct spdk_edriven_esrc *esrc)
{
	int rc = -1;

	if (egrp == NULL || esrc == NULL) {
		SPDK_ERRLOG("Unregistering with invalid input parameter\n");
		return -EINVAL;
	}

	SPDK_DEBUGLOG(SPDK_LOG_EDRIVEN, "egroup (%s) unregisters esrc (%s) with fd %d.\n", egrp->name,
		      esrc->name, esrc->fd);

	rc = epoll_ctl(egrp->epfd, EPOLL_CTL_DEL, esrc->fd, NULL);
	assert(rc == 0);
	(void)rc;

	// TODO: check if esrc is in egrp */
	TAILQ_REMOVE(&egrp->edriven_sources, esrc, next);
	free(esrc);

	assert(egrp->num_fds > 0);
	egrp->num_fds--;

	return 0;
}

static int
edriven_group_equeue_register(struct spdk_edriven_group *egrp,
			      spdk_edriven_fn equeue_fn, void *fn_arg, const char *eq_name)
{
	struct spdk_edriven_esrc *equeue_esrc;
	int rc = 0;
	int epevent_flag = EPOLLIN | EPOLLPRI;
	int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	assert(efd);

	SPDK_DEBUGLOG(SPDK_LOG_EDRIVEN, "Add equeue esrc (%s) in egrp (%s)\n", eq_name, egrp->name);

	equeue_esrc = edriven_group_esrc_register(egrp, efd, epevent_flag, equeue_fn, fn_arg, eq_name);
	assert(equeue_esrc != NULL);

	egrp->equeue_esrc = equeue_esrc;

	return rc;
}

static struct spdk_edriven_esrc *
spdk_edriven_group_egrp_register(struct spdk_edriven_group *egrp,
				 struct spdk_edriven_group *sub_egrp,
				 spdk_edriven_fn fn, void *arg, const char *esrc_name)
{
	int epevent_flag = EPOLLIN | EPOLLPRI;

	assert(egrp);
	assert(sub_egrp);
	assert(fn);

	SPDK_INFOLOG(SPDK_LOG_EDRIVEN, "Add equeue esrc (%s) in egrp (%s)\n", esrc_name, egrp->name);

	return edriven_group_esrc_register(egrp, sub_egrp->epfd, epevent_flag, fn, arg, esrc_name);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/* for thread.c when create and destroy thread */
int
spdk_edriven_create_thread(struct spdk_thread *thread, const char *name)
{
	struct spdk_edriven_group *thd_egrp;

	SPDK_INFOLOG(SPDK_LOG_EDRIVEN, "Create egrp (%s) for thread (%s)\n", name, thread->name);

	thd_egrp = spdk_edriven_group_init(spdk_thread_msg_queue_edriven, thread, name);
	if (thd_egrp == NULL) {
		return -1;
	}

	thread->thd_egrp = thd_egrp;
	thread->edriven = true;

	return 0;
}

int
spdk_edriven_destroy_thread(struct spdk_thread *thread)
{
	struct spdk_edriven_group *thd_egrp = thread->thd_egrp;
	int rc;

	SPDK_INFOLOG(SPDK_LOG_EDRIVEN, "destroy egrp (%s) for thread (%s)\n", thd_egrp->name, thread->name);

	rc = spdk_edriven_group_fini(thd_egrp);
	assert(rc == 0);

	thread->thd_egrp = NULL;

	return rc;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/* for reactor.c to register and unregister thread epfd into reactor epfd */
int
spdk_edriven_reactor_add_thread(int current_core, struct spdk_thread *thread)
{
	struct spdk_edriven_group *reactor_egrp = g_edriven_egrp[current_core];
	struct spdk_edriven_group *thd_egrp = thread->thd_egrp;
	struct spdk_edriven_esrc *thread_esrc;

	SPDK_INFOLOG(SPDK_LOG_EDRIVEN, "add thread egrp (%s) to reactor (%s)\n", thd_egrp->name,
		     reactor_egrp->name);

	assert(reactor_egrp);
	thread_esrc = spdk_edriven_group_egrp_register(reactor_egrp, thd_egrp, spdk_edriven_thread_main,
			thread, thd_egrp->name);
	assert(thread_esrc);

	thread->thd_esrc = thread_esrc;

	return 0;
}

int
spdk_edriven_reactor_remove_thread(int current_core, struct spdk_thread *thread)
{
	struct spdk_edriven_group *reactor_egrp = g_edriven_egrp[current_core];

	assert(reactor_egrp);
	assert(thread->thd_esrc);

	SPDK_INFOLOG(SPDK_LOG_EDRIVEN, "remove thread egrp (%s) from reactor (%s)\n",
		     thread->thd_egrp->name, reactor_egrp->name);

	edriven_group_esrc_unregister(reactor_egrp, thread->thd_esrc);
	thread->thd_esrc = NULL;

	return 0;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/* for others to register kinds of esrc */

// TODO: now just ignored thread list of pollers and poller state

static inline struct spdk_edriven_esrc *
_edriven_thread_register_general(spdk_poller_fn fn,
				 void *arg, int efd, int epevent_flag, const char *name)
{
	struct spdk_edriven_esrc *event_src;
	struct spdk_thread *thread;

	thread = spdk_get_thread();
	if (!thread) {
		assert(false);
		return NULL;
	}

	if (spdk_unlikely(thread->exit)) {
		SPDK_ERRLOG("thread %s is marked as exited\n", thread->name);
		return NULL;
	}

	assert(efd > 0);
	event_src = edriven_group_esrc_register(thread->thd_egrp, efd, epevent_flag, fn, arg, name);
	assert(event_src != NULL);

	/* edriven esrc shouldn't be inserted into poller tailq,
	 * Since it already linked into egrp's esrc list */
	//spdk_thread_insert_edriven_esrc(thread, event_src);

	return event_src;
}

int
spdk_edriven_thread_unregister_esrc(struct spdk_edriven_esrc **pesrc)
{
	struct spdk_thread *thread;
	struct spdk_edriven_esrc *esrc;
	int rc;
	int efd;

	esrc = *pesrc;
	if (esrc == NULL) {
		return -EINVAL;
	}

	*pesrc = NULL;

	thread = spdk_get_thread();
	if (!thread) {
		assert(false);
		return -EINVAL;
	}

	//spdk_thread_remove_edriven_esrc(thread, esrc);

	efd = esrc->fd;
	bool keepfd = esrc->keepfd;

	rc = edriven_group_esrc_unregister(thread->thd_egrp, esrc);
	assert(rc == 0);

	if (!keepfd) {
		close(efd);
	}

	return rc;
}

struct spdk_edriven_esrc *
spdk_edriven_thread_register_aio(spdk_poller_fn fn,
				 void *arg, const char *name)
{
	struct spdk_edriven_esrc *event_src;
	int epevent_flag = EPOLLIN | EPOLLET;
	int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

	event_src = _edriven_thread_register_general(fn, arg, efd, epevent_flag, name);

	return event_src;
}

int
spdk_edriven_esrc_get_efd(struct spdk_edriven_esrc *esrc)
{
	return esrc->fd;
}

// nbd->spdk_sp_fd is for both datain and dataout
struct spdk_edriven_esrc *
spdk_edriven_thread_register_nbd(spdk_poller_fn fn,
				 void *arg, const char *name, int datafd)
{
	struct spdk_edriven_esrc *event_src;
	int epevent_flag = EPOLLIN | EPOLLOUT | EPOLLET;

	event_src = _edriven_thread_register_general(fn, arg, datafd, epevent_flag, name);
	event_src->keepfd = true;

	return event_src;
}

int
spdk_edriven_thread_nbd_change_trigger(struct spdk_edriven_esrc *event_src, bool edge_trigger)
{
	int rc;
	struct epoll_event epevent;
	int epevent_flag = EPOLLIN | EPOLLOUT;
	struct spdk_thread *thread;

	thread = spdk_get_thread();

	if (edge_trigger) {
		epevent_flag |= EPOLLET;
	}

	epevent.events = epevent_flag;
	epevent.data.ptr = event_src;
	event_src->epevent_flag = epevent_flag;

	rc = epoll_ctl(thread->thd_egrp->epfd, EPOLL_CTL_MOD, event_src->fd, &epevent);
	assert(rc == 0);

	return rc;
}

struct spdk_edriven_esrc *
spdk_edriven_thread_register_vqkick(spdk_poller_fn fn,
				    void *arg, const char *name, int vq_kickfd)
{
	struct spdk_edriven_esrc *event_src;
	//int rc = 0;
	int epevent_flag = EPOLLIN;

	event_src = _edriven_thread_register_general(fn, arg, vq_kickfd, epevent_flag, name);

	event_src->keepfd = true;
	//TODO: for vring kicfd, it also requires read-out to clear the edage.
	event_src->clear_edge = true;

	return event_src;
}


/* register a timerfd to thread epfd.
 * A replacement to spdk_poller_register(,,period_microseconds,);
 */
static int
timerfd_prepare(uint64_t period_microseconds)
{
	int timerfd;
	struct itimerspec new_value;
	struct timespec now;
	uint64_t period_seconds = (period_microseconds) / 1000 / 1000;
	uint64_t period_nanoseconds = period_microseconds * 1000;

	if (period_microseconds == 0) {
		return -EINVAL;
	}

	int ret = clock_gettime(CLOCK_REALTIME, &now);
	assert(ret != -1);

	new_value.it_value.tv_sec = period_seconds;
	new_value.it_value.tv_nsec = period_nanoseconds;

	new_value.it_interval.tv_sec = period_seconds;
	new_value.it_interval.tv_nsec = period_nanoseconds;

	timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	assert(timerfd != -1);

	ret = timerfd_settime(timerfd, 0, &new_value, NULL);
	assert(ret != -1);
	(void)ret;

	return timerfd;
}

struct spdk_edriven_esrc *
spdk_edriven_thread_register_interval_esrc(spdk_poller_fn fn, void *arg,
		uint64_t period_microseconds, const char *name)
{
	struct spdk_edriven_esrc *event_src;
	int epevent_flag = EPOLLIN | EPOLLET;
	int timerfd;

	timerfd = timerfd_prepare(period_microseconds);
	assert(timerfd > 0);

	event_src = _edriven_thread_register_general(fn, arg, timerfd, epevent_flag, name);
	assert(event_src != NULL);
	event_src->timer = true;

	return event_src;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

/* reactor event start */
int
spdk_edriven_reactor_event_notify(int lcore_idx)
{
	struct spdk_edriven_group *reactor_egrp = g_edriven_egrp[lcore_idx];
	int rc;
	uint64_t notify = 1;

	rc = write(reactor_egrp->equeue_esrc->fd, &notify, sizeof(notify));
	assert(rc == sizeof(notify));
	(void)rc;

	return 0;
}

/* read efd of reactor event, so it won't be level triggered */
int
spdk_edriven_reactor_events_level_clear(int lcore_idx)
{
	struct spdk_edriven_group *reactor_egrp = g_edriven_egrp[lcore_idx];
	int rc;
	uint64_t notify = 0;

	rc = read(reactor_egrp->equeue_esrc->fd, &notify, sizeof(notify));
	assert(rc == sizeof(notify));
	(void)rc;

	return 0;
}

int
spdk_edriven_thread_msg_notify(struct spdk_thread *thread)
{
	struct spdk_edriven_group *thd_egrp = thread->thd_egrp;
	uint64_t notify = 1;
	int rc;

	rc = write(thd_egrp->equeue_esrc->fd, &notify, sizeof(notify));
	assert(rc == sizeof(notify));
	(void)rc;

	return 0;
}

int
spdk_edriven_thread_msg_level_clear(struct spdk_thread *thread)
{
	struct spdk_edriven_group *thd_egrp = thread->thd_egrp;
	uint64_t notify = 0;
	int rc;

	rc = read(thd_egrp->equeue_esrc->fd, &notify, sizeof(notify));
	assert(rc == sizeof(notify));
	(void)rc;

	return 0;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

static bool g_app_edriven_mode = false;

void
spdk_set_edriven_mode(void)
{
	SPDK_ERRLOG("Set SPDK running in edriven mode.");
	SPDK_INFOLOG(SPDK_LOG_EDRIVEN, "Set SPDK running in edriven mode.");
	g_app_edriven_mode = true;
}

bool
spdk_is_edriven_mode(void)
{
	return g_app_edriven_mode;
}


SPDK_LOG_REGISTER_COMPONENT("edriven", SPDK_LOG_EDRIVEN)
