/***************************************************************************
*             __________               __   ___.
*   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
*   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
*   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
*   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
*                     \/            \/     \/    \/            \/
* $Id$
*
* This is a memory allocator designed to provide reasonable management of free
* space and fast access to allocated data. More than one allocator can be used
* at a time by initializing multiple contexts.
*
* Copyright (C) 2009 Andrew Mahone
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
* KIND, either express or implied.
*
****************************************************************************/

#include <stdlib.h> /* for abs() */
#include "buflib.h"
#include "new_apis.h"
/* The main goal of this design is fast fetching of the pointer for a handle.
 * For that reason, the handles are stored in a table at the end of the buffer
 * with a fixed address, so that returning the pointer for a handle is a simple
 * table lookup. To reduce the frequency with which allocated blocks will need
 * to be moved to free space, allocations grow up in address from the start of
 * the buffer. The buffer is treated as an array of union buflib_data. Blocks
 * start with a length marker, which is included in their length. Free blocks
 * are marked by negative length, allocated ones use the a buflib_data in
 * the block to store a pointer to their handle table entry, so that it can be
 * quickly found and updated during compaction. Followed by that, there's
 * the pointer to the corresponding struct buflib. That pointer follows a
 * character array containing the string identifier of the allocation. After the
 * array there is another buflib_data containing the length of that string +
 * the sizeo of this buflib_data.
 * The allocator functions are passed a context struct so that two allocators
 * can be run, for example, one per core may be used, with convenience wrappers
 * for the single-allocator case that use a predefined context.
 */

/* Use this for the default callbacks.
 *
 * The default callbacks do nothing, therefore the address of this
 * acts as a magic as to not even call the default callbacks
 */
static struct buflib_callbacks default_callbacks;

/* Initialize buffer manager */
void
buflib_init(struct buflib_context *ctx, void *buf, size_t size)
{
    union buflib_data *bd_buf = buf;

    /* Align on sizeof(buflib_data), to prevent unaligned access */
    ALIGN_BUFFER(bd_buf, size, sizeof(union buflib_data));
    size /= sizeof(union buflib_data);
    /* The handle table is initialized with no entries */
    ctx->handle_table = bd_buf + size;
    ctx->last_handle = bd_buf + size;
    ctx->first_free_handle = bd_buf + size - 1;
    ctx->first_free_block = bd_buf;
    ctx->buf_start = bd_buf;
    /* A marker is needed for the end of allocated data, to make sure that it
     * does not collide with the handle table, and to detect end-of-buffer.
     */
    ctx->alloc_end = bd_buf;
    ctx->compact = true;
}

/* Allocate a new handle, returning 0 on failure */
static inline
union buflib_data* handle_alloc(struct buflib_context *ctx)
{
    union buflib_data *handle;
    /* first_free_handle is a lower bound on free handles, work through the
     * table from there until a handle containing NULL is found, or the end
     * of the table is reached.
     */
    for (handle = ctx->first_free_handle; handle >= ctx->last_handle; handle--)
        if (!handle->ptr)
            break;
    /* If the search went past the end of the table, it means we need to extend
     * the table to get a new handle.
     */
    if (handle < ctx->last_handle)
    {
        if (handle >= ctx->alloc_end)
            ctx->last_handle--;
        else
            return NULL;
    }
    handle->val = -1;
    return handle;
}

/* Free one handle, shrinking the handle table if it's the last one */
static inline
void handle_free(struct buflib_context *ctx, union buflib_data *handle)
{
    handle->ptr = 0;
    /* Update free handle lower bound if this handle has a lower index than the
     * old one.
     */
    if (handle > ctx->first_free_handle)
        ctx->first_free_handle = handle;
    if (handle == ctx->last_handle)
        ctx->last_handle++;
    else
        ctx->compact = false;
}

/* Shrink the handle table, returning true if its size was reduced, false if
 * not
 */
static inline
bool
handle_table_shrink(struct buflib_context *ctx)
{
    bool rv;
    union buflib_data *handle;
    for (handle = ctx->last_handle; !(handle->ptr); handle++);
    if (handle > ctx->first_free_handle)
        ctx->first_free_handle = handle - 1;
    rv = handle == ctx->last_handle;
    ctx->last_handle = handle;
    return rv;
}

/* Compact allocations and handle table, adjusting handle pointers as needed.
 * Return true if any space was freed or consolidated, false otherwise.
 */
static bool
buflib_compact(struct buflib_context *ctx)
{
    BDEBUGF("%s(): Compacting!\n", __func__);
    union buflib_data *block = ctx->first_free_block, *new_block;
    int shift = 0, len;
    /* Store the results of attempting to shrink the handle table */
    bool ret = handle_table_shrink(ctx);
    for(; block != ctx->alloc_end; block += len)
    {
        len = block->val;
        /* This block is free, add its length to the shift value */
        if (len < 0)
        {
            shift += len;
            len = -len;
            continue;
        }
        /* If shift is non-zero, it represents the number of places to move
         * blocks down in memory. Calculate the new address for this block,
         * update its entry in the handle table, and then move its contents.
         */
        if (shift)
        {
            union buflib_data* tmp = block[1].ptr;
            struct buflib_callbacks *ops = block[2].ops;
            int handle = ctx->handle_table - tmp;
            BDEBUGF("%s(): moving \"%s\"(id=%d) by %d(%d)\n", __func__, &block[3].name,
                    handle, shift, shift*sizeof(union buflib_data));
            new_block = block + shift;
            /* call the callback before moving */
            if (ops != (&default_callbacks) && ops->move_callback)
                ops->move_callback(handle, tmp->ptr, tmp->ptr+shift);

            tmp->ptr += shift; /* update handle table */
            memmove(new_block, block, len * sizeof(union buflib_data));
        }
    }
    /* Move the end-of-allocation mark, and return true if any new space has
     * been freed.
     */
    ctx->alloc_end += shift;
    ctx->first_free_block = ctx->alloc_end;
    ctx->compact = true;
    return ret || shift;
}

/* Shift buffered items by size units, and update handle pointers. The shift
 * value must be determined to be safe *before* calling.
 */
static void
buflib_buffer_shift(struct buflib_context *ctx, int shift)
{
    memmove(ctx->buf_start + shift, ctx->buf_start,
        (ctx->alloc_end - ctx->buf_start) * sizeof(union buflib_data));
    union buflib_data *ptr;
    for (ptr = ctx->last_handle; ptr < ctx->handle_table; ptr++)
        if (ptr->ptr)
            ptr->ptr += shift;
    ctx->first_free_block += shift;
    ctx->buf_start += shift;
    ctx->alloc_end += shift;
}

/* Shift buffered items up by size bytes, or as many as possible if size == 0.
 * Set size to the number of bytes freed.
 */
void*
buflib_buffer_out(struct buflib_context *ctx, size_t *size)
{
    if (!ctx->compact)
        buflib_compact(ctx);
    size_t avail = ctx->last_handle - ctx->alloc_end;
    size_t avail_b = avail * sizeof(union buflib_data);
    if (*size && *size < avail_b)
    {
        avail = (*size + sizeof(union buflib_data) - 1)
            / sizeof(union buflib_data);
        avail_b = avail * sizeof(union buflib_data);
    }
    *size = avail_b;
    void *ret = ctx->buf_start;
    buflib_buffer_shift(ctx, avail);
    return ret;
}

/* Shift buffered items down by size bytes */
void
buflib_buffer_in(struct buflib_context *ctx, int size)
{
    size /= sizeof(union buflib_data);
    buflib_buffer_shift(ctx, -size);
}

struct buflib_callbacks* buflib_default_callbacks(void)
{
    return &default_callbacks;
}

/* Allocate a buffer of size bytes, returning a handle for it */
int
buflib_alloc(struct buflib_context *ctx, size_t size)
{
    return buflib_alloc_ex(ctx, size, "", &default_callbacks);
}

int
buflib_alloc_ex(struct buflib_context *ctx, size_t size, const char *name,
                struct buflib_callbacks *ops)
{
    union buflib_data *handle, *block;
    size_t name_len = name ? ALIGN_UP(strlen(name), sizeof(union buflib_data)) : 0;
    bool last = false;
    /* This really is assigned a value before use */
    int block_len;
    size += name_len;
    size = (size + sizeof(union buflib_data) - 1) /
           sizeof(union buflib_data)
           /* add 3 objects for alloc len, pointer to handle table entry and
            * name length, and the ops pointer */
           + 4;
handle_alloc:
    handle = handle_alloc(ctx);
    if (!handle)
    {
        /* If allocation has failed, and compaction has succeded, it may be
         * possible to get a handle by trying again.
         */
        if (!ctx->compact && buflib_compact(ctx))
            goto handle_alloc;
        else
            return 0;
    }

buffer_alloc:
    for (block = ctx->first_free_block;; block += block_len)
    {
        /* If the last used block extends all the way to the handle table, the
         * block "after" it doesn't have a header. Because of this, it's easier
         * to always find the end of allocation by saving a pointer, and always
         * calculate the free space at the end by comparing it to the
         * last_handle pointer.
         */
        if(block == ctx->alloc_end)
        {
            last = true;
            block_len = ctx->last_handle - block;
            if ((size_t)block_len < size)
                block = NULL;
            break;
        }
        block_len = block->val;
        /* blocks with positive length are already allocated. */
        if(block_len > 0)
            continue;
        block_len = -block_len;
        /* The search is first-fit, any fragmentation this causes will be 
         * handled at compaction.
         */
        if ((size_t)block_len >= size)
            break;
    }
    if (!block)
    {
        /* Try compacting if allocation failed, but only if the handle
         * allocation did not trigger compaction already, since there will
         * be no further gain.
         */
        if (!ctx->compact && buflib_compact(ctx))
        {
            goto buffer_alloc;
        } else {
            handle->val=1;
            handle_free(ctx, handle);
            return 0;
        }
    }

    /* Set up the allocated block, by marking the size allocated, and storing
     * a pointer to the handle.
     */
    union buflib_data *name_len_slot;
    block->val = size;
    block[1].ptr = handle;
    block[2].ops = ops;
    strcpy(&block[3].name, name);
    name_len_slot = (union buflib_data*)ALIGN_UP((uintptr_t)((char*)&block[3] + name_len), sizeof(union buflib_data));
    name_len_slot->val = 1 + name_len/sizeof(union buflib_data);
    handle->ptr = name_len_slot + 1;
    /* If we have just taken the first free block, the next allocation search
     * can save some time by starting after this block.
     */
    if (block == ctx->first_free_block)
        ctx->first_free_block += size;
    block += size;
    /* alloc_end must be kept current if we're taking the last block. */
    if (last)
        ctx->alloc_end = block;
    /* Only free blocks *before* alloc_end have tagged length. */
    else if ((size_t)block_len > size)
        block->val = size - block_len;
    /* Return the handle index as a positive integer. */
    return ctx->handle_table - handle;
}

/* Free the buffer associated with handle_num. */
void
buflib_free(struct buflib_context *ctx, int handle_num)
{
    union buflib_data *handle = ctx->handle_table - handle_num,
                      *freed_block = handle->ptr,
                      *block = ctx->first_free_block,
                      *next_block = block;
    /* jump over the string to find the beginning of the block */
    size_t name_len = freed_block[-1].val;
    freed_block = freed_block - name_len - 3;
    /* We need to find the block before the current one, to see if it is free
     * and can be merged with this one.
     */
    while (next_block < freed_block)
    {
        block = next_block;
        next_block += abs(block->val);
    }
    /* If next_block == block, the above loop didn't go anywhere. If it did,
     * and the block before this one is empty, we can combine them.
     */
    if (next_block == freed_block && next_block != block && block->val < 0)
        block->val -= freed_block->val;
    /* Otherwise, set block to the newly-freed block, and mark it free, before
     * continuing on, since the code below exects block to point to a free
     * block which may have free space after it.
     */
    else
    {
        block = freed_block;
        block->val = -block->val;
    }
    next_block = block - block->val;
    /* Check if we are merging with the free space at alloc_end. */
    if (next_block == ctx->alloc_end)
        ctx->alloc_end = block;
    /* Otherwise, the next block might still be a "normal" free block, and the
     * mid-allocation free means that the buffer is no longer compact.
     */
    else {
        ctx->compact = false;
        if (next_block->val < 0)
            block->val += next_block->val;
    }
    handle_free(ctx, handle);
    handle->ptr = NULL;
    /* If this block is before first_free_block, it becomes the new starting
     * point for free-block search.
     */
    if (block < ctx->first_free_block)
        ctx->first_free_block = block;
}

size_t
buflib_available(struct buflib_context* ctx)
{
    size_t diff = ctx->last_handle - ctx->alloc_end;
    /* leave spaces for some future handles */
    diff -= 128;

    if (diff > 0)
        return diff*sizeof(union buflib_data);
    else
        return 0;
}

int
buflib_realloc(struct buflib_context* ctx, int handle, size_t new_size)
{
    /* not implemented */
    return -1;
}

void
buflib_shrink(struct buflib_context* ctx, int handle, void* newstart, void* newend)
{
    /* not implemented */
    return;
}
