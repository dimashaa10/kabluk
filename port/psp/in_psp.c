/*
 * port/psp/in_psp.c -- input from the PSP pad.
 *
 * Two channels, like every Quake port:
 *   IN_Commands()  turns button edges into Key_Event() calls, so anything the
 *                  player can bind (including the action key on Cross) works
 *                  through the normal bindings, menus included;
 *   IN_Move()      feeds the analog nub straight into forwardmove/sidemove,
 *                  which is what the engine's own movement code expects.
 *
 * Button map (deliberately the same shape as the desktop defaults):
 *   Cross       Enter      Circle   Escape     Square   Space (jump)
 *   Triangle    action key ('e')                Start    Escape
 *   D-pad       arrow keys        L trigger     previous weapon
 *   R trigger   next weapon       Select        console
 */

#include <pspctrl.h>
#include <pspthreadman.h>

#include "quakedef.h"

/* Normally defined by in_sdl.c; console.c and host_cmd.c refer to it. */
char	afk_name[16] = "";

#define PSP_STICK_DEADZONE 24

static SceCtrlData	pad_cur;
static SceCtrlData	pad_prev;
static int		pad_inited;

void IN_Init (void)
{
	sceCtrlSetSamplingCycle(0);	/* 0: drive it from our own frame rate */
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

	memset(&pad_cur, 0, sizeof(pad_cur));
	memset(&pad_prev, 0, sizeof(pad_prev));
	pad_inited = 1;
}

void IN_Shutdown (void)
{
	pad_inited = 0;
}

void IN_UpdateGrabs (void)
{
	/* No mouse to grab on a PSP. */
}

void IN_UpdateInputMode (void)
{
}

static void IN_Poll (void)
{
	if (!pad_inited)
		return;

	pad_prev = pad_cur;
	if (sceCtrlPeekBufferPositive(&pad_cur, 1) < 0)
		memset(&pad_cur, 0, sizeof(pad_cur));
}

static qboolean IN_WasDown (unsigned int mask)
{
	return (pad_prev.Buttons & mask) != 0;
}

static void IN_Button (unsigned int mask, int key)
{
	qboolean down = (pad_cur.Buttons & mask) != 0;

	if (down == IN_WasDown(mask))
		return;

	Key_Event (key, down);
}

void IN_Commands (void)
{
	IN_Poll();

	IN_Button (PSP_CTRL_CROSS,    K_ENTER);
	IN_Button (PSP_CTRL_CIRCLE,   K_ESCAPE);
	IN_Button (PSP_CTRL_START,    K_ESCAPE);
	IN_Button (PSP_CTRL_SQUARE,   K_SPACE);
	IN_Button (PSP_CTRL_TRIANGLE, 'e');		/* the action key */
	IN_Button (PSP_CTRL_SELECT,   K_TILDE);
	IN_Button (PSP_CTRL_UP,       K_UPARROW);
	IN_Button (PSP_CTRL_DOWN,     K_DOWNARROW);
	IN_Button (PSP_CTRL_LEFT,     K_LEFTARROW);
	IN_Button (PSP_CTRL_RIGHT,    K_RIGHTARROW);
	IN_Button (PSP_CTRL_LTRIGGER, '[');
	IN_Button (PSP_CTRL_RTRIGGER, ']');
}

void IN_Move (usercmd_t *cmd)
{
	int	dx, dy;

	/* Analog nub: 128 is the centre on both axes. */
	dx = (int)pad_cur.Lx - 128;
	dy = (int)pad_cur.Ly - 128;

	if (dx > -PSP_STICK_DEADZONE && dx < PSP_STICK_DEADZONE)
		dx = 0;
	if (dy > -PSP_STICK_DEADZONE && dy < PSP_STICK_DEADZONE)
		dy = 0;

	if (!dx && !dy)
		return;

	/* Scale to the same units cl_input.c uses for the keyboard. */
	cmd->sidemove   += (short)(dx * 2);
	cmd->forwardmove -= (short)(dy * 2);

	/* Keep the cl_*speed caps in charge, as the keyboard path does. */
	if (cmd->forwardmove >  cl_forwardspeed.value) cmd->forwardmove =  (short)cl_forwardspeed.value;
	if (cmd->forwardmove < -cl_backspeed.value)    cmd->forwardmove = -(short)cl_backspeed.value;
	if (cmd->sidemove    >  cl_sidespeed.value)    cmd->sidemove    =  (short)cl_sidespeed.value;
	if (cmd->sidemove    < -cl_sidespeed.value)    cmd->sidemove    = -(short)cl_sidespeed.value;
}
