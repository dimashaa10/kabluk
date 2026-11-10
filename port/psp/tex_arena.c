/*
 * port/psp/tex_arena.c -- see tex_arena.h for what this is and why it exists.
 *
 * Each chunk is a first-fit free list kept in address order, with the block
 * headers living inside the chunk itself.  Splitting happens on allocation and
 * coalescing on free, in both directions: that is the whole reason for the
 * file.  A free run smaller than TEX_ARENA_MIN_SPLIT is not split off, so the
 * list cannot be filled with slivers that fit nothing.
 */

#include "tex_arena.h"

/* A block has to hold the header plus a payload, and the payload has to be
 * 16-byte aligned whatever the header's own size is. */
#define TEX_ARENA_HDR     ((sizeof(tex_arena_block_t) + (TEX_ARENA_ALIGN - 1)) & \
                           ~(size_t)(TEX_ARENA_ALIGN - 1))

/* Tail smaller than this stays part of the allocated block. */
#define TEX_ARENA_MIN_SPLIT (TEX_ARENA_HDR + 64u)

void tex_arena_init (tex_arena_t *a)
{
	int i;

	if (!a)
		return;

	a->chunks = 0;

	for (i = 0; i < TEX_ARENA_MAX_CHUNKS; i++)
	{
		a->chunk[i].base = NULL;
		a->chunk[i].bytes = 0;
		a->chunk[i].used = 0;
		a->chunk[i].first = NULL;
	}
}

int tex_arena_add_chunk (tex_arena_t *a, void *base, size_t bytes)
{
	tex_arena_chunk_t *c;

	if (!a || !base)
		return 0;

	if (a->chunks >= TEX_ARENA_MAX_CHUNKS)
		return 0;

	if ((size_t)(unsigned long)base & (TEX_ARENA_ALIGN - 1))
		return 0;

	bytes &= ~(size_t)(TEX_ARENA_ALIGN - 1);

	if (bytes < TEX_ARENA_HDR + TEX_ARENA_ALIGN)
		return 0;

	c = &a->chunk[a->chunks];

	c->base = base;
	c->bytes = bytes;
	c->used = 0;
	c->first = (tex_arena_block_t *)base;
	c->first->size = bytes;
	c->first->free = 1;
	c->first->next = NULL;

	a->chunks++;
	return 1;
}

void *tex_arena_alloc (tex_arena_t *a, size_t need)
{
	int i;

	if (!a || need == 0)
		return NULL;

	need = (need + (TEX_ARENA_ALIGN - 1)) & ~(size_t)(TEX_ARENA_ALIGN - 1);

	/* First fit across the chunks, in the order they were added: older
	 * chunks fill up before a new one is asked for, which is what keeps the
	 * number of chunks (and the idle space at the end of each) small. */
	for (i = 0; i < a->chunks; i++)
	{
		tex_arena_chunk_t *c = &a->chunk[i];
		tex_arena_block_t *b;

		if (c->bytes < need + TEX_ARENA_HDR)
			continue;

		for (b = c->first; b; b = b->next)
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
			c->used += b->size;

			return (char *)b + TEX_ARENA_HDR;
		}
	}

	return NULL;
}

/* The chunk a pointer belongs to, or NULL when it is not ours. */
static tex_arena_chunk_t *tex_arena_chunk_of (tex_arena_t *a, void *ptr,
                                              tex_arena_block_t **out_block)
{
	int i;

	for (i = 0; i < a->chunks; i++)
	{
		tex_arena_chunk_t *c = &a->chunk[i];

		if ((char *)ptr < (char *)c->base ||
		    (char *)ptr >= (char *)c->base + c->bytes)
			continue;

		if (out_block)
			*out_block = (tex_arena_block_t *)((char *)ptr - TEX_ARENA_HDR);

		return c;
	}

	if (out_block)
		*out_block = NULL;

	return NULL;
}

void tex_arena_free (tex_arena_t *a, void *ptr)
{
	tex_arena_block_t *b = NULL, *p;
	tex_arena_chunk_t *c;

	if (!a || !ptr)
		return;

	c = tex_arena_chunk_of(a, ptr, &b);
	if (!c || !b)
		return;

	/* A pointer that is not a block header of this chunk (a stray address
	 * inside it) must not be written to. */
	{
		int found = 0;

		for (p = c->first; p; p = p->next)
			if (p == b)
			{
				found = 1;
				break;
			}

		if (!found || b->free)
			return;	/* not a block, or already free: a double free */
	}

	c->used -= b->size;
	b->free = 1;

	/* merge forwards, again if the block after the merged one is free too */
	while (b->next && b->next->free)
	{
		b->size += b->next->size;
		b->next = b->next->next;
	}

	/* merge backwards: find the block that precedes b */
	for (p = c->first; p && p->next != b; p = p->next)
		;

	if (p && p->free)
	{
		p->size += b->size;
		p->next = b->next;
	}
}

size_t tex_arena_used (const tex_arena_t *a)
{
	size_t used = 0;
	int i;

	if (!a)
		return 0;

	for (i = 0; i < a->chunks; i++)
		used += a->chunk[i].used;

	return used;
}

size_t tex_arena_size (const tex_arena_t *a)
{
	size_t bytes = 0;
	int i;

	if (!a)
		return 0;

	for (i = 0; i < a->chunks; i++)
		bytes += a->chunk[i].bytes;

	return bytes;
}

size_t tex_arena_largest_free (const tex_arena_t *a)
{
	size_t best = 0;
	int i;

	if (!a)
		return 0;

	for (i = 0; i < a->chunks; i++)
	{
		const tex_arena_block_t *b;

		for (b = a->chunk[i].first; b; b = b->next)
			if (b->free && b->size > best)
				best = b->size;
	}

	/* The whole block, not just its payload, is what a new allocation can
	 * take (and it has to hold a header of its own). */
	return best >= TEX_ARENA_HDR ? best - TEX_ARENA_HDR : 0;
}

int tex_arena_free_runs (const tex_arena_t *a)
{
	int runs = 0, i;

	if (!a)
		return 0;

	for (i = 0; i < a->chunks; i++)
	{
		const tex_arena_block_t *b;

		for (b = a->chunk[i].first; b; b = b->next)
			if (b->free)
				runs++;
	}

	return runs;
}

int tex_arena_idle_chunk (const tex_arena_t *a)
{
	int i;

	if (!a || a->chunks < 2)
		return -1;

	for (i = 0; i < a->chunks; i++)
		if (a->chunk[i].used == 0)
			return i;

	return -1;
}

void tex_arena_remove_chunk (tex_arena_t *a, int index)
{
	int i;

	if (!a || index < 0 || index >= a->chunks)
		return;

	for (i = index; i + 1 < a->chunks; i++)
		a->chunk[i] = a->chunk[i + 1];

	a->chunks--;

	a->chunk[a->chunks].base = NULL;
	a->chunk[a->chunks].bytes = 0;
	a->chunk[a->chunks].used = 0;
	a->chunk[a->chunks].first = NULL;
}

int tex_arena_check (const tex_arena_t *a, const char **why)
{
	int i;

	if (why)
		*why = "arena is not initialised";

	if (!a)
		return 0;

	for (i = 0; i < a->chunks; i++)
	{
		const tex_arena_chunk_t *c = &a->chunk[i];
		const tex_arena_block_t *b, *prev = NULL;
		size_t sum = 0, used = 0;

		if (!c->base || !c->first || c->bytes < TEX_ARENA_HDR)
		{
			if (why) *why = "a chunk has no block list";
			return 0;
		}

		for (b = c->first; b; b = b->next)
		{
			if ((char *)b < (char *)c->base ||
			    (char *)b + b->size > (char *)c->base + c->bytes)
			{
				if (why) *why = "a block lies outside its chunk";
				return 0;
			}

			if (prev && (char *)prev + prev->size != (char *)b)
			{
				if (why) *why = "the blocks do not tile the chunk";
				return 0;
			}

			if (prev && prev->free && b->free)
			{
				if (why) *why = "two free blocks are neighbours (coalescing lost them)";
				return 0;
			}

			sum += b->size;
			if (!b->free)
				used += b->size;

			prev = b;
		}

		if (sum != c->bytes)
		{
			if (why) *why = "the blocks do not add up to the chunk size";
			return 0;
		}

		if (used != c->used)
		{
			if (why) *why = "the used counter disagrees with the blocks";
			return 0;
		}
	}

	if (why)
		*why = NULL;

	return 1;
}
