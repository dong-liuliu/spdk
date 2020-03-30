

#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>

#include "spdk/stdinc.h"
#include "spdk/likely.h"

#include "spdk_internal/event.h"
#include "spdk_internal/log.h"
#include "spdk_internal/thread.h"

#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/env.h"
#include "spdk/util.h"

#include "spdk/queue.h"

#include "edriven.h"

/** Function to be registered for the specific interrupt */
typedef void (*reactor_edriven_callback_fn)(void *cb_arg1, void *cb_arg2);

/**
 * Function to call after a callback is unregistered.
 * Can be used to close fd and free cb_arg.
 */
typedef void (*reactor_edriven_unregister_callback_fn)(struct rte_edriven_handle *edriven_handle,
						void *cb_arg);

struct reactor_edriven_callback {
	TAILQ_ENTRY(reactor_edriven_callback) next;
	reactor_edriven_callback_fn cb_fn;  /**< callback address */
	void *cb_arg1, *cb_arg2;                /**< parameter for callback */
	//uint8_t pending_delete;      /**< delete after callback is called */
	//reactor_edriven_unregister_callback_fn ucb_fn; /**< fn to call before cb is deleted */
};

TAILQ_HEAD(reactor_edriven_cb_list, reactor_edriven_callback);

struct reactor_edriven_source {
	TAILQ_ENTRY(reactor_edriven_source) next;
	struct reactor_edriven_cb_list callbacks;  /**< user callbacks */
	int fd;	 /**< interrupt event file descriptor */
	int epevent_flag;
	//uint32_t active;
};

TAILQ_HEAD(reactor_edriven_source_list, reactor_edriven_source);

struct reactor_edriven_ctx {
	int epfd;
	int num_fds;

	/* Notification to process event_ring
	 * For each spdk_event_call, write 1 into this eventfd,
	 * After triggered, and event_queue_run_batch may not process all events,
	 * so event_queue_run_batch should write 1 into eventfd if event_ring
	 * is not empty, in order to get remained events processed in next loop.
	 */
	int eventfd;

	/* interrupt sources list */
	struct reactor_edriven_source_list edriven_sources;
};

static int reactor_edriven_callback_register(struct reactor_edriven_ctx *ectx,
		int efd, int epevent_flag,
		reactor_edriven_callback_fn cb, void *cb_arg1, void *cb_arg2)

static int reactor_edriven_ctx_add_eventfd(struct reactor_edriven_ctx *ectx,
		reactor_edriven_callback_fn cb, void *cb_arg1, void *cb_arg2)

#define MAX_LCORE_NUM	64
static struct reactor_edriven_ctx g_edriven_ctx[MAX_LCORE_NUM];

/* reactor init start */
//TODO: add event fd retrigger check
extern uint32_t _spdk_event_queue_run_batch(struct spdk_reactor *reactor);

static int
_reactor_event_callback(void *cb_arg1, void *cb_arg2)
{
	struct spdk_reactor *reactor = cb_arg1;
	uint32_t count;

	count = _spdk_event_queue_run_batch(reactor);

	return count;
}

static int
reactor_edriven_init(int lcore_idx)
{
	struct reactor_edriven_ctx *reactor_ectx = &g_edriven_ctx[lcore_idx];
	struct reactor_edriven_source_list *edriven_sources = &reactor_ectx->edriven_sources;
	int rc;

	/* init the global interrupt source head */
	TAILQ_INIT(edriven_sources);

	reactor_ectx->epfd = epoll_create1(EPOLL_CLOEXEC);
	assert(reactor_ectx->epfd);

	reactor_ectx->num_fds = 0;

	struct spdk_reactor *reactor = spdk_reactor_get(lcore_idx);

	rc = reactor_edriven_ctx_add_eventfd(reactor_ectx, _reactor_event_callback, reactor, NULL);
	assert(rc);

	return rc;
}

int
spdk_reactors_edriven_init(int num_lcores)
{
	int i, rc;

	for (i = 0; i < num_lcores; i++) {
		rc = reactor_edriven_init(i);
		assert(rc == 0);
	}

	return 0;
}
/* reactor init end */

//TODO: add event fd retrigger check
extern uint32_t _spdk_msg_queue_run_batch(struct spdk_thread *thread, uint32_t max_msgs);

// TODO: place this func to thread.c
/* process thread msg and critical msg */
int spdk_thread_msg_queue_edriven(struct spdk_thread *thread)
{
	struct spdk_thread *orig_thread;
	int rc;
	uint32_t msg_count;
	spdk_msg_fn critical_msg;
	int rc = 0;
	uint64_t now = spdk_get_ticks();

	orig_thread = _get_thread();
	tls_thread = thread;

	critical_msg = thread->critical_msg;
	if (spdk_unlikely(critical_msg != NULL)) {
		critical_msg(NULL);
		thread->critical_msg = NULL;
	}

	msg_count = _spdk_msg_queue_run_batch(thread, 0);
	if (msg_count) {
		rc = 1;
	}

	_spdk_thread_update_stats(thread, now, rc);

	tls_thread = orig_thread;

	return rc;
}

static int
_thread_msg_callback(void *cb_arg1, void *cb_arg2)
{
	struct spdk_lw_thread *lw_thread = cb_arg1;
	struct spdk_thread *thread = spdk_thread_get_from_ctx(lw_thread);

	return spdk_thread_msg_queue_edriven(thread);
}

int
spdk_reactor_edriven_schedule_thread(uint32_t current_core, struct spdk_lw_thread *lw_thread)
{
	struct reactor_edriven_ctx *thd_ectx;
	struct reactor_edriven_source_list *edriven_sources;
	struct spdk_thread *thread = spdk_thread_get_from_ctx(lw_thread);
	int rc;

	thd_ectx = calloc(1, sizeof(*thd_ectx));
	assert(thd_ectx);
	edriven_sources = &thd_ectx->edriven_sources;
	TAILQ_INIT(edriven_sources);

	thread->thd_ectx = thd_ectx;

	thd_ectx->num_fds = 0;
	thd_ectx->epfd = epoll_create1(EPOLL_CLOEXEC);
	assert(thd_ectx->epfd);

	/* register efd for msg queue to thread epfd */
	rc = reactor_edriven_ctx_add_eventfd(thd_ectx, _thread_msg_callback, lw_thread, NULL);
	assert(rc);

	/* register thread epfd into reactor epfd */
	{
		struct reactor_edriven_ctx *reactor_ectx = &g_edriven_ctx[current_core];

		// TODO: what is epevent for thread epfd
		int epevent_flag = EPOLLIN | EPOLLPRI;
		int efd = thd_ectx->epfd;

		rc = reactor_edriven_callback_register(reactor_ectx, efd, epevent_flag,
				_reactor_edriven_thread_main, thd_ectx, NULL);
		assert(rc == 0);
	}

	return rc;
}


/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

static int
reactor_edriven_callback_register(struct reactor_edriven_ctx *ectx,
		int efd, int epevent_flag,
		reactor_edriven_callback_fn cb, void *cb_arg1, void *cb_arg2)
{
	int rc;
	struct reactor_edriven_callback *callback;
	struct reactor_edriven_source *event_src;
	struct epoll_event epevent;

	/* first do parameter checking */
	if (ectx == NULL || efd < 0 || cb == NULL) {
		return -EINVAL;
	}

	/* allocate a new interrupt callback entity */
	callback = calloc(1, sizeof(*callback));
	if (callback == NULL) {
		SPDK_ERRLOG("Can not allocate memory\n");
		return -ENOMEM;
	}
	callback->cb_fn = cb;
	callback->cb_arg1 = cb_arg1;
	callback->cb_arg2 = cb_arg2;
	//callback->pending_delete = 0;
	//callback->ucb_fn = NULL;

	/* check if there is at least one callback registered for the fd */
	TAILQ_FOREACH(event_src, &ectx->edriven_sources, next) {
		if (event_src->fd == efd) {
			assert(event_src->epevent_flag == epevent_flag);

			TAILQ_INSERT_TAIL(&(event_src->callbacks), callback, next);
			rc = 0;
			break;
		}
	}

	/* no existing callbacks for this - add new source */
	if (event_src == NULL) {
		event_src = calloc(1, sizeof(*event_src));
		if (event_src == NULL) {
			SPDK_ERRLOG("Can not allocate memory\n");
			free(callback);
			rc = -ENOMEM;
		}

		event_src->fd= efd;
		event_src->epevent_flag = epevent_flag;
		TAILQ_INIT(&event_src->callbacks);
		TAILQ_INSERT_TAIL(&(event_src->callbacks), callback, next);

		epevent.events = epevent_flag;
		epevent.data.ptr = event_src;
		rc = epoll_ctl(ectx->epfd, EPOLL_CTL_ADD, efd, &epevent);
		assert(rc == 0);

		TAILQ_INSERT_TAIL(&ectx->edriven_sources, src, next);
		ectx->num_fds++;
	}

	return rc;
}

static int
reactor_edriven_ctx_add_eventfd(struct reactor_edriven_ctx *ectx,
		reactor_edriven_callback_fn cb, void *cb_arg1, void *cb_arg2)
{
	int rc;
	int epevent_flag = EPOLLIN | EPOLLPRI;
	int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	assert(efd);

	ectx->eventfd = efd;

	rc = reactor_edriven_callback_register(ectx, efd, epevent_flag, cb, cb_arg1, cb_arg2);
	assert(rc == 0);

	return rc;
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

static int
_reactor_edriven_process(struct epoll_event *events, int nfds)
{
	struct reactor_edriven_source *event_src;
	struct reactor_edriven_callback *callback, *next;
	int n;

	for (n = 0; n < nfds; n++) {
		/* find the edriven_source */
		event_src = events->data.ptr;

		/* call the edriven callbacks */
		TAILQ_FOREACH(callback, &event_src->callbacks, next) {
			callback->cb_fn(callback->cb_arg1, callback->cb_arg2);
		}
	}

	return 0;
}

static inline int
_reactor_edriven_epoll_wait(struct reactor_edriven_ctx *ectx, int timeout)
{
	struct epoll_event *events;
	int totalfds = 0;
	int nfds;

	/* dynamically allocate events */
	if (totalfds != ectx->num_fds) {
		totalfds = ectx->num_fds;
		events = realloc(events, totalfds * sizeof(struct epoll_event));
		assert(events);

		memset(events, 0, totalfds * sizeof(struct epoll_event));
	}

	/* epfd of thread ectx should not be blocked */
	nfds = epoll_wait(ectx->epfd, events, totalfds, 0);
	if (nfds < 0) {
		if (errno == EINTR)
			continue;

		SPDK_ERRLOG("reactor epoll_wait returns with fail. errno is %d\n", errno);
		return -errno;
	} else if (nfds == 0) {
		/* epoll_wait timeout, will never happens here */
		continue;
	}

	_reactor_edriven_process(events, nfds);

	return 0;
}


// replacement for part of reactor_run and spdk_thread_poll
static int
_reactor_edriven_thread_main(void *cb_arg1, void *cb_arg2)
{
#define _EPOLL_NOWAIT (0)
	int nonblock_timeout = _EPOLL_NOWAIT;
	struct reactor_edriven_ctx *thd_ectx = cb_arg1;

	struct spdk_lw_thread *lw_thread = cb_arg1;
	struct spdk_thread *thread = spdk_thread_get_from_ctx(lw_thread);
	struct spdk_thread *orig_thread;
	uint32_t msg_count;
	spdk_msg_fn critical_msg;
	int rc = 0;
	uint64_t now = spdk_get_ticks();

	orig_thread = _get_thread();
	tls_thread = thread;

	rc = _reactor_edriven_epoll_wait(thd_ectx, nonblock_timeout);

	_spdk_thread_update_stats(thread, now, rc);

	tls_thread = orig_thread;

	return rc;
}

int
spdk_reactor_edriven_mainloop(struct spdk_reactor *reactor)
{
	struct reactor_edriven_ctx *reactor_ctx = g_edriven_ctx[reactor->lcore];
#define _EPOLL_WAIT_FOREVER (-1)
	int nonblock_timeout = _EPOLL_WAIT_FOREVER;

	while(1) {
		_reactor_edriven_epoll_wait(reactor_ctx, nonblock_timeout);
	}


	/**
	 * when we return, we need to rebuild the
	 * list of fds to monitor.
	 */
	//close(pfd);

	return 0;
}

/* reactor event start */
int
spdk_reactor_event_notify(int lcore_idx)
{
	struct reactor_edriven_ctx *reactor_ectx = &g_edriven_ctx[lcore_idx];
	int rc;
	uint64_t notify = 1;

	rc = write(reactor_ectx->eventfd, &notify, sizeof(notify));
	assert(rc == sizeof(notify));

	return 0;
}

int
spdk_reactor_events_takeout()
{

}

int
spdk_thread_msg_notify(struct spdk_thread *thread)
{
	struct reactor_edriven_ctx *thd_ectx = thread->thd_ectx;
	uint64_t notify = 1;
	int rc;

	rc = write(thd_ectx->eventfd, &notify, sizeof(notify));
	assert(rc == sizeof(notify));

	return 0;
}

int
spdk_thread_msgs_takeout()
{

}

/* reactor event end */


static int
timerfd_prepare(uint64_t period_microseconds)
{
	int fd;
	struct itimerspec new_value;
    struct timespec now;

    int ret = clock_gettime(CLOCK_REALTIME, &now);
    assert(ret != -1);

    new_value.it_value.tv_sec = period_microseconds * 1000;
    new_value.it_value.tv_nsec = now.tv_nsec;

    new_value.it_interval.tv_sec = period_microseconds * 1000;
    new_value.it_interval.tv_nsec = 0;

	fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK );
	assert(fd != -1);

	ret = timerfd_settime(fd, 0, &new_value, NULL);
	assert(ret != -1);

	return fd;
}

static int
_poller_fn_to_edriven_cb(void *cb_arg1, void *cb_arg2)
{
	spdk_poller_fn fn = cb_arg1;

	return fn(cb_arg2);
}

int
spdk_thread_edriven_interval_register(struct spdk_thread *thread,
		spdk_poller_fn fn, void *arg,
	      uint64_t period_microseconds,
	      const char *name)
{
	int efd;
	int rc;
	int epevent_flag = EPOLLIN | EPOLLET;
	struct reactor_edriven_ctx *thd_ectx = thread->thd_ectx;

	if (period_microseconds == 0) {
		return -EINVAL;
	}

	efd = timerfd_prepare(period_microseconds);
	assert(efd > 0);

	rc = reactor_edriven_callback_register(thd_ectx, efd, epevent_flag,
						_poller_fn_to_edriven_cb, fn, arg);
	assert(rc == 0);

	return rc;
}

/* aio related operations
 * 1. create a eventfd for aio poll group, add add it to epfd.
 * 2. in add io_set_eventfd(&iocbp->iocb, efd); io_set_callback(&iocbp->iocb, aio_callback);
 * 3. if epoll_wait return efd event, then call *bdev_aio_group_poll for next operations.
 * */
int
spdk_thread_edriven_event_register(struct spdk_thread *thread,
		spdk_poller_fn fn, void *arg, const char *name)
{
	int efd;
	int rc;
	int epevent_flag = EPOLLIN | EPOLLET;
	struct reactor_edriven_ctx *thd_ectx = thread->thd_ectx;

	efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	assert(efd > 0);

	rc = reactor_edriven_callback_register(thd_ectx, efd, epevent_flag,
						_poller_fn_to_edriven_cb, fn, arg);
	assert(rc == 0);

	return rc;
	//reactor_thread_fd_callback_register(epfd, epevent, bdev_aio_group_poll, arg);
	//return event_src;
}




