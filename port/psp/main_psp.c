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
 * the maps, models and sounds of a 64 MB set of packs are resident. */
static const int psp_zone_sizes_mb[] = { 40, 32, 24, 20, 16, 12, 8 };
#define PSP_NUM_ZONE_SIZES \
	((int)(sizeof(psp_zone_sizes_mb) / sizeof(psp_zone_sizes_mb[0])))
#define PSP_MIN_MEMORY (8 * 1024 * 1024)

static quakeparms_t	parms;

int main (int argc, char *argv[])
{
	double	time, oldtime, newtime;

	psp_con_init();
	psp_con_printf("Kabluk for PSP -- starting up\n");

	/* We replaced SDL's main() (SDL_MAIN_HANDLED), so SDL has to be told that
	 * the application is ready before any SDL_Init call. */
	SDL_SetMainReady();
	if (SDL_Init(0) < 0)
		psp_con_panic("SDL_Init failed", NULL, 0, NULL, SDL_GetError());

	host_parms = &parms;
	memset(&parms, 0, sizeof(parms));
	parms.basedir = ".";
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

		for (i = 0; i < PSP_NUM_ZONE_SIZES; i++)
		{
			parms.memsize = psp_zone_sizes_mb[i] * 1024 * 1024;
			parms.membase = malloc(parms.memsize);
			if (parms.membase)
				break;
			Sys_Printf("Zone: %d MB did not fit\n", psp_zone_sizes_mb[i]);
		}
	}

	if (!parms.membase)
		Sys_Error("Not enough memory free: even %d MB failed\n",
		          PSP_MIN_MEMORY / (1024 * 1024));

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
