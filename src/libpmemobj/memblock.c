/*
 * Copyright 2016-2018, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * memblock.c -- implementation of memory block
 *
 * Memory block is a representation of persistent object that resides in the
 * heap. A valid memory block must be either a huge (free or used) chunk or a
 * block inside a run.
 *
 * Huge blocks are 1:1 correlated with the chunk headers in the zone whereas
 * run blocks are represented by bits in corresponding chunk bitmap.
 *
 * This file contains implementations of abstract operations on memory blocks.
 * Instead of storing the mbops structure inside each memory block the correct
 * method implementation is chosen at runtime.
 */

#include <string.h>

#include "obj.h"
#include "heap.h"
#include "memblock.h"
#include "out.h"
#include "valgrind_internal.h"

const size_t header_type_to_size[MAX_HEADER_TYPES] = {
	sizeof(struct allocation_header_legacy),
	sizeof(struct allocation_header_compact),
	0
};

const enum chunk_flags header_type_to_flag[MAX_HEADER_TYPES] = {
	0,
	CHUNK_FLAG_COMPACT_HEADER,
	CHUNK_FLAG_HEADER_NONE
};

/*
 * memblock_header_type -- determines the memory block's header type
 */
static enum header_type
memblock_header_type(const struct memory_block *m)
{
	struct chunk_header *hdr = heap_get_chunk_hdr(m->heap, m);

	if (hdr->flags & CHUNK_FLAG_COMPACT_HEADER)
		return HEADER_COMPACT;

	if (hdr->flags & CHUNK_FLAG_HEADER_NONE)
		return HEADER_NONE;

	return HEADER_LEGACY;
}

/*
 * memblock_header_legacy_get_size --
 *	(internal) returns the size stored in a legacy header
 */
static size_t
memblock_header_legacy_get_size(const struct memory_block *m)
{
	struct allocation_header_legacy *hdr = m->m_ops->get_real_data(m);

	return hdr->size;
}

/*
 * memblock_header_compact_get_size --
 *	(internal) returns the size stored in a compact header
 */
static size_t
memblock_header_compact_get_size(const struct memory_block *m)
{
	struct allocation_header_compact *hdr = m->m_ops->get_real_data(m);

	return hdr->size & ALLOC_HDR_FLAGS_MASK;
}

/*
 * memblock_header_none_get_size --
 *	(internal) determines the sizes of an object without a header
 */
static size_t
memblock_header_none_get_size(const struct memory_block *m)
{
	return m->m_ops->block_size(m);
}

/*
 * memblock_header_legacy_get_extra --
 *	(internal) returns the extra field stored in a legacy header
 */
static uint64_t
memblock_header_legacy_get_extra(const struct memory_block *m)
{
	struct allocation_header_legacy *hdr = m->m_ops->get_real_data(m);

	return hdr->type_num;
}

/*
 * memblock_header_compact_get_extra --
 *	(internal) returns the extra field stored in a compact header
 */
static uint64_t
memblock_header_compact_get_extra(const struct memory_block *m)
{
	struct allocation_header_compact *hdr = m->m_ops->get_real_data(m);

	return hdr->extra;
}

/*
 * memblock_header_none_get_extra --
 *	(internal) objects without a header don't have an extra field
 */
static uint64_t
memblock_header_none_get_extra(const struct memory_block *m)
{
	return 0;
}

/*
 * memblock_header_legacy_get_flags --
 *	(internal) returns the flags stored in a legacy header
 */
static uint16_t
memblock_header_legacy_get_flags(const struct memory_block *m)
{
	struct allocation_header_legacy *hdr = m->m_ops->get_real_data(m);

	return (uint16_t)(hdr->root_size >> ALLOC_HDR_SIZE_SHIFT);
}

/*
 * memblock_header_compact_get_flags --
 *	(internal) returns the flags stored in a compact header
 */
static uint16_t
memblock_header_compact_get_flags(const struct memory_block *m)
{
	struct allocation_header_compact *hdr = m->m_ops->get_real_data(m);

	return (uint16_t)(hdr->size >> ALLOC_HDR_SIZE_SHIFT);
}

/*
 * memblock_header_none_get_flags --
 *	(internal) objects without a header do not support flags
 */
static uint16_t
memblock_header_none_get_flags(const struct memory_block *m)
{
	return 0;
}

/*
 * memblock_header_legacy_write --
 *	(internal) writes a legacy header of an object
 */
static void
memblock_header_legacy_write(const struct memory_block *m,
	size_t size, uint64_t extra, uint16_t flags)
{
	struct allocation_header_legacy hdr;
	hdr.size = size;
	hdr.type_num = extra;
	hdr.root_size = ((uint64_t)flags << ALLOC_HDR_SIZE_SHIFT);

	struct allocation_header_legacy *hdrp = m->m_ops->get_real_data(m);

	VALGRIND_DO_MAKE_MEM_UNDEFINED(hdrp, sizeof(*hdrp));

	VALGRIND_ADD_TO_TX(hdrp, sizeof(*hdrp));
	pmemops_memcpy(&m->heap->p_ops, hdrp, &hdr,
		sizeof(hdr), /* legacy header is 64 bytes in size */
		PMEMOBJ_F_MEM_WC | PMEMOBJ_F_MEM_NODRAIN | PMEMOBJ_F_RELAXED);
	VALGRIND_REMOVE_FROM_TX(hdrp, sizeof(*hdrp));

	/* unused fields of the legacy headers are used as a red zone */
	VALGRIND_DO_MAKE_MEM_NOACCESS(hdrp->unused, sizeof(hdrp->unused));
}

/*
 * memblock_header_compact_write --
 *	(internal) writes a compact header of an object
 */
static void
memblock_header_compact_write(const struct memory_block *m,
	size_t size, uint64_t extra, uint16_t flags)
{
	COMPILE_ERROR_ON(ALLOC_HDR_COMPACT_SIZE > CACHELINE_SIZE);

	struct {
		struct allocation_header_compact hdr;
		uint8_t padding[CACHELINE_SIZE - ALLOC_HDR_COMPACT_SIZE];
	} padded;

	padded.hdr.size = size | ((uint64_t)flags << ALLOC_HDR_SIZE_SHIFT);
	padded.hdr.extra = extra;

	struct allocation_header_compact *hdrp = m->m_ops->get_real_data(m);

	VALGRIND_DO_MAKE_MEM_UNDEFINED(hdrp, sizeof(*hdrp));

	/*
	 * If possible write the entire header with a single memcpy, this allows
	 * the copy implementation to avoid a cache miss on a partial cache line
	 * write.
	 */
	size_t hdr_size = ALLOC_HDR_COMPACT_SIZE;
	if ((uintptr_t)hdrp % CACHELINE_SIZE == 0 && size >= sizeof(padded))
		hdr_size = sizeof(padded);

	VALGRIND_ADD_TO_TX(hdrp, hdr_size);

	pmemops_memcpy(&m->heap->p_ops, hdrp, &padded, hdr_size,
		PMEMOBJ_F_MEM_WC | PMEMOBJ_F_MEM_NODRAIN | PMEMOBJ_F_RELAXED);
	VALGRIND_DO_MAKE_MEM_UNDEFINED((char *)hdrp + ALLOC_HDR_COMPACT_SIZE,
		hdr_size - ALLOC_HDR_COMPACT_SIZE);

	VALGRIND_REMOVE_FROM_TX(hdrp, hdr_size);
}

/*
 * memblock_header_none_write --
 *	(internal) nothing to write
 */
static void
memblock_header_none_write(const struct memory_block *m,
	size_t size, uint64_t extra, uint16_t flags)
{
	/* NOP */
}

/*
 * memblock_header_legacy_invalidate --
 *	(internal) invalidates a legacy header
 */
static void
memblock_header_legacy_invalidate(const struct memory_block *m)
{
	struct allocation_header_legacy *hdr = m->m_ops->get_real_data(m);
	VALGRIND_SET_CLEAN(hdr, sizeof(*hdr));
}

/*
 * memblock_header_compact_invalidate --
 *	(internal) invalidates a compact header
 */
static void
memblock_header_compact_invalidate(const struct memory_block *m)
{
	struct allocation_header_compact *hdr = m->m_ops->get_real_data(m);
	VALGRIND_SET_CLEAN(hdr, sizeof(*hdr));
}

/*
 * memblock_no_header_invalidate --
 *	(internal) nothing to invalidate
 */
static void
memblock_header_none_invalidate(const struct memory_block *m)
{
	/* NOP */
}

/*
 * memblock_header_legacy_reinit --
 *	(internal) reinitializes a legacy header after a heap restart
 */
static void
memblock_header_legacy_reinit(const struct memory_block *m)
{
	struct allocation_header_legacy *hdr = m->m_ops->get_real_data(m);

	VALGRIND_DO_MAKE_MEM_DEFINED(hdr, sizeof(*hdr));

	/* unused fields of the legacy headers are used as a red zone */
	VALGRIND_DO_MAKE_MEM_NOACCESS(hdr->unused, sizeof(hdr->unused));
}

/*
 * memblock_header_compact_reinit --
 *	(internal) reinitializes a compact header after a heap restart
 */
static void
memblock_header_compact_reinit(const struct memory_block *m)
{
	struct allocation_header_compact *hdr = m->m_ops->get_real_data(m);

	VALGRIND_DO_MAKE_MEM_DEFINED(hdr, sizeof(*hdr));
}

/*
 * memblock_header_none_reinit --
 *	(internal) nothing to reinitialize
 */
static void
memblock_header_none_reinit(const struct memory_block *m)
{
	/* NOP */
}

static struct {
	/* determines the sizes of an object */
	size_t (*get_size)(const struct memory_block *m);

	/* returns the extra field (if available, 0 if not) */
	uint64_t (*get_extra)(const struct memory_block *m);

	/* returns the flags stored in a header (if available, 0 if not) */
	uint16_t (*get_flags)(const struct memory_block *m);

	/*
	 * Stores size, extra info and flags in header of an object
	 * (if available, does nothing otherwise).
	 */
	void (*write)(const struct memory_block *m,
		size_t size, uint64_t extra, uint16_t flags);
	void (*invalidate)(const struct memory_block *m);

	/*
	 * Reinitializes a header after a heap restart (if available, does
	 * nothing otherwise) (VG).
	 */
	void (*reinit)(const struct memory_block *m);
} memblock_header_ops[MAX_HEADER_TYPES] = {
	[HEADER_LEGACY] = {
		memblock_header_legacy_get_size,
		memblock_header_legacy_get_extra,
		memblock_header_legacy_get_flags,
		memblock_header_legacy_write,
		memblock_header_legacy_invalidate,
		memblock_header_legacy_reinit,
	},
	[HEADER_COMPACT] = {
		memblock_header_compact_get_size,
		memblock_header_compact_get_extra,
		memblock_header_compact_get_flags,
		memblock_header_compact_write,
		memblock_header_compact_invalidate,
		memblock_header_compact_reinit,
	},
	[HEADER_NONE] = {
		memblock_header_none_get_size,
		memblock_header_none_get_extra,
		memblock_header_none_get_flags,
		memblock_header_none_write,
		memblock_header_none_invalidate,
		memblock_header_none_reinit,
	}
};

/*
 * huge_block_size -- returns the compile-time constant which defines the
 *	huge memory block size.
 */
static size_t
huge_block_size(const struct memory_block *m)
{
	return CHUNKSIZE;
}

/*
 * run_block_size -- looks for the right chunk and returns the block size
 *	information that is attached to the run block metadata.
 */
static size_t
run_block_size(const struct memory_block *m)
{
	struct chunk_run *run = heap_get_chunk_run(m->heap, m);

	return run->block_size;
}

/*
 * huge_get_real_data -- returns pointer to the beginning data of a huge block
 */
static void *
huge_get_real_data(const struct memory_block *m)
{
	return heap_get_chunk(m->heap, m)->data;
}

/*
 * run_get_data_start -- (internal) returns the pointer to the beginning of
 *	allocations in a run
 */
static char *
run_get_data_start(struct chunk_header *hdr, struct chunk_run *run,
	enum header_type htype)
{
	if (hdr->flags & CHUNK_FLAG_ALIGNED) {
		/*
		 * Alignment is property of user data in allocations. And
		 * since objects have headers, we need to take them into
		 * account when calculating the address.
		 */
		uintptr_t hsize = header_type_to_size[htype];
		uintptr_t base = (uintptr_t)run->data + hsize;
		return (char *)(ALIGN_UP(base, run->alignment) - hsize);
	} else {
		return (char *)&run->data;
	}
}

/*
 * run_get_alignment_padding -- (internal) returns the number of bytes of
 *	padding in aligned runs
 */
static size_t
run_get_alignment_padding(struct chunk_header *hdr, struct chunk_run *run,
	enum header_type htype)
{
	return (size_t)run_get_data_start(hdr, run, htype) - (size_t)&run->data;
}

/*
 * run_get_real_data -- returns pointer to the beginning data of a run block
 */
static void *
run_get_real_data(const struct memory_block *m)
{
	struct chunk_run *run = heap_get_chunk_run(m->heap, m);
	struct chunk_header *hdr = heap_get_chunk_hdr(m->heap, m);
	ASSERT(run->block_size != 0);

	return run_get_data_start(hdr, run, m->header_type) +
		(run->block_size * m->block_off);
}

/*
 * block_get_user_data -- returns pointer to the data of a block
 */
static void *
block_get_user_data(const struct memory_block *m)
{
	return (char *)m->m_ops->get_real_data(m) +
		header_type_to_size[m->header_type];
}

/*
 * chunk_get_chunk_hdr_value -- (internal) get value of a header for redo log
 */
static uint64_t
chunk_get_chunk_hdr_value(uint16_t type, uint16_t flags, uint32_t size_idx)
{
	uint64_t val;
	COMPILE_ERROR_ON(sizeof(struct chunk_header) != sizeof(uint64_t));

	struct chunk_header hdr;
	hdr.type = type;
	hdr.flags = flags;
	hdr.size_idx = size_idx;
	memcpy(&val, &hdr, sizeof(val));

	return val;
}

/*
 * huge_prep_operation_hdr -- prepares the new value of a chunk header that will
 *	be set after the operation concludes.
 */
static void
huge_prep_operation_hdr(const struct memory_block *m, enum memblock_state op,
	struct operation_context *ctx)
{
	struct chunk_header *hdr = heap_get_chunk_hdr(m->heap, m);

	/*
	 * Depending on the operation that needs to be performed a new chunk
	 * header needs to be prepared with the new chunk state.
	 */
	uint64_t val = chunk_get_chunk_hdr_value(
		op == MEMBLOCK_ALLOCATED ? CHUNK_TYPE_USED : CHUNK_TYPE_FREE,
		hdr->flags,
		m->size_idx);

	if (ctx == NULL) {
		util_atomic_store_explicit64((uint64_t *)hdr, val,
			memory_order_relaxed);
		pmemops_persist(&m->heap->p_ops, hdr, sizeof(*hdr));
	} else {
		operation_add_entry(ctx, hdr, val, REDO_OPERATION_SET);
	}

	VALGRIND_DO_MAKE_MEM_NOACCESS(hdr + 1,
		(hdr->size_idx - 1) * sizeof(struct chunk_header));

	/*
	 * In the case of chunks larger than one unit the footer must be
	 * created immediately AFTER the persistent state is safely updated.
	 */
	if (m->size_idx == 1)
		return;

	struct chunk_header *footer = hdr + m->size_idx - 1;
	VALGRIND_DO_MAKE_MEM_UNDEFINED(footer, sizeof(*footer));

	val = chunk_get_chunk_hdr_value(CHUNK_TYPE_FOOTER, 0, m->size_idx);

	/*
	 * It's only safe to write the footer AFTER the persistent part of
	 * the operation have been successfully processed because the footer
	 * pointer might point to a currently valid persistent state
	 * of a different chunk.
	 * The footer entry change is updated as transient because it will
	 * be recreated at heap boot regardless - it's just needed for runtime
	 * operations.
	 */
	if (ctx == NULL) {
		util_atomic_store_explicit64((uint64_t *)footer, val,
			memory_order_relaxed);
		VALGRIND_SET_CLEAN(footer, sizeof(*footer));
	} else {
		operation_add_typed_entry(ctx,
			footer, val, REDO_OPERATION_SET, LOG_TRANSIENT);
	}
}

/*
 * run_prep_operation_hdr -- prepares the new value for a select few bytes of
 *	a run bitmap that will be set after the operation concludes.
 *
 * It's VERY important to keep in mind that the particular value of the
 * bitmap this method is modifying must not be changed after this function
 * is called and before the operation is processed.
 */
static void
run_prep_operation_hdr(const struct memory_block *m, enum memblock_state op,
	struct operation_context *ctx)
{
	struct chunk_run *r = heap_get_chunk_run(m->heap, m);

	ASSERT(m->size_idx <= BITS_PER_VALUE);

	/*
	 * Free blocks are represented by clear bits and used blocks by set
	 * bits - which is the reverse of the commonly used scheme.
	 *
	 * Here a bit mask is prepared that flips the bits that represent the
	 * memory block provided by the caller - because both the size index and
	 * the block offset are tied 1:1 to the bitmap this operation is
	 * relatively simple.
	 */
	uint64_t bmask;
	if (m->size_idx == BITS_PER_VALUE) {
		ASSERTeq(m->block_off % BITS_PER_VALUE, 0);
		bmask = UINT64_MAX;
	} else {
		bmask = ((1ULL << m->size_idx) - 1ULL) <<
				(m->block_off % BITS_PER_VALUE);
	}

	/*
	 * The run bitmap is composed of several 8 byte values, so a proper
	 * element of the bitmap array must be selected.
	 */
	int bpos = m->block_off / BITS_PER_VALUE;

	/* the bit mask is applied immediately by the add entry operations */
	if (op == MEMBLOCK_ALLOCATED) {
		operation_add_entry(ctx, &r->bitmap[bpos],
			bmask, REDO_OPERATION_OR);
	} else if (op == MEMBLOCK_FREE) {
		operation_add_entry(ctx, &r->bitmap[bpos],
			~bmask, REDO_OPERATION_AND);
	} else {
		ASSERT(0);
	}
}

/*
 * huge_get_lock -- because huge memory blocks are always allocated from a
 *	single bucket there's no reason to lock them - the bucket itself is
 *	protected.
 */
static os_mutex_t *
huge_get_lock(const struct memory_block *m)
{
	return NULL;
}

/*
 * run_get_lock -- gets the runtime mutex from the heap.
 */
static os_mutex_t *
run_get_lock(const struct memory_block *m)
{
	return heap_get_run_lock(m->heap, m->chunk_id);
}

/*
 * huge_get_state -- returns whether a huge block is allocated or not
 */
static enum memblock_state
huge_get_state(const struct memory_block *m)
{
	struct chunk_header *hdr = heap_get_chunk_hdr(m->heap, m);

	if (hdr->type == CHUNK_TYPE_USED)
		return MEMBLOCK_ALLOCATED;

	if (hdr->type == CHUNK_TYPE_FREE)
		return MEMBLOCK_FREE;

	return MEMBLOCK_STATE_UNKNOWN;
}

/*
 * huge_get_state -- returns whether a block from a run is allocated or not
 */
static enum memblock_state
run_get_state(const struct memory_block *m)
{
	struct zone *z = ZID_TO_ZONE(m->heap->layout, m->zone_id);
	struct chunk_header *hdr = &z->chunk_headers[m->chunk_id];
	ASSERTeq(hdr->type, CHUNK_TYPE_RUN);

	struct chunk_run *r = (struct chunk_run *)&z->chunks[m->chunk_id];

	unsigned v = m->block_off / BITS_PER_VALUE;
	uint64_t bitmap = r->bitmap[v];
	unsigned b = m->block_off % BITS_PER_VALUE;

	unsigned b_last = b + m->size_idx;
	ASSERT(b_last <= BITS_PER_VALUE);

	for (unsigned i = b; i < b_last; ++i) {
		if (!BIT_IS_CLR(bitmap, i)) {
			return MEMBLOCK_ALLOCATED;
		}
	}

	return MEMBLOCK_FREE;
}

/*
 * huge_ensure_header_type -- checks the header type of a chunk and modifies
 *	it if necessary. This is fail-safe atomic.
 */
static void
huge_ensure_header_type(const struct memory_block *m,
	enum header_type t)
{
	struct chunk_header *hdr = heap_get_chunk_hdr(m->heap, m);
	ASSERTeq(hdr->type, CHUNK_TYPE_FREE);

	if ((hdr->flags & header_type_to_flag[t]) == 0) {
		VALGRIND_ADD_TO_TX(hdr, sizeof(*hdr));
		uint16_t f = ((uint16_t)header_type_to_flag[t]);
		hdr->flags |= f;
		pmemops_persist(&m->heap->p_ops, hdr, sizeof(*hdr));
		VALGRIND_REMOVE_FROM_TX(hdr, sizeof(*hdr));
	}
}

/*
 * run_ensure_header_type -- runs must be created with appropriate header type.
 */
static void
run_ensure_header_type(const struct memory_block *m,
	enum header_type t)
{
#ifdef DEBUG
	struct chunk_header *hdr = heap_get_chunk_hdr(m->heap, m);
	ASSERTeq(hdr->type, CHUNK_TYPE_RUN);
	ASSERT((hdr->flags & header_type_to_flag[t]) == header_type_to_flag[t]);
#endif
}

/*
 * block_get_real_size -- returns the size of a memory block that includes all
 *	of the overhead (headers)
 */
static size_t
block_get_real_size(const struct memory_block *m)
{
	/*
	 * There are two valid ways to get a size. If the memory block
	 * initialized properly and the size index is set, the chunk unit size
	 * can be simply multiplied by that index, otherwise we need to look at
	 * the allocation header.
	 */
	if (m->size_idx != 0) {
		return m->m_ops->block_size(m) * m->size_idx;
	} else {
		return memblock_header_ops[m->header_type].get_size(m);
	}
}

/*
 * block_get_user_size -- returns the size of a memory block without overheads,
 *	this is the size of a data block that can be used.
 */
static size_t
block_get_user_size(const struct memory_block *m)
{
	return block_get_real_size(m) - header_type_to_size[m->header_type];
}

/*
 * block_write_header -- writes a header of an allocation
 */
static void
block_write_header(const struct memory_block *m,
	uint64_t extra_field, uint16_t flags)
{
	memblock_header_ops[m->header_type].write(m,
		block_get_real_size(m), extra_field, flags);
}

/*
 * block_invalidate -- invalidates allocation data and header
 */
static void
block_invalidate(const struct memory_block *m)
{
	void *data = m->m_ops->get_user_data(m);
	size_t size = m->m_ops->get_user_size(m);
	VALGRIND_SET_CLEAN(data, size);

	memblock_header_ops[m->header_type].invalidate(m);
}

/*
 * block_reinit_header -- reinitializes a block after a heap restart
 */
static void
block_reinit_header(const struct memory_block *m)
{
	memblock_header_ops[m->header_type].reinit(m);
}

/*
 * block_get_extra -- returns the extra field of an allocation
 */
static uint64_t
block_get_extra(const struct memory_block *m)
{
	return memblock_header_ops[m->header_type].get_extra(m);
}

/*
 * block_get_flags -- returns the flags of an allocation
 */
static uint16_t
block_get_flags(const struct memory_block *m)
{
	return memblock_header_ops[m->header_type].get_flags(m);
}

static const struct memory_block_ops mb_ops[MAX_MEMORY_BLOCK] = {
	[MEMORY_BLOCK_HUGE] = {
		.block_size = huge_block_size,
		.prep_hdr = huge_prep_operation_hdr,
		.get_lock = huge_get_lock,
		.get_state = huge_get_state,
		.get_user_data = block_get_user_data,
		.get_real_data = huge_get_real_data,
		.get_user_size = block_get_user_size,
		.get_real_size = block_get_real_size,
		.write_header = block_write_header,
		.invalidate = block_invalidate,
		.ensure_header_type = huge_ensure_header_type,
		.reinit_header = block_reinit_header,
		.get_extra = block_get_extra,
		.get_flags = block_get_flags,
	},
	[MEMORY_BLOCK_RUN] = {
		.block_size = run_block_size,
		.prep_hdr = run_prep_operation_hdr,
		.get_lock = run_get_lock,
		.get_state = run_get_state,
		.get_user_data = block_get_user_data,
		.get_real_data = run_get_real_data,
		.get_user_size = block_get_user_size,
		.get_real_size = block_get_real_size,
		.write_header = block_write_header,
		.invalidate = block_invalidate,
		.ensure_header_type = run_ensure_header_type,
		.reinit_header = block_reinit_header,
		.get_extra = block_get_extra,
		.get_flags = block_get_flags,
	}
};

/*
 * memblock_detect_type -- looks for the corresponding chunk header and
 *	depending on the chunks type returns the right memory block type
 */
static enum memory_block_type
memblock_detect_type(struct palloc_heap *heap, const struct memory_block *m)
{
	enum memory_block_type ret;

	switch (heap_get_chunk_hdr(heap, m)->type) {
		case CHUNK_TYPE_RUN:
		case CHUNK_TYPE_RUN_DATA:
			ret = MEMORY_BLOCK_RUN;
			break;
		case CHUNK_TYPE_FREE:
		case CHUNK_TYPE_USED:
		case CHUNK_TYPE_FOOTER:
			ret = MEMORY_BLOCK_HUGE;
			break;
		default:
			/* unreachable */
			FATAL("possible zone chunks metadata corruption");
	}
	return ret;
}

/*
 * memblock_from_offset -- resolves a memory block data from an offset that
 *	originates from the heap
 */
struct memory_block
memblock_from_offset_opt(struct palloc_heap *heap, uint64_t off, int size)
{
	struct memory_block m = MEMORY_BLOCK_NONE;
	m.heap = heap;

	off -= HEAP_PTR_TO_OFF(heap, &heap->layout->zone0);
	m.zone_id = (uint32_t)(off / ZONE_MAX_SIZE);

	off -= (ZONE_MAX_SIZE * m.zone_id) + sizeof(struct zone);
	m.chunk_id = (uint32_t)(off / CHUNKSIZE);

	struct chunk_header *hdr = heap_get_chunk_hdr(heap, &m);

	if (hdr->type == CHUNK_TYPE_RUN_DATA)
		m.chunk_id -= hdr->size_idx;

	off -= CHUNKSIZE * m.chunk_id;

	m.header_type = memblock_header_type(&m);

	off -= header_type_to_size[m.header_type];

	m.type = off != 0 ? MEMORY_BLOCK_RUN : MEMORY_BLOCK_HUGE;
	ASSERTeq(memblock_detect_type(heap, &m), m.type);

	m.m_ops = &mb_ops[m.type];

	uint64_t unit_size = m.m_ops->block_size(&m);

	if (off != 0) { /* run */
		struct chunk_run *run = heap_get_chunk_run(heap, &m);

		off -= run_get_alignment_padding(hdr, run, m.header_type);
		off -= RUN_METASIZE;
		m.block_off = (uint16_t)(off / unit_size);
		off -= m.block_off * unit_size;
	}

	m.size_idx = !size ? 0 : CALC_SIZE_IDX(unit_size,
		memblock_header_ops[m.header_type].get_size(&m));

	ASSERTeq(off, 0);

	return m;
}

/*
 * memblock_from_offset -- returns memory block with size
 */
struct memory_block
memblock_from_offset(struct palloc_heap *heap, uint64_t off)
{
	return memblock_from_offset_opt(heap, off, 1);
}

/*
 * memblock_rebuild_state -- fills in the runtime-state related fields of a
 *	memory block structure
 *
 * This function must be called on all memory blocks that were created by hand
 * (as opposed to retrieved from memblock_from_offset function).
 */
void
memblock_rebuild_state(struct palloc_heap *heap, struct memory_block *m)
{
	m->heap = heap;
	m->header_type = memblock_header_type(m);
	m->type = memblock_detect_type(heap, m);
	m->m_ops = &mb_ops[m->type];
}
