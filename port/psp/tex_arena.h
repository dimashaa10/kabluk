/*
 * port/psp/tex_arena.h -- the allocator the picture memory lives in.
 *
 * The GE on a PSP has no texture memory: it samples pictures straight out of
 * main memory, so every picture the engine keeps is a block of the same heap
 * the map, the models and the pictures being decoded are fighting over.  A
 * malloc per picture is what turns that into "752 KB free and a 256 KB
 * allocation fails": a pool of a few dozen blocks of every size from 8x8 to
 * 512x512 fragments, and the total free bytes stop describing anything useful.
 *
 * This arena is deliberately small and boring.  It owns nothing: the caller
 * hands it contiguous chunks of memory when (and only when) a picture needs
 * one, and it hands out 16-byte aligned pieces of them (16 bytes is what
 * sceGuTexImage() requires), merging neighbours inside a chunk the moment a
 * piece comes back.
 *
 * Chunks, not one big reservation, because a reservation is charged at the
 * wrong moment: taking 4 MB up front, before the interface has loaded a single
 * picture, starved the PNG decoder during startup (the Russian charset failed
 * with "outofmem" and the whole font disappeared).  A chunk is taken when an
 * allocation cannot be satisfied any other way, and a chunk that has become
 * completely free can be removed again -- so the arena's footprint follows
 * what is actually on screen.
 *
 * It does not allocate, does not grow by itself and does not depend on the C
 * library, which is also what makes it testable on a desktop:
 *
 *     tests/test_tex_arena.c
 */
#ifndef KABLUK_TEX_ARENA_H
#define KABLUK_TEX_ARENA_H

#include <stddef.h>

/* Granularity of every pointer the arena hands out. */
#define TEX_ARENA_ALIGN 16u

/* How many chunks one arena may hold.  gl_shim.c hands out chunks of 256 KB to
 * 1 MB, so this is a ceiling of several megabytes of pictures. */
#define TEX_ARENA_MAX_CHUNKS 24

typedef struct tex_arena_block_s
{
	size_t                     size;	/* whole block, header included */
	int                        free;
	struct tex_arena_block_s  *next;	/* next block, in address order */
} tex_arena_block_t;

typedef struct
{
	void              *base;	/* the chunk the owner gave us */
	size_t             bytes;	/* its size */
	size_t             used;	/* sum of the blocks that are not free */
	tex_arena_block_t *first;
} tex_arena_chunk_t;

typedef struct
{
	tex_arena_chunk_t chunk[TEX_ARENA_MAX_CHUNKS];
	int               chunks;
} tex_arena_t;

/* Empty the arena.  It holds no memory of its own, so this cannot fail. */
void tex_arena_init (tex_arena_t *a);

/* Hand the arena a chunk of memory.  base must be at least 16-byte aligned,
 * bytes must be big enough for a header and one piece, and the arena must be
 * the only owner of it afterwards.  Returns 1 on success. */
int tex_arena_add_chunk (tex_arena_t *a, void *base, size_t bytes);

/* Hand out need bytes, 16-byte aligned, or NULL when no chunk has a free run
 * that big.  The caller then either adds a chunk or evicts something. */
void *tex_arena_alloc (tex_arena_t *a, size_t need);

/* Give a pointer from tex_arena_alloc() back; NULL and foreign pointers are
 * ignored.  Free blocks are merged with their neighbours inside the chunk. */
void tex_arena_free (tex_arena_t *a, void *ptr);

/* Bytes currently handed out (headers included). */
size_t tex_arena_used (const tex_arena_t *a);

/* Bytes the owner has given the arena, all chunks together. */
size_t tex_arena_size (const tex_arena_t *a);

/* The biggest single run any chunk could serve right now.  This is the number
 * that matters when a 256 KB picture will not fit: it says whether the arena
 * is genuinely full or merely shredded. */
size_t tex_arena_largest_free (const tex_arena_t *a);

/* Number of separate free runs across all chunks -- a fragmentation score. */
int tex_arena_free_runs (const tex_arena_t *a);

/* Index of a chunk that holds nothing at all and may be given back to the
 * heap with tex_arena_remove_chunk(), or -1.  The last remaining chunk is
 * never offered: an empty arena is useless and re-taking memory at once is
 * worse than keeping a little. */
int tex_arena_idle_chunk (const tex_arena_t *a);

/* Forget a chunk (by the index tex_arena_idle_chunk() returned).  The caller
 * frees the memory; the arena never did own it. */
void tex_arena_remove_chunk (tex_arena_t *a, int index);

/* Consistency check used by the tests: every block inside its chunk, blocks
 * tiling the chunk exactly, no two free neighbours, the used counters
 * agreeing with the blocks.  Returns 1 when the arena is intact, 0 (and
 * leaves *why pointing at a literal string) when it is not. */
int tex_arena_check (const tex_arena_t *a, const char **why);

#endif /* KABLUK_TEX_ARENA_H */
