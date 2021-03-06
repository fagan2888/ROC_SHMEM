/******************************************************************************
 * Copyright (c) 2019 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *****************************************************************************/

#include "config.h"

#ifdef DEBUG
#define HIP_ENABLE_PRINTF 1
#endif

#include <hip/hip_runtime.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <roc_shmem.hpp>

#include "ro_net_internal.hpp"
#include "hdp_policy.hpp"
#include "context.hpp"
#include "backend.hpp"
#include "wg_state.hpp"
#include "util.hpp"

/***
 *
 * External Device-side API functions
 *
 ***/

__host__
ROContext::ROContext(const Backend &b, long option)
  : Context(b, true)
{
    struct ro_net_handle * handle =
        static_cast<const ROBackend *>(&b)->backend_handle;

    CHECK_HIP(hipMalloc(&backend_ctx, sizeof(ro_net_wg_handle)));

    type = RO_BACKEND;

    int buffer_id = b.num_wg - 1;
    b.bufferTokens[buffer_id] = 1;

    backend_ctx->write_idx = handle->queue_descs[buffer_id].write_idx;
    backend_ctx->read_idx = handle->queue_descs[buffer_id].read_idx;
    backend_ctx->status = handle->queue_descs[buffer_id].status;
    backend_ctx->host_read_idx =
        &handle->queue_descs[buffer_id].read_idx;
    backend_ctx->queue =  handle->queues[buffer_id];
    backend_ctx->queue_size =  handle->queue_size;
    backend_ctx->barrier_ptr = handle->barrier_ptr;
    backend_ctx->profiler.resetStats();
}

__device__
ROContext::ROContext(const Backend &b, long option)
    : Context(b, false)
{
    struct ro_net_handle * handle =
        static_cast<ROBackend *>(gpu_handle)->backend_handle;

    GPU_DPRINTF("Function: ro_ctx_create\n");

    backend_ctx = reinterpret_cast<ro_net_wg_handle *>(
        WGState::instance()->allocateDynamicShared(sizeof(ro_net_wg_handle)));

    if (is_thread_zero_in_block()) {
        type = RO_BACKEND;

        int buffer_id = WGState::instance()->get_global_buffer_id();

        // Device side memcpy is very slow, so do elementwise copy.
        backend_ctx->write_idx = handle->queue_descs[buffer_id].write_idx;
        backend_ctx->read_idx = handle->queue_descs[buffer_id].read_idx;
        backend_ctx->status = handle->queue_descs[buffer_id].status;
        backend_ctx->host_read_idx =
            &handle->queue_descs[buffer_id].read_idx;
        backend_ctx->queue =  handle->queues[buffer_id];
        backend_ctx->queue_size =  handle->queue_size;
        backend_ctx->barrier_ptr = handle->barrier_ptr;
        backend_ctx->profiler.resetStats();
        // TODO: Assuming that I am GPU 0, need ID for multi-GPU nodes!
    //    wg_handle->hdp_flush = handle->hdp_regs[0].HDP_MEM_FLUSH_CNTL;

    }
    __syncthreads();
}

__device__ void
ROContext::threadfence_system()
{
//    struct ro_net_wg_handle * handle =
//        (struct ro_net_wg_handle *) ctx->backend_ctx;

 //   *(handle->hdp_flush) = 0x1;
}

__device__ void
ROContext::putmem(void *dest, const void *source, size_t nelems, int pe)
{
    build_queue_element(RO_NET_PUT, dest, (void * ) source, nelems, pe, 0, 0,
                        nullptr, nullptr, backend_ctx, true);
}

__device__ void
ROContext::getmem(void *dest, const void *source, size_t nelems, int pe)
{
    build_queue_element(RO_NET_GET, dest, (void *) source, nelems, pe, 0, 0,
                        nullptr, nullptr, backend_ctx, true);
}

__device__ void
ROContext::putmem_nbi(void *dest, const void *source, size_t nelems, int pe)
{
    build_queue_element(RO_NET_PUT_NBI, dest, (void *) source, nelems, pe, 0,
                        0, nullptr, nullptr, backend_ctx, false);
}

__device__ void
ROContext::getmem_nbi(void *dest, const void *source, size_t nelems, int pe)
{
    build_queue_element(RO_NET_GET_NBI, dest, (void *) source, nelems, pe, 0,
                        0, nullptr, nullptr, backend_ctx, false);
}

__device__ void
ROContext::fence()
{
    build_queue_element(RO_NET_FENCE, nullptr, nullptr, 0, 0, 0, 0, nullptr,
                        nullptr, backend_ctx, true);
}

__device__ void
ROContext::quiet()
{
    build_queue_element(RO_NET_QUIET, nullptr, nullptr, 0, 0, 0, 0, nullptr,
                        nullptr, backend_ctx, true);
}

__device__ void
ROContext::barrier_all()
{
    build_queue_element(RO_NET_BARRIER_ALL,nullptr, nullptr, 0, 0, 0, 0,
                        nullptr, nullptr, backend_ctx, true);
}

__device__ void
ROContext::sync_all()
{
    build_queue_element(RO_NET_BARRIER_ALL, nullptr, nullptr, 0, 0, 0, 0,
                        nullptr, nullptr, backend_ctx, true);
}

__device__ void
ROContext::ctx_destroy()
{
    if (is_thread_zero_in_block()) {
        struct ro_net_handle * handle =
            static_cast<ROBackend *>(gpu_handle)->backend_handle;

        build_queue_element(RO_NET_FINALIZE, nullptr, nullptr, 0, 0, 0, 0,
                            nullptr, nullptr, backend_ctx, true);

        int buffer_id = WGState::instance()->get_global_buffer_id();
        handle->queue_descs[buffer_id].write_idx =
            backend_ctx->write_idx;

        ROStats &global_handle =
            handle->profiler[buffer_id];

        global_handle.accumulateStats(backend_ctx->profiler);
    }

    __syncthreads();
}

__device__ int64_t
ROContext::amo_fetch_cas(void *dst, int64_t value, int64_t cond, int pe)
{
    assert(0);
    return value;
}

__device__ void
ROContext::amo_cas(void *dst, int64_t value, int64_t cond, int pe)
{
    assert(0);
}

__device__ int64_t
ROContext::amo_fetch_add(void *dst, int64_t value, int64_t cond, int pe)
{
    assert(0);
    return value;
}

__device__ void
ROContext::amo_add(void *dst, int64_t value, int64_t cond, int pe)
{
    assert(0);
}

/***
 *
 * Internal Device-side API functions
 *
 ***/
__device__ bool isFull(uint64_t read_idx, uint64_t write_idx,
                       uint64_t queue_size) {
    return ((queue_size - (write_idx - read_idx)) == 0);
}

__device__ void build_queue_element(ro_net_cmds type, void* dst, void * src,
                                    size_t size, int pe, int logPE_stride,
                                    int PE_size, void* pWrk,
                                    long *pSync,
                                    struct ro_net_wg_handle *handle,
                                    bool blocking, ROC_SHMEM_OP op,
                                    ro_net_types datatype)
{
    int threadId = get_flat_block_id();

    uint64_t start = handle->profiler.startTimer();

    unsigned long long old_write_slot = handle->write_idx;
    unsigned long long write_slot;
    do {
        write_slot = old_write_slot;
        // If we think the queue might be full, poll on the in-memory read
        // index.  Otherwise, we are good to go!  In the common case we never
        // need to go to memory.
        while (isFull(handle->read_idx, write_slot, handle->queue_size))
        {
            __asm__ volatile ("global_load_dwordx2 %0 %1 off glc slc\n "
                              "s_waitcnt vmcnt(0)" :
                              "=v"(handle->read_idx) :
                              "v"(handle->host_read_idx));

        }
        // Double check that our write_idx is still available and update it.
        // If it isn't then we try again and validate that the new write
        // index is available for the taking.
        old_write_slot = atomicCAS((unsigned long long*) &handle->write_idx,
            write_slot, write_slot + 1);
    } while (write_slot != old_write_slot);

    handle->profiler.endTimer(start, WAITING_ON_SLOT);

    start = handle->profiler.startTimer();
    write_slot = write_slot % handle->queue_size;
    handle->queue[write_slot].type = type;
    handle->queue[write_slot].PE = pe;
    handle->queue[write_slot].size = size;
    handle->queue[write_slot].dst = dst;

    // Inline commands will pack the data value in the src field.
    if (type == RO_NET_P)
       memcpy(&handle->queue[write_slot].src, src, size);
    else
       handle->queue[write_slot].src = src;

    handle->queue[write_slot].threadId = threadId;

    /*
        * TODO: Might be more efficient to inline the stores.
    int1  val1 = make_int1((int)type);
    int2  val2 = make_int2(pe, size);
    long1 val3 = make_long1((long)src);
    long1 val4 = make_long1((long)dst);
    __store_dword(val1,
        (volatile int1*)&(handle->queue[write_slot].type));
    __store_dwordx2(val2,
        (volatile int2*)&(handle->queue[write_slot].PE));
    __store_long_dwordx2(val3,
        (volatile long1*)&(handle->queue[write_slot].src));
    __store_long_dwordx2(val4,
        (volatile long1*)&(handle->queue[write_slot].dst));
    */

    if (type == RO_NET_TO_ALL) {
        handle->queue[write_slot].logPE_stride = logPE_stride;
        handle->queue[write_slot].PE_size = PE_size;
        handle->queue[write_slot].pWrk = pWrk;
        handle->queue[write_slot].pSync = pSync;
        handle->queue[write_slot].op = op;
        handle->queue[write_slot].datatype = datatype;
    }

    handle->profiler.endTimer(start, PACK_QUEUE);

    // Make sure queue element data is visible to CPU
    start = handle->profiler.startTimer();
    __threadfence();
    handle->profiler.endTimer(start, THREAD_FENCE_1);

    // Make data as ready and make visible to CPU
    start = handle->profiler.startTimer();
    handle->queue[write_slot].valid = 1;
    __threadfence();
    handle->profiler.endTimer(start, THREAD_FENCE_2);

    // Blocking requires the CPU to complete the operation.
    start = handle->profiler.startTimer();
    if (blocking) {
        int net_status;
        do {
            // At will take at least 1-2us to satisfy any request, best case.
            // TODO: Vega supports 7 bits, Fiji only 4
            __asm__ volatile ("s_sleep 32\n"
                              "global_load_sbyte %0 %1 off glc slc\n "
                                "s_waitcnt vmcnt(0)" :
                                "=v"(net_status) :
                                "v"(&handle->status[threadId]));
        } while (net_status == 0);

        handle->status[threadId] = 0;
        __threadfence();
    }
    handle->profiler.endTimer(start, WAITING_ON_HOST);
}
