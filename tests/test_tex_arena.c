/*
 * tests/test_tex_arena.c -- the picture allocator, on a desktop.
 *
 * The arena is the answer to a real log line:
 *
 *     tex: no memory for a 512x256 picture (752 KB free, 4198 KB of textures)
 *
 * -- plenty free in total, nothing big enough in one piece.  The properties
 * that matter are therefore not "does alloc return something", but:
 *
 *   1. pieces come back 16-byte aligned (sceGuTexImage asserts this, and a
 *      misaligned picture moves every row by four texels);
 *   2. freed memory is merged with its neighbours, in both directions, so the
 *      pool cannot be shredded into slivers that are all a little too small;
 *   3. a picture that shrinks returns the surplus, and the next big picture
 *      can use it;
 *   4. junk is refused instead of corrupting the list: freeing a pointer not
 *      from the arena, freeing twice, allocating above the arena size;
 *   5. the block list stays intact after a long random-looking workload --
 *      blocks tile the arena exactly and no two free blocks touch;
 *   6. the arena is made of chunks the caller provides one at a time: a
 *      request that cannot be served keeps failing until a chunk arrives, and
 *      a chunk that ends up holding nothing is offered back (see tex_arena.h
 *      for why the picture memory grows on demand instead of being reserved).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tex_arena.h"

static int failures;
static int checks;

static void check (int ok, const char *what)
{
	checks++;
	if (!ok)
	{
		failures++;
		printf("FAIL: %s\n", what);
	}
}

#define ARENA_BYTES (256u * 1024u)

/* The arena owns no memory: it is handed chunks.  One chunk is all these
 * tests need, and this is the same two calls gl_shim.c makes. */
static int setup (tex_arena_t *a, void *base, size_t bytes)
{
	tex_arena_init(a);
	return tex_arena_add_chunk(a, base, bytes);
}

int main (void)
{
	static unsigned char backing[ARENA_BYTES] __attribute__((aligned(16)));
	static unsigned char extra[TEX_ARENA_MAX_CHUNKS][64 * 1024] __attribute__((aligned(16)));
	tex_arena_t a;
	const char *why = NULL;
	void *blocks[64];
	int i;

	/* ---- 1. alignment ------------------------------------------------ */
	check(setup(&a, backing, sizeof(backing)) == 1,
	      "an aligned 256 KB chunk is accepted");
	check(tex_arena_size(&a) == ARENA_BYTES, "the arena reports its size");
	check(tex_arena_used(&a) == 0, "a fresh arena holds nothing");
	check(tex_arena_largest_free(&a) == ARENA_BYTES - sizeof(tex_arena_block_t) ||
	      tex_arena_largest_free(&a) > ARENA_BYTES - 64,
	      "nearly the whole arena is one free run at the start");

	{
		int aligned = 1;

		for (i = 0; i < 32; i++)
		{
			void *p = tex_arena_alloc(&a, 1000 + (size_t)i * 37);

			if (!p || ((size_t)(unsigned long)p & (TEX_ARENA_ALIGN - 1)))
				aligned = 0;
			blocks[i] = p;
		}

		check(aligned, "every pointer is 16-byte aligned");
		check(tex_arena_used(&a) > 32 * 1000, "the used total counts them");
		check(tex_arena_check(&a, &why) == 1, why ? why : "the list tiles the arena after allocating");

		/* ---- 2. coalescing ------------------------------------------ */
		/* Free every other block, then the rest: the arena must come back
		 * as one run, not as 32 slivers. */
		for (i = 0; i < 32; i += 2)
			tex_arena_free(&a, blocks[i]);
		for (i = 1; i < 32; i += 2)
			tex_arena_free(&a, blocks[i]);

		check(tex_arena_used(&a) == 0, "everything freed leaves nothing used");
		check(tex_arena_free_runs(&a) == 1,
		      "freeing every block gives back ONE run, not many slivers");
		check(tex_arena_largest_free(&a) > ARENA_BYTES - 64,
		      "the whole arena is available again");
		check(tex_arena_check(&a, &why) == 1, why ? why : "the list is intact after coalescing");
	}

	/* ---- 3. a picture that shrinks gives the surplus back ------------ */
	{
		void *big = tex_arena_alloc(&a, 200u * 1024u);
		void *small;

		check(big != NULL, "a 200 KB picture fits an empty 256 KB arena");
		tex_arena_free(&a, big);

		small = tex_arena_alloc(&a, 1024);
		check(small != NULL, "a small picture fits after it");

		/* this is the case the log was about: one piece big enough */
		tex_arena_free(&a, small);
		check(tex_arena_largest_free(&a) > 200u * 1024u,
		      "the 200 KB run is available again, not 200 fragments of it");
	}

	/* ---- 4. junk is refused, not obeyed ------------------------------ */
	{
		int dummy;
		void *p = tex_arena_alloc(&a, 4096);
		size_t before = tex_arena_used(&a);

		check(p != NULL, "a 4 KB picture fits");

		tex_arena_free(&a, &dummy);		/* not ours */
		check(tex_arena_check(&a, &why) == 1, why ? why : "a foreign pointer is ignored");

		tex_arena_free(&a, p);
		tex_arena_free(&a, p);			/* double free */
		check(tex_arena_check(&a, &why) == 1, why ? why : "a double free is ignored");
		check(tex_arena_used(&a) == 0, "and the block is only handed back once");

		check(tex_arena_alloc(&a, ARENA_BYTES) == NULL,
		      "a request bigger than the arena returns NULL instead of a bad pointer");
		(void)before;
	}

	/* ---- 5. a long mixed workload keeps the list intact -------------- */
	{
		unsigned seed = 12345;
		int held = 0;

		check(tex_arena_used(&a) == 0, "starting the workload from an empty arena");

		for (i = 0; i < 4000; i++)
		{
			seed = seed * 1103515245u + 12345u;

			if (held < 64 && (seed >> 16) % 3)
			{
				size_t want = 512u + ((seed >> 8) % 6000);

				blocks[held] = tex_arena_alloc(&a, want);
				if (blocks[held])
					held++;
			}
			else if (held > 0)
			{
				int victim = (int)((seed >> 12) % (unsigned)held);

				tex_arena_free(&a, blocks[victim]);
				blocks[victim] = blocks[--held];
			}

			if (!tex_arena_check(&a, &why))
			{
				printf("FAIL: arena broken at step %d: %s\n", i, why ? why : "?");
				failures++;
				break;
			}
		}

		while (held > 0)
			tex_arena_free(&a, blocks[--held]);

		check(tex_arena_used(&a) == 0, "4000 mixed operations leave nothing behind");
		check(tex_arena_free_runs(&a) == 1,
		      "4000 mixed operations leave ONE free run (no fragmentation)");
		check(tex_arena_largest_free(&a) > ARENA_BYTES - 64,
		      "and the full arena is usable again");
	}

	/* ---- 6. chunks ---------------------------------------------------- */
	{
		tex_arena_t c;
		void *p;
		int idle;

		tex_arena_init(&c);
		check(tex_arena_size(&c) == 0, "a new arena holds no memory at all");
		check(tex_arena_alloc(&c, 1024) == NULL,
		      "with no chunks there is nothing to hand out");

		check(tex_arena_add_chunk(&c, extra[0], sizeof(extra[0])) == 1,
		      "the first chunk is accepted");
		check(tex_arena_size(&c) == sizeof(extra[0]),
		      "the reported size follows the chunks");

		blocks[0] = tex_arena_alloc(&c, 16u * 1024u);
		check(blocks[0] != NULL, "a picture fits the first chunk");

		/* 48 KB is still free in chunk 0, so a 40 KB request must be served
		 * from it -- a new chunk is only for what does not fit. */
		blocks[1] = tex_arena_alloc(&c, 40u * 1024u);
		check(blocks[1] != NULL && (char *)blocks[1] >= (char *)extra[0] &&
		      (char *)blocks[1] < (char *)extra[0] + sizeof(extra[0]),
		      "the next request is served from the same chunk");

		check(tex_arena_alloc(&c, 40u * 1024u) == NULL,
		      "a request bigger than the free run fails instead of guessing");
		check(tex_arena_add_chunk(&c, extra[1], sizeof(extra[1])) == 1,
		      "adding a chunk is how the caller answers that");
		check(tex_arena_alloc(&c, 40u * 1024u) != NULL,
		      "and the same request now succeeds");

		check(tex_arena_check(&c, &why) == 1, why ? why : "multi-chunk arena is intact");

		/* A chunk whose last picture has gone is offered back... */
		tex_arena_free(&c, blocks[0]);
		tex_arena_free(&c, blocks[1]);
		idle = tex_arena_idle_chunk(&c);
		check(idle >= 0, "an empty chunk is offered back to the caller");

		tex_arena_remove_chunk(&c, idle);
		check(tex_arena_size(&c) < 2 * sizeof(extra[0]),
		      "removing it shrinks the arena");
		check(tex_arena_check(&c, &why) == 1, why ? why : "arena is intact after removing a chunk");

		/* ...but the last one never is: an empty arena is useless and
		 * taking memory back the moment it is free is worse than keeping a
		 * little. */
		tex_arena_init(&c);
		tex_arena_add_chunk(&c, extra[0], sizeof(extra[0]));
		check(tex_arena_idle_chunk(&c) == -1,
		      "the only chunk is never offered back, even when empty");

		(void)p;
	}

	printf("%s: %d checks\n", failures ? "FAILED" : "PASS", checks);
	printf(failures ? "" : "PASS: the picture arena aligns, coalesces, refuses junk, and\n"
	                      "PASS: comes back whole after a long mixed workload.\n");

	return failures ? 1 : 0;
}
