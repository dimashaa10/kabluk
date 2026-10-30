/*
 * port/psp/cd_psp.c -- CD audio, which a PSP does not have.
 *
 * The engine treats a failing CDAudio_Init() as "no music from CD" and carries
 * on; that is exactly right here.  Music for the port will come from files
 * (bgmusic) rather than a disc.
 */

#include "quakedef.h"

int CDAudio_Init (void)
{
	return -1;	/* -1: no CD audio, which CDAudio_Init callers tolerate */
}

void CDAudio_Shutdown (void)
{
}

int CDAudio_Play (byte track, qboolean looping)
{
	(void)track; (void)looping;
	return -1;
}

void CDAudio_Stop (void)
{
}

void CDAudio_Pause (void)
{
}

void CDAudio_Resume (void)
{
}

void CDAudio_Update (void)
{
}
