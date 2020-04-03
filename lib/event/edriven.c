

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

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
extern int spdk_reactor_event_callback(void *cb_arg);
extern int spdk_thread_msg_queue_edriven(void *cb_arg);
extern void spdk_thread_insert_edriven(struct spdk_thread *thread, struct spdk_edriven_event_source *event_src);
extern void spdk_thread_remove_edriven(struct spdk_thread *thread, struct spdk_edriven_event_source *event_src);


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/** Function to be registered for the specific interrupt */
typedef void (*reactor_edriven_callback_fn)(void *cb_arg1, void *cb_arg2);

struct reactor_edriven_callback {
	TAILQ_ENTRY(reactor_edriven_callback) next;
	reactor_edriven_callback_fn cb_fn;  /**< callback address */
	void *cb_arg;                /**< parameter for callback */
};

TAILQ_HEAD(reactor_edriven_cb_list, reactor_edriven_callback);

struct spdk_edriven_event_source {
	TAILQ_ENTRY(spdk_edriven_event_source) next;
	struct reactor_edriven_cb_list callbacks;  /**< user callbacks */
	int fd;	 /**< interrupt event file descriptor */
	int epevent_flag;
	//uint32_t active;
};

TAILQ_HEAD(spdk_edriven_event_source_list, spdk_edriven_event_source);

struct reactor_edriven_ctx {
	int epfd;
	int num_fds;

	/* Notification to process event_ring
	 * For each spdk_event_call, write 1 into this eventfd,
	 * After triggered, and event_queue_run_batch may not process all events,
	 * so event_queue_run_batch should write 1 into eventfd if event_ring
	 * is not empty, in order to get remained events processed in next loop.
	 */
	struct spdk_edriven_event_source *event_src;

	/* interrupt sources list */
	struct spdk_edriven_event_source_list edriven_sources;
};

#define MAX_LCORE_NUM	64
static struct reactor_edriven_ctx g_edriven_ctx[MAX_LCORE_NUM];

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/

#define NEW_EDRIVEN
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
static int edriven_ctx_add_eventfd(struct reactor_edriven_ctx *ectx,
		reactor_edriven_callback_fn cb, void *cb_arg);

static int
reactor_edriven_init(int lcore_idx)
{
	struct spdk_reactor *reactor = spdk_reactor_get(lcore_idx);
	struct reactor_edriven_ctx *reactor_ectx = &g_edriven_ctx[lcore_idx];
	struct spdk_edriven_event_source_list *edriven_sources;
	int rc;

	/* init the global interrupt source head */
	edriven_sources = &reactor_ectx->edriven_sources;
	TAILQ_INIT(edriven_sources);

	reactor_ectx->num_fds = 0;
	reactor_ectx->epfd = epoll_create1(EPOLL_CLOEXEC);
	assert(reactor_ectx->epfd);

	rc = edriven_ctx_add_eventfd(reactor_ectx, spdk_reactor_event_callback, reactor);
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

static int
reactor_edriven_fini(int lcore_idx)
{
	struct reactor_edriven_ctx *reactor_ectx = &g_edriven_ctx[lcore_idx];

	/* register efd for msg queue to thread epfd */
	int rc = edriven_callback_unregister(reactor_ectx, reactor_ectx->event_src, _reactor_event_callback);
	assert(rc == 0);

	close(reactor_ectx->epfd);

	return 0;
}

int
spdk_reactors_edriven_fini(int num_lcores)
{
	int i, rc;

	for (i = 0; i < num_lcores; i++) {
		rc = reactor_edriven_fini(i);
		assert(rc == 0);
	}

	return 0;
}

/*~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~*/
static int
_reactor_edriven_process(struct epoll_event *events, int nfds)
{
	struct spdk_edriven_event_source *event_src;
	struct reactor_edriven_callback *callback, *next;
	int n;

	for (n = 0; n < nfds; n++) {
		/* find the edriven_source */
		event_src = events->data.ptr;

		/* call the edriven callbacks */
		TAILQ_FOREACH(callback, &event_src->callbacks, next) {
			callback->cb_fn(callback->cb_arg);
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
_reactor_edriven_thread_main(void *cb_arg)
{
	int nonblock_timeout = 0 // _EPOLL_NOWAIT;
	struct reactor_edriven_ctx *thd_ectx = cb_arg;

	struct spdk_lw_thread *lw_thread = cb_arg;
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
spdk_reactor_edriven_main(struct spdk_reactor *reactor)
{
	struct reactor_edriven_ctx *reactor_ctx = g_edriven_ctx[reactor->lcore];
	int nonblock_timeout = -1; //_EPOLL_WAIT_FOREVER;

	_reactor_edriven_epoll_wait(reactor_ctx, nonblock_timeout);

	return 0;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

static struct spdk_edriven_event_source *
edriven_callback_register(struct reactor_edriven_ctx *ectx, int efd, int epevent_flag,
		reactor_edriven_callback_fn fn, void *arg, const char *name)
{
	struct reactor_edriven_callback *callback;
	struct spdk_edriven_event_source *event_src;
	struct epoll_event epevent;
	int rc;

	/* first do parameter checking */
	if (ectx == NULL || efd < 0 || cb == NULL) {
		errno = EINVAL;
		return NULL;
	}

	/* allocate a new interrupt callback entity */
	callback = calloc(1, sizeof(*callback));
	if (callback == NULL) {
		SPDK_ERRLOG("Can not allocate memory\n");
		errno = ENOMEM;
		return NULL;
	}
	callback->cb_fn = fn;
	callback->cb_arg = arg;
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
			errno = ENOMEM;
			return NULL;
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

	return event_src;
}

/* unregister a callback_fn from event_src, if fn is NULL or
 * event_src has no callback_fn after unregister, then remove the event_src from edriven_ctx*/
// don't foget to record and then close the fd in event_src
static int
edriven_callback_unregister(struct reactor_edriven_ctx *ectx,
		struct spdk_edriven_event_source *event_src, reactor_edriven_callback_fn fn)
{
	struct reactor_edriven_callback *cb, *next;
	int ret = -1;

	if (ectx == NULL || event_src == NULL || event_src->fd <=0) {
		SPDK_ERRLOG("Unregistering with invalid input parameter\n");
		return -EINVAL;
	}

	/*walk through the callbacks and remove all that match. */
	for (cb = TAILQ_FIRST(&event_src->callbacks); cb != NULL; cb = next) {

		next = TAILQ_NEXT(cb, next);

		if (fn == NULL || cb->cb_fn == fn) {
			TAILQ_REMOVE(&event_src->callbacks, cb, next);
			free(cb);
			ret++;
		}
	}

	/* all callbacks for that source are removed. */
	if (TAILQ_EMPTY(&event_src->callbacks)) {
		int rc = epoll_ctl(ectx->epfd, EPOLL_CTL_DEL, event_src->fd, NULL);
		assert(rc==0);

		TAILQ_REMOVE(&ectx->edriven_sources, event_src, next);
		free(event_src);
	}

	return ret;
}

static int
edriven_ctx_add_eventfd(struct reactor_edriven_ctx *ectx,
		reactor_edriven_callback_fn cb, void *cb_arg)
{
	struct spdk_edriven_event_source *event_src;
	int rc = 0;
	int epevent_flag = EPOLLIN | EPOLLPRI;
	int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	assert(efd);

	event_src = edriven_callback_register(ectx, efd, epevent_flag, cb, cb_arg);
	assert(event_src != NULL);

	ectx->event_src = event_src;

	return rc;
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

int
spdk_reactor_edriven_create_thread(struct spdk_thread *thread)
{
	struct reactor_edriven_ctx *thd_ectx;
	struct spdk_edriven_event_source_list *edriven_sources;
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
	rc = edriven_ctx_add_eventfd(thd_ectx, spdk_thread_msg_queue_edriven, thread);
	assert(rc);

	return rc;
}

/* register thread epfd into reactor epfd */
int
spdk_reactor_edriven_add_thread(uint32_t current_core, struct spdk_thread *thread)
{
	struct reactor_edriven_ctx *reactor_ectx = &g_edriven_ctx[current_core];
	struct reactor_edriven_ctx *thd_ectx = thread->thd_ectx;
	struct spdk_edriven_event_source *thread_event_src;

	// TODO: what is epevent for thread epfd
	int epevent_flag = EPOLLIN | EPOLLPRI;
	int efd = thd_ectx->epfd;

	thread_event_src = edriven_callback_register(reactor_ectx, efd, epevent_flag,
			_reactor_edriven_thread_main, thd_ectx);
	assert(thread_event_src == 0);

	thread->thd_event_src = thread_event_src;

	return 0;
}

int
spdk_reactor_edriven_remove_thread(uint32_t current_core, struct spdk_thread *thread)
{
	struct reactor_edriven_ctx *reactor_ectx = &g_edriven_ctx[current_core];
	struct spdk_edriven_event_source *thread_event_src = thread->thd_event_src;
	int rc;

	rc = edriven_callback_unregister(reactor_ectx, thread_event_src, _reactor_edriven_thread_main);

	return rc;
}

int spdk_reactor_edriven_destroy_thread(struct spdk_thread *thread)
{
	struct reactor_edriven_ctx *thd_ectx = thread->thd_ectx;

	/* register efd for msg queue to thread epfd */
	int rc = edriven_callback_unregister(thd_ectx, thd_ectx->event_src, spdk_thread_msg_queue_edriven);
	assert(rc == 0);

	//TODO: close efd
	close(thd_ectx->epfd);
	free(thd_ectx);

	return 0;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

int
spdk_edriven_source_get_efd(struct spdk_edriven_event_source *esrc)
{
	return esrc->fd;
}

// ignore thread list of pollers and poller state
struct spdk_edriven_event_source *
spdk_thread_edriven_register(spdk_poller_fn fn,
		     void *arg, const char *name)
{
	struct spdk_edriven_event_source *event_src;
	int rc = 0;
	int epevent_flag = EPOLLIN | EPOLLET;
	int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
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
	event_src = edriven_callback_register(thread->thd_ectx, efd, epevent_flag, fn, arg);
	assert(event_src != NULL);

	spdk_thread_insert_edriven(thread, event_src);

	return event_src;
}

int spdk_thread_edriven_unregister(struct spdk_edriven_event_source **pesrc)
{
		struct spdk_thread *thread;
		struct spdk_edriven_event_source *esrc;
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


		spdk_thread_remove_edriven(thread, esrc);

		efd = esrc->fd;
		rc = edriven_callback_unregister(thread->thd_ectx, esrc, NULL);
		assert(rc == 0);

		close(efd);

		return rc;
}

/* register a timerfd to thread epfd.
 * A replacement to spdk_poller_register(,,period_microseconds,);
 */
static int
timerfd_prepare(uint64_t period_microseconds)
{
	int fd;
	struct itimerspec new_value;
    struct timespec now;

	if (period_microseconds == 0) {
		return -EINVAL;
	}

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

struct spdk_edriven_event_source *spdk_thread_edriven_interval_register(spdk_poller_fn fn, void *arg,
	      uint64_t period_microseconds, const char *name)
{
	struct spdk_edriven_event_source *event_src;
	int rc = 0;
	int epevent_flag = EPOLLIN | EPOLLET;
	int efd;
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

	efd = timerfd_prepare(period_microseconds);
	assert(efd > 0);

	event_src = edriven_callback_register(thread->thd_ectx, efd, epevent_flag, fn, arg);
	assert(event_src != NULL);

	spdk_thread_insert_edriven(thread, event_src);

	return event_src;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

// TODO: place this func to thread.c

/* reactor event start */
int
spdk_reactor_event_notify(int lcore_idx)
{
	struct reactor_edriven_ctx *reactor_ectx = &g_edriven_ctx[lcore_idx];
	int rc;
	uint64_t notify = 1;

	rc = write(reactor_ectx->event_src->fd, &notify, sizeof(notify));
	assert(rc == sizeof(notify));

	return 0;
}

/* read efd of reactor event, so it won't be level triggered */
int
spdk_reactor_events_level_clear(int lcore_idx)
{
	struct reactor_edriven_ctx *reactor_ectx = &g_edriven_ctx[lcore_idx];
	int rc;
	uint64_t notify = 0;

	rc = read(reactor_ectx->event_src->fd, &notify, sizeof(notify));
	assert(rc == sizeof(notify));

	return 0;
}

int
spdk_thread_msg_notify(struct spdk_thread *thread)
{
	struct reactor_edriven_ctx *thd_ectx = thread->thd_ectx;
	uint64_t notify = 1;
	int rc;

	rc = write(thd_ectx->event_src->fd, &notify, sizeof(notify));
	assert(rc == sizeof(notify));

	return 0;
}

int
spdk_thread_msg_level_clear(struct spdk_thread *thread)
{
	struct reactor_edriven_ctx *thd_ectx = thread->thd_ectx;
	uint64_t notify = 0;
	int rc;

	rc = read(thd_ectx->event_src->fd, &notify, sizeof(notify));
	assert(rc == sizeof(notify));

	return 0;
}

int
spdk_thread_msgs_takeout()
{
	return 0;
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~



