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

#ifndef SPDK_VHOST_FS_INTERNAL_H
#define SPDK_VHOST_FS_INTERNAL_H

#include "spdk/stdinc.h"
#include <linux/virtio_blk.h>

// TODO: extract required structs from fuse_common.h
#define FUSE_USE_VERSION 30
#include "linux/fuse.h"
#include "linux/fuse_kernel.h"

#include "spdk/env.h"
#include "spdk/bdev.h"
#include "spdk/conf.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/vhost.h"
//#include "spdk/blob.h"
#include "spdk/blob_bdev.h"
#include "spdk/blobfs.h"

#include "vhost_internal.h"

struct spdk_vhost_fs_task {
	struct spdk_bdev_io *bdev_io;
	struct spdk_vhost_fs_session *fvsession;
	struct spdk_vhost_virtqueue *vq;

	volatile uint8_t *status;

	uint16_t req_idx;

	/* In is used by fuse_out_header and its followings;
	 * Out is used by fuse in header and its followings
	 */
	uint16_t in_iovcnt;
	uint16_t out_iovcnt;
	struct iovec in_iovs[SPDK_VHOST_IOVS_MAX];
	struct iovec out_iovs[SPDK_VHOST_IOVS_MAX];

	/* If set, the task is currently used for I/O processing. */
	bool used;

	/** Number of bytes that were written. */
	uint32_t used_len;

	/* fuse related */
	uint64_t unique;
//	struct fuse_ctx ctx;
};

struct spdk_vhost_fs_dev {
	struct spdk_vhost_dev vdev;
	struct spdk_filesystem *fs;

	struct spdk_bdev *bdev;
	struct spdk_bs_dev *bs_dev;

	const char *name;
	spdk_vhost_fs_construct_cb cb_fn;
	void *cb_arg;
};

struct spdk_vhost_fs_session {
	/* The parent session must be the very first field in this struct */
	struct spdk_vhost_session vsession;
	struct spdk_vhost_fs_dev *fvdev;
	struct spdk_poller *requestq_poller;
	struct spdk_io_channel *io_channel;
	struct spdk_vhost_dev_destroy_ctx destroy_ctx;

	struct fuse_conn_info cinfo;
};

struct spdk_fuse_op {
	/* Return 0, successfully submitted; or -errno if failed. */
	int (*func)(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg);
	const char *name;
};


static inline void
fs_task_finish(struct spdk_vhost_fs_task *task)
{
	assert(task->fvsession->vsession.task_cnt > 0);
	task->fvsession->vsession.task_cnt--;
	task->used = false;
}

extern struct spdk_fuse_op *spdk_fuse_ll_ops;

#endif /* SPDK_VHOST_FS_INTERNAL_H */
