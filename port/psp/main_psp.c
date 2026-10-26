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

/* 64 MB usable on a PSP-E100x, but the engine's zone wants a contiguous block
 * and the frame buffers, GU display list and code sit above it.  24 MB is the
 * number to start from; -mem <MB> overrides it on the command line. */
#define PSP_DEFAULT_MEMORY (24 * 1024 * 1024)

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

	parms.memsize = PSP_DEFAULT_MEMORY;
	if (COM_CheckParm("-mem"))
	{
		int p = COM_CheckParm("-mem") + 1;
		if (p < com_argc)
			parms.memsize = Q_atoi(com_argv[p]) * 1024 * 1024;
	}

	parms.membase = malloc(parms.memsize);
	if (!parms.membase)
		Sys_Error("Not enough memory free; check the memstick\n");

	Sys_Printf("Host_Init (%d MB)\n", parms.memsize / (1024 * 1024));
	Host_Init();

	oldtime = Sys_DoubleTime();

	while (1)
	{
		newtime = Sys_DoubleTime();
		time = newtime - oldtime;

		Host_Frame(time);

		oldtime = newtime;

		if (time < sys_ticrate.value)
			sceKernelDelayThread(1000);	/* 1 ms: let the system breathe */
	}

	return 0;
}
