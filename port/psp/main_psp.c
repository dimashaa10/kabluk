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


/* How the size of the zone is decided -- measured, not assumed.
 *
 * Four facts, from pspsdk's source and from this port's own logs:
 *
 *   1. newlib gets ONE block from the system.  pspsdk src/libcglue/glue.c,
 *      _sbrk(): the first call allocates
 *
 *          sceKernelMaxFreeMemSize() - 512 KB
 *
 *      out of PSP_MEMORY_PARTITION_USER, records heap_bottom/heap_top once,
 *      and every later call can only move a pointer INSIDE that block.  So
 *      malloc() can never reach a byte outside it: the block *is* the pool.
 *
 *   2. Its size is not a property of the console.  The same EBOOT was given
 *      34.4 MB in one run ("malloc 35710/40 KB in 1 chunks") and more than
 *      46 MB in another (a 10 MB request and a 36 MB request both succeeded).
 *      It is whatever sceKernelMaxFreeMemSize() answers at that moment, so it
 *      has to be read, not guessed at.
 *
 *   3. Freeing a big block does not give it back.  newlib's free() calls
 *      malloc_trim() (newlib/libc/stdlib/_mallocr.c), which hands the bytes at
 *      the top of the heap back through MORECORE(-n) and subtracts them from
 *      mallinfo's `arena`.  After the zone had been taken, releasing a 10 MB
 *      block left
 *
 *          malloc pool: 5 KB free of 36870 KB in 1 chunks
 *
 *      and the next 2 MB request (IPLog_Init) failed: a heap whose top has been
 *      given away does not get it back.  Nothing here allocates a block "for
 *      later" and frees it any more -- not a reserve, not the zone.
 *
 *   4. The engine already works this way.  Quake/zone.c: Memory_Init() takes
 *      one Hunk_Alloc block (parms.memsize, handed over here) and carves a
 *      DYNAMIC_SIZE zone inside it; Z_Malloc() serves small strings and
 *      structures from that zone and everything big from the hunk.  The port
 *      only has to decide, once, how much of the console's memory that block
 *      should be.
 *
 * So: zone = (what _sbrk() will take) - PSP_MALLOC_FLOOR, rounded down to a
 * megabyte, and the rest of the heap is left to malloc() for the pictures, the
 * decoder and the engine's own Q_malloc() calls.  The downward probe stays as
 * a safety net (if the block turns out smaller than measured, the next
 * megabyte down is tried before giving up).  -mem <MB> overrides all of it. */
#define PSP_MIN_MEMORY (8 * 1024 * 1024)

/* What malloc() must keep, i.e. what the zone may not take.
 *
 * The 2D GL shim keeps one 16-bit copy of every uploaded picture for the GE to
 * sample (sceGu reads textures out of main memory; EDRAM holds the frame
 * buffers), and its arena is capped at 5 MB (gl_shim.c, SHIM_ARENA_LIMIT).  On
 * top of that a single picture decode needs its own buffer -- 640x480 at 4
 * bytes is 1.2 MB -- and IPLog_Init() asks Q_malloc() for 2 MB (that request
 * is what ended the run with "Not enough memory free" when the pool was
 * starved).  gfx.wad, the .cfg/.rc files and the pak directory scan come out
 * of the same pool.  12 MB is those four numbers with room to breathe, and it
 * is the figure that keeps the zone from eating the pool. */
#define PSP_MALLOC_FLOOR (12 * 1024 * 1024)

/* What pspsdk keeps for the rest of the SDK when it sizes the heap:
 * _sbrk() takes sceKernelMaxFreeMemSize() minus this (512 KB,
 * DEFAULT_HEAP_THRESHOLD_SIZE_KB in src/libcglue/glue.c).  Subtracting it here
 * as well means the first zone attempt is the size the block can really hold,
 * instead of one megabyte of probe failures every boot. */
#define PSP_HEAP_THRESHOLD (512 * 1024)

static quakeparms_t	parms;

/* 64 KB that is deliberately never freed; see main(). */
static void *heap_anchor;

/* Measured before this program allocates anything, see main().
 *
 * sceKernelMaxFreeMemSize() is the number pspsdk's _sbrk() reads to size the
 * heap block, so reading it first is reading the pool's real ceiling.  The
 * total is kept for the log: the two disagreeing is how you tell a console
 * whose memory is fragmented at boot from one that simply has less. */
static unsigned int mem_max_free;
static unsigned int mem_total_free;

static quakeparms_t	parms;

/* 64 KB that is deliberately never freed; see main(). */
static void *heap_anchor;

int main (int argc, char *argv[])
{
	double	time, oldtime, newtime;

	/* First statement of the program, because it is only true before this
	 * program has allocated anything: pspsdk's _sbrk() sizes the newlib heap
	 * block as sceKernelMaxFreeMemSize() - 512 KB, once, on the first
	 * malloc().  Read afterwards, this number is what that block left over.
	 * See the memory note above. */
	mem_max_free = sceKernelMaxFreeMemSize();
	mem_total_free = sceKernelTotalFreeMemSize();

	psp_con_init();

	/* The build stamp is not decoration.  When a change on the PC side does
	 * not change what the console does, the first question is whether the
	 * EBOOT on the memory stick is the one that was just built -- and the
	 * only way to answer it from a screenshot is to print the compile time
	 * on the screen.  If this line does not show today's build, the old
	 * EBOOT is still on the card (or in a second game folder). */
	psp_con_printf("Kabluk for PSP -- build " __DATE__ " " __TIME__ "\n");

	/* The two numbers the zone choice is based on.  If the console gave this
	 * run less memory than the last one, the log says so here instead of
	 * leaving it to be inferred from a pool that ran dry three screens
	 * later. */
	psp_con_printf("mem: biggest free block %d KB, all free %d KB, measured first\n",
	               (int)(mem_max_free / 1024), (int)(mem_total_free / 1024));

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
		unsigned int ceiling;
		int want, tried = 0;

		/* mem_max_free was read before anything allocated, so it is the number
		 * _sbrk() sizes the block from; minus the 512 KB pspsdk keeps, it is
		 * the block.  If the block already existed when we measured, what we
		 * read was the leftover instead: the block is then what the heap says
		 * it has plus that leftover. */
		ceiling = mem_max_free > PSP_HEAP_THRESHOLD ?
		          mem_max_free - PSP_HEAP_THRESHOLD : 0;

		if (psp_heap_arena() > ceiling)
			ceiling = psp_heap_arena() + mem_max_free;

		want = (int)(ceiling > (PSP_MALLOC_FLOOR + PSP_MIN_MEMORY) ?
		             ceiling - PSP_MALLOC_FLOOR : PSP_MIN_MEMORY);
		want &= ~(1024 * 1024 - 1);	/* whole megabytes */

		Sys_Printf("heap ceiling: %d MB (%d MB free at start), malloc keeps %d MB\n",
		           (int)(ceiling / (1024 * 1024)),
		           (int)(mem_total_free / (1024 * 1024)),
		           (int)(PSP_MALLOC_FLOOR / (1024 * 1024)));

		psp_heap_expect_failure(1);

		for (; want >= PSP_MIN_MEMORY; want -= 1024 * 1024)
		{
			parms.membase = malloc((size_t)want);
			if (parms.membase)
			{
				parms.memsize = want;
				Sys_Printf("Zone: %d MB of the %d MB pool, chosen by measurement\n",
				           want / (1024 * 1024),
				           (int)(ceiling / (1024 * 1024)));
				break;
			}

			/* One line per attempt is noise when the measurement is off by a
			 * long way; the first few are enough to see how far it is off. */
			if (tried++ < 4)
				Sys_Printf("Zone: %d MB did not fit\n", want / (1024 * 1024));
		}

		psp_heap_expect_failure(0);

		/* What is left is the pool: the pictures, the decoder and the
		 * engine's own Q_malloc() calls.  Nothing is going to hand it back, so
		 * its size is worth a line of its own -- and the number has to include
		 * the part of the block malloc() has not taken yet, or a healthy pool
		 * reads as "0 MB free" simply because nothing has asked for it. */
		if (parms.membase)
		{
			unsigned int left = psp_heap_free();

			if (ceiling > psp_heap_arena())
				left += ceiling - psp_heap_arena();

			Sys_Printf("malloc pool: %d MB left (%d KB free now, %d KB not yet taken)\n",
			           (int)(left / (1024 * 1024)),
			           (int)(psp_heap_free() / 1024),
			           (int)((left - psp_heap_free()) / 1024));
		}
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
