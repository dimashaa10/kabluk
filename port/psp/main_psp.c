/*
 * port/psp/main_psp.c -- the PSP entry point.
 *
 * Same shape as main_sdl.c: build quakeparms_t, hand the memory block to the
 * engine, then call Host_Frame forever.  What changes is the memory budget
 * (a PSP has 32 or 64 MB of RAM, not 384) and the fact that we own main():
 * port/psp/Makefile defines SDL_MAIN_HANDLED, so SDL no longer renames it and
 * we have to tell SDL that initialisation is our business (SDL_SetMainReady).
 *
 * Data files are expected next to the EBOOT, i.e. in the game directory on the
 * memory stick; the loader runs the module with that directory as cwd.
 */

#include <pspkernel.h>
#include <pspthreadman.h>

#include "quakedef.h"
#include "psp_console.h"

/* port/psp/psp_heap.c -- newlib's own malloc accounting (_mallinfo_r). */
extern unsigned int psp_heap_in_use (void);
extern unsigned int psp_heap_sbrk (void);
extern unsigned int psp_heap_sbrk_peak_bytes (void);
extern unsigned int psp_heap_free (void);
extern unsigned int psp_heap_arena (void);
/* Bytes malloc() can still reach, measured with a reversible sbrk() probe. */
extern unsigned int psp_heap_room (void);
/* Hand the picture arena one block it owns for the whole run. */
extern void         psp_gl_shim_set_pool (void *base, size_t bytes);
extern unsigned int psp_heap_chunks (void);
/* How many malloc() calls came back NULL.  Counted by the --wrap=malloc hook
 * in psp_heap.c, because a NULL nothing checks is a write through address 0,
 * and on this console that is not a message: it is the power going off. */
extern unsigned int psp_heap_malloc_fails (void);
/* Tells the malloc hook that the next failure is this file probing for the
 * size of the zone, not a caller being starved.  See psp_heap.c. */
extern void         psp_heap_expect_failure (int on);
extern void         psp_heap_report (const char *where);
/* The watchdog measures this thread's stack once a second; it has to be told
 * which thread that is, from the thread itself. */
extern void         psp_heap_note_main_thread (void);

/* port/psp/gl_shim.c -- how much memory the 16-bit copies of the pictures
 * take.  They come out of the same pool as everything above. */
extern size_t       psp_gl_shim_texture_bytes (void);


/* How the memory is split -- measured, not assumed.
 *
 * The facts this is built on, from pspsdk's source and from this port's logs:
 *
 *   1. newlib gets ONE block from the system.  pspsdk src/libcglue/glue.c,
 *      _sbrk(): the first call allocates
 *
 *          sceKernelMaxFreeMemSize() - 512 KB
 *
 *      out of PSP_MEMORY_PARTITION_USER and records heap_bottom/heap_top.  Every
 *      later call moves a pointer INSIDE that block, so malloc() can never reach
 *      a byte outside it: the block is the whole pool.
 *
 *   2. That block is created BEFORE main() -- by whatever in the C runtime
 *      malloc()s first -- so from main() onward the partition reports a flat
 *      ~750 KB of free memory no matter how big the block is:
 *
 *          Free RAM: 752 KB total, 512 KB contiguous
 *
 *      Sizing anything from sceKernelMaxFreeMemSize() here is meaningless, and
 *      that mistake cost a whole round: the log of build 18:11 read
 *      "heap ceiling: 0 MB" and the zone fell to the 8 MB minimum, after which
 *      Hunk_Alloc failed on the first 1 MB screen buffer.  So the room is
 *      measured with sbrk() instead (psp_heap_room(), psp_heap.c): a request
 *      that does not fit returns -1 and moves nothing, so asking is free.
 *
 *   3. free() of a big block does not give it back.  newlib's free() calls
 *      malloc_trim() (newlib/libc/stdlib/_mallocr.c), which hands the top of the
 *      heap back through MORECORE(-n) and subtracts it from mallinfo's arena.
 *      Measured, in build 17:53: a 10 MB reserve was freed and the next 2 MB
 *      request failed with "malloc pool: 5 KB free of 36870 KB".  So nothing
 *      here allocates a block "for later" and frees it.
 *
 *   4. The engine already works this way: Quake/zone.c, Memory_Init() takes one
 *      block (parms.memsize) and carves a DYNAMIC_SIZE zone inside it.
 *
 * The split: the port takes two blocks out of the room malloc() has left.
 *
 *      pictures  PSP_PICTURE_POOL   -- handed to the picture arena, one chunk,
 *                                      owned for the whole run
 *      zone      everything else minus PSP_ENGINE_RESERVE
 *      reserve   left in the heap -- NOT taken: gfx.wad, IPLog's 2 MB request,
 *                                      the decoder's w*h*4 buffers, the cfg file
 *
 * The reserve is the part that is deliberately not allocated: memory that stays
 * in the heap is the only memory malloc() can still use.  -mem <MB> overrides
 * the zone. */
#define PSP_MIN_MEMORY (8 * 1024 * 1024)

/* The pictures' own block.
 *
 * The 2D GL shim keeps one 16-bit copy of every uploaded picture for the GE to
 * sample (sceGu reads textures out of main memory; EDRAM holds the frame
 * buffers) and its arena is capped at 5 MB (gl_shim.c, SHIM_ARENA_LIMIT).  On
 * top of the cap the arena needs room for the largest single picture, so the
 * pool is 6 MB -- taken once, in one piece, and never returned.  A pooled arena
 * also stops asking malloc() for 256 KB-1 MB chunks while a frame is being
 * drawn, which is what used to shred the free space the decoder needed. */
#define PSP_PICTURE_POOL (6 * 1024 * 1024)

/* What is deliberately LEFT for malloc().
 *
 * gfx.wad (about a megabyte), IPLog_Init()'s Q_malloc(2 MB) -- the request that
 * ended build 17:53 -- the picture decoder's w*h*4 buffer (640x480 at 4 bytes is
 * 1.2 MB) and the .cfg/.rc files.  These are the engine's own malloc() calls and
 * they cannot be moved into the zone, so the zone must not take this. */
#define PSP_ENGINE_RESERVE (5 * 1024 * 1024)

static quakeparms_t	parms;

/* 64 KB that is deliberately never freed; see main(). */
static void *heap_anchor;

static quakeparms_t	parms;

/* 64 KB that is deliberately never freed; see main(). */
static void *heap_anchor;

int main (int argc, char *argv[])
{
	double	time, oldtime, newtime;

	psp_con_init();

	/* The build stamp is not decoration.  When a change on the PC side does
	 * not change what the console does, the first question is whether the
	 * EBOOT on the memory stick is the one that was just built -- and the
	 * only way to answer it from a screenshot is to print the compile time
	 * on the screen.  If this line does not show today's build, the old
	 * EBOOT is still on the card (or in a second game folder). */
	psp_con_printf("Kabluk for PSP -- build " __DATE__ " " __TIME__ "\n");

	/* How much the heap can still grow is the number the whole split is based
	 * on, and it is measured here, with sbrk() -- see the memory note at the
	 * top of this file.  Printed before anything takes any of it. */
	psp_con_printf("heap room: %d KB that malloc can still reach (measured)\n",
	               (int)(psp_heap_room() / 1024));

	/* From here on a stall is visible: if nothing is printed for eight seconds
	 * the watchdog names the step we are stuck in, on the screen and in the
	 * log.  A hang used to look exactly like a healthy console. */
	psp_con_watchdog();

	/* Tell psp_heap.c which thread's stack the status line is about.  Done
	 * here, on the main thread, before anything else can ask. */
	psp_heap_note_main_thread ();

	/* We replaced SDL's main() (SDL_MAIN_HANDLED), so SDL has to be told that
	 * the application is ready before any SDL_Init call. */
	SDL_SetMainReady();
	if (SDL_Init(0) < 0)
		psp_con_panic("SDL_Init failed", NULL, 0, NULL, SDL_GetError());

	host_parms = &parms;
	memset(&parms, 0, sizeof(parms));
	parms.basedir = ".";

	/* userdir has to point at the same string as basedir, and the desktop
	 * sys layer sets it with the comment "code elsewhere relies on this !".
	 * It is not decoration: common.c compares the two *pointers*
	 * (host_parms->userdir != host_parms->basedir) to decide whether to use
	 * separate user directories, passes both through va("%s...") and hands
	 * them to strcmp().
	 *
	 * Left NULL -- which is what a memset'ed struct gives you -- the first
	 * of those comparisons says "yes, separate directories", the paths are
	 * built from a NULL string, and COM_InitFilesystem dies inside
	 * strcmp(com_basedir, host_parms->userdir) at
	 *	 if (!COM_AddPackage(...) && strcmp(com_basedir, host_parms->userdir))
	 * That is a data abort at address 0: no Sys_Error, no message, no log
	 * line -- the screen simply goes out right after "COM_InitFilesystem",
	 * which is exactly what the breadcrumbs showed. */
	parms.userdir = parms.basedir;
	parms.numcpus = 1;
	parms.argc = argc;
	parms.argv = argv;
	parms.errstate = 0;

	COM_InitArgv(parms.argc, parms.argv);

	isDedicated = false;

	Sys_Init();

	Sys_Printf("Initializing " ENGINE_NAME_AND_VER "\n");

	Sys_Printf("Free RAM: %d KB total, %d KB contiguous\n",
	           sceKernelTotalFreeMemSize() / 1024,
	           sceKernelMaxFreeMemSize() / 1024);

	/* One small block, kept for the whole run: it makes sure the newlib heap
	 * exists at all (the first malloc() is what makes pspsdk's _sbrk() take the
	 * partition block -- PSP_HEAP_SIZE_KB(-1) only says "as much as there is").
	 *
	 * It is NOT here to "pin the heap": that idea was tried in the other
	 * direction last round (hold 10 MB, then free it once the zone was chosen)
	 * and it failed, because free() of a big block hands the top of the heap
	 * back through malloc_trim() and the memory does not come back.  Nothing
	 * big is allocated-then-freed anywhere in this file any more. */
	heap_anchor = malloc(64 * 1024);
	Sys_Printf("malloc(64 KB): %s\n", heap_anchor ? "ok" : "FAILED (no heap)");

	if (COM_CheckParm("-mem"))
	{
		int p = COM_CheckParm("-mem") + 1;
		if (p < com_argc)
		{
			parms.memsize = Q_atoi(com_argv[p]) * 1024 * 1024;
			parms.membase = malloc(parms.memsize);
		}
	}
	else
	{
		unsigned int room = psp_heap_room();
		unsigned int pool = PSP_PICTURE_POOL;
		int want, tried = 0;

		/* If the console is too small for the nice split, give up the picture
		 * pool first: the arena can still grow chunk by chunk with memalign(),
		 * it just fragments more.  The engine reserve is not negotiable -- the
		 * run that ignored it died inside IPLog_Init. */
		if (room < (unsigned)(pool + PSP_ENGINE_RESERVE + PSP_MIN_MEMORY))
			pool = 0;

		want = (int)(room > (pool + PSP_ENGINE_RESERVE + PSP_MIN_MEMORY) ?
		             room - pool - PSP_ENGINE_RESERVE : PSP_MIN_MEMORY);
		want &= ~(1024 * 1024 - 1);	/* whole megabytes */

		Sys_Printf("heap room: %d MB for the zone, the pictures and malloc\n",
		           (int)(room / (1024 * 1024)));

		/* The pictures first: the smaller block, and the one whose absence is
		 * felt immediately (white rectangles instead of a menu). */
		if (pool)
		{
			void *block;

			psp_heap_expect_failure(1);
			block = malloc(pool);
			psp_heap_expect_failure(0);

			if (block)
			{
				psp_gl_shim_set_pool(block, pool);
				Sys_Printf("pictures: %d MB, owned by the picture arena\n",
				           (int)(pool / (1024 * 1024)));
			}
			else
				Sys_Printf("pictures: no %d MB block -- the arena will ask "
				           "malloc for chunks instead\n",
				           (int)(pool / (1024 * 1024)));
		}

		psp_heap_expect_failure(1);

		for (; want >= PSP_MIN_MEMORY; want -= 1024 * 1024)
		{
			parms.membase = malloc((size_t)want);
			if (parms.membase)
			{
				parms.memsize = want;
				Sys_Printf("Zone: %d MB of the %d MB measured room\n",
				           want / (1024 * 1024),
				           (int)(room / (1024 * 1024)));
				break;
			}

			/* One line per attempt is noise when the measurement is off by a
			 * long way; the first few are enough to see how far it is off. */
			if (tried++ < 4)
				Sys_Printf("Zone: %d MB did not fit\n", want / (1024 * 1024));
		}

		psp_heap_expect_failure(0);

		/* What is left is the engine's own malloc() room.  Nothing hands it
		 * back later, so its size is worth a line of its own. */
		if (parms.membase)
			Sys_Printf("malloc keeps: %d MB free now, %d KB still in the block\n",
			           (int)(psp_heap_free() / (1024 * 1024)),
			           (int)(psp_heap_room() / 1024));
	}

	if (!parms.membase)
		Sys_Error("Not enough memory for the %d MB minimum zone\n"
		          "(free to the kernel: %d KB, largest free block: %d KB)",
		          PSP_MIN_MEMORY / (1024 * 1024),
		          sceKernelTotalFreeMemSize() / 1024,
		          sceKernelMaxFreeMemSize() / 1024);

	Sys_Printf("Host_Init (%d MB)\n", parms.memsize / (1024 * 1024));
	Host_Init();

	/* Everything the engine prints after Con_Init() goes to the engine's own
	 * console buffer, which needs a renderer to be visible -- and there is
	 * none yet.  So these two markers are the only way to tell "Host_Init
	 * returned and the frame loop is running" from "the module died in one
	 * of the initialisers".  psp_trace() writes to the screen and to
	 * kabluk.log, and forces the card write. */
	psp_trace("Host_Init returned");
	/* Baseline for everything that follows: what the engine asked malloc()
	 * for while it was starting up.  Comparing this with the numbers the
	 * two-second heartbeat prints is how a leak is told apart from a large
	 * one-off allocation. */
	psp_heap_report("after Host_Init");
	psp_con_printf("entering main loop, state %d\n", (int)cls.state);

	/* The breadcrumbs are NOT switched off here any more.  The first frames
	 * of the main loop are the ones that crash -- the screen update, the HUD
	 * and the menu all run for the first time there, and once the card
	 * writes stop, a crash in frame 1 leaves a log that ends in the middle
	 * of Host_Init.  Five frames are enough to get past the startup, and
	 * the marks are deduplicated anyway, so this costs one burst of writes
	 * and then falls silent.  See the frame loop below. */

	oldtime = Sys_DoubleTime();

	{
		int frame = 0;
		double nextbeat = oldtime + 2.0;

		while (1)
		{
			newtime = Sys_DoubleTime();
			time = newtime - oldtime;

			/* Named for the watchdog: "host frame" plus the breadcrumbs
			 * say whether a freeze is in the engine's own code or in the
			 * GPU half of it. */
			psp_phase("host frame: input/commands/client/screen");

			Host_Frame(time);

			oldtime = newtime;
			frame++;

			/* A heartbeat every two seconds: if the screen freezes on one
			 * of these, the loop itself is fine and the fault is inside
			 * the frame; if the numbers stop, it is the last one that
			 * matters. */
			if (newtime >= nextbeat)
			{
				/* The malloc numbers belong on this line, not on a
				 * separate one: read side by side with the frame
				 * count they show whether the pool grows per
				 * frame (a leak) or was simply never big enough
				 * for one large request. */
				psp_tracef("run: f %d state %d, heap %d KB grown "
				           "(peak %d KB), malloc %d/%d KB in %d chunks, "
				           "NULL %d, tex %d KB",
				           frame, (int)cls.state,
				           (int)(psp_heap_sbrk() / 1024),
				           (int)(psp_heap_sbrk_peak_bytes() / 1024),
				           (int)(psp_heap_in_use() / 1024),
				           (int)(psp_heap_free() / 1024),
				           (int)psp_heap_chunks(),
				           (int)psp_heap_malloc_fails(),
				           (int)(psp_gl_shim_texture_bytes() / 1024));
				nextbeat = newtime + 2.0;
			}

			/* Five frames drawn: the startup screens are behind us and the
			 * card can stop being written to on every change. */
			if (frame == 5)
				psp_con_boot_done();

			if (time < sys_ticrate.value)
				sceKernelDelayThread(1000);	/* 1 ms: let the system breathe */
		}
	}

	return 0;
}
