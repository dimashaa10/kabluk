/*
 * port/psp/sdl_stub.c -- the half of the SDL2 API the engine actually calls,
 * implemented on top of the PSP SDK.
 *
 * Why not link libSDL2 from pspdev?  Two reasons.
 *
 * 1. It does not boot.  The EBOOT imports every library the SDL2 build was
 *    linked against (pspgl, libpspvram, and whatever else), and the PSP refuses
 *    to start a module whose import list names a library that is not resident
 *    at boot: "Запуск игры невозможен (8002013C)", which is
 *    SCE_KERNEL_ERROR_LIBRARY_NOTFOUND.  The test EBOOTs, which link only the
 *    SDK, boot fine -- that is the difference.
 *
 * 2. We do not want it.  SDL's video backend would drag the whole GL stack in
 *    behind it, and M4 draws with sceGu directly anyway.
 *
 * So the engine keeps its SDL-shaped code and this file answers for it.  The
 * set of functions below is not a guess: it is exactly what `nm -u` reports
 * for the 75 engine objects plus our own port files (52 symbols).
 *
 * The type signatures come from the real SDL headers, which are still on the
 * include path, so the compiler checks every one of these against the
 * declaration the engine was compiled against.
 */

#include <pspkernel.h>

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <SDL.h>

#include "psp_console.h"

/* ------------------------------------------------------------------ */
/* core                                                                */
/* ------------------------------------------------------------------ */

void SDL_SetMainReady (void)
{
}

int SDL_Init (Uint32 flags)
{
	(void) flags;
	return 0;
}

void SDL_Quit (void)
{
}

const char *SDL_GetError (void)
{
	return "";
}

const char *SDL_GetPlatform (void)
{
	return "PSP";
}

void SDL_GetVersion (SDL_version *ver)
{
	ver->major = 2;
	ver->minor = 0;
	ver->patch = 14;
}

int SDL_GetCPUCount (void)
{
	return 1;			/* Allegrex, single core */
}

int SDL_GetSystemRAM (void)
{
	return 64;			/* PSP-E100x */
}

/* sceKernelGetSystemTimeWide counts microseconds since boot. */
Uint32 SDL_GetTicks (void)
{
	return (Uint32) (sceKernelGetSystemTimeWide() / 1000);
}

Uint64 SDL_GetTicks64 (void)
{
	return (Uint64) (sceKernelGetSystemTimeWide() / 1000);
}

void SDL_Delay (Uint32 ms)
{
	sceKernelDelayThread(ms * 1000);
}

SDL_AssertState SDL_ReportAssertion (SDL_AssertData *data, const char *func,
                                     const char *file, int line)
{
	/* The engine trips these on paths we do not run; report and continue
	 * rather than halting the module in the middle of a frame. */
	psp_con_printf("ASSERT %s:%d %s: %s\n", file ? file : "?", line,
	               func ? func : "?", data ? data->condition : "?");
	return SDL_ASSERTION_ALWAYS_IGNORE;
}

/* ------------------------------------------------------------------ */
/* strings -- the engine uses SDL's copies so that a build without libc  */
/* string helpers still works; we have a full newlib, so forward.        */
/* ------------------------------------------------------------------ */

char *SDL_strdup (const char *str)
{
	return str ? strdup(str) : NULL;
}

long SDL_strtol (const char *str, char **endptr, int base)
{
	return strtol(str, endptr, base);
}

Uint64 SDL_strtoull (const char *str, char **endptr, int base)
{
	return (Uint64) strtoull(str, endptr, base);
}

double SDL_strtod (const char *str, char **endptr)
{
	return strtod(str, endptr);
}

float SDL_roundf (float x)
{
	return roundf(x);
}

void *SDL_memset (void *dst, int c, size_t len)
{
	return memset(dst, c, len);
}

char *SDL_strtokr (char *s1, const char *s2, char **saveptr)
{
	return strtok_r(s1, s2, saveptr);
}

/* ------------------------------------------------------------------ */
/* threads, mutexes, conditions                                        */
/*                                                                     */
/* Threads do not exist in this port: SDL_CreateThread hands back an   */
/* object and NEVER calls the function.                                */
/*                                                                     */
/* It used to call it, "to completion", on the theory that the only    */
/* caller was the website check.  That froze the console in URI_Init:   */
/* pr_ext.c starts its URI worker that way, and the worker is           */
/*                                                                     */
/*     while (uri_worker_running) {                                     */
/*         req = dequeue_request_blocking ();   <- SDL_CondWait        */
/*         ...                                                          */
/*     }                                                                */
/*                                                                     */
/* With a cond var that returns immediately (there is no other thread   */
/* to wake it), "to completion" means spinning forever on the main      */
/* thread, inside Host_Init, with no output and no crash -- the engine  */
/* simply never came back from URI_Init.                                */
/*                                                                     */
/* Everything that starts a thread here is network work: the URI        */
/* worker, the website check, Discord, server pings, the macOS HID      */
/* thread.  An E1008 has no Wi-Fi at all, so not running them costs      */
/* nothing.  The mutex/cond stubs stay, because code locks them while   */
/* the worker is "not running".                                        */
/* ------------------------------------------------------------------ */

struct SDL_mutex { int owned; };
struct SDL_cond  { int dummy; };
struct SDL_Thread { int status; };

SDL_mutex *SDL_CreateMutex (void)
{
	SDL_mutex *m = malloc(sizeof(*m));
	if (m)
		m->owned = 0;
	return m;
}

int SDL_LockMutex (SDL_mutex *m)
{
	if (m)
		m->owned++;
	return 0;
}

int SDL_UnlockMutex (SDL_mutex *m)
{
	if (m && m->owned > 0)
		m->owned--;
	return 0;
}

void SDL_DestroyMutex (SDL_mutex *m)
{
	free(m);
}

SDL_cond *SDL_CreateCond (void)
{
	return malloc(sizeof(SDL_cond));
}

int SDL_CondWait (SDL_cond *c, SDL_mutex *m)
{
	(void) c; (void) m;
	return 0;
}

int SDL_CondSignal (SDL_cond *c)
{
	(void) c;
	return 0;
}

int SDL_CondBroadcast (SDL_cond *c)
{
	(void) c;
	return 0;
}

void SDL_DestroyCond (SDL_cond *c)
{
	free(c);
}

SDL_Thread *SDL_CreateThread (SDL_ThreadFunction fn, const char *name,
                              void *data)
{
	SDL_Thread *t = malloc(sizeof(*t));

	/* fn is deliberately not called -- see the comment above. */
	(void) name;
	(void) fn;
	(void) data;

	if (!t)
		return NULL;

	t->status = 0;
	return t;
}

void SDL_WaitThread (SDL_Thread *t, int *status)
{
	if (!t)
		return;
	if (status)
		*status = t->status;
	free(t);
}

void SDL_DetachThread (SDL_Thread *t)
{
	free(t);
}

/* ------------------------------------------------------------------ */
/* atomics -- one core, so plain reads and writes are enough; the      */
/* builtins keep the compiler from reordering them across calls.        */
/* ------------------------------------------------------------------ */

int SDL_AtomicSet (SDL_atomic_t *a, int v)
{
	return __sync_lock_test_and_set(&a->value, v);
}

int SDL_AtomicGet (SDL_atomic_t *a)
{
	return __sync_add_and_fetch(&a->value, 0);
}

int SDL_AtomicAdd (SDL_atomic_t *a, int v)
{
	return __sync_fetch_and_add(&a->value, v);
}

SDL_bool SDL_AtomicCAS (SDL_atomic_t *a, int oldval, int newval)
{
	return __sync_bool_compare_and_swap(&a->value, oldval, newval)
	       ? SDL_TRUE : SDL_FALSE;
}

/* ------------------------------------------------------------------ */
/* timers -- nothing pumps an event loop for us, so a timer is a       */
/* one-shot callback run at the next call.  The only user is the       */
/* "exec cfg after connect" delay in cl_main.c, where running it once  */
/* immediately is no worse than running it late.                       */
/* ------------------------------------------------------------------ */

SDL_TimerID SDL_AddTimer (Uint32 interval, SDL_TimerCallback callback,
                          void *param)
{
	(void) interval;
	if (callback)
		callback(interval, param);
	return 0;
}

SDL_bool SDL_RemoveTimer (SDL_TimerID id)
{
	(void) id;
	return SDL_TRUE;
}

/* ------------------------------------------------------------------ */
/* audio -- SNDDMA_Init already declines, this is the belt to that     */
/* braces: any device open fails, so the engine keeps its null driver. */
/* ------------------------------------------------------------------ */

SDL_AudioDeviceID SDL_OpenAudioDevice (const char *device, int iscapture,
                                       const SDL_AudioSpec *desired,
                                       SDL_AudioSpec *obtained,
                                       int allowed_changes)
{
	(void) device; (void) iscapture; (void) desired; (void) obtained;
	(void) allowed_changes;
	return 0;
}

void SDL_CloseAudioDevice (SDL_AudioDeviceID dev)
{
	(void) dev;
}

void SDL_PauseAudioDevice (SDL_AudioDeviceID dev, int pause_on)
{
	(void) dev; (void) pause_on;
}

Uint32 SDL_GetQueuedAudioSize (SDL_AudioDeviceID dev)
{
	(void) dev;
	return 0;
}

Uint32 SDL_DequeueAudio (SDL_AudioDeviceID dev, void *data, Uint32 len)
{
	(void) dev; (void) data; (void) len;
	return 0;
}

const char *SDL_GetAudioDeviceName (int index, int iscapture)
{
	(void) index; (void) iscapture;
	return "";
}

/* ------------------------------------------------------------------ */
/* window, cursor, clipboard -- vid_psp.c owns the real surface        */
/* ------------------------------------------------------------------ */

int SDL_FlashWindow (SDL_Window *window, SDL_FlashOperation operation)
{
	(void) window; (void) operation;
	return 0;
}

int SDL_OpenURL (const char *url)
{
	psp_con_printf("SDL_OpenURL: %s\n", url ? url : "");
	return 0;
}

int SDL_SetClipboardText (const char *text)
{
	(void) text;
	return 0;
}

SDL_Cursor *SDL_CreateSystemCursor (SDL_SystemCursor id)
{
	(void) id;
	return NULL;
}

SDL_Cursor *SDL_GetCursor (void)
{
	return NULL;
}

void SDL_SetCursor (SDL_Cursor *cursor)
{
	(void) cursor;
}

int SDL_ShowCursor (int toggle)
{
	(void) toggle;
	return 0;			/* there is no mouse to hide */
}

Uint32 SDL_GetMouseState (int *x, int *y)
{
	if (x)
		*x = 0;
	if (y)
		*y = 0;
	return 0;
}

/* ------------------------------------------------------------------ */
/* shared objects -- no dlopen on a static EBOOT                       */
/* ------------------------------------------------------------------ */

void *SDL_LoadObject (const char *sofile)
{
	(void) sofile;
	return NULL;
}

void *SDL_LoadFunction (void *handle, const char *name)
{
	(void) handle; (void) name;
	return NULL;
}

void SDL_UnloadObject (void *handle)
{
	(void) handle;
}
