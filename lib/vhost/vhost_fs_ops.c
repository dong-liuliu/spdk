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
#include "spdk/blobfs.h"
#include "spdk/bdev.h"
#include "spdk/event.h"
#include "spdk/thread.h"
#include "spdk/blob_bdev.h"
#include "spdk/log.h"

#include "vhost_fs_internal.h"
#include "linux/fuse_lowlevel.h"
#include "linux/fuse_misc.h"
#include "../blobfs/blobfs_internal.h"

static int
send_reply_none(struct spdk_vhost_fs_task *task, int error)
{
	fprintf(stderr, "fuse out none: error is %d, unique is 0x%lx\n",
			error, task->unique);

	fs_request_finish(task, error);
	return 0;
}

static int
send_reply(struct spdk_vhost_fs_task *task, int error)
{
	struct fuse_out_header *out = task->in_iovs[0].iov_base;

	if (error <= -1000 || error > 0) {
		fprintf(stderr, "fuse: bad error value: %i\n",	error);
		error = -ERANGE;
	}

	task->used_len += sizeof(*out);

	out->unique = task->unique;
	out->error = error;
	out->len = task->used_len;

	fprintf(stderr, "fuse out header: len is 0x%x error is %d, unique is 0x%lx\n",
			out->len, out->error, out->unique);

	fs_request_finish(task, error);
	return 0;
}

static int
fuse_reply_err(struct spdk_vhost_fs_task *task, int err)
{
	return send_reply(task, -err);
}

static int
fuse_reply_ok(struct spdk_vhost_fs_task *task)
{
	return send_reply(task, 0);
}

static size_t
iov_length(const struct iovec *iov, size_t count)
{
	size_t seg;
	size_t ret = 0;

	for (seg = 0; seg < count; seg++)
		ret += iov[seg].iov_len;
	return ret;
}

//TODO: can be optimized later; it is aiming to readdir and so on
static int
fuse_reply_buf(struct spdk_vhost_fs_task *task, char *buf, uint32_t bufsize)
{
	int i;
	uint32_t bufoff = 0;
	uint32_t bufrem = bufsize;
	uint32_t size;

	for (i = 1; i < task->in_iovcnt || bufrem > 0; i++) {
		size = task->in_iovs[i].iov_len;
		if (bufrem < task->in_iovs[i].iov_len) {
			size = bufrem;
		}
		memcpy(task->in_iovs[i].iov_base, buf + bufoff, size);
		bufoff += size;
		bufrem -= size;
	}

	if (bufrem != 0) {
		fprintf(stderr, "Failed to send whole buf by in_iovs! Remain 0x%x bytes", bufrem);
	}


	task->used_len = bufsize;

	return send_reply(task, 0);
}

static unsigned long
calc_timeout_sec(double t)
{
	if (t > (double) ULONG_MAX)
		return ULONG_MAX;
	else if (t < 0.0)
		return 0;
	else
		return (unsigned long) t;
}

static unsigned int
calc_timeout_nsec(double t)
{
	double f = t - (double) calc_timeout_sec(t);
	if (f < 0.0)
		return 0;
	else if (f >= 0.999999999)
		return 999999999;
	else
		return (unsigned int) (f * 1.0e9);
}

static void
convert_stat(const struct stat *stbuf, struct fuse_attr *attr)
{
	attr->ino	= stbuf->st_ino;
	attr->mode	= stbuf->st_mode;
	attr->nlink	= stbuf->st_nlink;
	attr->uid	= stbuf->st_uid;
	attr->gid	= stbuf->st_gid;
	attr->rdev	= stbuf->st_rdev;
	attr->size	= stbuf->st_size;
	attr->blksize	= stbuf->st_blksize;
	attr->blocks	= stbuf->st_blocks;
	attr->atime	= stbuf->st_atime;
	attr->mtime	= stbuf->st_mtime;
	attr->ctime	= stbuf->st_ctime;
	attr->atimensec = ST_ATIM_NSEC(stbuf);
	attr->mtimensec = ST_MTIM_NSEC(stbuf);
	attr->ctimensec = ST_CTIM_NSEC(stbuf);
}

static int
fuse_reply_attr(struct spdk_vhost_fs_task *task, const struct stat *attr,
		    double attr_timeout)
{
	struct fuse_attr_out *outarg;

	outarg = task->in_iovs[1].iov_base;
	memset(outarg, 0, sizeof(*outarg));
	outarg->attr_valid = calc_timeout_sec(attr_timeout);
	outarg->attr_valid_nsec = calc_timeout_nsec(attr_timeout);
	convert_stat(attr, &outarg->attr);


	task->used_len = sizeof(*outarg);

	return send_reply(task, 0);
}

static void
file_stat_async_cb(void *ctx, struct spdk_file_stat *stat, int fserrno)
{
	struct spdk_vhost_fs_task *task = ctx;
	struct stat stbuf = {};

	if (fserrno) {
		fuse_reply_err(task, -fserrno);
		return;
	}

	stbuf.st_mode = S_IFREG | 0644;
	stbuf.st_nlink = 1;
	stbuf.st_size = stat->size;
	stbuf.st_ino = (uint64_t)stbuf.st_ino;

	fuse_reply_attr(task, &stbuf, 0);
}

static int
do_getattr(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_getattr_in *arg = (struct fuse_getattr_in *) in_arg;
	struct spdk_file *file;
	const char *file_path;

	if (1) {
		fprintf(stderr, "do_getattr: nodeid is %ld\n", node_id);
			fprintf(stderr, "getattr_flags=0x%x\n", arg->getattr_flags);
			fprintf(stderr, "fh=0x%lx\n", arg->fh);
			fprintf(stderr, "dummy=0x%x\n", arg->dummy);
	}

	if (task->fvsession->cinfo.proto_minor < 9) {
		SPDK_ERRLOG("Client Fuse Version is not compatible\n");
		fuse_reply_err(task, EPROTONOSUPPORT);

		return 0;
	}

	if (node_id == 1) {
		struct stat stbuf = {};

		stbuf.st_mode = S_IFDIR | 0755;
		stbuf.st_nlink = 2;
		stbuf.st_ino = 0x12345;
		fuse_reply_attr(task, &stbuf, 0);
	} else {
		file = (struct spdk_file *)node_id;
		task->u.fp = file;
		file_path = spdk_file_get_name(file);

		spdk_fs_file_stat_async(task->fvsession->fvdev->fs, file_path, file_stat_async_cb, task);
	}

	return 0;
}

static int
do_setattr(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_setattr_in *arg = (struct fuse_setattr_in *) in_arg;

	(void)arg;

	fuse_reply_err(task, ENOSYS);
	return 0;
}

#if 0
static void
file_open_async_cb(void *ctx, struct spdk_file *f, int fserrno)
{
	struct spdk_fuse_req *req = ctx;
	struct fuse_file_info *fi;

	if (fserrno != 0) {
		fuse_reply_err(req, -fserrno);
		return;
	}

	// TODO: clarify cache
	fi->direct_io = 1;

	fuse_reply_open(req, &fi);
}

static void
do_open(struct spdk_fuse_req *req, uint64_t node_id, const void *in_arg)
{
	struct fuse_open_in *arg = (struct fuse_open_in *) in_arg;
	struct fuse_file_info fi;
	struct spdk_file *file;
	int rc;

	memset(&fi, 0, sizeof(fi));
	fi.flags = arg->flags;
	// TODO: check and add flags
	fi.flags = ;

	spdk_fs_open_file_async(fs, file_path, flags, file_open_async_cb, req);

	return;
}

static void
file_close_async_cb(void *ctx, int fserrno)
{
	fuse_reply_err(req, -fserrno);
}

static int
do_release(fuse_req_t req, fuse_ino_t nodeid, const void *inarg)
{
	struct fuse_release_in *arg = (struct fuse_release_in *) inarg;
	struct fuse_file_info fi;

	memset(&fi, 0, sizeof(fi));
	fi.flags = arg->flags;
	fi.fh = arg->fh;
	if (req->se->conn.proto_minor >= 8) {
		fi.flush = (arg->release_flags & FUSE_RELEASE_FLUSH) ? 1 : 0;
		fi.lock_owner = arg->lock_owner;
	}
	if (arg->release_flags & FUSE_RELEASE_FLOCK_UNLOCK) {
		fi.flock_release = 1;
		fi.lock_owner = arg->lock_owner;
	}


	struct spdk_file *file = (struct spdk_file *)info->fh;

	spdk_file_close_async(file, );

	return 0;
}

static int
do_create(fuse_req_t req, fuse_ino_t nodeid, const void *inarg)
{
	struct fuse_create_in *arg = (struct fuse_create_in *) inarg;

//	if (req->se->op.create) {
//		struct fuse_file_info fi;
//		char *name = PARAM(arg);
//
//		memset(&fi, 0, sizeof(fi));
//		fi.flags = arg->flags;
//
//		if (req->se->conn.proto_minor >= 12)
//			req->ctx.umask = arg->umask;
//		else
//			name = (char *) inarg + sizeof(struct fuse_open_in);
//
//		req->se->op.create(req, nodeid, name, arg->mode, &fi);
//	} else
	fuse_reply_err(req, ENOSYS);
	return 0;
}


static int
do_flush(fuse_req_t req, fuse_ino_t nodeid, const void *inarg)
{
//	struct fuse_flush_in *arg = (struct fuse_flush_in *) inarg;
//	struct fuse_file_info fi;
//
//	memset(&fi, 0, sizeof(fi));
//	fi.fh = arg->fh;
//	fi.flush = 1;
//	if (req->se->conn.proto_minor >= 7)
//		fi.lock_owner = arg->lock_owner;

//	spdk_file_sync_async();

//	if (req->se->op.flush)
//		req->se->op.flush(req, nodeid, &fi);
//	else
//		fuse_reply_err(req, ENOSYS);

	fuse_reply_err(req, 0);

	return 0;
}


static void
do_read(fuse_req_t req, fuse_ino_t nodeid, const void *inarg)
{
	struct fuse_read_in *arg = (struct fuse_read_in *) inarg;

	if (req->se->op.read) {
		struct fuse_file_info fi;

		memset(&fi, 0, sizeof(fi));
		fi.fh = arg->fh;
		if (req->se->conn.proto_minor >= 9) {
			fi.lock_owner = arg->lock_owner;
			fi.flags = arg->flags;
		}
		req->se->op.read(req, nodeid, arg->size, arg->offset, &fi);
	} else
		fuse_reply_err(req, ENOSYS);
}

static void
do_write(fuse_req_t req, fuse_ino_t nodeid, const void *inarg)
{
	struct fuse_write_in *arg = (struct fuse_write_in *) inarg;
	struct fuse_file_info fi;
	char *param;

	memset(&fi, 0, sizeof(fi));
	fi.fh = arg->fh;
	fi.writepage = (arg->write_flags & 1) != 0;

	if (req->se->conn.proto_minor < 9) {
		param = ((char *) arg) + FUSE_COMPAT_WRITE_IN_SIZE;
	} else {
		fi.lock_owner = arg->lock_owner;
		fi.flags = arg->flags;
		param = PARAM(arg);
	}

	if (req->se->op.write)
		req->se->op.write(req, nodeid, param, arg->size,
				 arg->offset, &fi);
	else
		fuse_reply_err(req, ENOSYS);
}

#endif

static int
do_statfs(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_statfs_out *outarg = task->in_iovs[1].iov_base;

	if (1) {
		fprintf(stderr, "do_statfs\n");
	}

	outarg->st.bsize = 4096;
	outarg->st.namelen = 255;

	task->used_len = sizeof(*outarg);

	fuse_reply_ok(task);

	return 0;
}

static int
do_init(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_init_in *arg = (struct fuse_init_in *) in_arg;
	struct fuse_init_out *outarg;
	struct fuse_conn_info *cinfo = &task->fvsession->cinfo;
	size_t outargsize = sizeof(*outarg);

	(void) node_id;
	if (1) {
		fprintf(stderr, "INIT: %u.%u\n", arg->major, arg->minor);
		if (arg->major == 7 && arg->minor >= 6) {
			fprintf(stderr, "flags=0x%08x\n", arg->flags);
			fprintf(stderr, "max_readahead=0x%08x\n",
				arg->max_readahead);
		}
	}

	cinfo->proto_major = arg->major;
	cinfo->proto_minor = arg->minor;
	cinfo->capable = 0;
	cinfo->want = 0;

	outarg = task->in_iovs[1].iov_base;
	if (sizeof(*outarg) != task->in_iovs[1].iov_len) {
		assert(false);
	}

	outarg->major = FUSE_KERNEL_VERSION;
	outarg->minor = FUSE_KERNEL_MINOR_VERSION;

	if (arg->major < 7) {
		fprintf(stderr, "fuse: unsupported protocol version: %u.%u\n",
			arg->major, arg->minor);
		fuse_reply_err(task, EPROTO);
		return 0;
	}

	if (arg->major > 7) {
		/* Wait for a second INIT request with a 7.X version */
		fuse_reply_ok(task);
		return 0;
	}

	cinfo->max_readahead = arg->max_readahead;
	cinfo->capable = arg->flags;
	cinfo->time_gran = 1;
	cinfo->max_write = 0x20000;

	cinfo->max_background = (1 << 16) - 1;
	cinfo->congestion_threshold =  cinfo->max_background * 3 / 4;

	/* Always enable big writes, this is superseded
	   by the max_write option */
	outarg->flags |= FUSE_BIG_WRITES;
	outarg->max_readahead = cinfo->max_readahead;
	outarg->max_write = cinfo->max_write;
	outarg->max_background = cinfo->max_background;
	outarg->congestion_threshold = cinfo->congestion_threshold;
	outarg->time_gran = cinfo->time_gran;

	if (1) {
		fprintf(stderr, "   INIT: %u.%u\n", outarg->major, outarg->minor);
		fprintf(stderr, "   flags=0x%08x\n", outarg->flags);
		fprintf(stderr, "   max_readahead=0x%08x\n",
			outarg->max_readahead);
		fprintf(stderr, "   max_write=0x%08x\n", outarg->max_write);
		fprintf(stderr, "   max_background=%i\n",
			outarg->max_background);
		fprintf(stderr, "   congestion_threshold=%i\n",
			outarg->congestion_threshold);
		fprintf(stderr, "   time_gran=%u\n",
			outarg->time_gran);
	}

	task->used_len = outargsize;
	fuse_reply_ok(task);

	return 0;
}

static int
do_destroy(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	if (1) {
			fprintf(stderr, "do_destroy\n");
	}
	// TODO: destory process

	fuse_reply_ok(task);
	return 0;
}

static void
_do_lookup_stat(void *ctx, struct spdk_file_stat *stat, int fserrno)
{
	struct spdk_vhost_fs_task *task = ctx;
	struct fuse_entry_out *earg = task->in_iovs[1].iov_base;
	struct fuse_entryver_out *ever;
	uint64_t entry_size;

	//TODO: add content for entryver
	ever = (struct fuse_entryver_out *) (task->in_iovs[1].iov_base + sizeof(struct fuse_entry_out));
	ever;

	entry_size = sizeof(struct fuse_entry_out) + sizeof(struct fuse_entryver_out);
	assert(task->in_iovs[1].iov_len >= entry_size);

	memset(earg, 0, entry_size);

	/* Set nodeid to be the memaddr of spdk-file */
	earg->nodeid = (uint64_t)task->u.fp;

	earg->attr_valid = 0;
	earg->entry_valid = 0;

	earg->attr.mode = S_IFREG | 0644;
	earg->attr.nlink = 1;
	earg->attr.ino = stat->blobid;
	earg->attr.size = stat->size;
	earg->attr.blksize = 4096;
	earg->attr.blocks = (stat->size + 4095) / 4096;

	task->used_len = entry_size;

	fuse_reply_ok(task);
	return;
}

static void
_do_lookup_open(void *ctx, struct spdk_file *f, int fserrno)
{
	struct spdk_vhost_fs_task *task = ctx;
	const char *file_path;

	file_path = spdk_file_get_name(f);
	task->u.fp = f;

	spdk_fs_file_stat_async(task->fvsession->fvdev->fs, file_path, _do_lookup_stat, task);

	return;
}

static int
do_lookup(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	char *name = (char *) in_arg;
	struct spdk_file *file;
	const char *filename;
	spdk_fs_iter iter;

	if (1) {
		fprintf(stderr, "do_lookup(parent node_id=%" PRIu64 ", name=%s)\n",
				node_id, name);
	}

	/* Directory is not supported yet */
	if (node_id != 1) {
		fuse_reply_err(task, ENOSYS);
		return -1;
	}

	iter = spdk_fs_iter_first(task->fvsession->fvdev->fs);
	while (iter != NULL) {
		file = spdk_fs_iter_get_file(iter);
		iter = spdk_fs_iter_next(iter);
		filename = spdk_file_get_name(file);

		fprintf(stderr, "existed file name is %s, requested filename is %s\n",
				filename, name);
		//TODO: verify why filename of blobfs has a prefix '/'
		if (strcmp(filename + 1, name) == 0) {
			spdk_fs_open_file_async(task->fvsession->fvdev->fs, filename, 0, _do_lookup_open, task);
			return 0;
		}
	}

	fuse_reply_err(task, ENOENT);
	return 0;
}

static void
_do_forget_close(void *ctx, int fserrno)
{
	struct spdk_vhost_fs_task *task = ctx;

	fprintf(stderr, "do_forget done for task %p\n", task);
	return;
}

//TODO: add refcount for node_id; it needs more consideration.
static int
do_forget(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_forget_in *arg = (struct fuse_forget_in *) in_arg;
	struct spdk_file *file = (struct spdk_file *)node_id;

	if (1) {
		fprintf(stderr, "do_forget(node_id=%" PRIu64 ", nlookup=%lu)\n",
				node_id, arg->nlookup);
	}

	spdk_file_close_async(file, _do_forget_close, task);

	send_reply_none(task, 0);

	return 0;
}

static int
do_opendir(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_open_in *i_arg = (struct fuse_open_in *) in_arg;
	struct fuse_open_out *o_arg = task->in_iovs[1].iov_base;
	spdk_fs_iter *iter_p;

	if (1) {
		fprintf(stderr, "do_opendir(node_id=%" PRIu64 ", flags=0x%x, unused=0x%x)\n",
				node_id, i_arg->flags, i_arg->unused);
	}

	/* Only support root dir */
	if (node_id != 1) {
		fuse_reply_err(task, ENOENT);
		return -1;
	}

	iter_p = calloc(1, sizeof(*iter_p));
	*iter_p = spdk_fs_iter_first(task->fvsession->fvdev->fs);
	memset(o_arg, 0, sizeof(*o_arg));
	o_arg->fh = (uint64_t)iter_p;

	task->used_len = sizeof(*o_arg);
	fuse_reply_ok(task);

	return 0;
}

static int
do_releasedir(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_release_in *arg = (struct fuse_release_in *) in_arg;

	if (1) {
		fprintf(stderr, "do_releasedir(node_id=%" PRIu64 ", fh=0x%lx, flags=0x%x, releaseflags=0x%x, lockowner=0x%lx)\n",
				node_id, arg->fh, arg->flags, arg->release_flags, arg->lock_owner);
	}

	/* Only support root dir */
	if (node_id != 1) {
		fuse_reply_err(task, ENOENT);
		return -1;
	}

	free((spdk_fs_iter *)arg->fh);
	fuse_reply_ok(task);

	return 0;
}

static int
do_readdir(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	struct fuse_read_in *arg = (struct fuse_read_in *) in_arg;
	struct spdk_file *file;
	const char *filename;
	spdk_fs_iter *iter_p = (spdk_fs_iter *)arg->fh;
	char *buf;
	uint32_t bufsize, bufoff;

	if (1) {
		fprintf(stderr, "do_readdir(node_id=%" PRIu64 ", fh=0x%lx, "
				"offset=0x%lx, size=0x%x, "
				"readflags=0x%x, lockowner=0x%lx, flags=0x%x)\n",
				node_id, arg->fh, arg->offset, arg->size,
				arg->read_flags, arg->lock_owner, arg->flags);
	}

	/* Only support root dir */
	if (node_id != 1) {
		fuse_reply_err(task, ENOENT);
		return -1;
	}

	buf = calloc(1, arg->size);
	if (buf == NULL) {
		fuse_reply_err(task, ENOMEM);
		return -1;
	}
	bufsize = arg->size;
	bufoff = 0;

	//TODO: consider situation that bufsize is not enough and require continous readdir cmd
	while (*iter_p != NULL) {
		struct fuse_dirent *dirent;
		size_t entlen;
		size_t entlen_padded;

		dirent = (struct fuse_dirent *)(buf + bufoff);

		file = spdk_fs_iter_get_file(*iter_p);
		*iter_p = spdk_fs_iter_next(*iter_p);
		filename = spdk_file_get_name(file);

		fprintf(stderr, "Find file %s\n", filename);

		entlen = FUSE_NAME_OFFSET + strlen(filename) - 1;
		entlen_padded = FUSE_DIRENT_ALIGN(entlen);
		bufoff += entlen_padded;
		if (bufoff > bufsize) {
			fprintf(stderr, "bufsize is not enough\n");
			fuse_reply_err(task, ENOSYS);
			return -1;
		}

		// TODO: correct dirent contents
		dirent->ino = (uint64_t)file;
		dirent->off = 0;
		dirent->namelen = strlen(filename) - 1;
		dirent->type =  DT_REG;
		strncpy(dirent->name, filename + 1, dirent->namelen);
		memset(dirent->name + dirent->namelen, 0, entlen_padded - entlen);
	}

	fuse_reply_buf(task, buf, bufoff);
    free(buf);

    return 0;
}

static int
do_nothing(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	fuse_reply_err(task, ENOSYS);
	return -1;
}

struct spdk_fuse_op spdk_fuse_ll_ops_array[] = {
	[FUSE_INIT]	   = { do_init,	       "INIT"	     },
	[FUSE_DESTROY]	   = { do_destroy,     "DESTROY"     },
	[FUSE_STATFS]	   = { do_statfs,      "STATFS"	     },

	[FUSE_LOOKUP]	   = { do_lookup,      "LOOKUP"	     },
	[FUSE_FORGET]	   = { do_forget,      "FORGET"	     },
	[FUSE_GETATTR]	   = { do_getattr,     "GETATTR"     },
	[FUSE_SETATTR]	   = { do_setattr,     "SETATTR"     },

	[FUSE_OPENDIR]	   = { do_opendir,     "OPENDIR"     },
	[FUSE_READDIR]	   = { do_readdir,     "READDIR"     },
	[FUSE_RELEASEDIR]  = { do_releasedir,  "RELEASEDIR"  },

#if 1
	[FUSE_OPEN]	   = { do_nothing,	       "OPEN"	     },
	[FUSE_READ]	   = { do_nothing,	       "READ"	     },
	[FUSE_WRITE]	   = { do_nothing,       "WRITE"	     },
	[FUSE_RELEASE]	   = { do_nothing,     "RELEASE"     },
	[FUSE_FLUSH]	   = { do_nothing,       "FLUSH"	     },
	[FUSE_CREATE]	   = { do_nothing,      "CREATE"	     },

	[FUSE_READLINK]	   = { do_nothing,    "READLINK"    },
	[FUSE_SYMLINK]	   = { do_nothing,     "SYMLINK"     },
	[FUSE_MKNOD]	   = { do_nothing,       "MKNOD"	     },
	[FUSE_MKDIR]	   = { do_nothing,       "MKDIR"	     },
	[FUSE_UNLINK]	   = { do_nothing,      "UNLINK"	     },
	[FUSE_RMDIR]	   = { do_nothing,       "RMDIR"	     },
	[FUSE_RENAME]	   = { do_nothing,      "RENAME"	     },
	[FUSE_LINK]	   = { do_nothing,	       "LINK"	     },
	[FUSE_FSYNC]	   = { do_nothing,       "FSYNC"	     },
	[FUSE_SETXATTR]	   = { do_nothing,    "SETXATTR"    },
	[FUSE_GETXATTR]	   = { do_nothing,    "GETXATTR"    },
	[FUSE_LISTXATTR]   = { do_nothing,   "LISTXATTR"   },
	[FUSE_REMOVEXATTR] = { do_nothing, "REMOVEXATTR" },
	[FUSE_FSYNCDIR]	   = { do_nothing,    "FSYNCDIR"    },
	[FUSE_GETLK]	   = { do_nothing,       "GETLK"	     },
	[FUSE_SETLK]	   = { do_nothing,       "SETLK"	     },
	[FUSE_SETLKW]	   = { do_nothing,      "SETLKW"	     },
	[FUSE_ACCESS]	   = { do_nothing,      "ACCESS"	     },
	[FUSE_INTERRUPT]   = { do_nothing,   "INTERRUPT"   },
	[FUSE_BMAP]	   = { do_nothing,	       "BMAP"	     },
	[FUSE_IOCTL]	   = { do_nothing,       "IOCTL"	     },
	[FUSE_POLL]	   = { do_nothing,        "POLL"	     },
	[FUSE_FALLOCATE]   = { do_nothing,   "FALLOCATE"   },
	[FUSE_NOTIFY_REPLY] = { (void *) 1,    "NOTIFY_REPLY" },
	[FUSE_BATCH_FORGET] = { do_nothing, "BATCH_FORGET" },
	[FUSE_READDIRPLUS] = { do_nothing,	"READDIRPLUS"},
	[FUSE_RENAME2]     = { do_nothing,      "RENAME2"    },
	[FUSE_COPY_FILE_RANGE] = { do_nothing, "COPY_FILE_RANGE" },
	[FUSE_SETUPMAPPING]  = { do_nothing, "SETUPMAPPING" },
	[FUSE_REMOVEMAPPING] = { do_nothing, "REMOVEMAPPING" },
	[CUSE_INIT]	   = { do_nothing, "CUSE_INIT"   },
#endif
};

struct spdk_fuse_op *spdk_fuse_ll_ops = spdk_fuse_ll_ops_array;


