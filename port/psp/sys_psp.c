/*
 * port/psp/sys_psp.c -- system services on the PSP.
 *
 * This is the file the rest of the engine cannot do without: every file read,
 * every Sys_Printf and the whole frame clock go through here.  Mapped onto:
 *
 *   files      sceIoOpen / sceIoRead / sceIoWrite / sceIoLseek / sceIoClose
 *   directories sceIoDopen / sceIoDread / sceIoDclose
 *   time       sceKernelGetSystemTimeWide (microseconds, never wraps)
 *   sleeping   sceKernelDelayThread
 *   output     the on-screen console from psp_console.c
 *
 * The engine's file API is int handles, so SceUID maps onto it directly.
 */

#include <pspkernel.h>
#include <pspiofilemgr.h>

#include "quakedef.h"
#include "psp_console.h"

/* Owned by the desktop sys layer normally; the backends here define them. */
qboolean	isDedicated = false;
cvar_t		sys_throttle = {"sys_throttle", "0.05", CVAR_ARCHIVE};

#define PSP_MAX_HANDLES 128

static SceUID	sys_handles[PSP_MAX_HANDLES];
static FILE	*sys_stdio[PSP_MAX_HANDLES];

static void Sys_ClearHandles (void)
{
	int i;

	for (i = 0; i < PSP_MAX_HANDLES; i++)
	{
		sys_handles[i] = -1;
		sys_stdio[i] = NULL;
	}
}

static int Sys_AllocHandle (void)
{
	int i;

	for (i = 0; i < PSP_MAX_HANDLES; i++)
		if (sys_handles[i] < 0 && sys_stdio[i] == NULL)
			return i;

	Sys_Error("Sys_AllocHandle: out of file handles");
	return 0;
}

void Sys_Init (void)
{
	Sys_ClearHandles();

	psp_con_init();
	psp_con_printf("PSP: %s\n", PLATFORM_STRING);
}

/*
================
File IO
================
*/

qofs_t Sys_FileOpenRead (const char *path, int *hndl)
{
	SceUID fd;
	SceIoStat st;

	fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
	if (fd < 0)
	{
		*hndl = -1;
		return -1;
	}

	if (sceIoGetstat(path, &st) < 0)
	{
		sceIoClose(fd);
		*hndl = -1;
		return -1;
	}

	*hndl = Sys_AllocHandle();
	sys_handles[*hndl] = fd;

	return (qofs_t)st.st_size;
}

int Sys_FileOpenWrite (const char *path)
{
	SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
	int h;

	if (fd < 0)
		Sys_Error("Error opening %s: %d", path, (int)fd);

	h = Sys_AllocHandle();
	sys_handles[h] = fd;

	return h;
}

int Sys_FileOpenStdio (FILE *file)
{
	int h = Sys_AllocHandle();

	sys_stdio[h] = file;
	return h;
}

void Sys_FileClose (int handle)
{
	if (handle < 0 || handle >= PSP_MAX_HANDLES)
		return;

	if (sys_stdio[handle])
	{
		fclose(sys_stdio[handle]);
		sys_stdio[handle] = NULL;
		return;
	}

	if (sys_handles[handle] >= 0)
	{
		sceIoClose(sys_handles[handle]);
		sys_handles[handle] = -1;
	}
}

void Sys_FileSeek (int handle, qofs_t position)
{
	if (handle < 0 || handle >= PSP_MAX_HANDLES)
		return;

	if (sys_stdio[handle])
	{
		fseek(sys_stdio[handle], (long)position, SEEK_SET);
		return;
	}

	if (sys_handles[handle] >= 0)
		sceIoLseek(sys_handles[handle], (SceOff)position, PSP_SEEK_SET);
}

int Sys_FileRead (int handle, void *dest, int count)
{
	if (handle < 0 || handle >= PSP_MAX_HANDLES || count <= 0)
		return 0;

	if (sys_stdio[handle])
		return (int)fread(dest, 1, (size_t)count, sys_stdio[handle]);

	if (sys_handles[handle] < 0)
		return 0;

	return (int)sceIoRead(sys_handles[handle], dest, (SceSize)count);
}

int Sys_FileWrite (int handle, const void *data, int count)
{
	if (handle < 0 || handle >= PSP_MAX_HANDLES || count <= 0)
		return 0;

	if (sys_stdio[handle])
		return (int)fwrite(data, 1, (size_t)count, sys_stdio[handle]);

	if (sys_handles[handle] < 0)
		return 0;

	return (int)sceIoWrite(sys_handles[handle], data, (SceSize)count);
}

void Sys_mkdir (const char *path)
{
	sceIoMkdir(path, 0777);	/* failure is fine: the caller checks later */
}

int Sys_FileType (const char *path)
{
	SceIoStat st;

	if (sceIoGetstat(path, &st) < 0)
		return FS_ENT_NONE;

	if (FIO_S_ISDIR(st.st_mode))
		return FS_ENT_DIRECTORY;
	if (FIO_S_ISREG(st.st_mode))
		return FS_ENT_FILE;

	return FS_ENT_NONE;
}

/*
================
Misc
================
*/

void Sys_Quit (void)
{
	psp_con_fflush(NULL);
	sceKernelExitGame();

	for (;;)
		sceKernelDelayThread(1000 * 1000);
}

void Sys_Error (const char *error, ...)
{
	va_list argptr;
	char msg[1024];

	va_start(argptr, error);
	vsnprintf(msg, sizeof(msg), error, argptr);
	va_end(argptr);

	psp_con_panic("FATAL ERROR", NULL, 0, NULL, msg);
}

void Sys_Printf (const char *fmt, ...)
{
	va_list argptr;
	char msg[1024];

	va_start(argptr, fmt);
	vsnprintf(msg, sizeof(msg), fmt, argptr);
	va_end(argptr);

	psp_con_fputs(msg, stdout);
}

double Sys_DoubleTime (void)
{
	/* sceKernelGetSystemTimeWide gives microseconds and does not wrap. */
	return (double)sceKernelGetSystemTimeWide() / 1000000.0;
}

const char *Sys_ConsoleInput (void)
{
	return NULL;	/* no keyboard: the on-screen console takes commands */
}

void Sys_Sleep (unsigned long msecs)
{
	sceKernelDelayThread((SceUInt)(msecs * 1000));
}

void Sys_SendKeyEvents (void)
{
	/* Input is polled once per frame in in_psp.c. */
}
