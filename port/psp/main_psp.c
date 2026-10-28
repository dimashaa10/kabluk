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
/* in_psp.c: gives the pad's face buttons their defaults once the config file
 * has been read.  Declared here for the same reason gu_psp.c declares
 * Draw_EvictCachePics(): one prototype is cheaper than pulling the engine's
 * input header into a file whose job is the frame loop. */
extern void         IN_ApplyPadDefaults (void);

/* in psp_console.c: what the log has cost the memory card since the last
 * heartbeat. */
extern void         psp_con_card_take_stats (unsigned int *writes,
                                             unsigned int *us);

extern size_t       psp_gl_shim_texture_bytes (void);
extern void         psp_gl_shim_set_pool (void *base, size_t bytes);

/* port/psp/psp_mem.c -- the memory layout: the zone and the pictures, taken
 * from the firmware partition once, before anything else allocates. */
#include "psp_mem.h"

/* port/psp/gu_psp.c -- where the frame time goes: the engine's own drawing,
 * the wait for the GE, and the wait for the panel, as three separate numbers.
 * The heartbeat below prints them, because a frame rate of seventeen on a
 * console that should manage sixty is a question only these three can
 * answer. */
#include "gu_psp.h"


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

			/* One screen gets a close-to-live trail on the card: the
			 * save/load screen, which is where the port dies.  Every
			 * other screen keeps the batched log.
			 *
			 * "Every menu" was too much: a menu frame draws twenty
			 * pictures, so it wrote twenty times to the memory stick,
			 * and the frame went from 20 ms to 350 ms -- the game felt
			 * broken, and the diagnostic was the thing that broke it.
			 * The save/load screen draws two pictures and is the one
			 * being investigated, so it pays and nothing else does.
			 *
			 * It turned out to draw sixty lines a frame, not two --
			 * the motif redraws every frame and every draw marks its
			 * picture -- so even one round trip per line cost ~290 ms
			 * per frame here.  The trail is now throttled in
			 * psp_console.c: at most one card write per 15 ms, in
			 * order, nothing dropped until the buffer is full. */
			{
				static int traced_saves = -1;
				int on_saves = (key_dest == key_menu && M_IsSaveMenu());

				if (on_saves != traced_saves)
				{
					traced_saves = on_saves;
					psp_tracef("trace: the card is written %s",
					           on_saves ? "about ten times a second on the save/load screen"
					                    : "once a second again");
				}

				psp_con_trace_sync(on_saves);
			}

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
				double t_cpu, t_ge, t_vb, t_all;
				int    t_frames;
				unsigned int card_writes, card_us;

				/* The four times on the same line as the rest:
				 *  cpu     engine drawing, frame_begin -> sceGuFinish
				 *  ge      sceGuFinish -> sceGuSync (the GE drawing)
				 *  vblank  sceDisplayWaitVblankStart (pacing)
				 *  all     the whole frame, so the remainder -- swap,
				 *          input, the client frame -- is visible too.
				 * If cpu is the biggest, the cost is ours; if vblank
				 * is, the frame is simply locked to the panel; if all
				 * is far above all three, the time is outside the
				 * drawing code entirely and the search moves there. */
				t_frames = psp_gu_timing_take (&t_cpu, &t_ge, &t_vb, &t_all);

				/* And what the log cost the memory card over the same
				 * frames.  This is the number that settles the argument
				 * instead of settling it by opinion: a frame of 250 ms
				 * with 240 ms of "card" is the diagnostic, a frame of
				 * 250 ms with 5 ms of "card" is the menu. */
				psp_con_card_take_stats (&card_writes, &card_us);

				if (t_frames > 0)
					psp_tracef("run: f %d state %d, malloc %d KB used, %d KB "
					           "left in the heap block, NULL %d, tex %d KB, "
					           "%.1f ms/frame over %d (cpu %.1f, ge %.1f, "
					           "vblank %.1f), card %u writes %u ms",
					           frame, (int)cls.state,
					           (int)(psp_heap_in_use() / 1024),
					           (int)(psp_heap_block_left() / 1024),
					           (int)psp_heap_malloc_fails(),
					           (int)(psp_gl_shim_texture_bytes() / 1024),
					           t_all, t_frames, t_cpu, t_ge, t_vb,
					           card_writes, card_us / 1000);
				else
					psp_tracef("run: f %d state %d, malloc %d KB used, %d KB "
					           "left in the heap block, NULL %d, tex %d KB, "
					           "no frames drawn in the last 2 s",
					           frame, (int)cls.state,
					           (int)(psp_heap_in_use() / 1024),
					           (int)(psp_heap_block_left() / 1024),
					           (int)psp_heap_malloc_fails(),
					           (int)(psp_gl_shim_texture_bytes() / 1024));
				nextbeat = newtime + 2.0;
			}

			/* Five frames drawn: the startup screens are behind us and the
			 * card can stop being written to on every change.  By now the
			 * startup script (kabluk.rc -> default.cfg -> config.cfg) has
			 * been executed, which is why the pad's default bindings are
			 * applied here and not at Host_Init: the config has to be in
			 * first or `unbindall` in it would undo them. */
			if (frame == 5)
			{
				psp_con_boot_done();
				IN_ApplyPadDefaults();
			}

			if (time < sys_ticrate.value)
				sceKernelDelayThread(1000);	/* 1 ms: let the system breathe */
		}
	}

	return 0;
}
