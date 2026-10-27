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

/* The engine wants one contiguous block for its zone, and how much is
 * available depends on whether the firmware granted the extra-memory request
 * (MEMSIZE=1 in PARAM.SFO): about 52 MB of user partition with it, 24 MB
 * without.
 *
 * So the size is discovered rather than assumed: try these from the top down
 * and take the first that malloc returns.  -mem <MB> overrides the list.
 *
 * 40 MB is not greed, it is the size of the data this game has to hold once
 * the maps, models and sounds of a 42 MB set of packs are resident. */
static const int psp_zone_sizes_mb[] = { 40, 36, 32, 28, 24, 20, 16, 12, 8 };
#define PSP_NUM_ZONE_SIZES \
	((int)(sizeof(psp_zone_sizes_mb) / sizeof(psp_zone_sizes_mb[0])))
#define PSP_MIN_MEMORY (8 * 1024 * 1024)

/* Memory the engine keeps OUTSIDE the zone, for plain malloc().
 *
 * The zone is one huge block handed to the engine, but the engine still calls
 * malloc() for a few things of its own -- gfx.wad is loaded with it, so are the
 * .cfg/.rc files, and the pak reader uses it for a directory scan.  Handing the
 * whole heap to the zone leaves 768 KB, and then "not enough memory" arrives
 * from somewhere unrelated and confusing.
 *
 * The block is therefore sized as "the biggest one that fits MINUS this", which
 * keeps a real reserve on top of the zone. */
#define PSP_MALLOC_RESERVE (3 * 1024 * 1024)

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

	/* One small block, kept for the whole run on purpose.  It does two things:
	 * it makes sure the newlib heap exists at all (PSP_HEAP_SIZE_KB(-1) sizes it
	 * on the first malloc), and -- because it is never freed -- it pins the
	 * bottom of the heap, so a later free() cannot hand the whole partition
	 * back to the system and strand the big block. */
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
		int i;
		/* The reserve is taken off BEFORE the attempt, not after a successful
		 * probe.
		 *
		 * The first version asked malloc() for the full candidate, freed that
		 * block, and then allocated candidate-minus-reserve.  It looked safe
		 * and was not: newlib hands a freed block at the top of the heap back
		 * to the system, and asking for it again afterwards failed -- 40 MB
		 * probed fine, and then 37 MB came back NULL, which the error message
		 * below reported as "even 8 MB failed" and sent us chasing ghosts.
		 *
		 * Trying the size we actually want, largest first, has no such step:
		 * every attempt is a real allocation that either becomes the zone or
		 * fails small. */
		for (i = 0; i < PSP_NUM_ZONE_SIZES; i++)
		{
			int want = psp_zone_sizes_mb[i] * 1024 * 1024 - PSP_MALLOC_RESERVE;

			if (want < PSP_MIN_MEMORY)
				want = PSP_MIN_MEMORY;

			parms.membase = malloc((size_t)want);
			if (parms.membase)
			{
				parms.memsize = want;
				Sys_Printf("Zone: %d MB (+%d MB kept for malloc)\n",
				           want / (1024 * 1024), PSP_MALLOC_RESERVE / (1024 * 1024));
				break;
			}

			Sys_Printf("Zone: %d MB did not fit\n", want / (1024 * 1024));
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
	psp_con_printf("entering main loop, state %d\n", (int)cls.state);

	/* From here on the engine draws pictures hundreds of times a second, and
	 * every changed breadcrumb would be an open/write/close on the memory
	 * stick.  The boot trail has done its job. */
	psp_con_boot_done();

	oldtime = Sys_DoubleTime();

	{
		int frame = 0;
		double nextbeat = oldtime + 2.0;

		while (1)
		{
			newtime = Sys_DoubleTime();
			time = newtime - oldtime;

			Host_Frame(time);

			oldtime = newtime;
			frame++;

			/* A heartbeat every two seconds: if the screen freezes on one
			 * of these, the loop itself is fine and the fault is inside
			 * the frame; if the numbers stop, it is the last one that
			 * matters. */
			if (newtime >= nextbeat)
			{
				psp_con_printf("run: frame %d, state %d\n",
				               frame, (int)cls.state);
				nextbeat = newtime + 2.0;
			}

			if (time < sys_ticrate.value)
				sceKernelDelayThread(1000);	/* 1 ms: let the system breathe */
		}
	}

	return 0;
}
