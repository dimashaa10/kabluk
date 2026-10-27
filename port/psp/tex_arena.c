/*
 * port/psp/tex_arena.c -- see tex_arena.h for what this is and why it exists.
 *
 * The allocator is a first-fit free list kept in address order, with the
 * block headers living inside the arena itself.  Splitting happens on
 * allocation and coalescing on free, in both directions: that is the whole
 * reason for the file.  A free run that is smaller than
 * TEX_ARENA_MIN_SPLIT is not split, so the list cannot be filled with
 * slivers.
 */

#include "tex_arena.h"

/* A block has to hold the header plus a payload, and the payload has to be
 * 16-byte aligned whatever the header's own size is. */
#define TEX_ARENA_HDR     ((sizeof(tex_arena_block_t) + (TEX_ARENA_ALIGN - 1)) & \
                           ~(size_t)(TEX_ARENA_ALIGN - 1))

/* Tail smaller than this stays part of the allocated block. */
#define TEX_ARENA_MIN_SPLIT (TEX_ARENA_HDR + 64u)

static int tex_arena_ok (const tex_arena_t *a)
{
	return a && a->base && a->first && a->bytes >= TEX_ARENA_HDR;
}

int tex_arena_init (tex_arena_t *a, void *base, size_t bytes)
{
	if (!a)
		return 0;

	a->base = NULL;
	a->bytes = 0;
	a->first = NULL;

	if (!base || ((size_t)(unsigned long)base & (TEX_ARENA_ALIGN - 1)))
		return 0;

	if (bytes < TEX_ARENA_HDR + TEX_ARENA_ALIGN)
		return 0;

	bytes &= ~(size_t)(TEX_ARENA_ALIGN - 1);

	a->base = base;
	a->bytes = bytes;
	a->first = (tex_arena_block_t *)base;
	a->first->size = bytes;
	a->first->free = 1;
	a->first->next = NULL;

	return 1;
}

void *tex_arena_alloc (tex_arena_t *a, size_t need)
{
	tex_arena_block_t *b;

	if (!tex_arena_ok(a) || need == 0)
		return NULL;

	need = (need + (TEX_ARENA_ALIGN - 1)) & ~(size_t)(TEX_ARENA_ALIGN - 1);

	if (need > a->bytes)
		return NULL;

	for (b = a->first; b; b = b->next)
	{
		size_t total = need + TEX_ARENA_HDR;

		if (!b->free || b->size < total)
			continue;

		if (b->size - total >= TEX_ARENA_MIN_SPLIT)
		{
			tex_arena_block_t *rest =
				(tex_arena_block_t *)((char *)b + total);

			rest->size = b->size - total;
			rest->free = 1;
			rest->next = b->next;

			b->next = rest;
			b->size = total;
		}

		b->free = 0;
		return (char *)b + TEX_ARENA_HDR;
	}

	return NULL;
}

void tex_arena_free (tex_arena_t *a, void *ptr)
{
	tex_arena_block_t *b, *p;

	if (!tex_arena_ok(a) || !ptr)
		return;

	/* Refuse anything that is not a payload of this arena rather than
	 * writing a "block header" into whatever the caller passed. */
	if ((char *)ptr < (char *)a->base + TEX_ARENA_HDR ||
	    (char *)ptr >= (char *)a->base + a->bytes)
		return;

	b = (tex_arena_block_t *)((char *)ptr - TEX_ARENA_HDR);
	if (b->free)
		return;	/* already free: a double free must not corrupt the list */

	/* Every block in the list is 16-byte aligned, so an address that is not
	 * on the list is not ours to free. */
	{
		int found = 0;

		for (p = a->first; p; p = p->next)
			if (p == b)
			{
				found = 1;
				break;
			}

		if (!found)
			return;
	}

	b->free = 1;

	/* merge forwards, again if the block after the merged one is free too */
	while (b->next && b->next->free)
	{
		b->size += b->next->size;
		b->next = b->next->next;
	}

	/* merge backwards: find the block that precedes b */
	for (p = a->first; p && p->next != b; p = p->next)
		;

	if (p && p->free)
	{
		p->size += b->size;
		p->next = b->next;
	}
}

size_t tex_arena_used (const tex_arena_t *a)
{
	const tex_arena_block_t *b;
	size_t used = 0;

	if (!tex_arena_ok(a))
		return 0;

	for (b = a->first; b; b = b->next)
		if (!b->free)
			used += b->size;

	return used;
}

size_t tex_arena_size (const tex_arena_t *a)
{
	return a ? a->bytes : 0;
}

size_t tex_arena_largest_free (const tex_arena_t *a)
{
	const tex_arena_block_t *b;
	size_t best = 0;

	if (!tex_arena_ok(a))
		return 0;

	for (b = a->first; b; b = b->next)
		if (b->free && b->size > best)
			best = b->size;

	/* The whole block, not just its payload, is what a new allocation can
	 * take (and it has to hold a header of its own). */
	return best >= TEX_ARENA_HDR ? best - TEX_ARENA_HDR : 0;
}

int tex_arena_free_runs (const tex_arena_t *a)
{
	const tex_arena_block_t *b;
	int runs = 0;

	if (!tex_arena_ok(a))
		return 0;

	for (b = a->first; b; b = b->next)
		if (b->free)
			runs++;

	return runs;
}

int tex_arena_check (const tex_arena_t *a, const char **why)
{
	const tex_arena_block_t *b, *prev = NULL;
	size_t sum = 0;

	if (why)
		*why = "arena is not initialised";

	if (!tex_arena_ok(a))
		return 0;

	for (b = a->first; b; b = b->next)
	{
		if ((char *)b < (char *)a->base ||
		    (char *)b + b->size > (char *)a->base + a->bytes)
		{
			if (why) *why = "a block lies outside the arena";
			return 0;
		}

		if (prev && (char *)prev + prev->size != (char *)b)
		{
			if (why) *why = "the blocks do not tile the arena";
			return 0;
		}

		if (prev && prev->free && b->free)
		{
			if (why) *why = "two free blocks are neighbours (coalescing lost them)";
			return 0;
		}

		sum += b->size;
		prev = b;
	}

	if (sum != a->bytes)
	{
		if (why) *why = "the blocks do not add up to the arena size";
		return 0;
	}

	if (why)
		*why = NULL;

	return 1;
}
