/*
  Copyright (c) 2008-2012 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

/*TODO: check for non null wb_file_data before getting wb_file */


#ifndef _CONFIG_H
#define _CONFIG_H
#include "config.h"
#endif

#include "glusterfs.h"
#include "logging.h"
#include "dict.h"
#include "xlator.h"
#include "list.h"
#include "compat.h"
#include "compat-errno.h"
#include "common-utils.h"
#include "call-stub.h"
#include "statedump.h"
#include "write-behind-mem-types.h"

#define MAX_VECTOR_COUNT          8
#define WB_AGGREGATE_SIZE         131072 /* 128 KB */
#define WB_WINDOW_SIZE            1048576 /* 1MB */

typedef struct list_head list_head_t;
struct wb_conf;
struct wb_page;
struct wb_inode;

typedef struct wb_inode {
        size_t       window_conf;
        size_t       window_current;
        size_t       aggregate_current;
        int32_t      op_ret;
        int32_t      op_errno;
        list_head_t  request;
        list_head_t  passive_requests;
        gf_lock_t    lock;
        xlator_t    *this;
}wb_inode_t;

typedef struct wb_file {
        int32_t           flags;
        int               disabled;
        fd_t             *fd;
        size_t            disable_till;
        enum _gf_boolean  dont_wind;
} wb_file_t;


typedef struct wb_request {
        list_head_t           list;
        list_head_t           winds;
        list_head_t           unwinds;
        list_head_t           other_requests;
        call_stub_t          *stub;
        size_t                write_size;
        int32_t               refcount;
        wb_inode_t           *wb_inode;
        glusterfs_fop_t       fop;
        gf_lkowner_t          lk_owner;
        union {
                struct  {
                        char  write_behind;
                        char  stack_wound;
                        char  got_reply;
                        char  virgin;
                        char  flush_all;     /* while trying to sync to back-end,
                                             * don't wait till a data of size
                                             * equal to configured aggregate-size
                                             * is accumulated, instead sync
                                             * whatever data currently present in
                                             * request queue.
                                             */

                }write_request;

                struct {
                        char marked_for_resume;
                }other_requests;
        }flags;
} wb_request_t;

struct wb_conf {
        uint64_t         aggregate_size;
        uint64_t         window_size;
        uint64_t         disable_till;
        gf_boolean_t     enable_O_SYNC;
        gf_boolean_t     flush_behind;
        gf_boolean_t     enable_trickling_writes;
};

typedef struct wb_local {
        list_head_t     winds;
        int32_t         flags;
        fd_t           *fd;
        wb_request_t   *request;
        int             op_ret;
        int             op_errno;
        call_frame_t   *frame;
        int32_t         reply_count;
        wb_inode_t     *wb_inode;
} wb_local_t;

typedef struct wb_conf wb_conf_t;
typedef struct wb_page wb_page_t;

int32_t
wb_process_queue (call_frame_t *frame, wb_inode_t *wb_inode);

ssize_t
wb_sync (call_frame_t *frame, wb_inode_t *wb_inode, list_head_t *winds);

ssize_t
__wb_mark_winds (list_head_t *list, list_head_t *winds, size_t aggregate_size,
                 char enable_trickling_writes);

wb_inode_t *
__wb_inode_ctx_get (xlator_t *this, inode_t *inode)
{
        uint64_t    value    = 0;
        wb_inode_t *wb_inode = NULL;

        __inode_ctx_get (inode, this, &value);
        wb_inode = (wb_inode_t *)(unsigned long) value;

        return wb_inode;
}


wb_inode_t *
wb_inode_ctx_get (xlator_t *this, inode_t *inode)
{
        wb_inode_t *wb_inode = NULL;

        GF_VALIDATE_OR_GOTO ("write-behind", this, out);
        GF_VALIDATE_OR_GOTO (this->name, inode, out);

        LOCK (&inode->lock);
        {
                wb_inode = __wb_inode_ctx_get (this, inode);
        }
        UNLOCK (&inode->lock);
out:
        return wb_inode;
}


wb_file_t *
__wb_fd_ctx_get (xlator_t *this, fd_t *fd)
{
        wb_file_t *wb_file = NULL;
        uint64_t   value   = 0;

        __fd_ctx_get (fd, this, &value);
        wb_file = (wb_file_t *)(unsigned long)value;

        return wb_file;
}


wb_file_t *
wb_fd_ctx_get (xlator_t *this, fd_t *fd)
{
        wb_file_t *wb_file = NULL;

        GF_VALIDATE_OR_GOTO ("write-behind", this, out);
        GF_VALIDATE_OR_GOTO (this->name, fd, out);

        LOCK (&fd->lock);
        {
                wb_file = __wb_fd_ctx_get (this, fd);
        }
        UNLOCK (&fd->lock);

out:
        return wb_file;
}

/*
  Below is a succinct explanation of the code deciding whether two regions
  overlap, from Pavan <tcp@gluster.com>.

  For any two ranges to be non-overlapping, either the end of the first
  range is lesser than the start of the second, or vice versa. Example -

  <--------->       <-------------->
  p         q       x              y

  ( q < x ) or (y < p) = > No overlap.

  To check for *overlap*, we can negate this (using de morgan's laws), and
  it becomes -

  (q >= x ) and (y >= p)

  Either that, or you write the negation using -

  if (! ((q < x) or (y < p)) ) {
  "Overlap"
  }
*/

static inline char
wb_requests_overlap (wb_request_t *request1, wb_request_t *request2)
{
        off_t            r1_start   = 0, r1_end = 0, r2_start = 0, r2_end = 0;
        enum _gf_boolean do_overlap = 0;

        r1_start = request1->stub->args.writev.off;
        r1_end = r1_start + iov_length (request1->stub->args.writev.vector,
                                        request1->stub->args.writev.count);

        r2_start = request2->stub->args.writev.off;
        r2_end = r2_start + iov_length (request2->stub->args.writev.vector,
                                        request2->stub->args.writev.count);

        do_overlap = ((r1_end >= r2_start) && (r2_end >= r1_start));

        return do_overlap;
}


static inline char
wb_overlap (list_head_t *list, wb_request_t *request)
{
        char          overlap = 0;
        wb_request_t *tmp     = NULL;

        GF_VALIDATE_OR_GOTO ("write-behind", list, out);
        GF_VALIDATE_OR_GOTO ("write-behind", request, out);

        list_for_each_entry (tmp, list, list) {
                if (tmp == request) {
                        break;
                }

                overlap = wb_requests_overlap (tmp, request);
                if (overlap) {
                        break;
                }
        }

out:
        return overlap;
}


static int
__wb_request_unref (wb_request_t *this)
{
        int ret = -1;

        GF_VALIDATE_OR_GOTO ("write-behind", this, out);

        if (this->refcount <= 0) {
                gf_log ("wb-request", GF_LOG_WARNING,
                        "refcount(%d) is <= 0", this->refcount);
                goto out;
        }

        ret = --this->refcount;
        if (this->refcount == 0) {
                list_del_init (&this->list);
                if (this->stub && this->stub->fop == GF_FOP_WRITE) {
                        call_stub_destroy (this->stub);
                }

                GF_FREE (this);
        }

out:
        return ret;
}


static int
wb_request_unref (wb_request_t *this)
{
        wb_inode_t *wb_inode = NULL;
        int         ret      = -1;

        GF_VALIDATE_OR_GOTO ("write-behind", this, out);

        wb_inode = this->wb_inode;

        LOCK (&wb_inode->lock);
        {
                ret = __wb_request_unref (this);
        }
        UNLOCK (&wb_inode->lock);

out:
        return ret;
}


static wb_request_t *
__wb_request_ref (wb_request_t *this)
{
        GF_VALIDATE_OR_GOTO ("write-behind", this, out);

        if (this->refcount < 0) {
                gf_log ("wb-request", GF_LOG_WARNING,
                        "refcount(%d) is < 0", this->refcount);
                this = NULL;
                goto out;
        }

        this->refcount++;

out:
        return this;
}


wb_request_t *
wb_request_ref (wb_request_t *this)
{
        wb_inode_t *wb_inode = NULL;

        GF_VALIDATE_OR_GOTO ("write-behind", this, out);

        wb_inode = this->wb_inode;
        LOCK (&wb_inode->lock);
        {
                this = __wb_request_ref (this);
        }
        UNLOCK (&wb_inode->lock);

out:
        return this;
}


wb_request_t *
wb_enqueue (wb_inode_t *wb_inode, call_stub_t *stub)
{
        wb_request_t *request = NULL, *tmp = NULL;
        call_frame_t *frame   = NULL;
        wb_local_t   *local   = NULL;
        struct iovec *vector  = NULL;
        int32_t       count   = 0;

        GF_VALIDATE_OR_GOTO ("write-behind", wb_inode, out);
        GF_VALIDATE_OR_GOTO (wb_inode->this->name, stub, out);

        request = GF_CALLOC (1, sizeof (*request), gf_wb_mt_wb_request_t);
        if (request == NULL) {
                goto out;
        }

        INIT_LIST_HEAD (&request->list);
        INIT_LIST_HEAD (&request->winds);
        INIT_LIST_HEAD (&request->unwinds);
        INIT_LIST_HEAD (&request->other_requests);

        request->stub = stub;
        request->wb_inode = wb_inode;
        request->fop  = stub->fop;

        frame = stub->frame;
        local = frame->local;
        if (local) {
                local->request = request;
        }

        if (stub->fop == GF_FOP_WRITE) {
                vector = stub->args.writev.vector;
                count = stub->args.writev.count;

                request->write_size = iov_length (vector, count);
                if (local) {
                        local->op_ret = request->write_size;
                        local->op_errno = 0;
                }

                request->flags.write_request.virgin = 1;
        }

        request->lk_owner = frame->root->lk_owner;

        LOCK (&wb_inode->lock);
        {
                list_add_tail (&request->list, &wb_inode->request);
                if (stub->fop == GF_FOP_WRITE) {
                        /* reference for stack winding */
                        __wb_request_ref (request);

                        /* reference for stack unwinding */
                        __wb_request_ref (request);

                        wb_inode->aggregate_current += request->write_size;
                } else {
                        list_for_each_entry (tmp, &wb_inode->request, list) {
                                if (tmp->stub && tmp->stub->fop
                                    == GF_FOP_WRITE) {
                                        tmp->flags.write_request.flush_all = 1;
                                }
                        }

                        /*reference for resuming */
                        __wb_request_ref (request);
                }
        }
        UNLOCK (&wb_inode->lock);

out:
        return request;
}


wb_inode_t *
__wb_inode_create (xlator_t *this, inode_t *inode)
{
        wb_inode_t *wb_inode = NULL;
        wb_conf_t  *conf     = NULL;

        GF_VALIDATE_OR_GOTO ("write-behind", this, out);
        GF_VALIDATE_OR_GOTO (this->name, inode, out);

        conf = this->private;

        wb_inode = GF_CALLOC (1, sizeof (*wb_inode), gf_wb_mt_wb_inode_t);
        if (wb_inode == NULL) {
                goto out;
        }

        INIT_LIST_HEAD (&wb_inode->request);
        INIT_LIST_HEAD (&wb_inode->passive_requests);

        wb_inode->this = this;

        wb_inode->window_conf = conf->window_size;

        LOCK_INIT (&wb_inode->lock);

        __inode_ctx_put (inode, this, (uint64_t)(unsigned long)wb_inode);

out:
        return wb_inode;
}


wb_file_t *
wb_file_create (xlator_t *this, fd_t *fd, int32_t flags)
{
        wb_file_t *file = NULL;
        wb_conf_t *conf = NULL;

        GF_VALIDATE_OR_GOTO ("write-behind", this, out);
        GF_VALIDATE_OR_GOTO (this->name, fd, out);

        conf = this->private;

        file = GF_CALLOC (1, sizeof (*file), gf_wb_mt_wb_file_t);
        if (file == NULL) {
                goto out;
        }

        /*
          fd_ref() not required, file should never decide the existence of
          an fd
        */
        file->fd= fd;
        /* If O_DIRECT then, we disable chaching */
        if (((flags & O_DIRECT) == O_DIRECT)
            || ((flags & O_ACCMODE) == O_RDONLY)
            || (((flags & O_SYNC) == O_SYNC)
                && conf->enable_O_SYNC == _gf_true)) {
                file->disabled = 1;
        }

        file->flags = flags;

        fd_ctx_set (fd, this, (uint64_t)(unsigned long)file);

out:
        return file;
}


wb_inode_t *
wb_inode_create (xlator_t *this, inode_t *inode)
{
        wb_inode_t *wb_inode = NULL;

        GF_VALIDATE_OR_GOTO ("write-behind", this, out);
        GF_VALIDATE_OR_GOTO (this->name, inode, out);

        LOCK (&inode->lock);
        {
                wb_inode = __wb_inode_ctx_get (this, inode);
                if (wb_inode == NULL) {
                        wb_inode = __wb_inode_create (this, inode);
                }
        }
        UNLOCK (&inode->lock);

out:
        return wb_inode;
}


void
wb_inode_destroy (wb_inode_t *wb_inode)
{
        GF_VALIDATE_OR_GOTO ("write-behind", wb_inode, out);

        LOCK_DESTROY (&wb_inode->lock);
        GF_FREE (wb_inode);
out:
        return;
}


int32_t
wb_lookup_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
               int32_t op_ret, int32_t op_errno, inode_t *inode,
               struct iatt *buf, dict_t *dict, struct iatt *postparent)
{
        wb_inode_t *wb_inode = NULL;

        if (op_ret < 0) {
                goto unwind;
        }

        wb_inode = wb_inode_create (this, inode);
        if (wb_inode == NULL) {
                op_ret = -1;
                op_errno = ENOMEM;
        }

unwind:
        STACK_UNWIND_STRICT (lookup, frame, op_ret, op_errno, inode, buf,
                             dict, postparent);

        return 0;
}


int32_t
wb_lookup (call_frame_t *frame, xlator_t *this, loc_t *loc,
           dict_t *xdata)
{
        STACK_WIND (frame, wb_lookup_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->lookup, loc, xdata);
        return 0;
}


int32_t
wb_sync_cbk (call_frame_t *frame, void *cookie, xlator_t *this, int32_t op_ret,
             int32_t op_errno, struct iatt *prebuf, struct iatt *postbuf,
             dict_t *xdata)
{
        wb_local_t   *local             = NULL;
        list_head_t  *winds             = NULL;
        wb_inode_t   *wb_inode          = NULL;
        wb_request_t *request           = NULL, *dummy = NULL;
        wb_local_t   *per_request_local = NULL;
        int32_t       ret               = -1;
	int32_t	      total_write_size	= 0;
        fd_t         *fd                = NULL;

        GF_ASSERT (frame);
        GF_ASSERT (this);

        local = frame->local;
        winds = &local->winds;

        fd = local->fd;

        wb_inode = wb_inode_ctx_get (this, fd->inode);
        GF_VALIDATE_OR_GOTO (this->name, wb_inode, out);

        LOCK (&wb_inode->lock);
        {
                list_for_each_entry_safe (request, dummy, winds, winds) {
                        request->flags.write_request.got_reply = 1;

                        if (!request->flags.write_request.write_behind
                            && (op_ret == -1)) {
                                per_request_local = request->stub->frame->local;
                                per_request_local->op_ret = op_ret;
                                per_request_local->op_errno = op_errno;
                        }

                        if (request->flags.write_request.write_behind) {
                                wb_inode->window_current -= request->write_size;
				total_write_size += request->write_size;
                        }

                        __wb_request_unref (request);
                }

                if (op_ret == -1) {
                        wb_inode->op_ret = op_ret;
                        wb_inode->op_errno = op_errno;
		} else if (op_ret < total_write_size) {
			/*
			 * We've encountered a short write, for whatever reason.
			 * Set an EIO error for the next fop. This should be
			 * valid for writev or flush (close).
			 *
			 * TODO: Retry the write so we can potentially capture
			 * a real error condition (i.e., ENOSPC).
			 */
			wb_inode->op_ret = -1;
			wb_inode->op_errno = EIO;
		}
        }
        UNLOCK (&wb_inode->lock);

        ret = wb_process_queue (frame, wb_inode);
        if (ret == -1) {
                if (errno == ENOMEM) {
                        LOCK (&wb_inode->lock);
                        {
                                wb_inode->op_ret = -1;
                                wb_inode->op_errno = ENOMEM;
                        }
                        UNLOCK (&wb_inode->lock);
                }

                gf_log (this->name, GF_LOG_WARNING,
                        "request queue processing failed");
        }

        /* safe place to do fd_unref */
        fd_unref (fd);

        frame->local = NULL;

        if (local != NULL) {
                mem_put (frame->local);
        }

        STACK_DESTROY (frame->root);

out:
        return 0;
}


ssize_t
wb_sync (call_frame_t *frame, wb_inode_t *wb_inode, list_head_t *winds)
{
        wb_request_t  *dummy                = NULL, *request = NULL;
        wb_request_t  *first_request        = NULL, *next = NULL;
        size_t         total_count          = 0, count = 0;
        size_t         copied               = 0;
        call_frame_t  *sync_frame           = NULL;
        struct iobref *iobref               = NULL;
        wb_local_t    *local                = NULL;
        struct iovec  *vector               = NULL;
        ssize_t        current_size         = 0, bytes = 0;
        size_t         bytecount            = 0;
        wb_conf_t     *conf                 = NULL;
        fd_t          *fd                   = NULL;
        int32_t        op_errno             = -1;
        off_t          next_offset_expected = 0;
        gf_lkowner_t   lk_owner             = {0, };

        GF_VALIDATE_OR_GOTO_WITH_ERROR ((wb_inode ? wb_inode->this->name
                                         : "write-behind"), frame,
                                        out, bytes, -1);
        GF_VALIDATE_OR_GOTO_WITH_ERROR (frame->this->name, wb_inode, out, bytes,
                                        -1);
        GF_VALIDATE_OR_GOTO_WITH_ERROR (frame->this->name, winds, out, bytes,
                                        -1);

        conf = wb_inode->this->private;
        list_for_each_entry (request, winds, winds) {
                total_count += request->stub->args.writev.count;
                if (total_count > 0) {
                        break;
                }
        }

        if (total_count == 0) {
                gf_log (wb_inode->this->name, GF_LOG_TRACE,
                        "no vectors are to be synced");
                goto out;
        }

        list_for_each_entry_safe (request, dummy, winds, winds) {
                if (!vector) {
                        vector = GF_MALLOC (VECTORSIZE (MAX_VECTOR_COUNT),
                                            gf_wb_mt_iovec);
                        if (vector == NULL) {
                                bytes = -1;
                                op_errno = ENOMEM;
                                goto out;
                        }

                        iobref = iobref_new ();
                        if (iobref == NULL) {
                                bytes = -1;
                                op_errno = ENOMEM;
                                goto out;
                        }

                        local = mem_get0 (THIS->local_pool);
                        if (local == NULL) {
                                bytes = -1;
                                op_errno = ENOMEM;
                                goto out;
                        }

                        INIT_LIST_HEAD (&local->winds);

                        first_request = request;
                        current_size = 0;

                        next_offset_expected = request->stub->args.writev.off
                                + request->write_size;
                        lk_owner = request->lk_owner;
                }

                count += request->stub->args.writev.count;
                bytecount = VECTORSIZE (request->stub->args.writev.count);
                memcpy (((char *)vector)+copied,
                        request->stub->args.writev.vector,
                        bytecount);
                copied += bytecount;

                current_size += request->write_size;

                if (request->stub->args.writev.iobref) {
                        iobref_merge (iobref,
                                      request->stub->args.writev.iobref);
                }

                next = NULL;
                if (request->winds.next != winds) {
                        next = list_entry (request->winds.next,
                                           wb_request_t, winds);
                }

                list_del_init (&request->winds);
                list_add_tail (&request->winds, &local->winds);

                if ((!next)
                    || ((count + next->stub->args.writev.count)
                        > MAX_VECTOR_COUNT)
                    || ((current_size + next->write_size)
                        > conf->aggregate_size)
                    || (next_offset_expected != next->stub->args.writev.off)
                    || (!is_same_lkowner (&lk_owner, &next->lk_owner))
                    || (request->stub->args.writev.fd
                        != next->stub->args.writev.fd)) {

                        sync_frame = copy_frame (frame);
                        if (sync_frame == NULL) {
                                bytes = -1;
                                op_errno = ENOMEM;
                                goto out;
                        }

                        frame->root->lk_owner = lk_owner;

                        local->wb_inode = wb_inode;
                        sync_frame->local = local;

                        local->fd = fd = fd_ref (request->stub->args.writev.fd);

                        bytes += current_size;
                        STACK_WIND (sync_frame, wb_sync_cbk,
                                    FIRST_CHILD(sync_frame->this),
                                    FIRST_CHILD(sync_frame->this)->fops->writev,
                                    fd, vector, count,
                                    first_request->stub->args.writev.off,
                                    first_request->stub->args.writev.flags,
                                    iobref, NULL);

                        iobref_unref (iobref);
                        GF_FREE (vector);
                        first_request = NULL;
                        iobref = NULL;
                        vector = NULL;
                        sync_frame = NULL;
                        local = NULL;
                        copied = count = 0;
                }
        }

out:
        if (sync_frame != NULL) {
                sync_frame->local = NULL;
                STACK_DESTROY (sync_frame->root);
        }

        if (local != NULL) {
                /* had we winded these requests, we would have unrefed
                 * in wb_sync_cbk.
                 */
                list_for_each_entry_safe (request, dummy, &local->winds,
                                          winds) {
                        wb_request_unref (request);
                }

                mem_put (local);
                local = NULL;
        }

        if (iobref != NULL) {
                iobref_unref (iobref);
        }

        GF_FREE (vector);

        if (bytes == -1) {
                /*
                 * had we winded these requests, we would have unrefed
                 * in wb_sync_cbk.
                 */
                if (local) {
                        list_for_each_entry_safe (request, dummy, &local->winds,
                                                  winds) {
                                wb_request_unref (request);
                        }
                }

                if (wb_inode != NULL) {
                        LOCK (&wb_inode->lock);
                        {
                                wb_inode->op_ret = -1;
                                wb_inode->op_errno = op_errno;
                        }
                        UNLOCK (&wb_inode->lock);
                }
        }

        return bytes;
}


int32_t
wb_stat_cbk (call_frame_t *frame, void *cookie, xlator_t *this, int32_t op_ret,
             int32_t op_errno, struct iatt *buf, dict_t *xdata)
{
        wb_local_t   *local         = NULL;
        wb_request_t *request       = NULL;
        call_frame_t *process_frame = NULL;
        wb_inode_t   *wb_inode      = NULL;
        int32_t       ret           = -1;

        GF_ASSERT (frame);
        GF_ASSERT (this);

        local = frame->local;
        wb_inode = local->wb_inode;

        request = local->request;
        if (request) {
                process_frame = copy_frame (frame);
                if (process_frame == NULL) {
                        op_ret = -1;
                        op_errno = ENOMEM;
                }
        }

        STACK_UNWIND_STRICT (stat, frame, op_ret, op_errno, buf, xdata);

        if (request != NULL) {
                wb_request_unref (request);
        }

        if (process_frame != NULL) {
                ret = wb_process_queue (process_frame, wb_inode);
                if (ret == -1) {
                        if ((errno == ENOMEM) && (wb_inode != NULL)) {
                                LOCK (&wb_inode->lock);
                                {
                                        wb_inode->op_ret = -1;
                                        wb_inode->op_errno = ENOMEM;
                                }
                                UNLOCK (&wb_inode->lock);
                        }

                        gf_log (this->name, GF_LOG_WARNING,
                                "request queue processing failed");
                }

                STACK_DESTROY (process_frame->root);
        }

        return 0;
}


static int32_t
wb_stat_helper (call_frame_t *frame, xlator_t *this, loc_t *loc, dict_t *xdata)
{
        GF_ASSERT (frame);
        GF_ASSERT (this);

        STACK_WIND (frame, wb_stat_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->stat, loc, xdata);
        return 0;
}


int32_t
wb_stat (call_frame_t *frame, xlator_t *this, loc_t *loc, dict_t *xdata)
{
        wb_inode_t   *wb_inode     = NULL;
        wb_local_t   *local        = NULL;
        call_stub_t  *stub         = NULL;
        wb_request_t *request      = NULL;
        int32_t       ret          = -1, op_errno = EINVAL;

        GF_ASSERT (frame);
        GF_VALIDATE_OR_GOTO (frame->this->name, this, unwind);
        GF_VALIDATE_OR_GOTO (frame->this->name, loc, unwind);

        if (loc->inode) {
                wb_inode = wb_inode_ctx_get (this, loc->inode);
        }

        local = mem_get0 (this->local_pool);
        if (local == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        local->wb_inode = wb_inode;

        frame->local = local;

        if (wb_inode) {
                stub = fop_stat_stub (frame, wb_stat_helper, loc, xdata);
                if (stub == NULL) {
                        op_errno = ENOMEM;
                        goto unwind;
                }

                request = wb_enqueue (wb_inode, stub);
                if (request == NULL) {
                        op_errno = ENOMEM;
                        goto unwind;
                }

                ret = wb_process_queue (frame, wb_inode);
                if (ret == -1) {
                        gf_log (this->name, GF_LOG_WARNING,
                                "request queue processing failed");
                }
        } else {
                STACK_WIND (frame, wb_stat_cbk, FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->stat, loc, xdata);
        }

        return 0;
unwind:
        STACK_UNWIND_STRICT (stat, frame, -1, op_errno, NULL, NULL);

        if (stub) {
                call_stub_destroy (stub);
        }

        return 0;
}


int32_t
wb_fstat_cbk (call_frame_t *frame, void *cookie, xlator_t *this, int32_t op_ret,
              int32_t op_errno, struct iatt *buf, dict_t *xdata)
{
        wb_local_t   *local    = NULL;
        wb_request_t *request  = NULL;
        wb_inode_t   *wb_inode = NULL;
        int32_t       ret      = -1;

        GF_ASSERT (frame);

        local = frame->local;
        wb_inode = local->wb_inode;

        request = local->request;
        if ((wb_inode != NULL) && (request != NULL)) {
                wb_request_unref (request);
                ret = wb_process_queue (frame, wb_inode);
                if (ret == -1) {
                        if (errno == ENOMEM) {
                                op_ret = -1;
                                op_errno = ENOMEM;
                        }

                        gf_log (this->name, GF_LOG_WARNING,
                                "request queue processing failed");
                }
        }

        STACK_UNWIND_STRICT (fstat, frame, op_ret, op_errno, buf, xdata);

        return 0;
}


int32_t
wb_fstat_helper (call_frame_t *frame, xlator_t *this, fd_t *fd, dict_t *xdata)
{
        GF_ASSERT (frame);
        GF_ASSERT (this);

        STACK_WIND (frame, wb_fstat_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->fstat, fd, xdata);
        return 0;
}


int32_t
wb_fstat (call_frame_t *frame, xlator_t *this, fd_t *fd, dict_t *xdata)
{
        wb_inode_t   *wb_inode     = NULL;
        wb_local_t   *local        = NULL;
        call_stub_t  *stub         = NULL;
        wb_request_t *request      = NULL;
        int32_t       ret          = -1;
        int           op_errno     = EINVAL;

        GF_ASSERT (frame);
        GF_VALIDATE_OR_GOTO (frame->this->name, this, unwind);
        GF_VALIDATE_OR_GOTO (frame->this->name, fd, unwind);

        wb_inode = wb_inode_ctx_get (this, fd->inode);
        if ((!IA_ISDIR (fd->inode->ia_type)) && (wb_inode == NULL)) {
                gf_log (this->name, GF_LOG_WARNING,
                        "wb_inode not found for fd %p", fd);
                op_errno = EBADFD;
                goto unwind;
        }

        local = mem_get0 (this->local_pool);
        if (local == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        local->wb_inode = wb_inode;

        frame->local = local;

        if (wb_inode) {
                stub = fop_fstat_stub (frame, wb_fstat_helper, fd, xdata);
                if (stub == NULL) {
                        op_errno = ENOMEM;
                        goto unwind;
                }

                request = wb_enqueue (wb_inode, stub);
                if (request == NULL) {
                        op_errno = ENOMEM;
                        goto unwind;
                }

                /*
                  FIXME:should the request queue be emptied in case of error?
                */
                ret = wb_process_queue (frame, wb_inode);
                if (ret == -1) {
                        gf_log (this->name, GF_LOG_WARNING,
                                "request queue processing failed");
                }
        } else {
                STACK_WIND (frame, wb_fstat_cbk, FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->fstat, fd, xdata);
        }

        return 0;

unwind:
        STACK_UNWIND_STRICT (fstat, frame, -1, op_errno, NULL, NULL);

        if (stub) {
                call_stub_destroy (stub);
        }

        return 0;
}


int32_t
wb_truncate_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                 int32_t op_ret, int32_t op_errno, struct iatt *prebuf,
                 struct iatt *postbuf, dict_t *xdata)
{
        wb_local_t   *local         = NULL;
        wb_request_t *request       = NULL;
        wb_inode_t   *wb_inode      = NULL;
        call_frame_t *process_frame = NULL;
        int32_t       ret           = -1;

        GF_ASSERT (frame);

        local = frame->local;
        wb_inode = local->wb_inode;
        request = local->request;

        if ((request != NULL) && (wb_inode != NULL)) {
                process_frame = copy_frame (frame);
                if (process_frame == NULL) {
                        op_ret = -1;
                        op_errno = ENOMEM;
                }
        }

        STACK_UNWIND_STRICT (truncate, frame, op_ret, op_errno, prebuf,
                             postbuf, xdata);

        if (request) {
                wb_request_unref (request);
        }

        if (process_frame != NULL) {
                ret = wb_process_queue (process_frame, wb_inode);
                if (ret == -1) {
                        if ((errno == ENOMEM) && (wb_inode != NULL)) {
                                LOCK (&wb_inode->lock);
                                {
                                        wb_inode->op_ret = -1;
                                        wb_inode->op_errno = ENOMEM;
                                }
                                UNLOCK (&wb_inode->lock);
                        }

                        gf_log (this->name, GF_LOG_WARNING,
                                "request queue processing failed");
                }

                STACK_DESTROY (process_frame->root);
        }

        return 0;
}


static int32_t
wb_truncate_helper (call_frame_t *frame, xlator_t *this, loc_t *loc,
                    off_t offset, dict_t *xdata)
{
        GF_ASSERT (frame);
        GF_ASSERT (this);

        STACK_WIND (frame, wb_truncate_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->truncate, loc, offset, xdata);

        return 0;
}


int32_t
wb_truncate (call_frame_t *frame, xlator_t *this, loc_t *loc, off_t offset,
             dict_t *xdata)
{
        wb_inode_t   *wb_inode     = NULL;
        wb_local_t   *local        = NULL;
        call_stub_t  *stub         = NULL;
        wb_request_t *request      = NULL;
        int32_t       ret          = -1, op_errno = EINVAL;

        GF_ASSERT (frame);
        GF_VALIDATE_OR_GOTO (frame->this->name, this, unwind);
        GF_VALIDATE_OR_GOTO (frame->this->name, loc, unwind);

        if (loc->inode) {
                wb_inode = wb_inode_ctx_get (this, loc->inode);
        }

        local = mem_get0 (this->local_pool);
        if (local == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        local->wb_inode = wb_inode;

        frame->local = local;
        if (wb_inode) {
                stub = fop_truncate_stub (frame, wb_truncate_helper, loc,
                                          offset, xdata);
                if (stub == NULL) {
                        op_errno = ENOMEM;
                        goto unwind;
                }

                request = wb_enqueue (wb_inode, stub);
                if (request == NULL) {
                        op_errno = ENOMEM;
                        goto unwind;
                }

                ret = wb_process_queue (frame, wb_inode);
                if (ret == -1) {
                        gf_log (this->name, GF_LOG_WARNING,
                                "request queue processing failed");
                }
        } else {
                STACK_WIND (frame, wb_truncate_cbk, FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->truncate, loc, offset,
                            xdata);
        }

        return 0;

unwind:
        STACK_UNWIND_STRICT (truncate, frame, -1, op_errno, NULL, NULL, NULL);

        if (stub) {
                call_stub_destroy (stub);
        }

        return 0;
}


int32_t
wb_ftruncate_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                  int32_t op_ret, int32_t op_errno, struct iatt *prebuf,
                  struct iatt *postbuf, dict_t *xdata)
{
        wb_local_t   *local    = NULL;
        wb_request_t *request  = NULL;
        wb_inode_t   *wb_inode = NULL;
        int32_t       ret      = -1;

        GF_ASSERT (frame);

        local = frame->local;
        wb_inode = local->wb_inode;
        request = local->request;

        if ((request != NULL) && (wb_inode != NULL)) {
                wb_request_unref (request);
                ret = wb_process_queue (frame, wb_inode);
                if (ret == -1) {
                        if (errno == ENOMEM) {
                                op_ret = -1;
                                op_errno = ENOMEM;
                        }

                        gf_log (this->name, GF_LOG_WARNING,
                                "request queue processing failed");
                }
        }

        STACK_UNWIND_STRICT (ftruncate, frame, op_ret, op_errno, prebuf,
                             postbuf, xdata);

        return 0;
}


static int32_t
wb_ftruncate_helper (call_frame_t *frame, xlator_t *this, fd_t *fd,
                     off_t offset, dict_t *xdata)
{
        GF_ASSERT (frame);
        GF_ASSERT (this);

        STACK_WIND (frame, wb_ftruncate_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->ftruncate, fd, offset, xdata);
        return 0;
}


int32_t
wb_ftruncate (call_frame_t *frame, xlator_t *this, fd_t *fd, off_t offset,
              dict_t *xdata)
{
        wb_inode_t   *wb_inode     = NULL;
        wb_local_t   *local        = NULL;
        call_stub_t  *stub         = NULL;
        wb_request_t *request      = NULL;
        int32_t       ret          = -1;
        int           op_errno     = EINVAL;

        GF_ASSERT (frame);
        GF_VALIDATE_OR_GOTO (frame->this->name, this, unwind);
        GF_VALIDATE_OR_GOTO (frame->this->name, fd, unwind);

        wb_inode = wb_inode_ctx_get (this, fd->inode);
        if ((!IA_ISDIR (fd->inode->ia_type)) && (wb_inode == NULL)) {
                gf_log (this->name, GF_LOG_WARNING,
                        "wb_inode not found for fd %p", fd);
                op_errno = EBADFD;
                goto unwind;
        }

        local = mem_get0 (this->local_pool);
        if (local == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        local->wb_inode = wb_inode;

        frame->local = local;

        if (wb_inode) {
                stub = fop_ftruncate_stub (frame, wb_ftruncate_helper, fd,
                                           offset, xdata);
                if (stub == NULL) {
                        op_errno = ENOMEM;
                        goto unwind;
                }

                request = wb_enqueue (wb_inode, stub);
                if (request == NULL) {
                        op_errno = ENOMEM;
                        goto unwind;
                }

                ret = wb_process_queue (frame, wb_inode);
                if (ret == -1) {
                        gf_log (this->name, GF_LOG_WARNING,
                                "request queue processing failed");
                }
        } else {
                STACK_WIND (frame, wb_ftruncate_cbk, FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->ftruncate, fd, offset, xdata);
        }

        return 0;

unwind:
        STACK_UNWIND_STRICT (ftruncate, frame, -1, op_errno, NULL, NULL, NULL);

        if (stub) {
                call_stub_destroy (stub);
        }

        return 0;
}


int32_t
wb_setattr_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                int32_t op_ret, int32_t op_errno, struct iatt *statpre,
                struct iatt *statpost, dict_t *xdata)
{
        wb_local_t   *local         = NULL;
        wb_request_t *request       = NULL;
        call_frame_t *process_frame = NULL;
        wb_inode_t   *wb_inode      = NULL;
        int32_t       ret           = -1;

        GF_ASSERT (frame);

        local = frame->local;
        wb_inode = local->wb_inode;
        request = local->request;

        if (request) {
                process_frame = copy_frame (frame);
                if (process_frame == NULL) {
                        op_ret = -1;
                        op_errno = ENOMEM;
                }
        }

        STACK_UNWIND_STRICT (setattr, frame, op_ret, op_errno, statpre,
                             statpost, xdata);

        if (request) {
                wb_request_unref (request);
        }

        if (request && (process_frame != NULL)) {
                ret = wb_process_queue (process_frame, wb_inode);
                if (ret == -1) {
                        if ((errno == ENOMEM) && (wb_inode != NULL)) {
                                LOCK (&wb_inode->lock);
                                {
                                        wb_inode->op_ret = -1;
                                        wb_inode->op_errno = ENOMEM;
                                }
                                UNLOCK (&wb_inode->lock);
                        }

                        gf_log (this->name, GF_LOG_WARNING,
                                "request queue processing failed");
                }

                STACK_DESTROY (process_frame->root);
        }

        return 0;
}


static int32_t
wb_setattr_helper (call_frame_t *frame, xlator_t *this, loc_t *loc,
                   struct iatt *stbuf, int32_t valid, dict_t *xdata)
{
        GF_ASSERT (frame);
        GF_ASSERT (this);

        STACK_WIND (frame, wb_setattr_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->setattr, loc, stbuf, valid, xdata);
        return 0;
}


int32_t
wb_setattr (call_frame_t *frame, xlator_t *this, loc_t *loc,
            struct iatt *stbuf, int32_t valid, dict_t *xdata)
{
        wb_inode_t   *wb_inode     = NULL;
        wb_local_t   *local        = NULL;
        call_stub_t  *stub         = NULL;
        wb_request_t *request      = NULL;
        int32_t       ret          = -1, op_errno = EINVAL;

        GF_ASSERT (frame);
        GF_VALIDATE_OR_GOTO (frame->this->name, this, unwind);
        GF_VALIDATE_OR_GOTO (frame->this->name, loc, unwind);

        local = mem_get0 (this->local_pool);
        if (local == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        frame->local = local;

        if (loc->inode) {
                wb_inode = wb_inode_ctx_get (this, loc->inode);
        }

        local->wb_inode = wb_inode;

        if (wb_inode) {
                stub = fop_setattr_stub (frame, wb_setattr_helper, loc, stbuf,
                                         valid, xdata);
                if (stub == NULL) {
                        op_errno = ENOMEM;
                        goto unwind;
                }

                request = wb_enqueue (wb_inode, stub);
                if (request == NULL) {
                        op_errno = ENOMEM;
                        goto unwind;
                }

                ret = wb_process_queue (frame, wb_inode);
                if (ret == -1) {
                        gf_log (this->name, GF_LOG_WARNING,
                                "request queue processing failed");
                }
        } else {
                STACK_WIND (frame, wb_setattr_cbk, FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->setattr, loc, stbuf,
                            valid, xdata);
        }

        return 0;
unwind:
        STACK_UNWIND_STRICT (setattr, frame, -1, op_errno, NULL, NULL, NULL);

        if (stub) {
                call_stub_destroy (stub);
        }

        return 0;
}

void
wb_disable_all (xlator_t *this, fd_t *origfd)
{
        inode_t   *inode   = NULL;
        fd_t      *otherfd = NULL;
        wb_file_t *wb_file = NULL;

        inode = origfd->inode;

        LOCK(&inode->lock);
        {
                list_for_each_entry (otherfd, &inode->fd_list, inode_list) {
                        if (otherfd == origfd) {
                                continue;
                        }

                        wb_file = wb_fd_ctx_get (this, otherfd);
                        if (wb_file == NULL) {
                                continue;
                        }

                        gf_log(this->name,GF_LOG_DEBUG,
                               "disabling wb on %p because %p is O_SYNC",
                               otherfd, origfd);
                        wb_file->disabled = 1;
                }
        }
        UNLOCK(&inode->lock);
}

int32_t
wb_open_cbk (call_frame_t *frame, void *cookie, xlator_t *this, int32_t op_ret,
             int32_t op_errno, fd_t *fd, dict_t *xdata)
{
        int32_t     flags   = 0;
        wb_file_t  *file    = NULL;
        wb_local_t *local   = NULL;

        GF_ASSERT (frame);
        GF_VALIDATE_OR_GOTO_WITH_ERROR (frame->this->name, this, out, op_errno,
                                        EINVAL);
        local = frame->local;
        GF_VALIDATE_OR_GOTO_WITH_ERROR (this->name, local, out, op_errno,
                                        EINVAL);

        flags = local->flags;

        if (op_ret != -1) {
                file = wb_file_create (this, fd, flags);
                if (file == NULL) {
                        op_ret = -1;
                        op_errno = ENOMEM;
                        goto out;
                }
        }

out:
        frame->local = NULL;
        if (local != NULL) {
                mem_put (local);
        }

        STACK_UNWIND_STRICT (open, frame, op_ret, op_errno, fd, xdata);
        return 0;
}


int32_t
wb_open (call_frame_t *frame, xlator_t *this, loc_t *loc, int32_t flags,
         fd_t *fd, dict_t *xdata)
{
        wb_local_t *local    = NULL;
        int32_t     op_errno = EINVAL;

        local = mem_get0 (this->local_pool);
        if (local == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        local->flags = flags;

        frame->local = local;

        STACK_WIND (frame, wb_open_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->open, loc, flags, fd, xdata);
        return 0;

unwind:
        STACK_UNWIND_STRICT (open, frame, -1, op_errno, NULL, NULL);
        return 0;
}


int32_t
wb_create_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
               int32_t op_ret, int32_t op_errno, fd_t *fd, inode_t *inode,
               struct iatt *buf, struct iatt *preparent,
               struct iatt *postparent, dict_t *xdata)
{
        long        flags    = 0;
        wb_inode_t *wb_inode = NULL;
        wb_file_t  *file     = NULL;
        wb_local_t *local    = NULL;

        GF_ASSERT (frame);
        GF_VALIDATE_OR_GOTO_WITH_ERROR (frame->this->name, this, out,
                                        op_errno, EINVAL);

        if (op_ret != -1) {
                if (frame->local) {
                        flags = (long) frame->local;
                }

                file = wb_file_create (this, fd, flags);
                if (file == NULL) {
                        op_ret = -1;
                        op_errno = ENOMEM;
                        goto out;
                }

                LOCK (&inode->lock);
                {
                        wb_inode = __wb_inode_create (this, inode);
                        if (wb_inode == NULL) {
                                op_ret = -1;
                                op_errno = ENOMEM;
                        }
                }
                UNLOCK (&inode->lock);
        }

        frame->local = NULL;

out:
        STACK_UNWIND_STRICT (create, frame, op_ret, op_errno, fd, inode, buf,
                             preparent, postparent, xdata);

        if (local != NULL) {
                mem_put (local);
        }

        return 0;
}


int32_t
wb_create (call_frame_t *frame, xlator_t *this, loc_t *loc, int32_t flags,
           mode_t mode, mode_t umask, fd_t *fd, dict_t *xdata)
{
        int32_t     op_errno = EINVAL;
        wb_local_t *local    = NULL;

        GF_ASSERT (frame);
        GF_VALIDATE_OR_GOTO (frame->this->name, this, unwind);
        GF_VALIDATE_OR_GOTO (frame->this->name, fd, unwind);
        GF_VALIDATE_OR_GOTO (frame->this->name, loc, unwind);

        local = mem_get0 (this->local_pool);
        if (local == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        local->flags = flags;

        frame->local = local;

        STACK_WIND (frame, wb_create_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->create,
                    loc, flags, mode, umask, fd, xdata);
        return 0;

unwind:
        STACK_UNWIND_STRICT (create, frame, -1, op_errno, NULL, NULL, NULL,
                             NULL, NULL, NULL);
        return 0;
}


/* Mark all the contiguous write requests for winding starting from head of
 * request list. Stops marking at the first non-write request found. If
 * file is opened with O_APPEND, make sure all the writes marked for winding
 * will fit into a single write call to server.
 */
size_t
__wb_mark_wind_all (wb_inode_t *wb_inode, list_head_t *list, list_head_t *winds)
{
        wb_request_t     *request       = NULL, *prev_request = NULL;
        wb_file_t        *wb_file       = NULL, *prev_wb_file = NULL;
        wb_file_t        *last_wb_file  = NULL;
        size_t            size          = 0;
        char              first_request = 1, overlap = 0;
        wb_conf_t        *conf          = NULL;
        int               count         = 0;
        enum _gf_boolean  dont_wind_set = 0;

        GF_VALIDATE_OR_GOTO ("write-behind", wb_inode, out);
        GF_VALIDATE_OR_GOTO (wb_inode->this->name, list, out);
        GF_VALIDATE_OR_GOTO (wb_inode->this->name, winds, out);

        conf = wb_inode->this->private;

        list_for_each_entry (request, list, list)
        {
                if ((request->stub == NULL)
                    || (request->stub->fop != GF_FOP_WRITE)) {
                        break;
                }

                wb_file = wb_fd_ctx_get (wb_inode->this,
                                         request->stub->args.writev.fd);
                if (wb_file == NULL) {
                        gf_log (wb_inode->this->name, GF_LOG_WARNING,
                                "write behind wb_file pointer is"
                                " not stored in context of fd(%p)",
                                request->stub->args.writev.fd);
                        goto out;
                }

                /* If write requests from two fds are interleaved, for
                 * each of them, we can only send first set of adjacent
                 * requests that are on same fd. This is because, fds
                 * with O_APPEND cannot have more than one write fop in
                 * progress while syncing, so that order is not messed
                 * up. Since we group adjacent requests with same fd into
                 * single write call whenever possible, we need the above said
                 * measure.
                 */
                if ((prev_wb_file != NULL) && (prev_wb_file->flags & O_APPEND)
                    && (prev_request->stub->args.writev.fd
                        != request->stub->args.writev.fd)
                    && (!prev_wb_file->dont_wind)) {
                        prev_wb_file->dont_wind = 1;
                        dont_wind_set = 1;
                        last_wb_file = prev_wb_file;
                }

                prev_request = request;
                prev_wb_file = wb_file;

                if (!request->flags.write_request.stack_wound) {
                        if (first_request) {
                                first_request = 0;
                        } else {
                                overlap = wb_overlap (list, request);
                                if (overlap) {
                                        continue;
                                }
                        }

                        if ((wb_file->flags & O_APPEND)
                            && (((size + request->write_size)
                                 > conf->aggregate_size)
                                || ((count + request->stub->args.writev.count)
                                    > MAX_VECTOR_COUNT)
                                || (wb_file->dont_wind))) {
                                continue;
                        }

                        size += request->write_size;

                        wb_inode->aggregate_current -= request->write_size;

                        count += request->stub->args.writev.count;

                        request->flags.write_request.stack_wound = 1;
                        list_add_tail (&request->winds, winds);
                }
        }

out:
        if (wb_inode != NULL) {
                wb_inode->aggregate_current -= size;
        }

        if (dont_wind_set && (list != NULL)) {
                list_for_each_entry (request, list, list) {
                        wb_file = wb_fd_ctx_get (wb_inode->this,
                                                 request->stub->args.writev.fd);
                        if (wb_file != NULL) {
                                wb_file->dont_wind = 0;
                        }

                        if (wb_file == last_wb_file) {
                                break;
                        }
                }
        }

        return size;
}


int32_t
__wb_can_wind (list_head_t *list, char *other_fop_in_queue,
               char *overlapping_writes, char *incomplete_writes,
               char *wind_all)
{
        wb_request_t *request         = NULL;
        char          first_request   = 1;
        int32_t       ret             = -1;
        char          overlap         = 0;

        GF_VALIDATE_OR_GOTO ("write-behind", list, out);

        list_for_each_entry (request, list, list)
        {
                if ((request->stub == NULL)
                    || (request->stub->fop != GF_FOP_WRITE)) {
                        if (request->stub && other_fop_in_queue) {
                                *other_fop_in_queue = 1;
                        }
                        break;
                }

                if (request->flags.write_request.stack_wound
                    && !request->flags.write_request.got_reply
                    && (incomplete_writes != NULL)) {
                        *incomplete_writes = 1;
                        break;
                }

                if (!request->flags.write_request.stack_wound) {
                        if (first_request) {
                                char flush = 0;
                                first_request = 0;

                                flush = request->flags.write_request.flush_all;
                                if (wind_all != NULL) {
                                        *wind_all = flush;
                                }
                        }

                        overlap = wb_overlap (list, request);
                        if (overlap) {
                                if (overlapping_writes != NULL) {
                                        *overlapping_writes = 1;
                                }

                                break;
                        }
                }
        }

        ret = 0;
out:
        return ret;
}


ssize_t
__wb_mark_winds (list_head_t *list, list_head_t *winds, size_t aggregate_conf,
                 char enable_trickling_writes)
{
        size_t        size                   = 0;
        char          other_fop_in_queue     = 0;
        char          incomplete_writes      = 0;
        char          overlapping_writes     = 0;
        wb_request_t *request                = NULL;
        wb_inode_t   *wb_inode               = NULL;
        char          wind_all               = 0;
        int32_t       ret                    = 0;

        GF_VALIDATE_OR_GOTO ("write-behind", list, out);
        GF_VALIDATE_OR_GOTO ("write-behind", winds, out);

        if (list_empty (list)) {
                goto out;
        }

        request = list_entry (list->next, typeof (*request), list);
        wb_inode = request->wb_inode;

        ret = __wb_can_wind (list, &other_fop_in_queue,
                             &overlapping_writes, &incomplete_writes,
                             &wind_all);
        if (ret == -1) {
                gf_log (wb_inode->this->name, GF_LOG_WARNING,
                        "cannot decide whether to wind or not");
                goto out;
        }

        if (!incomplete_writes && ((enable_trickling_writes)
                                   || (wind_all) || (overlapping_writes)
                                   || (other_fop_in_queue)
                                   || (wb_inode->aggregate_current
                                       >= aggregate_conf))) {
                size = __wb_mark_wind_all (wb_inode, list, winds);
        }

out:
        return size;
}


size_t
__wb_mark_unwind_till (list_head_t *list, list_head_t *unwinds, size_t size)
{
        size_t        written_behind = 0;
        wb_request_t *request        = NULL;
        wb_inode_t   *wb_inode       = NULL;

        if (list_empty (list)) {
                goto out;
        }

        request = list_entry (list->next, typeof (*request), list);
        wb_inode = request->wb_inode;

        list_for_each_entry (request, list, list)
        {
                if ((request->stub == NULL)
                    || (request->stub->fop != GF_FOP_WRITE)) {
                        continue;
                }

                if (written_behind <= size) {
                        if (!request->flags.write_request.write_behind) {
                                written_behind += request->write_size;
                                request->flags.write_request.write_behind = 1;
                                list_add_tail (&request->unwinds, unwinds);

                                if (!request->flags.write_request.got_reply) {
                                        wb_inode->window_current
                                                += request->write_size;
                                }
                        }
                } else {
                        break;
                }
        }

out:
        return written_behind;
}


void
__wb_mark_unwinds (list_head_t *list, list_head_t *unwinds)
{
        wb_request_t *request  = NULL;
        wb_inode_t   *wb_inode = NULL;

        GF_VALIDATE_OR_GOTO ("write-behind", list, out);
        GF_VALIDATE_OR_GOTO ("write-behind", unwinds, out);

        if (list_empty (list)) {
                goto out;
        }

        request = list_entry (list->next, typeof (*request), list);
        wb_inode = request->wb_inode;

        if (wb_inode->window_current <= wb_inode->window_conf) {
                __wb_mark_unwind_till (list, unwinds,
                                       wb_inode->window_conf
                                       - wb_inode->window_current);
        }

out:
        return;
}


uint32_t
__wb_get_other_requests (list_head_t *list, list_head_t *other_requests)
{
        wb_request_t *request = NULL;
        uint32_t      count   = 0;

        GF_VALIDATE_OR_GOTO ("write-behind", list, out);
        GF_VALIDATE_OR_GOTO ("write-behind", other_requests, out);

        list_for_each_entry (request, list, list) {
                if ((request->stub == NULL)
                    || (request->stub->fop == GF_FOP_WRITE)) {
                        break;
                }

                if (!request->flags.other_requests.marked_for_resume) {
                        request->flags.other_requests.marked_for_resume = 1;
                        list_add_tail (&request->other_requests,
                                       other_requests);
                        count++;
                }
        }

out:
        return count;
}


int32_t
wb_stack_unwind (list_head_t *unwinds)
{
        struct iatt   buf     = {0,};
        wb_request_t *request = NULL, *dummy = NULL;
        call_frame_t *frame   = NULL;
        wb_local_t   *local   = NULL;
        int           ret     = 0, write_requests_removed = 0;

        GF_VALIDATE_OR_GOTO ("write-behind", unwinds, out);

        list_for_each_entry_safe (request, dummy, unwinds, unwinds) {
                frame = request->stub->frame;
                local = frame->local;

                STACK_UNWIND (frame, local->op_ret, local->op_errno,
                              &buf, &buf, NULL, NULL);

                ret = wb_request_unref (request);
                if (ret == 0) {
                        write_requests_removed++;
                }
        }

out:
        return write_requests_removed;
}


int32_t
wb_resume_other_requests (call_frame_t *frame, wb_inode_t *wb_inode,
                          list_head_t *other_requests)
{
        int32_t       ret          = -1;
        wb_request_t *request      = NULL, *dummy = NULL;
        int32_t       fops_removed = 0;
        char          wind         = 0;
        call_stub_t  *stub         = NULL;

        GF_VALIDATE_OR_GOTO ((wb_inode ? wb_inode->this->name : "write-behind"),
                             frame, out);
        GF_VALIDATE_OR_GOTO (frame->this->name, wb_inode, out);
        GF_VALIDATE_OR_GOTO (frame->this->name, other_requests, out);

        if (list_empty (other_requests)) {
                ret = 0;
                goto out;
        }

        list_for_each_entry_safe (request, dummy, other_requests,
                                  other_requests) {
                wind = request->stub->wind;
                stub = request->stub;

                LOCK (&wb_inode->lock);
                {
                        request->stub = NULL;
                }
                UNLOCK (&wb_inode->lock);

                if (!wind) {
                        wb_request_unref (request);
                        fops_removed++;
                }

                call_resume (stub);
        }

        ret = 0;

        if (fops_removed > 0) {
                ret = wb_process_queue (frame, wb_inode);
                if (ret == -1) {
                        gf_log (frame->this->name, GF_LOG_WARNING,
                                "request queue processing failed");
                }
        }

out:
        return ret;
}


int32_t
wb_do_ops (call_frame_t *frame, wb_inode_t *wb_inode, list_head_t *winds,
           list_head_t *unwinds, list_head_t *other_requests)
{
        int32_t ret = -1, write_requests_removed = 0;

        GF_VALIDATE_OR_GOTO ((wb_inode ? wb_inode->this->name : "write-behind"),
                             frame, out);
        GF_VALIDATE_OR_GOTO (frame->this->name, wb_inode, out);

        ret = wb_stack_unwind (unwinds);

        write_requests_removed = ret;

        ret = wb_sync (frame, wb_inode, winds);
        if (ret == -1) {
                gf_log (frame->this->name, GF_LOG_WARNING,
                        "syncing of write requests failed");
        }

        ret = wb_resume_other_requests (frame, wb_inode, other_requests);
        if (ret == -1) {
                gf_log (frame->this->name, GF_LOG_WARNING,
                        "cannot resume non-write requests in request queue");
        }

        /* wb_stack_unwind does wb_request_unref after unwinding a write
         * request. Hence if a write-request was just freed in wb_stack_unwind,
         * we have to process request queue once again to unblock requests
         * blocked on the writes just unwound.
         */
        if (write_requests_removed > 0) {
                ret = wb_process_queue (frame, wb_inode);
                if (ret == -1) {
                        gf_log (frame->this->name, GF_LOG_WARNING,
                                "request queue processing failed");
                }
        }

out:
        return ret;
}


inline int
__wb_copy_into_holder (wb_request_t *holder, wb_request_t *request)
{
        char          *ptr    = NULL;
        struct iobuf  *iobuf  = NULL;
        struct iobref *iobref = NULL;
        int            ret    = -1;

        if (holder->flags.write_request.virgin) {
                /* TODO: check the required size */
                iobuf = iobuf_get (request->wb_inode->this->ctx->iobuf_pool);
                if (iobuf == NULL) {
                        goto out;
                }

                iobref = iobref_new ();
                if (iobref == NULL) {
                        iobuf_unref (iobuf);
                        goto out;
                }

                ret = iobref_add (iobref, iobuf);
                if (ret != 0) {
                        iobuf_unref (iobuf);
                        iobref_unref (iobref);
                        gf_log (request->wb_inode->this->name, GF_LOG_WARNING,
                                "cannot add iobuf (%p) into iobref (%p)",
                                iobuf, iobref);
                        goto out;
                }

                iov_unload (iobuf->ptr, holder->stub->args.writev.vector,
                            holder->stub->args.writev.count);
                holder->stub->args.writev.vector[0].iov_base = iobuf->ptr;

                iobref_unref (holder->stub->args.writev.iobref);
                holder->stub->args.writev.iobref = iobref;

                iobuf_unref (iobuf);

                holder->flags.write_request.virgin = 0;
        }

        ptr = holder->stub->args.writev.vector[0].iov_base + holder->write_size;

        iov_unload (ptr, request->stub->args.writev.vector,
                    request->stub->args.writev.count);

        holder->stub->args.writev.vector[0].iov_len += request->write_size;
        holder->write_size += request->write_size;

        request->flags.write_request.stack_wound = 1;
        list_move_tail (&request->list, &request->wb_inode->passive_requests);

        ret = 0;
out:
        return ret;
}


/* this procedure assumes that write requests have only one vector to write */
void
__wb_collapse_write_bufs (list_head_t *requests, size_t page_size)
{
        off_t         offset_expected = 0;
        size_t        space_left      = 0;
        wb_request_t *request         = NULL, *tmp = NULL, *holder = NULL;
        int           ret             = 0;

        GF_VALIDATE_OR_GOTO ("write-behind", requests, out);

        list_for_each_entry_safe (request, tmp, requests, list) {
                if ((request->stub == NULL)
                    || (request->stub->fop != GF_FOP_WRITE)
                    || (request->flags.write_request.stack_wound)) {
                        holder = NULL;
                        continue;
                }

                if (request->flags.write_request.write_behind) {
                        if (holder == NULL) {
                                holder = request;
                                continue;
                        }

                        offset_expected = holder->stub->args.writev.off
                                + holder->write_size;

                        if ((request->stub->args.writev.off != offset_expected)
                            || (!is_same_lkowner (&request->lk_owner,
                                                  &holder->lk_owner))
                            || (holder->stub->args.writev.fd
                                != request->stub->args.writev.fd)) {
                                holder = request;
                                continue;
                        }

                        space_left = page_size - holder->write_size;

                        if (space_left >= request->write_size) {
                                ret = __wb_copy_into_holder (holder, request);
                                if (ret != 0) {
                                        break;
                                }

                                __wb_request_unref (request);
                        } else {
                                holder = request;
                        }
                } else {
                        break;
                }
        }

out:
        return;
}


int32_t
wb_process_queue (call_frame_t *frame, wb_inode_t *wb_inode)
{
        list_head_t winds  = {0, }, unwinds = {0, }, other_requests = {0, };
        size_t      size   = 0;
        wb_conf_t  *conf   = NULL;
        uint32_t    count  = 0;
        int32_t     ret    = -1;

        INIT_LIST_HEAD (&winds);
        INIT_LIST_HEAD (&unwinds);
        INIT_LIST_HEAD (&other_requests);

        GF_VALIDATE_OR_GOTO ((wb_inode ? wb_inode->this->name : "write-behind"),
                             frame, out);
        GF_VALIDATE_OR_GOTO (wb_inode->this->name, frame, out);

        conf = wb_inode->this->private;
        GF_VALIDATE_OR_GOTO (wb_inode->this->name, conf, out);

        size = conf->aggregate_size;
        LOCK (&wb_inode->lock);
        {
                /*
                 * make sure requests are marked for unwinding and adjacent
                 * contiguous write buffers (each of size less than that of
                 * an iobuf) are packed properly so that iobufs are filled to
                 * their maximum capacity, before calling __wb_mark_winds.
                 */
                __wb_mark_unwinds (&wb_inode->request, &unwinds);

                __wb_collapse_write_bufs (&wb_inode->request,
                                          wb_inode->this->ctx->page_size);

                count = __wb_get_other_requests (&wb_inode->request,
                                                 &other_requests);

                if (count == 0) {
                        __wb_mark_winds (&wb_inode->request, &winds, size,
                                         conf->enable_trickling_writes);
                }

        }
        UNLOCK (&wb_inode->lock);

        ret = wb_do_ops (frame, wb_inode, &winds, &unwinds, &other_requests);

out:
        return ret;
}


int32_t
wb_writev_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
               int32_t op_ret, int32_t op_errno, struct iatt *prebuf,
               struct iatt *postbuf, dict_t *xdata)
{
        GF_ASSERT (frame);

        STACK_UNWIND_STRICT (writev, frame, op_ret, op_errno, prebuf, postbuf,
                             xdata);
        return 0;
}


int32_t
wb_writev (call_frame_t *frame, xlator_t *this, fd_t *fd, struct iovec *vector,
           int32_t count, off_t offset, uint32_t flags, struct iobref *iobref,
           dict_t *xdata)
{
        wb_inode_t   *wb_inode      = NULL;
        wb_file_t    *wb_file       = NULL;
        char          wb_disabled   = 0;
        call_frame_t *process_frame = NULL;
        call_stub_t  *stub          = NULL;
        wb_local_t   *local         = NULL;
        wb_request_t *request       = NULL;
        int32_t       ret           = -1;
        size_t        size          = 0;
        int32_t       op_ret        = -1, op_errno = EINVAL;

        GF_ASSERT (frame);

        GF_VALIDATE_OR_GOTO_WITH_ERROR ("write-behind", this, unwind, op_errno,
                                        EINVAL);
        GF_VALIDATE_OR_GOTO_WITH_ERROR (this->name, fd, unwind, op_errno,
                                        EINVAL);

        if (vector != NULL) {
                size = iov_length (vector, count);
        }

        wb_inode = wb_inode_ctx_get (this, fd->inode);
        if ((!IA_ISDIR (fd->inode->ia_type)) && (wb_inode == NULL)) {
                gf_log (this->name, GF_LOG_WARNING,
                        "write behind wb_inode pointer is"
                        " not stored in context of inode(%p), returning EBADFD",
                        fd->inode);
                op_errno = EBADFD;
                goto unwind;
        }

        if (wb_file != NULL) {
                if (wb_file->disabled || wb_file->disable_till) {
                        if (size > wb_file->disable_till) {
                                wb_file->disable_till = 0;
                        } else {
                                wb_file->disable_till -= size;
                        }
                        wb_disabled = 1;
                }
        } else {
                wb_disabled = 1;
        }

        if (wb_inode != NULL) {
                LOCK (&wb_inode->lock);
                {
                        op_ret = wb_inode->op_ret;
                        op_errno = wb_inode->op_errno;
                }
                UNLOCK (&wb_inode->lock);
        }

        if (op_ret == -1) {
                goto unwind;
        }

        if (wb_disabled) {
                STACK_WIND (frame, wb_writev_cbk, FIRST_CHILD (frame->this),
                            FIRST_CHILD (frame->this)->fops->writev,
                            fd, vector, count, offset, flags, iobref, xdata);
                return 0;
        }

        process_frame = copy_frame (frame);
        if (process_frame == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        local = mem_get0 (this->local_pool);
        if (local == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        frame->local = local;
        local->wb_inode = wb_inode;

        stub = fop_writev_stub (frame, NULL, fd, vector, count, offset, flags,
                                iobref, xdata);
        if (stub == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        request = wb_enqueue (wb_inode, stub);
        if (request == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        ret = wb_process_queue (process_frame, wb_inode);
        if (ret == -1) {
                gf_log (this->name, GF_LOG_WARNING,
                        "request queue processing failed");
        }

        STACK_DESTROY (process_frame->root);

        return 0;

unwind:
        local = frame->local;
        frame->local = NULL;
        mem_put (local);

        STACK_UNWIND_STRICT (writev, frame, -1, op_errno, NULL, NULL, NULL);

        if (process_frame) {
                STACK_DESTROY (process_frame->root);
        }

        if (stub) {
                call_stub_destroy (stub);
        }

        return 0;
}


int32_t
wb_readv_cbk (call_frame_t *frame, void *cookie, xlator_t *this, int32_t op_ret,
              int32_t op_errno, struct iovec *vector, int32_t count,
              struct iatt *stbuf, struct iobref *iobref, dict_t *xdata)
{
        wb_local_t   *local    = NULL;
        wb_inode_t   *wb_inode = NULL;
        wb_request_t *request  = NULL;
        int32_t       ret      = 0;

        GF_ASSERT (frame);

        local = frame->local;
        wb_inode = local->wb_inode;
        request = local->request;

        if ((request != NULL) && (wb_inode != NULL)) {
                wb_request_unref (request);

                ret = wb_process_queue (frame, wb_inode);
                if (ret == -1) {
                        if (errno == ENOMEM) {
                                op_ret = -1;
                                op_errno = ENOMEM;
                        }

                        gf_log (frame->this->name, GF_LOG_WARNING,
                                "request queue processing failed");
                }
        }

        STACK_UNWIND_STRICT (readv, frame, op_ret, op_errno, vector, count,
                             stbuf, iobref, xdata);

        return 0;
}


static int32_t
wb_readv_helper (call_frame_t *frame, xlator_t *this, fd_t *fd, size_t size,
                 off_t offset, uint32_t flags, dict_t *xdata)
{
        STACK_WIND (frame, wb_readv_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->readv, fd, size, offset, flags,
                    xdata);

        return 0;
}


int32_t
wb_readv (call_frame_t *frame, xlator_t *this, fd_t *fd, size_t size,
          off_t offset, uint32_t flags, dict_t *xdata)
{
        wb_inode_t   *wb_inode     = NULL;
        wb_local_t   *local        = NULL;
        call_stub_t  *stub         = NULL;
        int32_t       ret          = -1, op_errno = 0;
        wb_request_t *request      = NULL;

        GF_ASSERT (frame);
        GF_VALIDATE_OR_GOTO_WITH_ERROR (frame->this->name, this, unwind,
                                        op_errno, EINVAL);
        GF_VALIDATE_OR_GOTO_WITH_ERROR (this->name, fd, unwind, op_errno,
                                        EINVAL);

        wb_inode = wb_inode_ctx_get (this, fd->inode);
        if ((!IA_ISDIR (fd->inode->ia_type)) && (wb_inode == NULL)) {
                gf_log (this->name, GF_LOG_WARNING,
                        "write behind wb_inode pointer is"
                        " not stored in context of inode(%p), returning "
                        "EBADFD", fd->inode);
                op_errno = EBADFD;
                goto unwind;
        }

        local = mem_get0 (this->local_pool);
        if (local == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        local->wb_inode = wb_inode;

        frame->local = local;
        if (wb_inode) {
                stub = fop_readv_stub (frame, wb_readv_helper, fd, size,
                                       offset, flags, xdata);
                if (stub == NULL) {
                        op_errno = ENOMEM;
                        goto unwind;
                }

                request = wb_enqueue (wb_inode, stub);
                if (request == NULL) {
                        call_stub_destroy (stub);
                        op_errno = ENOMEM;
                        goto unwind;
                }

                ret = wb_process_queue (frame, wb_inode);
                if (ret == -1) {
                        gf_log (this->name, GF_LOG_WARNING,
                                "request queue processing failed");
                }
        } else {
                STACK_WIND (frame, wb_readv_cbk, FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->readv,
                            fd, size, offset, flags, xdata);
        }

        return 0;

unwind:
        STACK_UNWIND_STRICT (readv, frame, -1, op_errno, NULL, 0, NULL, NULL,
                             NULL);
        return 0;
}


int32_t
wb_ffr_bg_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
               int32_t op_ret, int32_t op_errno, dict_t *xdata)
{
        STACK_DESTROY (frame->root);
        return 0;
}


int32_t
wb_ffr_cbk (call_frame_t *frame, void *cookie, xlator_t *this, int32_t op_ret,
            int32_t op_errno, dict_t *xdata)
{
        wb_local_t *local    = NULL;
        wb_inode_t *wb_inode = NULL;

        GF_ASSERT (frame);

        local = frame->local;
        wb_inode = local->wb_inode;

        if (wb_inode != NULL) {
                LOCK (&wb_inode->lock);
                {
                        if (wb_inode->op_ret == -1) {
                                op_ret = wb_inode->op_ret;
                                op_errno = wb_inode->op_errno;

                                wb_inode->op_ret = 0;
                        }
                }
                UNLOCK (&wb_inode->lock);
        }

        STACK_UNWIND_STRICT (flush, frame, op_ret, op_errno, xdata);

        return 0;
}


int32_t
wb_flush_helper (call_frame_t *frame, xlator_t *this, fd_t *fd, dict_t *xdata)
{
        wb_conf_t    *conf        = NULL;
        wb_local_t   *local       = NULL;
        wb_inode_t   *wb_inode    = NULL;
        call_frame_t *flush_frame = NULL, *process_frame = NULL;
        int32_t       op_ret      = -1, op_errno = -1, ret = -1;

        GF_ASSERT (frame);
        GF_VALIDATE_OR_GOTO_WITH_ERROR (frame->this->name, this, unwind,
                                        op_errno, EINVAL);

        conf = this->private;

        local = frame->local;
        wb_inode = local->wb_inode;

        LOCK (&wb_inode->lock);
        {
                op_ret = wb_inode->op_ret;
                op_errno = wb_inode->op_errno;
        }
        UNLOCK (&wb_inode->lock);

        if (local && local->request) {
                process_frame = copy_frame (frame);
                if (process_frame == NULL) {
                        op_errno = ENOMEM;
                        goto unwind;
                }

                wb_request_unref (local->request);
        }

        if (conf->flush_behind) {
                flush_frame = copy_frame (frame);
                if (flush_frame == NULL) {
                        op_errno = ENOMEM;
                        goto unwind;
                }

                STACK_WIND (flush_frame, wb_ffr_bg_cbk, FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->flush, fd, xdata);
        } else {
                STACK_WIND (frame, wb_ffr_cbk, FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->flush, fd, xdata);
        }

        if (process_frame != NULL) {
                ret = wb_process_queue (process_frame, wb_inode);
                if (ret == -1) {
                        gf_log (this->name, GF_LOG_WARNING,
                                "request queue processing failed");
                }

                STACK_DESTROY (process_frame->root);
        }

        if (conf->flush_behind) {
                STACK_UNWIND_STRICT (flush, frame, op_ret, op_errno, NULL);
        }

        return 0;

unwind:
        STACK_UNWIND_STRICT (flush, frame, -1, op_errno, NULL);
        return 0;
}


int32_t
wb_flush (call_frame_t *frame, xlator_t *this, fd_t *fd, dict_t *xdata)
{
        wb_conf_t    *conf         = NULL;
        wb_inode_t   *wb_inode     = NULL;
        wb_local_t   *local        = NULL;
        call_stub_t  *stub         = NULL;
        call_frame_t *flush_frame  = NULL;
        wb_request_t *request      = NULL;
        int32_t       ret          = 0, op_errno = 0;

        GF_ASSERT (frame);
        GF_VALIDATE_OR_GOTO_WITH_ERROR (frame->this->name, this, unwind,
                                        op_errno, EINVAL);
        GF_VALIDATE_OR_GOTO_WITH_ERROR (this->name, fd, unwind, op_errno,
                                        EINVAL);

        conf = this->private;

        wb_inode = wb_inode_ctx_get (this, fd->inode);
        if ((!IA_ISDIR (fd->inode->ia_type)) && (wb_inode == NULL)) {
                gf_log (this->name, GF_LOG_WARNING,
                        "write behind wb_inode pointer is"
                        " not stored in context of inode(%p), "
                        "returning EBADFD", fd->inode);
                op_errno = EBADFD;
                goto unwind;
        }

        if (wb_inode != NULL) {
                local = mem_get0 (this->local_pool);
                if (local == NULL) {
                        op_errno = ENOMEM;
                        goto unwind;
                }

                local->wb_inode = wb_inode;

                frame->local = local;

                stub = fop_flush_stub (frame, wb_flush_helper, fd, xdata);
                if (stub == NULL) {
                        op_errno = ENOMEM;
                        goto unwind;
                }

                request = wb_enqueue (wb_inode, stub);
                if (request == NULL) {
                        call_stub_destroy (stub);
                        op_errno = ENOMEM;
                        goto unwind;
                }

                ret = wb_process_queue (frame, wb_inode);
                if (ret == -1) {
                        gf_log (this->name, GF_LOG_WARNING,
                                "request queue processing failed");
                }
        } else {
                if (conf->flush_behind) {
                        flush_frame = copy_frame (frame);
                        if (flush_frame == NULL) {
                                op_errno = ENOMEM;
                                goto unwind;
                        }

                        STACK_UNWIND_STRICT (flush, frame, 0, 0, NULL);

                        STACK_WIND (flush_frame, wb_ffr_bg_cbk,
                                    FIRST_CHILD(this),
                                    FIRST_CHILD(this)->fops->flush, fd, xdata);
                } else {
                        STACK_WIND (frame, wb_ffr_cbk, FIRST_CHILD(this),
                                    FIRST_CHILD(this)->fops->flush, fd, xdata);
                }
        }

        return 0;

unwind:
        STACK_UNWIND_STRICT (flush, frame, -1, op_errno, NULL);
        return 0;
}


static int32_t
wb_fsync_cbk (call_frame_t *frame, void *cookie, xlator_t *this, int32_t op_ret,
              int32_t op_errno, struct iatt *prebuf, struct iatt *postbuf,
              dict_t *xdata)
{
        wb_local_t   *local    = NULL;
        wb_inode_t   *wb_inode = NULL;
        wb_request_t *request  = NULL;
        int32_t       ret      = -1;

        GF_ASSERT (frame);

        local = frame->local;
        wb_inode = local->wb_inode;
        request = local->request;

        if (wb_inode != NULL) {
                LOCK (&wb_inode->lock);
                {
                        if (wb_inode->op_ret == -1) {
                                op_ret = wb_inode->op_ret;
                                op_errno = wb_inode->op_errno;

                                wb_inode->op_ret = 0;
                        }
                }
                UNLOCK (&wb_inode->lock);

                if (request) {
                        wb_request_unref (request);
                        ret = wb_process_queue (frame, wb_inode);
                        if (ret == -1) {
                                if (errno == ENOMEM) {
                                        op_ret = -1;
                                        op_errno = ENOMEM;
                                }

                                gf_log (this->name, GF_LOG_WARNING,
                                        "request queue processing failed");
                        }
                }

        }

        STACK_UNWIND_STRICT (fsync, frame, op_ret, op_errno, prebuf, postbuf,
                             xdata);

        return 0;
}


static int32_t
wb_fsync_helper (call_frame_t *frame, xlator_t *this, fd_t *fd,
                 int32_t datasync, dict_t *xdata)
{
        STACK_WIND (frame, wb_fsync_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->fsync, fd, datasync, xdata);
        return 0;
}


int32_t
wb_fsync (call_frame_t *frame, xlator_t *this, fd_t *fd, int32_t datasync,
          dict_t *xdata)
{
        wb_inode_t   *wb_inode     = NULL;
        wb_local_t   *local        = NULL;
        call_stub_t  *stub         = NULL;
        wb_request_t *request      = NULL;
        int32_t       ret          = -1, op_errno = 0;

        GF_ASSERT (frame);
        GF_VALIDATE_OR_GOTO_WITH_ERROR (frame->this->name, this, unwind,
                                        op_errno, EINVAL);
        GF_VALIDATE_OR_GOTO_WITH_ERROR (frame->this->name, fd, unwind,
                                        op_errno, EINVAL);

        wb_inode = wb_inode_ctx_get (this, fd->inode);
        if (wb_inode == NULL && (!IA_ISDIR (fd->inode->ia_type))) {
                gf_log (this->name, GF_LOG_WARNING,
                        "write behind wb_inode pointer is"
                        " not stored in context of inode(%p), "
                        "returning EBADFD", fd->inode);
                op_errno = EBADFD;
                goto unwind;
        }

        local = mem_get0 (this->local_pool);
        if (local == NULL) {
                op_errno = ENOMEM;
                goto unwind;
        }

        frame->local = local;
        local->wb_inode = wb_inode;

        if (wb_inode) {
                stub = fop_fsync_stub (frame, wb_fsync_helper, fd, datasync,
                                       xdata);
                if (stub == NULL) {
                        op_errno = ENOMEM;
                        goto unwind;
                }

                request = wb_enqueue (wb_inode, stub);
                if (request == NULL) {
                        op_errno = ENOMEM;
                        call_stub_destroy (stub);
                        goto unwind;
                }

                ret = wb_process_queue (frame, wb_inode);
                if (ret == -1) {
                        gf_log (this->name, GF_LOG_WARNING,
                                "request queue processing failed");
                }
        } else {
                STACK_WIND (frame, wb_fsync_cbk, FIRST_CHILD(this),
                            FIRST_CHILD(this)->fops->fsync, fd, datasync,
                            xdata);
        }

        return 0;

unwind:
        STACK_UNWIND_STRICT (fsync, frame, -1, op_errno, NULL, NULL, NULL);
        return 0;
}


int32_t
wb_readdirp_cbk (call_frame_t *frame, void *cookie, xlator_t *this,
                 int32_t op_ret, int32_t op_errno, gf_dirent_t *entries,
                 dict_t *xdata)
{
        gf_dirent_t *entry      = NULL;

        if (op_ret <= 0) {
                goto unwind;
        }

        list_for_each_entry (entry, &entries->list, list) {
                if (!entry->inode)
			continue;
                wb_inode_create (this, entry->inode);
        }

unwind:
        STACK_UNWIND_STRICT (readdirp, frame, op_ret, op_errno, entries, xdata);
        return 0;
}


int32_t
wb_readdirp (call_frame_t *frame, xlator_t *this, fd_t *fd,
             size_t size, off_t off, dict_t *xdata)
{
        STACK_WIND (frame, wb_readdirp_cbk, FIRST_CHILD(this),
                    FIRST_CHILD(this)->fops->readdirp, fd, size, off, xdata);
        return 0;
}


int32_t
wb_release (xlator_t *this, fd_t *fd)
{
        uint64_t    wb_file_ptr = 0;
        wb_file_t  *wb_file      = NULL;

        GF_VALIDATE_OR_GOTO ("write-behind", this, out);
        GF_VALIDATE_OR_GOTO (this->name, fd, out);

        fd_ctx_del (fd, this, &wb_file_ptr);
        wb_file = (wb_file_t *)(long) wb_file_ptr;

        GF_FREE (wb_file);

out:
        return 0;
}


int32_t
wb_forget (xlator_t *this, inode_t *inode)
{
        uint64_t    tmp      = 0;
        wb_inode_t *wb_inode = NULL;

        inode_ctx_del (inode, this, &tmp);

        wb_inode = (wb_inode_t *)(long)tmp;

        if (wb_inode != NULL) {
                LOCK (&wb_inode->lock);
                {
                        GF_ASSERT (list_empty (&wb_inode->request));
                }
                UNLOCK (&wb_inode->lock);

                wb_inode_destroy (wb_inode);
        }

        return 0;
}


int
wb_priv_dump (xlator_t *this)
{
        wb_conf_t      *conf                            = NULL;
        char            key_prefix[GF_DUMP_MAX_BUF_LEN] = {0, };
        int             ret                             = -1;

        GF_VALIDATE_OR_GOTO ("write-behind", this, out);

        conf = this->private;
        GF_VALIDATE_OR_GOTO (this->name, conf, out);

        gf_proc_dump_build_key (key_prefix, "xlator.performance.write-behind",
                                "priv");

        gf_proc_dump_add_section (key_prefix);

        gf_proc_dump_write ("aggregate_size", "%d", conf->aggregate_size);
        gf_proc_dump_write ("window_size", "%d", conf->window_size);
        gf_proc_dump_write ("enable_O_SYNC", "%d", conf->enable_O_SYNC);
        gf_proc_dump_write ("flush_behind", "%d", conf->flush_behind);
        gf_proc_dump_write ("enable_trickling_writes", "%d",
                            conf->enable_trickling_writes);

        ret = 0;
out:
        return ret;
}


void
__wb_dump_requests (struct list_head *head, char *prefix, char passive)
{
        char          key[GF_DUMP_MAX_BUF_LEN]        = {0, };
        char          key_prefix[GF_DUMP_MAX_BUF_LEN] = {0, }, flag = 0;
        wb_request_t *request                         = NULL;

        list_for_each_entry (request, head, list) {
                gf_proc_dump_build_key (key, prefix, passive ? "passive-request"
                                        : "active-request");
                gf_proc_dump_build_key (key_prefix, key,
                                        (char *)gf_fop_list[request->fop]);

                gf_proc_dump_add_section(key_prefix);

                gf_proc_dump_write ("request-ptr", "%p", request);

                gf_proc_dump_write ("refcount", "%d", request->refcount);

                if (request->fop == GF_FOP_WRITE) {
                        flag = request->flags.write_request.stack_wound;
                        gf_proc_dump_write ("stack_wound", "%d", flag);

                        gf_proc_dump_write ("size", "%"GF_PRI_SIZET,
                                            request->write_size);

                        gf_proc_dump_write ("offset", "%"PRId64,
                                            request->stub->args.writev.off);

                        flag = request->flags.write_request.write_behind;
                        gf_proc_dump_write ("write_behind", "%d", flag);

                        flag = request->flags.write_request.got_reply;
                        gf_proc_dump_write ("got_reply", "%d", flag);

                        flag = request->flags.write_request.virgin;
                        gf_proc_dump_write ("virgin", "%d", flag);

                        flag = request->flags.write_request.flush_all;
                        gf_proc_dump_write ("flush_all", "%d", flag);
                } else {
                        flag = request->flags.other_requests.marked_for_resume;
                        gf_proc_dump_write ("marked_for_resume", "%d", flag);
                }
        }
}


int
wb_inode_dump (xlator_t *this, inode_t *inode)
{
        wb_inode_t *wb_inode                       = NULL;
        int32_t     ret                            = -1;
        char       *path                           = NULL;
        char       key_prefix[GF_DUMP_MAX_BUF_LEN] = {0, };

        if ((inode == NULL) || (this == NULL)) {
                ret = 0;
                goto out;
        }

        wb_inode = wb_inode_ctx_get (this, inode);
        if (wb_inode == NULL) {
                ret = 0;
                goto out;
        }

        gf_proc_dump_build_key (key_prefix, "xlator.performance.write-behind",
                                "wb_inode");

        gf_proc_dump_add_section (key_prefix);

        __inode_path (inode, NULL, &path);
        if (path != NULL) {
                gf_proc_dump_write ("path", "%s", path);
                GF_FREE (path);
        }

        gf_proc_dump_write ("inode", "%p", inode);

        gf_proc_dump_write ("window_conf", "%"GF_PRI_SIZET,
                            wb_inode->window_conf);

        gf_proc_dump_write ("window_current", "%"GF_PRI_SIZET,
                            wb_inode->window_current);

        gf_proc_dump_write ("aggregate_current", "%"GF_PRI_SIZET,
                            wb_inode->aggregate_current);

        gf_proc_dump_write ("op_ret", "%d", wb_inode->op_ret);

        gf_proc_dump_write ("op_errno", "%d", wb_inode->op_errno);

        LOCK (&wb_inode->lock);
        {
                if (!list_empty (&wb_inode->request)) {
                        __wb_dump_requests (&wb_inode->request, key_prefix, 0);
                }

                if (!list_empty (&wb_inode->passive_requests)) {
                        __wb_dump_requests (&wb_inode->passive_requests,
                                            key_prefix, 1);
                }
        }
        UNLOCK (&wb_inode->lock);

        ret = 0;
out:
        return ret;
}


int
wb_fd_dump (xlator_t *this, fd_t *fd)
{
        wb_file_t  *wb_file                        = NULL;
        char       *path                           = NULL;
        char       key_prefix[GF_DUMP_MAX_BUF_LEN] = {0, };
        int        ret                             = -1;
        gf_boolean_t  section_added                = _gf_false;

        gf_proc_dump_build_key (key_prefix, "xlator.performance.write-behind",
                                "wb_file");

        if ((fd == NULL) || (this == NULL)) {
                goto out;
        }

        ret = TRY_LOCK(&fd->lock);
        if (ret)
                goto out;
        {
                wb_file = __wb_fd_ctx_get (this, fd);
        }
        UNLOCK(&fd->lock);

        if (wb_file == NULL) {
                goto out;
        }

        gf_proc_dump_add_section (key_prefix);
        section_added = _gf_true;

        __inode_path (fd->inode, NULL, &path);
        if (path != NULL) {
                gf_proc_dump_write ("path", "%s", path);
                GF_FREE (path);
        }

        gf_proc_dump_write ("fd", "%p", fd);

        gf_proc_dump_write ("flags", "%d", wb_file->flags);

        gf_proc_dump_write ("flags", "%s",
                            (wb_file->flags & O_APPEND) ? "O_APPEND"
                            : "!O_APPEND");

        gf_proc_dump_write ("disabled", "%d", wb_file->disabled);

out:
        if (ret && fd && this) {
                if (_gf_false == section_added)
                        gf_proc_dump_add_section (key_prefix);
                gf_proc_dump_write ("Unable to dump the fd",
                                    "(Lock acquisition failed) %s",
                                    uuid_utoa (fd->inode->gfid));
        }
        return 0;
}


int32_t
mem_acct_init (xlator_t *this)
{
        int     ret = -1;

        if (!this) {
                goto out;
        }

        ret = xlator_mem_acct_init (this, gf_wb_mt_end + 1);

        if (ret != 0) {
                gf_log (this->name, GF_LOG_ERROR, "Memory accounting init"
                        "failed");
        }

out:
        return ret;
}


int
reconfigure (xlator_t *this, dict_t *options)
{
        wb_conf_t *conf = NULL;
        int        ret  = -1;

        conf = this->private;

        GF_OPTION_RECONF ("cache-size", conf->window_size, options, size, out);

        GF_OPTION_RECONF ("flush-behind", conf->flush_behind, options, bool,
                          out);

        ret = 0;
out:
        return ret;
}


int32_t
init (xlator_t *this)
{
        wb_conf_t *conf    = NULL;
        int32_t    ret     = -1;

        if ((this->children == NULL)
            || this->children->next) {
                gf_log (this->name, GF_LOG_ERROR,
                        "FATAL: write-behind (%s) not configured with exactly "
                        "one child", this->name);
                goto out;
        }

        if (this->parents == NULL) {
                gf_log (this->name, GF_LOG_WARNING,
                        "dangling volume. check volfilex");
        }

        conf = GF_CALLOC (1, sizeof (*conf), gf_wb_mt_wb_conf_t);
        if (conf == NULL) {
                goto out;
        }

        GF_OPTION_INIT("enable-O_SYNC", conf->enable_O_SYNC, bool, out);

        /* configure 'options aggregate-size <size>' */
        conf->aggregate_size = WB_AGGREGATE_SIZE;

        /* configure 'option window-size <size>' */
        GF_OPTION_INIT ("cache-size", conf->window_size, size, out);

        if (!conf->window_size && conf->aggregate_size) {
                gf_log (this->name, GF_LOG_WARNING,
                        "setting window-size to be equal to "
                        "aggregate-size(%"PRIu64")",
                        conf->aggregate_size);
                conf->window_size = conf->aggregate_size;
        }

        if (conf->window_size < conf->aggregate_size) {
                gf_log (this->name, GF_LOG_ERROR,
                        "aggregate-size(%"PRIu64") cannot be more than "
                        "window-size(%"PRIu64")", conf->aggregate_size,
                        conf->window_size);
                goto out;
        }

        /* configure 'option flush-behind <on/off>' */
        GF_OPTION_INIT ("flush-behind", conf->flush_behind, bool, out);

        GF_OPTION_INIT ("enable-trickling-writes",
                        conf->enable_trickling_writes, bool, out);

        this->local_pool = mem_pool_new (wb_local_t, 64);
        if (!this->local_pool) {
                ret = -1;
                gf_log (this->name, GF_LOG_ERROR,
                        "failed to create local_t's memory pool");
                goto out;
        }

        this->private = conf;
        ret = 0;

out:
        if (ret) {
                GF_FREE (conf);
        }
        return ret;
}


void
fini (xlator_t *this)
{
        wb_conf_t *conf = NULL;

        GF_VALIDATE_OR_GOTO ("write-behind", this, out);

        conf = this->private;
        if (!conf) {
                goto out;
        }

        this->private = NULL;
        GF_FREE (conf);

out:
        return;
}


struct xlator_fops fops = {
        .lookup      = wb_lookup,
        .writev      = wb_writev,
        .open        = wb_open,
        .create      = wb_create,
        .readv       = wb_readv,
        .flush       = wb_flush,
        .fsync       = wb_fsync,
        .stat        = wb_stat,
        .fstat       = wb_fstat,
        .truncate    = wb_truncate,
        .ftruncate   = wb_ftruncate,
        .setattr     = wb_setattr,
        .readdirp    = wb_readdirp,
};

struct xlator_cbks cbks = {
        .forget   = wb_forget,
        .release  = wb_release,
};

struct xlator_dumpops dumpops = {
        .priv      =  wb_priv_dump,
        .inodectx  =  wb_inode_dump,
        .fdctx     =  wb_fd_dump,
};

struct volume_options options[] = {
        { .key  = {"flush-behind"},
          .type = GF_OPTION_TYPE_BOOL,
          .default_value = "on",
          .description = "If this option is set ON, instructs write-behind "
                          "translator to perform flush in background, by "
                          "returning success (or any errors, if any of "
                          "previous  writes were failed) to application even "
                          "before flush is sent to backend filesystem. "
        },
        { .key  = {"cache-size", "window-size"},
          .type = GF_OPTION_TYPE_SIZET,
          .min  = 512 * GF_UNIT_KB,
          .max  = 1 * GF_UNIT_GB,
          .default_value = "1MB",
          .description = "Size of the write-behind buffer for a single file "
                         "(inode)."

        },
        { .key = {"disable-for-first-nbytes"},
          .type = GF_OPTION_TYPE_SIZET,
          .min = 0,
          .max = 1 * GF_UNIT_MB,
          .default_value = "0",
        },
        { .key = {"enable-O_SYNC"},
          .type = GF_OPTION_TYPE_BOOL,
          .default_value = "on",
        },
        { .key = {"enable-trickling-writes"},
          .type = GF_OPTION_TYPE_BOOL,
          .default_value = "on",
        },
        { .key = {NULL} },
};
