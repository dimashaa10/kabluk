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

qboolean SNDDMA_Init (dma_t *dma)
{
	(void)dma;
	return false;
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
