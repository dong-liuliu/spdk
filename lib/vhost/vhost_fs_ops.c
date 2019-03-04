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
#include "../blobfs/blobfs_internal.h"

static int
send_reply(struct spdk_vhost_fs_task *task, int error)
{
	struct fuse_out_header *out = task->out_iovs[0].iov_base;

	if (error <= -1000 || error > 0) {
		fprintf(stderr, "fuse: bad error value: %i\n",	error);
		error = -ERANGE;
	}

	out->unique = task->unique;
	out->error = error;

	task->used_len += sizeof(*out);

	spdk_vhost_vq_used_ring_enqueue(&task->fvsession->vsession, task->vq, task->req_idx, task->used_len);

		*task->status = error;
		SPDK_DEBUGLOG(SPDK_LOG_VHOST_BLK, "Finished task (%p) req_idx=%d\n status: %s\n", task,
			      task->req_idx, !error ? "OK" : "FAIL");
		fs_task_finish(task);
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

#if 0
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

int
fuse_reply_attr(struct spdk_vhost_fs_task *task, const struct stat *attr,
		    double attr_timeout)
{
	struct fuse_attr_out arg;
	size_t size = sizeof(struct fuse_attr_out);

	memset(&arg, 0, sizeof(arg));
	arg.attr_valid = calc_timeout_sec(attr_timeout);
	arg.attr_valid_nsec = calc_timeout_nsec(attr_timeout);
	convert_stat(attr, &arg.attr);

	return send_reply_ok(task, &arg, size);
}

static void
file_stat_async_cb(void *ctx, struct spdk_file_stat *stat, int fserrno)
{
	struct spdk_fuse_req *req = ctx;
	struct stat stbuf;

	if (fserrno) {
		fuse_reply_err(req, -fserrno);
		return;
	}

	stbuf->st_mode = S_IFREG | 0644;
	stbuf->st_nlink = 1;
	stbuf->st_size = stat->size;

	fuse_reply_attr(req, &stbuf, 0);
}

static int
do_getattr(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	char *file_path;

	if (task->fvsession->cinfo.proto_minor >= 9) {
//		struct fuse_getattr_in *arg = (struct fuse_getattr_in *) inarg;

		SPDK_ERRLOG("Client Fuse Version is not compatible\n");
		fuse_reply_err(task, EPROTONOSUPPORT);

		return 0;
	}

	if (node_id == 1) {
		file_path = "/";
	} else {
		SPDK_ERRLOG("node is not root dir, not support yet\n");
//		file_path = spdk_file_get_name();
		file_path = "/";
	}

	if (!strcmp(file_path, "/")) {
		struct stat stbuf;

		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		fuse_reply_attr(task, &stbuf, 0);
	} else {
		spdk_fs_file_stat_async(task, file_path, file_stat_async_cb, task);
	}

	return 0;
}

static int
do_setattr(struct spdk_fuse_req *req, uint64_t node_id, const void *in_arg)
{
	struct fuse_setattr_in *arg = (struct fuse_setattr_in *) in_arg;

	(void)arg;

	fuse_reply_err(req, ENOSYS);
	return 0;
}

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
do_opendir(struct spdk_fuse_req *req, uint64_t node_id, const void *in_arg)
{
	struct fuse_open_in *arg = (struct fuse_open_in *) in_arg;
	struct fuse_file_info fi;

	memset(&fi, 0, sizeof(fi));
	fi.flags = arg->flags;

	/* Only support root dir */
	if (!strcmp(file_path, "/")) {
		fuse_reply_open(req, &fi);
	} else {
		fuse_reply_err(req, ENOENT);
	}

	return 0;
}

static int
do_releasedir(struct spdk_fuse_req *req, uint64_t node_id, const void *in_arg)
{
	struct fuse_release_in *arg = (struct fuse_release_in *) in_arg;
	struct fuse_file_info fi;

	memset(&fi, 0, sizeof(fi));
	fi.flags = arg->flags;
	fi.fh = arg->fh;

	/* Only support root dir */
	if (!strcmp(file_path, "/")) {
		fuse_reply_err(req, 0);
	} else {
		fuse_reply_err(req, ENOENT);
	}

	return 0;
}


/* `buf` is allowed to be empty so that the proper size may be
   allocated by the caller */
size_t fuse_add_direntry(fuse_req_t req, char *buf, size_t bufsize,
			      const char *name,
			      const struct fuse_entry_param *e, off_t off)
{

}

static int
do_readdir(struct spdk_fuse_req *req, uint64_t node_id, const void *in_arg)
{
	struct fuse_read_in *arg = (struct fuse_read_in *) inarg;
	struct fuse_file_info fi;
	struct spdk_file *file;
	const char *filename;
	spdk_fs_iter iter;

	/* Only support root dir */
	if (strcmp(file_path, "/")) {
		fuse_reply_err(req, ENOENT);
		return 0;
	}

	memset(&fi, 0, sizeof(fi));
	fi.fh = arg->fh;

	buf = calloc(1, arg->size);
	if (buf == NULL) {

	}

	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);

	iter = spdk_fs_iter_first(g_fs);
	while (iter != NULL) {
		file = spdk_fs_iter_get_file(iter);
		iter = spdk_fs_iter_next(iter);
		filename = spdk_file_get_name(file);
		filler(buf, &filename[1], NULL, 0, 0);
	}

error:
    // If there's an error, we can only signal it if we haven't stored
    // any entries yet - otherwise we'd end up with wrong lookup
    // counts for the entries that are already in the buffer. So we
    // return what we've collected until that point.
    if (err && rem == size)
	    fuse_reply_err(req, err);
    else
	    fuse_reply_buf(req, buf, size - rem);
    free(buf);
}

static int
do_lookup(struct spdk_fuse_req *req, uint64_t node_id, const void *in_arg)
{
	char *name = (char *) in_arg;

	if (lo_debug(req))
		fprintf(stderr, "lo_lookup(parent=%" PRIu64 ", name=%s)\n",
			parent, name);

	// TODO: check whether node id is rootdir


	iter = spdk_fs_iter_first(g_fs);
	while (iter != NULL) {
		file = spdk_fs_iter_get_file(iter);
		iter = spdk_fs_iter_next(iter);
		filename = spdk_file_get_name(file);
		if (strcmp(filename, name) == 0) {

			fuse_reply_entry(req, &e);
			return 0;
		}
	}

	fuse_reply_err(req, err);
	return 0;
}

static int
do_forget(struct spdk_fuse_req *req, uint64_t node_id, const void *in_arg)
{
	struct fuse_forget_in *arg = (struct fuse_forget_in *) inarg;

	fuse_reply_none(req);

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


static int
do_statfs(fuse_req_t req, fuse_ino_t nodeid, const void *inarg)
{
	struct statvfs buf = {
			.f_namemax = 255,
			.f_bsize = 512,
	};
		fuse_reply_statfs(req, &buf);

		return 0;
}
#endif

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

	// TODO: destory process

	fuse_reply_ok(task);
	return 0;
}

static int
do_nothing(struct spdk_vhost_fs_task *task, uint64_t node_id, const void *in_arg)
{
	fuse_reply_err(task, ENOSYS);
	return 0;
}

struct spdk_fuse_op spdk_fuse_ll_ops_array[] = {
	[FUSE_INIT]	   = { do_init,	       "INIT"	     },
	[FUSE_DESTROY]	   = { do_destroy,     "DESTROY"     },

#if 1
	[FUSE_LOOKUP]	   = { do_nothing,      "LOOKUP"	     },
	[FUSE_FORGET]	   = { do_nothing,      "FORGET"	     },
	[FUSE_GETATTR]	   = { do_nothing,     "GETATTR"     },
	[FUSE_SETATTR]	   = { do_nothing,     "SETATTR"     },
	[FUSE_OPEN]	   = { do_nothing,	       "OPEN"	     },
	[FUSE_READ]	   = { do_nothing,	       "READ"	     },
	[FUSE_WRITE]	   = { do_nothing,       "WRITE"	     },
	[FUSE_STATFS]	   = { do_nothing,      "STATFS"	     },
	[FUSE_RELEASE]	   = { do_nothing,     "RELEASE"     },
	[FUSE_FLUSH]	   = { do_nothing,       "FLUSH"	     },
	[FUSE_OPENDIR]	   = { do_nothing,     "OPENDIR"     },
	[FUSE_READDIR]	   = { do_nothing,     "READDIR"     },
	[FUSE_RELEASEDIR]  = { do_nothing,  "RELEASEDIR"  },
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


