/*
 * port/psp/psp_mem.c -- the port's memory, taken from the firmware, once.
 *
 * THE PROBLEM THIS FILE REPLACES
 *
 * The port used to hand the engine one huge malloc() block and let everything
 * else -- the picture arena, the decoder, gfx.wad, IPLog -- share newlib's heap
 * with it.  Three builds died on that arrangement, each with a different and
 * completely predictable symptom:
 *
 *   1) the zone took most of the heap and malloc() ran out mid-startup:
 *          heap: after Host_Init: in use 34430 KB, free 9 KB of 34439 KB
 *          *** FATAL ERROR *** Not enough memory free
 *   2) a 10 MB "reserve" was allocated and then freed to leave room, and the
 *      memory did not come back:
 *          malloc pool: 5 KB free of 36870 KB in 1 chunks
 *          malloc: NULL for 2097152 bytes -- last IPLog_Init
 *   3) the size was read with sceKernelMaxFreeMemSize() from main(), which
 *      reports the partition's leftover, not the heap's room -- because newlib
 *      had already taken the whole partition at its first malloc(), before
 *      main():
 *          mem: biggest free block 512 KB -> heap ceiling: 0 MB -> Zone: 8 MB
 *          *** FATAL ERROR *** Hunk_Alloc: failed on 1048608 bytes
 *
 * The cause is one fact about the platform, from pspsdk src/libcglue/glue.c,
 * _sbrk(): newlib's heap is a SINGLE block,
 *
 *      sceKernelMaxFreeMemSize() - 512 KB,
 *
 * allocated at the first malloc() -- which happens in the C runtime.  After
 * that, malloc() can only sub-allocate inside that block: no more can ever be
 * got, none of it can be given back (free() of a big block calls malloc_trim(),
 * which returns the top of the heap through MORECORE(-n) for good), and the
 * partition reports a flat ~750 KB free no matter how much of the block is
 * unused.  So a design that keeps a second, third and fourth arena inside that
 * block is fighting the allocator on its own terms, and losing.
 *
 * WHAT THIS FILE DOES INSTEAD
 *
 * It asks the firmware directly.  sceKernelAllocPartitionMemory() is a
 * user-mode call (SysMemUserForUser; pspsdk's own _sbrk() uses it, see
 * src/libcglue/glue.c), it returns a block plus a UID, its size is exact, and
 * it neither fragments newlib's heap nor depends on where the allocator's top
 * chunk happens to be.  Three blocks, taken once, in this order:
 *
 *      pictures   PSP_PICTURE_POOL (4 MB)  -- the picture arena's one chunk
 *      zone       whatever is left, minus slack -- the engine's hunk
 *      (newlib)   PSP_HEAP_SIZE_KB, see psp_boot.c -- malloc() proper
 *
 * The order matters: the picture pool is the smaller and the one whose absence
 * is felt immediately (a menu drawn as white rectangles), so it is taken first
 * and given up last.  The zone is the one that can be almost any size -- the
 * engine streams its maps -- so it takes what remains.
 *
 * Nothing here is ever freed: a block freed once cannot be re-taken on this
 * platform (fact 3 above), and there is nothing to re-take it for.  That is why
 * there is no allocator in this file, only a layout.
 *
 * The sizes are the only numbers in the port that are a POLICY rather than a
 * measurement, and each has its reason:
 *
 *   PSP_PICTURE_POOL  the arena's cap (gl_shim.c, SHIM_ARENA_LIMIT) plus room
 *                     for the biggest single picture; measured use in the
 *                     17:37 log was 2488 KB of pictures for the whole menu
 *                     system, so 6 MB is the cap with headroom, not a guess.
 *
 *   PSP_HEAP_SIZE_KB  10 MB for everything the engine malloc()s: gfx.wad
 *                     (~1 MB), IPLog_Init's Q_malloc(2 MB) which is what ended
 *                     build 17:53, the picture decoder's w*h*4 buffer (640x480
 *                     at 4 bytes = 1.2 MB), the model loader's scratch and the
 *                     .cfg/.rc files.
 *
 *   slack (1 MB)      left unallocated: the firmware and the SDK take small
 *                     blocks during the run (threads, the GE list, io buffers),
 *                     and a partition with no room left makes those fail in
 *                     code that has no idea why.
 */

#include <string.h>

#include <pspkernel.h>
#include <pspsysmem.h>
#include <pspmodulemgr.h>	/* PSP_MEMORY_PARTITION_USER, PSP_MEMORY_PARTITION_KERNEL */
#include <pspthreadman.h>

#include "quakedef.h"
#include "psp_mem.h"

/* newlib's own accounting (psp_heap.c), for the report line. */
extern unsigned int psp_heap_in_use (void);
extern unsigned int psp_heap_block_bytes (void);
extern unsigned int psp_heap_block_left (void);

/* Pictures: the arena's cap (gl_shim.c keeps SHIM_ARENA_LIMIT in step with
 * this) plus the room one big picture needs on top of it.
 *
 * It was 6 MB, sized from "2488 KB of pictures for the whole menu system, so
 * 6 MB is the cap with headroom".  The hardware then measured the menu at
 * 1679 KB in use of 6144 KB -- and the console died loading a map because the
 * engine's hunk (the zone, "everything that is left") could not hold the world
 * plus one player model:
 *
 *     Cache_TryAlloc: 3536192 is greater then free hunk
 *
 * Two megabytes of a picture pool that is not half used are worth more to the
 * hunk than to a menu, so the pool is 4 MB now and the zone gets the rest.
 * The arena still has room for every picture the interface actually asks for
 * (1.7 MB measured), and it is still the biggest single block, so a picture
 * bigger than the menu's entire stack still fits. */
#define PSP_PICTURE_POOL	(4u * 1024u * 1024u)

/* Left to the firmware and the SDK.  See the note above. */
#define PSP_PARTITION_SLACK	(1u * 1024u * 1024u)

/* The smallest zone worth starting with.  Below this the engine cannot load a
 * map at all, and a clear "not enough memory" beats a mystery crash later. */
#define PSP_MIN_ZONE		(8u * 1024u * 1024u)

static SceUID  mem_zone_id = -1;
static SceUID  mem_picture_id = -1;
static void   *mem_zone_base;
static void   *mem_picture_base;
static unsigned int mem_zone_bytes;
static unsigned int mem_picture_bytes;

/* How much is left in the partition right now, and the biggest single block a
 * request could get.  Both are firmware answers, not estimates: this is the
 * same pair of calls pspsdk's _sbrk() makes when it sizes the heap. */
static unsigned int mem_free (void)
{
	return sceKernelTotalFreeMemSize();
}

static unsigned int mem_largest (void)
{
	return sceKernelMaxFreeMemSize();
}

/* One block from the user partition, largest first.
 *
 * `want` is a wish, not a requirement: the partition may be fragmented, and a
 * block that does not fit fails cleanly (a negative UID) without disturbing
 * anything.  Stepping down by a megabyte finds the biggest size that does fit
 * in a few calls, which is exactly what the engine's hunk needs. */
static int mem_take (const char *name, unsigned int want, unsigned int floor,
                     SceUID *id, void **base, unsigned int *bytes)
{
	unsigned int size = want & ~(1024u * 1024u - 1u);
	unsigned int tries = 0;

	if (size < floor)
		size = floor;

	for (; size >= floor; size -= 1024u * 1024u)
	{
		SceUID block = sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER,
		                                             name, PSP_SMEM_Low, size, NULL);

		if (block >= 0)
		{
			void *head = sceKernelGetBlockHeadAddr(block);

			if (!head)
			{
				/* Should never happen: a UID without an address is
				 * the firmware contradicting itself.  Give it back
				 * rather than keep a block we cannot use. */
				sceKernelFreePartitionMemory(block);
				break;
			}

			*id = block;
			*base = head;
			*bytes = size;
			return 1;
		}

		/* First few refusals are worth a line; a long march down is not. */
		if (tries++ < 3)
			Sys_Printf ("mem: %s: %u KB refused (%d)\n",
			            name, size / 1024, (int)block);
	}

	return 0;
}

int psp_mem_init (int zone_mb)
{
	unsigned int room;

	Sys_Printf ("mem: partition has %u KB free, largest block %u KB\n",
	            mem_free() / 1024, mem_largest() / 1024);

	/* 1. The pictures, first: smallest, and the arena degrades worst without
	 *    it (every picture would be a white rectangle). */
	if (mem_take ("pictures", PSP_PICTURE_POOL, 1024u * 1024u,
	              &mem_picture_id, &mem_picture_base, &mem_picture_bytes))
		Sys_Printf ("mem: pictures %u KB (one block, the whole arena)\n",
		            mem_picture_bytes / 1024);
	else
		Sys_Printf ("mem: no picture pool -- the arena will memalign its "
		            "chunks from the heap\n");

	/* 2. The zone: everything that is left, minus the slack the firmware and
	 *    the SDK need during the run. */
	room = mem_largest();

	if (zone_mb > 0)
	{
		unsigned int want = (unsigned int)zone_mb * 1024u * 1024u;

		if (want > room)
		{
			Sys_Printf ("mem: -mem %d MB is more than the %u KB largest "
			            "block, using what fits\n", zone_mb, room / 1024);
			want = room;
		}

		if (mem_take ("zone", want, PSP_MIN_ZONE, &mem_zone_id,
		              &mem_zone_base, &mem_zone_bytes))
			Sys_Printf ("mem: zone %u KB (-mem %d)\n",
			            mem_zone_bytes / 1024, zone_mb);
		else
			Sys_Printf ("mem: zone: -mem %d MB did not fit\n", zone_mb);
	}
	else if (room > PSP_MIN_ZONE + PSP_PARTITION_SLACK)
	{
		if (mem_take ("zone", room - PSP_PARTITION_SLACK, PSP_MIN_ZONE,
		              &mem_zone_id, &mem_zone_base, &mem_zone_bytes))
			Sys_Printf ("mem: zone %u KB (everything but %u KB slack)\n",
			            mem_zone_bytes / 1024, PSP_PARTITION_SLACK / 1024);
	}

	if (!mem_zone_base)
	{
		/* Last chance: the minimum, even if it eats the slack.  A small
		 * zone that runs is better than a clean refusal to start. */
		mem_take ("zone", PSP_MIN_ZONE, PSP_MIN_ZONE, &mem_zone_id,
		          &mem_zone_base, &mem_zone_bytes);

		if (mem_zone_base)
			Sys_Printf ("mem: zone only %u KB -- the map may not fit\n",
			            mem_zone_bytes / 1024);
	}

	psp_mem_report();

	return mem_zone_base ? 1 : 0;
}

void *psp_mem_zone (unsigned int *bytes)
{
	if (bytes)
		*bytes = mem_zone_bytes;

	return mem_zone_base;
}

void *psp_mem_pictures (unsigned int *bytes)
{
	if (bytes)
		*bytes = mem_picture_bytes;

	return mem_picture_base;
}

void psp_mem_report (void)
{
	/* The heap line is newlib's own accounting: what malloc() has taken, and
	 * how much of its block is still reachable -- `used` of `block`, not of
	 * what the allocator happens to be holding right now (which reads as
	 * "4 KB free" on a healthy run; see psp_heap.c). */
	Sys_Printf ("mem: zone %u KB, pictures %u KB, newlib heap %u KB used of "
	            "%u KB (%u KB left), partition free %u KB\n",
	            mem_zone_bytes / 1024, mem_picture_bytes / 1024,
	            psp_heap_in_use() / 1024, psp_heap_block_bytes() / 1024,
	            psp_heap_block_left() / 1024, mem_free() / 1024);
}
