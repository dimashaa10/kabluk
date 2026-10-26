/*
 * port/psp/pl_psp.c -- the small "platform utilities" layer.
 *
 * platform.h asks for four things: a window icon, video shutdown, clipboard
 * text and an error dialog.  A PSP has none of them, so they are no-ops; the
 * one that matters (PL_ErrorDialog) routes to the on-screen console instead of
 * a message box that could never be dismissed.
 */

#include "quakedef.h"

void PL_SetWindowIcon (void)
{
}

void PL_VID_Shutdown (void)
{
}

char *PL_GetClipboardData (void)
{
	return NULL;	/* callers already handle NULL */
}

void PL_ErrorDialog (const char *text)
{
	if (text && text[0])
		Sys_Printf("ERROR: %s\n", text);
}
