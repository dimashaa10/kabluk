/*
 * port/psp/snd_psp.c -- sound output, stage M3: the mixer runs, the device
 * refuses to open.
 *
 * Returning false from SNDDMA_Init() is the documented way to tell snd_dma.c
 * that no device is available; the engine prints "sound not available" and
 * keeps going with S_* calls that do nothing.  Stage M4/M5 replaces this with
 * sceAudio: sceAudioChReserve + sceAudioOutputBlocking on a background thread
 * feeding dma.buffer.
 *
 * Deliberately free of PSP calls so the file builds on the host as well.
 */

#include "quakedef.h"

/* snd_dma.c owns this pointer, and every backend sets it to the block the engine
 * hands in -- even the ones that then refuse to open a device (see snd_sdl.c).
 *
 * The reason is S_Startup():
 *
 *	sound_started = SNDDMA_Init (&sn);
 *	if (!sound_started) Con_Printf ("Failed initializing sound\n");
 *	GetSoundtime ();          <- reads shm->samples, shm->channels
 *
 * GetSoundtime() runs before S_Init() gets a chance to notice that sound never
 * started.  With shm still NULL that is a read through address 0: the console
 * went out immediately after "Failed initializing sound", which is exactly
 * where Host_Init stopped. */
extern volatile dma_t	*shm;

qboolean SNDDMA_Init (dma_t *dma)
{
	memset (dma, 0, sizeof (*dma));

	/* GetSoundtime() divides by channels, and S_Startup() calls it even when
	 * there is no device.  Keep the divisors sane; samples stays 0, so the
	 * clock it computes is simply zero. */
	dma->channels   = 1;
	dma->samplebits = 16;
	dma->speed      = 0;
	dma->samples    = 0;

	shm = dma;

	return false;   /* no device: the engine keeps running without sound */
}

int SNDDMA_GetDMAPos (void)
{
	return 0;
}

void SNDDMA_Shutdown (void)
{
}

void SNDDMA_LockBuffer (void)
{
}

void SNDDMA_Submit (void)
{
}

void SNDDMA_BlockSound (void)
{
}

void SNDDMA_UnblockSound (void)
{
}
