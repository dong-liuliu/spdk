/*-
 *   BSD LICENSE
 *
 *   Copyright(c) Intel Corporation. All rights reserved.
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

#include <linux/virtio_blk.h>

#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/conf.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/vhost.h"

#include "vhost_internal.h"
#include "vhost_fs_internal.h"

/* forward declaration */
static const struct spdk_vhost_dev_backend vhost_fs_device_backend;

static int
process_fs_request(struct spdk_vhost_fs_task *task,
		    struct spdk_vhost_fs_session *fvsession,
		    struct spdk_vhost_virtqueue *vq);


static void
invalid_fs_request(struct spdk_vhost_fs_task *task, uint8_t status)
{
	if (task->status) {
		*task->status = status;
	}

	spdk_vhost_vq_used_ring_enqueue(&task->fvsession->vsession, task->vq, task->req_idx,
					task->used_len);
	fs_task_finish(task);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS_DATA, "Invalid request (status=%" PRIu8")\n", status);
}

//static void
//fs_request_finish(bool success, struct spdk_vhost_fs_task *task)
//{
//	*task->status = success ? VIRTIO_BLK_S_OK : VIRTIO_BLK_S_IOERR;
//	spdk_vhost_vq_used_ring_enqueue(&task->fvsession->vsession, task->vq, task->req_idx,
//					task->used_len);
//	SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK, "Finished task (%p) req_idx=%d\n status: %s\n", task,
//		      task->req_idx, success ? "OK" : "FAIL");
//	fs_task_finish(task);
//}

/*
 * Process task's descriptor chain and setup data related fields.
 */
static int
fs_task_iovs_setup(struct spdk_vhost_fs_task *task, struct spdk_vhost_virtqueue *vq,
	       uint16_t req_idx, uint32_t *length)
{
	struct spdk_vhost_fs_session *fvsession = task->fvsession;
	struct spdk_vhost_session *vsession = &fvsession->vsession;
	struct spdk_vhost_dev *vdev = vsession->vdev;
	struct vring_desc *desc, *desc_table;
	uint32_t desc_table_size, len = 0;
	uint32_t desc_handled_cnt;
	int rc;

	rc = spdk_vhost_vq_get_desc(vsession, vq, req_idx, &desc, &desc_table, &desc_table_size);
	if (rc != 0) {
		SPDK_ERRLOG("%s: Invalid descriptor at index %"PRIu16".\n", vdev->name, req_idx);
		return -1;
	}

	desc_handled_cnt = 0;
	while (1) {
		struct iovec *iovs;
		uint16_t *cnt;

		if (!spdk_vhost_vring_desc_is_wr(desc)) {
			iovs = task->out_iovs;
			cnt = &task->out_iovcnt;
		} else {
			iovs = task->in_iovs;
			cnt = &task->in_iovcnt;
		}

		/*
		 * Maximum cnt reached?
		 * Should not happen if request is well formatted, otherwise this is a BUG.
		 */
		if (spdk_unlikely(*cnt == SPDK_COUNTOF(task->in_iovs))) {
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "Max IOVs in request reached (req_idx = %"PRIu16").\n",
				      req_idx);
			return -1;
		}

		if (spdk_unlikely(spdk_vhost_vring_desc_to_iov(vsession, iovs, cnt, desc))) {
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "Invalid descriptor %" PRIu16" (req_idx = %"PRIu16").\n",
				      req_idx, *cnt);
			return -1;
		}

		len += desc->len;

		rc = spdk_vhost_vring_desc_get_next(&desc, desc_table, desc_table_size);
		if (rc != 0) {
			SPDK_ERRLOG("%s: Descriptor chain at index %"PRIu16" terminated unexpectedly.\n",
				    vdev->name, req_idx);
			return -1;
		} else if (desc == NULL) {
			break;
		}

		desc_handled_cnt++;
		if (spdk_unlikely(desc_handled_cnt > desc_table_size)) {
			/* Break a cycle and report an error, if any. */
			SPDK_ERRLOG("%s: found a cycle in the descriptor chain: desc_table_size = %d, desc_handled_cnt = %d.\n",
				    vdev->name, desc_table_size, desc_handled_cnt);
			return -1;
		}
	}

	/*
	 * There must be least two descriptors.
	 * First contain request so it must be readable.
	 */
	if (spdk_unlikely(task->out_iovcnt == 0)) {
		return -1;
	}

	*length = len;
	return 0;
}

//static void
//fs_request_complete_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
//{
//	struct spdk_vhost_blk_task *task = cb_arg;
//
//	spdk_bdev_free_io(bdev_io);
//	fs_request_finish(success, task);
//}


static int
process_fs_request(struct spdk_vhost_fs_task *task,
		    struct spdk_vhost_fs_session *fvsession,
		    struct spdk_vhost_virtqueue *vq)
{
//	struct spdk_vhost_fs_dev *fvdev = fvsession->fvdev;
//	const struct virtio_blk_outhdr *req;
	struct fuse_in_header *fuse_in;
	struct iovec *iov;
//	uint32_t type;
	uint32_t payload_len;
//	uint64_t flush_bytes;
	int rc;

	if (fs_task_iovs_setup(task, vq, task->req_idx, &payload_len)) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "Invalid request (req_idx = %"PRIu16").\n", task->req_idx);

		invalid_fs_request(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	iov = &task->out_iovs[0];
	if (spdk_unlikely(iov->iov_len != sizeof(struct fuse_in_header))) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS,
			      "First descriptor size is %zu but expected %zu (req_idx = %"PRIu16").\n",
			      iov->iov_len, sizeof(*fuse_in), task->req_idx);
//		assert(false);
//		invalid_fs_request(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	fuse_in = iov->iov_base;

	if (task->in_iovcnt > 0) {
		iov = &task->in_iovs[0];
		if (spdk_unlikely(iov->iov_len != sizeof(struct fuse_out_header))) {
			SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS,
				      "Last descriptor size is %zu but expected %d (req_idx = %"PRIu16").\n",
				      iov->iov_len, 1, task->req_idx);
//			invalid_fs_request(task, VIRTIO_BLK_S_UNSUPP);
//			return -1;
		}
	}

	task->status = iov->iov_base;
//	payload_len -= sizeof(*req) + sizeof(*task->status);
//	task->iovcnt -= 2;

//	switch (fuse_in) {
//	// TODO: preprocess
//	case FUSE_NOTIFY_REPLY:
//		break;
//	default:
//
//	}
//
//	if (in->opcode == ) {
//		// TODO: preprocess
//	}

	task->unique = fuse_in->unique;
//	task-> = fuse_in->pid;

	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "Send request type '%"PRIu32"'.\n", fuse_in->opcode);

	char *argin = task->out_iovs[1].iov_base;
	if (task->out_iovs[1].iov_len > sizeof(struct fuse_in_header)) {
		argin = task->out_iovs[0].iov_base + sizeof(struct fuse_in_header);
	}
	rc = spdk_fuse_ll_ops[fuse_in->opcode].func(task, fuse_in->nodeid, argin);
	if (rc != 0) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "Not supported request type '%"PRIu32"'.\n", fuse_in->opcode);
//		invalid_fs_request(task, VIRTIO_BLK_S_UNSUPP);
		return -1;
	}

	return 0;
}

static void
process_vq(struct spdk_vhost_fs_session *fvsession, struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_fs_dev *fvdev = fvsession->fvdev;
	struct spdk_vhost_fs_task *task;
	struct spdk_vhost_session *vsession = &fvsession->vsession;
	int rc;
	uint16_t reqs[32];
	uint16_t reqs_cnt, i;

	reqs_cnt = spdk_vhost_vq_avail_ring_get(vq, reqs, SPDK_COUNTOF(reqs));
	if (!reqs_cnt) {
		return;
	}

	for (i = 0; i < reqs_cnt; i++) {
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "====== Starting processing request idx %"PRIu16"======\n",
			      reqs[i]);

		if (spdk_unlikely(reqs[i] >= vq->vring.size)) {
			SPDK_ERRLOG("%s: request idx '%"PRIu16"' exceeds virtqueue size (%"PRIu16").\n",
				    fvdev->vdev.name, reqs[i], vq->vring.size);
			spdk_vhost_vq_used_ring_enqueue(vsession, vq, reqs[i], 0);
			continue;
		}

		task = &((struct spdk_vhost_fs_task *)vq->tasks)[reqs[i]];
		if (spdk_unlikely(task->used)) {
			SPDK_ERRLOG("%s: request with idx '%"PRIu16"' is already pending.\n",
				    fvdev->vdev.name, reqs[i]);
			spdk_vhost_vq_used_ring_enqueue(vsession, vq, reqs[i], 0);
			continue;
		}

		vsession->task_cnt++;

		task->used = true;
		task->in_iovcnt = 0;
		task->out_iovcnt = 0;
		task->status = NULL;
		task->used_len = 0;

		rc = process_fs_request(task, fvsession, vq);

		if (likely(rc == 0)) {
//					task_submit(task);
					SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "====== Task %p req_idx %d submitted ======\n", task,
						      task->req_idx);
				} else if (rc > 0) {
//					spdk_vhost_scsi_task_cpl(&task->scsi);
					SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "====== Task %p req_idx %d finished early ======\n", task,
						      task->req_idx);
				} else {
//					invalid_request(task);
					SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "====== Task %p req_idx %d failed ======\n", task,
						      task->req_idx);
				}
	}
}

static int
vdev_worker(void *arg)
{
	struct spdk_vhost_fs_session *fvsession = arg;
	struct spdk_vhost_session *vsession = &fvsession->vsession;

	uint16_t q_idx;

	for (q_idx = 0; q_idx < vsession->max_queues; q_idx++) {
		process_vq(fvsession, &vsession->virtqueue[q_idx]);
	}

	spdk_vhost_session_used_signal(vsession);

	return -1;
}

#if 0
static void
no_bdev_process_vq(struct spdk_vhost_blk_session *bvsession, struct spdk_vhost_virtqueue *vq)
{
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	struct iovec iovs[SPDK_VHOST_IOVS_MAX];
	uint32_t length;
	uint16_t iovcnt, req_idx;

	if (spdk_vhost_vq_avail_ring_get(vq, &req_idx, 1) != 1) {
		return;
	}

	iovcnt = SPDK_COUNTOF(iovs);
	if (blk_iovs_setup(bvsession, vq, req_idx, iovs, &iovcnt, &length) == 0) {
		*(volatile uint8_t *)iovs[iovcnt - 1].iov_base = VIRTIO_BLK_S_IOERR;
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK_DATA, "Aborting request %" PRIu16"\n", req_idx);
	}

	spdk_vhost_vq_used_ring_enqueue(vsession, vq, req_idx, 0);
}

static int
no_bdev_vdev_worker(void *arg)
{
	struct spdk_vhost_blk_session *bvsession = arg;
	struct spdk_vhost_session *vsession = &bvsession->vsession;
	uint16_t q_idx;

	for (q_idx = 0; q_idx < vsession->max_queues; q_idx++) {
		no_bdev_process_vq(bvsession, &vsession->virtqueue[q_idx]);
	}

	spdk_vhost_session_used_signal(vsession);

	if (vsession->task_cnt == 0 && bvsession->io_channel) {
		spdk_put_io_channel(bvsession->io_channel);
		bvsession->io_channel = NULL;
	}

	return -1;
}
#endif

static struct spdk_vhost_fs_session *
to_fs_session(struct spdk_vhost_session *vsession)
{
	if (vsession == NULL) {
		return NULL;
	}

	if (vsession->vdev->backend != &vhost_fs_device_backend) {
		SPDK_ERRLOG("%s: not a vhost-fs device\n", vsession->vdev->name);
		return NULL;
	}

	return (struct spdk_vhost_fs_session *)vsession;
}

static struct spdk_vhost_fs_dev *
to_fs_dev(struct spdk_vhost_dev *vdev)
{
	// TODO:
	if (vdev == NULL) {
		return NULL;
	}

	if (vdev->backend != &vhost_fs_device_backend) {
		SPDK_ERRLOG("%s: not a vhost-fs device\n", vdev->name);
		return NULL;
	}

	return SPDK_CONTAINEROF(vdev, struct spdk_vhost_fs_dev, vdev);
}

static int
_spdk_vhost_session_bdev_remove_cb(struct spdk_vhost_dev *vdev, struct spdk_vhost_session *vsession,
				   void *ctx)
{
	struct spdk_vhost_fs_session *fvsession;

	if (vdev == NULL) {
		/* Nothing to do */
		return 0;
	}

	if (vsession == NULL) {
		/* All sessions have been notified, time to close the bdev */
//		struct spdk_vhost_fs_dev *fvdev = to_fs_dev(vdev);
//
//		assert(fvdev != NULL);

//		spdk_bdev_close(fvdev->bdev_desc);
//		bvdev->bdev_desc = NULL;
//		bvdev->bdev = NULL;
		return 0;
	}

	fvsession = (struct spdk_vhost_fs_session *)vsession;
	if (fvsession->requestq_poller) {
		spdk_poller_unregister(&fvsession->requestq_poller);
//		fvsession->requestq_poller = spdk_poller_register(no_bdev_vdev_worker, bvsession, 0);
	}

	return 0;
}

static void
bdev_remove_cb(void *remove_ctx)
{
	struct spdk_vhost_fs_dev *fvdev = remove_ctx;

	SPDK_WARNLOG("Controller %s: Hot-removing bdev - all further requests will fail.\n",
		     fvdev->vdev.name);

	spdk_vhost_lock();
	spdk_vhost_dev_foreach_session(&fvdev->vdev, _spdk_vhost_session_bdev_remove_cb, NULL);
	spdk_vhost_unlock();
}

static void
free_task_pool(struct spdk_vhost_fs_session *fvsession)
{
	struct spdk_vhost_session *vsession = &fvsession->vsession;
	struct spdk_vhost_virtqueue *vq;
	uint16_t i;

	for (i = 0; i < vsession->max_queues; i++) {
		vq = &vsession->virtqueue[i];
		if (vq->tasks == NULL) {
			continue;
		}

		spdk_dma_free(vq->tasks);
		vq->tasks = NULL;
	}
}

static int
alloc_task_pool(struct spdk_vhost_fs_session *fvsession)
{
	struct spdk_vhost_session *vsession = &fvsession->vsession;
	struct spdk_vhost_fs_dev *fvdev = fvsession->fvdev;
	struct spdk_vhost_virtqueue *vq;
	struct spdk_vhost_fs_task *task;
	uint32_t task_cnt;
	uint16_t i;
	uint32_t j;

	for (i = 0; i < vsession->max_queues; i++) {
		vq = &vsession->virtqueue[i];
		if (vq->vring.desc == NULL) {
			continue;
		}

		task_cnt = vq->vring.size;
		if (task_cnt > SPDK_VHOST_MAX_VQ_SIZE) {
			/* sanity check */
			SPDK_ERRLOG("Controller %s: virtuque %"PRIu16" is too big. (size = %"PRIu32", max = %"PRIu32")\n",
				    fvdev->vdev.name, i, task_cnt, SPDK_VHOST_MAX_VQ_SIZE);
			free_task_pool(fvsession);
			return -1;
		}
		vq->tasks = spdk_dma_zmalloc(sizeof(struct spdk_vhost_fs_task) * task_cnt,
					     SPDK_CACHE_LINE_SIZE, NULL);
		if (vq->tasks == NULL) {
			SPDK_ERRLOG("Controller %s: failed to allocate %"PRIu32" tasks for virtqueue %"PRIu16"\n",
				    fvdev->vdev.name, task_cnt, i);
			free_task_pool(fvsession);
			return -1;
		}

		for (j = 0; j < task_cnt; j++) {
			task = &((struct spdk_vhost_fs_task *)vq->tasks)[j];
			task->fvsession = fvsession;
			task->req_idx = j;
			task->vq = vq;
		}
	}

	return 0;
}

static int
spdk_vhost_fs_start_cb(struct spdk_vhost_dev *vdev,
			struct spdk_vhost_session *vsession, void *event_ctx)
{
	struct spdk_vhost_fs_dev *fvdev;
	struct spdk_vhost_fs_session *fvsession;
	int i, rc = 0;

	fvsession = to_fs_session(vsession);
	if (fvsession == NULL) {
		SPDK_ERRLOG("Trying to start non-fs controller as a fs one.\n");
		rc = -1;
		goto out;
	}

	fvdev = to_fs_dev(vdev);
	assert(fvdev != NULL);
	fvsession->fvdev = fvdev;

	/* validate all I/O queues are in a contiguous index range */
	for (i = 0; i < vsession->max_queues; i++) {
		if (vsession->virtqueue[i].vring.desc == NULL) {
			SPDK_ERRLOG("%s: queue %"PRIu32" is empty\n", vdev->name, i);
			rc = -1;
			goto out;
		}
	}

	rc = alloc_task_pool(fvsession);
	if (rc != 0) {
		SPDK_ERRLOG("%s: failed to alloc task pool.\n", fvdev->vdev.name);
		goto out;
	}

	if (fvdev->fs) {
		// TODO: async API doesn't need io_channel, need to check whether this step is required.
		fvsession->io_channel = spdk_fs_alloc_io_channel(fvdev->fs);
		if (!fvsession->io_channel) {
			free_task_pool(fvsession);
			SPDK_ERRLOG("Controller %s: IO channel allocation failed\n", vdev->name);
			rc = -1;
			goto out;
		}
	}

	fvsession->requestq_poller = spdk_poller_register(vdev_worker,
				     fvsession, 0);
	SPDK_INFOLOG(SPDK_LOG_VHOST, "Started poller for vhost controller %s on lcore %d\n",
		     vdev->name, vsession->lcore);

out:
	spdk_vhost_session_event_done(event_ctx, rc);
	return rc;
}

static int
spdk_vhost_fs_start(struct spdk_vhost_session *vsession)
{
	int rc;
//	struct spdk_vhost_fs_session *fvsession;

	vsession->lcore = spdk_vhost_allocate_reactor(vsession->vdev->cpumask);
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "controller allocated lcore %d\n", vsession->lcore);
	/* the load process of blobfs only can be applied at the master core */
	assert(vsession->lcore == 0);
	rc = spdk_vhost_session_send_event(vsession, spdk_vhost_fs_start_cb,
					   3, "start session");

	if (rc != 0) {
		spdk_vhost_free_reactor(vsession->lcore);
		vsession->lcore = -1;
	}

	return rc;
}

static int
destroy_session_poller_cb(void *arg)
{
	struct spdk_vhost_fs_session *fvsession = arg;
	struct spdk_vhost_session *vsession = &fvsession->vsession;
	int i;

	if (vsession->task_cnt > 0) {
		return -1;
	}

	for (i = 0; i < vsession->max_queues; i++) {
		vsession->virtqueue[i].next_event_time = 0;
		spdk_vhost_vq_used_signal(vsession, &vsession->virtqueue[i]);
	}

	SPDK_INFOLOG(SPDK_LOG_VHOST, "Stopping poller for vhost controller %s\n", vsession->vdev->name);

	if (fvsession->io_channel) {
		spdk_put_io_channel(fvsession->io_channel);
		fvsession->io_channel = NULL;
	}

	free_task_pool(fvsession);
	spdk_poller_unregister(&fvsession->destroy_ctx.poller);
	spdk_vhost_session_event_done(fvsession->destroy_ctx.event_ctx, 0);

	return -1;
}

static int
spdk_vhost_fs_stop_cb(struct spdk_vhost_dev *vdev,
		       struct spdk_vhost_session *vsession, void *event_ctx)
{
	struct spdk_vhost_fs_session *fvsession;

	fvsession = to_fs_session(vsession);
	if (fvsession == NULL) {
		SPDK_ERRLOG("Trying to stop non-fs controller as a fs one.\n");
		goto err;
	}

	fvsession->destroy_ctx.event_ctx = event_ctx;
	spdk_poller_unregister(&fvsession->requestq_poller);
	fvsession->destroy_ctx.poller = spdk_poller_register(destroy_session_poller_cb,
					fvsession, 1000);
	return 0;

err:
	spdk_vhost_session_event_done(event_ctx, -1);
	return -1;
}

static int
spdk_vhost_fs_stop(struct spdk_vhost_session *vsession)
{
	int rc;

	SPDK_NOTICELOG("Start to stop vhost fs session\n");
	rc = spdk_vhost_session_send_event(vsession, spdk_vhost_fs_stop_cb,
					   3, "stop session");
	if (rc != 0) {
		return rc;
	}

	spdk_vhost_free_reactor(vsession->lcore);
	vsession->lcore = -1;
	return 0;
}

static struct spdk_bdev *
spdk_vhost_fs_get_bdev(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_fs_dev *fvdev = to_fs_dev(vdev);

	assert(fvdev != NULL);
	return fvdev->bdev;
}

static void
spdk_vhost_fs_dump_info_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_bdev *bdev = spdk_vhost_fs_get_bdev(vdev);
	struct spdk_vhost_fs_dev *fvdev;

	fvdev = to_fs_dev(vdev);
	if (fvdev == NULL) {
		return;
	}

	assert(fvdev != NULL);
	spdk_json_write_named_object_begin(w, "fuse");

//	spdk_json_write_named_bool(w, "readonly", fvdev->readonly);

	spdk_json_write_name(w, "bdev");
	if (bdev) {
		spdk_json_write_string(w, spdk_bdev_get_name(bdev));
	} else {
		spdk_json_write_null(w);
	}

	spdk_json_write_object_end(w);
}

#if 0
static void
spdk_vhost_fs_write_config_json(struct spdk_vhost_dev *vdev, struct spdk_json_write_ctx *w)
{
	struct spdk_vhost_blk_dev *bvdev;

	bvdev = to_blk_dev(vdev);
	if (bvdev == NULL) {
		return;
	}

	if (!bvdev->bdev) {
		return;
	}

	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "method", "construct_vhost_blk_controller");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "ctrlr", vdev->name);
	spdk_json_write_named_string(w, "dev_name", spdk_bdev_get_name(bvdev->bdev));
	spdk_json_write_named_string(w, "cpumask", spdk_cpuset_fmt(vdev->cpumask));
	spdk_json_write_named_bool(w, "readonly", bvdev->readonly);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static int spdk_vhost_blk_destroy(struct spdk_vhost_dev *dev);

static int
spdk_vhost_fs_get_config(struct spdk_vhost_dev *vdev, uint8_t *config,
			  uint32_t len)
{
	struct virtio_blk_config blkcfg;
	struct spdk_vhost_blk_dev *bvdev;
	struct spdk_bdev *bdev;
	uint32_t blk_size;
	uint64_t blkcnt;

	bvdev = to_blk_dev(vdev);
	if (bvdev == NULL) {
		SPDK_ERRLOG("Trying to get virito_blk configuration failed\n");
		return -1;
	}

	bdev = bvdev->bdev;
	if (bdev == NULL) {
		/* We can't just return -1 here as this GET_CONFIG message might
		 * be caused by a QEMU VM reboot. Returning -1 will indicate an
		 * error to QEMU, who might then decide to terminate itself.
		 * We don't want that. A simple reboot shouldn't break the system.
		 *
		 * Presenting a block device with block size 0 and block count 0
		 * doesn't cause any problems on QEMU side and the virtio-pci
		 * device is even still available inside the VM, but there will
		 * be no block device created for it - the kernel drivers will
		 * silently reject it.
		 */
		blk_size = 0;
		blkcnt = 0;
	} else {
		blk_size = spdk_bdev_get_block_size(bdev);
		blkcnt = spdk_bdev_get_num_blocks(bdev);
	}

	memset(&blkcfg, 0, sizeof(blkcfg));
	blkcfg.blk_size = blk_size;
	/* minimum I/O size in blocks */
	blkcfg.min_io_size = 1;
	/* expressed in 512 Bytes sectors */
	blkcfg.capacity = (blkcnt * blk_size) / 512;
	blkcfg.size_max = 131072;
	/*  -2 for REQ and RESP and -1 for region boundary splitting */
	blkcfg.seg_max = SPDK_VHOST_IOVS_MAX - 2 - 1;
	/* QEMU can overwrite this value when started */
	blkcfg.num_queues = SPDK_VHOST_MAX_VQUEUES;

	if (bdev && spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
		/* 16MiB, expressed in 512 Bytes */
		blkcfg.max_discard_sectors = 32768;
		blkcfg.max_discard_seg = 1;
		blkcfg.discard_sector_alignment = blk_size / 512;
	}
	if (bdev && spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_WRITE_ZEROES)) {
		blkcfg.max_write_zeroes_sectors = 32768;
		blkcfg.max_write_zeroes_seg = 1;
	}

	memcpy(config, &blkcfg, spdk_min(len, sizeof(blkcfg)));

	return 0;
}
#endif

static int spdk_vhost_fs_destroy(struct spdk_vhost_dev *vdev);

static const struct spdk_vhost_dev_backend vhost_fs_device_backend = {
	// TODO: clarify virtio features for blobfs
	.virtio_features = SPDK_VHOST_FEATURES,
	.disabled_features = 0,
//	.disabled_features = SPDK_VHOST_DISABLED_FEATURES,
//	.virtio_features = SPDK_VHOST_FEATURES |
//	(1ULL << VIRTIO_BLK_F_SIZE_MAX) | (1ULL << VIRTIO_BLK_F_SEG_MAX) |
//	(1ULL << VIRTIO_BLK_F_GEOMETRY) | (1ULL << VIRTIO_BLK_F_RO) |
//	(1ULL << VIRTIO_BLK_F_BLK_SIZE) | (1ULL << VIRTIO_BLK_F_TOPOLOGY) |
//	(1ULL << VIRTIO_BLK_F_BARRIER)  | (1ULL << VIRTIO_BLK_F_SCSI) |
//	(1ULL << VIRTIO_BLK_F_FLUSH)    | (1ULL << VIRTIO_BLK_F_CONFIG_WCE) |
//	(1ULL << VIRTIO_BLK_F_MQ)       | (1ULL << VIRTIO_BLK_F_DISCARD) |
//	(1ULL << VIRTIO_BLK_F_WRITE_ZEROES),
//	.disabled_features = SPDK_VHOST_DISABLED_FEATURES | (1ULL << VIRTIO_BLK_F_GEOMETRY) |
//	(1ULL << VIRTIO_BLK_F_RO) | (1ULL << VIRTIO_BLK_F_FLUSH) | (1ULL << VIRTIO_BLK_F_CONFIG_WCE) |
//	(1ULL << VIRTIO_BLK_F_BARRIER) | (1ULL << VIRTIO_BLK_F_SCSI) | (1ULL << VIRTIO_BLK_F_DISCARD) |
//	(1ULL << VIRTIO_BLK_F_WRITE_ZEROES),

	.session_ctx_size = sizeof(struct spdk_vhost_fs_session) - sizeof(struct spdk_vhost_session),
	.start_session =  spdk_vhost_fs_start,
	.stop_session = spdk_vhost_fs_stop,
//	.vhost_get_config = spdk_vhost_fs_get_config,
	.dump_info_json = spdk_vhost_fs_dump_info_json,
//	.write_config_json = spdk_vhost_fs_write_config_json,
	.remove_device = spdk_vhost_fs_destroy,
};

#if 0
int
spdk_vhost_blk_controller_construct(void)
{
	struct spdk_conf_section *sp;
	unsigned ctrlr_num;
	char *bdev_name;
	char *cpumask;
	char *name;
	bool readonly;

	for (sp = spdk_conf_first_section(NULL); sp != NULL; sp = spdk_conf_next_section(sp)) {
		if (!spdk_conf_section_match_prefix(sp, "VhostBlk")) {
			continue;
		}

		if (sscanf(spdk_conf_section_get_name(sp), "VhostBlk%u", &ctrlr_num) != 1) {
			SPDK_ERRLOG("Section '%s' has non-numeric suffix.\n",
				    spdk_conf_section_get_name(sp));
			return -1;
		}

		name = spdk_conf_section_get_val(sp, "Name");
		if (name == NULL) {
			SPDK_ERRLOG("VhostBlk%u: missing Name\n", ctrlr_num);
			return -1;
		}

		cpumask = spdk_conf_section_get_val(sp, "Cpumask");
		readonly = spdk_conf_section_get_boolval(sp, "ReadOnly", false);

		bdev_name = spdk_conf_section_get_val(sp, "Dev");
		if (bdev_name == NULL) {
			continue;
		}

		if (spdk_vhost_blk_construct(name, cpumask, bdev_name, readonly) < 0) {
			return -1;
		}
	}

	return 0;
}
#endif

static void
fs_init_cb(void *ctx, struct spdk_filesystem *fs, int fserrno)
{
//	struct spdk_event *event;
	struct spdk_vhost_fs_dev *fvdev = ctx;
	uint64_t features = 0;
	int ret;
	char *cpumask;

	if (fserrno) {
		SPDK_ERRLOG("Failed to mount BlobFS for %s\n", fvdev->name);
		ret = -fserrno;
		goto out;
	}

	SPDK_NOTICELOG("Mounted BlobFS on bdev %s for vhost %s\n", spdk_bdev_get_name(fvdev->bdev), fvdev->name);
	fvdev->fs = fs;

	// TODO: currently, blobfs only supports running on master core
	cpumask = "0x1";
	ret = spdk_vhost_dev_register(&fvdev->vdev, fvdev->name, cpumask, &vhost_fs_device_backend);
	if (ret != 0) {
		SPDK_ERRLOG("Failed to register vhost dev for %s\n", fvdev->name);
		goto out;
	}

	// TODO: check and set features for vhost fs
	SPDK_DEBUGLOG(SPDK_LOG_VHOST_FS, "Controller %s enable features 0x%lx\n", fvdev->name, features);
	if (features && rte_vhost_driver_enable_features(fvdev->vdev.path, features)) {
		SPDK_ERRLOG("Controller %s: failed to enable features 0x%"PRIx64"\n", fvdev->name, features);

		if (spdk_vhost_dev_unregister(&fvdev->vdev) != 0) {
			SPDK_ERRLOG("Controller %s: failed to remove controller\n", fvdev->name);
		}

		ret = -1;
		goto out;
	}

	SPDK_INFOLOG(SPDK_LOG_VHOST, "Controller %s: using bdev '%s'\n", fvdev->name,
			spdk_bdev_get_name(fvdev->bdev));

out:
	if (ret != 0 && fvdev) {
		spdk_dma_free(fvdev);
	}
	spdk_vhost_unlock();

	fvdev->cb_fn(fvdev->cb_arg, ret);
}

static void
__fs_call_fn(void *arg1, void *arg2)
{
	fs_request_fn fn;

	fn = (fs_request_fn)arg1;
	fn(arg2);
}

static void
__fs_send_request(fs_request_fn fn, void *arg)
{
	struct spdk_event *event;

	event = spdk_event_allocate(0, __fs_call_fn, (void *)fn, arg);
	spdk_event_call(event);
}

int
spdk_vhost_fs_construct(const char *name, const char *cpumask, const char *dev_name, bool readonly,
		spdk_vhost_fs_construct_cb cb_fn, void *cb_arg)
{
	struct spdk_vhost_fs_dev *fvdev = NULL;
	struct spdk_bdev *bdev;
	int ret = 0;

	spdk_vhost_lock();
	bdev = spdk_bdev_get_by_name(dev_name);
	if (bdev == NULL) {
		SPDK_ERRLOG("Controller %s: bdev '%s' not found\n",
			    name, dev_name);
		ret = -ENODEV;
		goto err;
	}

	fvdev = spdk_dma_zmalloc(sizeof(*fvdev), SPDK_CACHE_LINE_SIZE, NULL);
	if (fvdev == NULL) {
		ret = -ENOMEM;
		goto err;
	}

	fvdev->bdev = bdev;
	fvdev->bs_dev = spdk_bdev_create_bs_dev(bdev, bdev_remove_cb, fvdev);
	if (fvdev->bs_dev == NULL) {
		SPDK_ERRLOG("Failed to mount blobstore on bdev %s\n",
				spdk_bdev_get_name(bdev));
		goto err;
	}

	SPDK_NOTICELOG("Mounting BlobFS on bdev %s for vhost %s\n", spdk_bdev_get_name(bdev), name);

	fvdev->cb_fn = cb_fn;
	fvdev->cb_arg = cb_arg;
	fvdev->name = strdup(name);
	spdk_fs_load(fvdev->bs_dev, __fs_send_request, fs_init_cb, fvdev);

	return 0;

err:
	if (ret != 0 && fvdev) {
		spdk_dma_free(fvdev);
	}
	spdk_vhost_unlock();

	cb_fn(cb_arg, ret);
	return 0;
}

static int
spdk_vhost_fs_destroy(struct spdk_vhost_dev *vdev)
{
	struct spdk_vhost_fs_dev *fvdev = to_fs_dev(vdev);
	int rc;

	if (!fvdev) {
		return -EINVAL;
	}

	rc = spdk_vhost_dev_unregister(&fvdev->vdev);
	if (rc != 0) {
		return rc;
	}

	spdk_fs_unload(fvdev->fs, NULL, NULL);

	spdk_dma_free(fvdev);
	return 0;
}

SPDK_LOG_REGISTER_COMPONENT("vhost_fs", SPDK_LOG_VHOST_FS)
SPDK_LOG_REGISTER_COMPONENT("vhost_fs_data", SPDK_LOG_VHOST_FS_DATA)
