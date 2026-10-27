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
 * This arena is deliberately small and boring: it is given one contiguous
 * block by its owner, hands out 16-byte aligned pieces of it (16 bytes is
 * what sceGuTexImage() requires), and merges neighbours the moment a piece
 * comes back.  It does not allocate, does not grow, and does not depend on
 * the C library -- which is also what makes it testable on a desktop:
 *
 *     tests/test_tex_arena.c
 *
 * The caller owns the memory: gl_shim.c reserves it once, on the first
 * picture, and keeps using it.
 */
#ifndef KABLUK_TEX_ARENA_H
#define KABLUK_TEX_ARENA_H

#include <stddef.h>

/* Granularity of every pointer the arena hands out. */
#define TEX_ARENA_ALIGN 16u

typedef struct tex_arena_block_s
{
	size_t                     size;	/* whole block, header included */
	int                        free;
	struct tex_arena_block_s  *next;	/* next block, in address order */
} tex_arena_block_t;

typedef struct
{
	void              *base;	/* the block the owner gave us           */
	size_t             bytes;	/* its size                              */
	tex_arena_block_t *first;	/* head of the address-ordered list      */
} tex_arena_t;

/* Take ownership of base[0..bytes).  base must be at least 16-byte aligned
 * and bytes must leave room for at least one block; returns 1 when the arena
 * is usable, 0 otherwise (the arena is then "empty" and allocs fail cleanly,
 * which lets the caller fall back to plain malloc). */
int tex_arena_init (tex_arena_t *a, void *base, size_t bytes);

/* Hand out need bytes, 16-byte aligned, or NULL when no single free run is
 * big enough.  Zero is not a size we allocate: NULL comes back. */
void *tex_arena_alloc (tex_arena_t *a, size_t need);

/* Give a pointer from tex_arena_alloc() back and merge it with free
 * neighbours.  NULL and foreign pointers are ignored. */
void tex_arena_free (tex_arena_t *a, void *ptr);

/* Bytes currently handed out (headers included). */
size_t tex_arena_used (const tex_arena_t *a);

/* Bytes the owner gave the arena at init. */
size_t tex_arena_size (const tex_arena_t *a);

/* The biggest single run a new allocation could use.  This is the number that
 * matters when a 256 KB picture will not fit: it says whether the arena is
 * genuinely full or merely shredded. */
size_t tex_arena_largest_free (const tex_arena_t *a);

/* Number of separate free runs -- a fragmentation score, for the log. */
int tex_arena_free_runs (const tex_arena_t *a);

/* Consistency check used by the tests: every block in the list inside the
 * arena, sizes adding up exactly to its size, no two free neighbours.
 * Returns 1 when the arena is intact, 0 (and leaves *why pointing at a
 * literal string) when it is not. */
int tex_arena_check (const tex_arena_t *a, const char **why);

#endif /* KABLUK_TEX_ARENA_H */
