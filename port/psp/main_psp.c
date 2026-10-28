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
extern unsigned int psp_heap_arena (void);
extern unsigned int psp_heap_chunks (void);
/* How much malloc() can still reach inside the 10 MB heap block (psp_boot.c).
 * mallinfo alone reports 0 KB here when the allocator has simply not grown yet,
 * which reads as "about to die" on a healthy run. */
extern unsigned int psp_heap_block_left (void);
/* How many malloc() calls came back NULL.  Counted by the --wrap=malloc hook
 * in psp_heap.c, because a NULL nothing checks is a write through address 0,
 * and on this console that is not a message: it is the power going off. */
extern unsigned int psp_heap_malloc_fails (void);
extern void         psp_heap_report (const char *where);
/* The watchdog measures this thread's stack once a second; it has to be told
 * which thread that is, from the thread itself. */
extern void         psp_heap_note_main_thread (void);

/* port/psp/gl_shim.c -- the picture arena: how much memory the 16-bit copies
 * of the pictures take, and the one block it owns (see psp_mem.c). */
extern size_t       psp_gl_shim_texture_bytes (void);
extern void         psp_gl_shim_set_pool (void *base, size_t bytes);

/* port/psp/psp_mem.c -- the memory layout: the zone and the pictures, taken
 * from the firmware partition once, before anything else allocates. */
#include "psp_mem.h"


/* The memory layout lives in psp_mem.c: the pictures' pool and the engine's
 * zone are two blocks taken straight from the firmware partition
 * (sceKernelAllocPartitionMemory), and newlib's heap is a fixed 10 MB, set in
 * psp_boot.c.  This file only asks psp_mem for the zone and hands the pictures
 * their block.
 *
 * The three builds that died here -- heap at 9 KB free, a "reserve" that was
 * freed and never came back, a zone sized from a number that describes the
 * partition's leftover -- and the pspsdk call each of them got wrong are
 * written down in psp_mem.c, next to the code that replaces them. */

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

	/* The heap exists from the first malloc() in this program's C runtime
	 * (pspsdk _sbrk(), sized by PSP_HEAP_SIZE_KB in psp_boot.c).  This block is
	 * just an early check that newlib works at all, and it is kept -- freeing
	 * it would hand the top of the heap back to the system for good. */
	heap_anchor = malloc(64 * 1024);
	Sys_Printf("malloc(64 KB): %s\n", heap_anchor ? "ok" : "FAILED (no heap)");

	/* The layout: the pictures' block and the engine's zone, both taken from
	 * the firmware partition (psp_mem.c).  Done here, before Host_Init, so
	 * that nothing else has allocated when the sizes are chosen. */
	{
		int p = COM_CheckParm("-mem");
		int zone_mb = (p && p + 1 < com_argc) ? Q_atoi(com_argv[p + 1]) : 0;
		unsigned int zone_bytes = 0, pic_bytes = 0;
		void *zone, *pics;

		psp_mem_init(zone_mb);

		zone = psp_mem_zone(&zone_bytes);
		pics = psp_mem_pictures(&pic_bytes);

		/* The picture arena owns its block for the whole run: one chunk, no
		 * malloc() while a frame is drawn, and nothing to hand back -- a
		 * block freed once cannot be re-taken on this platform.  Without a
		 * block of its own the arena grows chunk by chunk instead. */
		if (pics)
			psp_gl_shim_set_pool(pics, pic_bytes);

		parms.membase = zone;
		parms.memsize = (int)zone_bytes;
	}

	if (!parms.membase)
	{
		/* psp_mem.c already said what it tried and what the partition had;
		 * this only has to stop instead of starting a game that cannot load
		 * a map. */
		psp_mem_report();
		Sys_Error("no memory for the engine's zone (see the mem: lines above)");
	}

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
				psp_tracef("run: f %d state %d, malloc %d KB used, %d KB left "
				           "in the heap block, NULL %d, tex %d KB",
				           frame, (int)cls.state,
				           (int)(psp_heap_in_use() / 1024),
				           (int)(psp_heap_block_left() / 1024),
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
