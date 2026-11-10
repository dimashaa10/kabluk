/*
 * port/psp/in_psp.c -- input from the PSP pad.
 *
 * Two channels, and the split is the one the engine itself makes
 * (Quake/in_sdl.c does the same for a desktop controller):
 *
 *   IN_Commands()  turns the pad into KEY EVENTS.  Every button is an engine
 *                  key number (pad_map.h explains which and why), so anything
 *                  the player can bind -- fire, jump, the action key, weapon
 *                  change, even "toggleconsole" -- is a binding in the normal
 *                  bindings, changed from Controls -> Keys.  The engine's own
 *                  menus already understand the controller keys, so confirm
 *                  and back work without a single line of menu code.
 *
 *   IN_Move()      feeds the analog nub straight into forwardmove/sidemove,
 *                  which is what the engine's movement code expects.  The nub
 *                  is for walking and nothing else: looking around is a
 *                  binding (the arrow keys turn, as they do on a keyboard).
 *
 * The button table is in pad_map.h/.c, which is why it can be tested on a
 * desktop.
 */

#include <pspctrl.h>
#include <pspthreadman.h>

#include "quakedef.h"
#include "pad_map.h"
#include "psp_console.h"

/* Normally defined by in_sdl.c; console.c and host_cmd.c refer to it. */
char	afk_name[16] = "";

#define PSP_STICK_DEADZONE 24

/* The button bits in pad_map.h are pspsdk's.  This is what keeps them pspsdk's:
 * a typo there would press the wrong button on the console and nowhere else,
 * and no desktop build could ever see it.  A wrong number is a build error. */
_Static_assert(PAD_BTN_SELECT   == PSP_CTRL_SELECT,   "pad_map: SELECT");
_Static_assert(PAD_BTN_START    == PSP_CTRL_START,    "pad_map: START");
_Static_assert(PAD_BTN_UP       == PSP_CTRL_UP,       "pad_map: UP");
_Static_assert(PAD_BTN_RIGHT    == PSP_CTRL_RIGHT,    "pad_map: RIGHT");
_Static_assert(PAD_BTN_DOWN     == PSP_CTRL_DOWN,     "pad_map: DOWN");
_Static_assert(PAD_BTN_LEFT     == PSP_CTRL_LEFT,     "pad_map: LEFT");
_Static_assert(PAD_BTN_LTRIGGER == PSP_CTRL_LTRIGGER, "pad_map: LTRIGGER");
_Static_assert(PAD_BTN_RTRIGGER == PSP_CTRL_RTRIGGER, "pad_map: RTRIGGER");
_Static_assert(PAD_BTN_TRIANGLE == PSP_CTRL_TRIANGLE, "pad_map: TRIANGLE");
_Static_assert(PAD_BTN_CIRCLE   == PSP_CTRL_CIRCLE,   "pad_map: CIRCLE");
_Static_assert(PAD_BTN_CROSS    == PSP_CTRL_CROSS,    "pad_map: CROSS");
_Static_assert(PAD_BTN_SQUARE   == PSP_CTRL_SQUARE,   "pad_map: SQUARE");

static SceCtrlData	pad_cur;
static int		pad_inited;

/* One entry per button in psp_pad_keys[]. */
static pad_repeat_t	pad_state[PAD_KEY_MAX];

void IN_Init (void)
{
	int i;

	/* Cycle 0: the pad is sampled once per game frame, not by a timer of its
	 * own.  Analog mode is what puts Lx/Ly in the data at all. */
	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

	memset(&pad_cur, 0, sizeof(pad_cur));

	for (i = 0; i < PAD_KEY_MAX; i++)
	{
		pad_state[i].down = 0;
		pad_state[i].next = 0.0;
	}

	/* Centre, not zero: a zeroed SceCtrlData is the stick pushed into the
	 * top-left corner, and the first IN_Move() before the first poll would
	 * have walked the player that way. */
	pad_cur.Lx = 128;
	pad_cur.Ly = 128;

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

void IN_ClearStates (void)
{
	int i;

	/* The engine has forgotten which keys are down (it does this when a
	 * binding grab starts or the video mode changes).  If the pad still
	 * thought a button was held, it would never send the down event for it
	 * again -- the button would look dead until it was pressed twice. */
	for (i = 0; i < PAD_KEY_MAX; i++)
	{
		pad_state[i].down = 0;
		pad_state[i].next = 0.0;
	}
}

/* ---- the pad's own defaults ---------------------------------------------
 *
 * The face buttons are engine keys, and an engine key with no binding does
 * nothing at all.  So the useful ones are given an action here (the table is
 * in pad_map.c) -- but not in Quake/default_cfg.h, and the reason is the
 * startup script:
 *
 *     exec kabluk.rc -> exec default.cfg -> exec config.cfg
 *
 * `exec default.cfg` first tries the file inside the game's pak, and
 * QuakeSpasm's default.cfg begins with `unbindall`.  Whatever a compiled-in
 * default said would therefore be wiped out by that file a line later.  This
 * runs after the config has been read (main_psp.c calls it once, a few frames
 * in), and only fills in keys the config left empty -- a player who has picked
 * his own fire button keeps it.
 *
 * These are ordinary bindings: Controls -> Keys shows them and changes them
 * like any other key.
 */
void IN_ApplyPadDefaults (void)
{
	int i, n = 0;

	for (i = 0; i < psp_pad_default_count; i++)
	{
		int key = psp_pad_defaults[i].key;

		if (keybindings[0][key] || keybindings[1][key])
			continue;

		Cbuf_AddText (va("bind \"%s\" \"%s\"\n",
		                 Key_KeynumToString(key),
		                 psp_pad_defaults[i].binding));
		n++;
	}

	psp_tracef ("in: pad defaults: %d of %d action buttons were unbound "
	            "(see pad_map.c: cross +attack, square +jump, triangle +use)",
	            n, psp_pad_default_count);
}

void IN_SendKeyEvents (void)
{
	/* Sys_SendKeyEvents() hands input in here on the desktop backends; on the
	 * PSP the poll belongs to IN_Commands(), so this is the same thing.  It
	 * exists because Scr_ModalMessage()'s loop calls Sys_SendKeyEvents() and
	 * nothing else. */
	IN_Commands ();
}

static void IN_Poll (void)
{
	if (!pad_inited)
		return;

	if (sceCtrlPeekBufferPositive(&pad_cur, 1) < 0)
	{
		/* A failed read is not "everything was released".  Zeroing the
		 * sample here sent a burst of key-up events -- a held +forward
		 * stopped for a frame, and a +use click could be cut in half --
		 * so the last good sample stands until the next one arrives.
		 * The message is capped: a pad that stops answering would
		 * otherwise write to the memory stick every frame. */
		static int read_errors;

		if (read_errors < 4)
		{
			read_errors++;
			psp_tracef ("in: sceCtrlPeekBufferPositive failed, holding the "
			            "previous pad sample (%d)", read_errors);
		}

		return;
	}
}

/*
 * Bring one button up to date and send whatever key event it produced.
 *
 * The repeat is for the directions, and only outside the game.
 *
 * Outside it is the whole point: a menu moves one item per key event, so
 * without it a ten item list is ten separate presses.
 *
 * Inside the game it is both useless and wrong.  Useless, because +forward and
 * +left are key *states* -- the engine reads keydown[] every frame and drops
 * the second key-down of the same key (Quake/keys.c: "ignore autorepeats in
 * game mode").  Wrong, because whatever the player binds to a direction would
 * then be pushed into the command buffer ten times a second, and a bound
 * action (an impulse, +use) fired repeatedly is not a held key.
 */
static void IN_Button (int index, double now)
{
	const pad_key_t *k = &psp_pad_keys[index];
	int isdown = (pad_cur.Buttons & k->button) != 0;
	int repeat_ok = k->repeat && key_dest != key_game;
	int ev;

	ev = psp_pad_step(&pad_state[index], isdown, now, repeat_ok);

	if (ev == PAD_EVENT_DOWN)
		Key_Event (k->key, true);
	else if (ev == PAD_EVENT_UP)
		Key_Event (k->key, false);
}

void IN_Commands (void)
{
	double now;
	int i;

	IN_Poll ();

	now = Sys_DoubleTime ();

	for (i = 0; i < psp_pad_key_count && i < PAD_KEY_MAX; i++)
		IN_Button (i, now);
}

void IN_Move (usercmd_t *cmd)
{
	float	fwd, side;

	/* The nub walks, and only while the player is playing.  With a menu open
	 * or the game paused, a stick left off-centre must not keep the player
	 * moving -- the desktop controller code refuses for the same reason
	 * (Quake/in_sdl.c, IN_JoyMove). */
	if (key_dest != key_game || cl.paused)
		return;

	side = psp_pad_axis ((int)pad_cur.Lx, PSP_STICK_DEADZONE);
	fwd  = -psp_pad_axis ((int)pad_cur.Ly, PSP_STICK_DEADZONE);	/* up = forward */

	if (side == 0.0f && fwd == 0.0f)
		return;

	/* Same units as the keyboard path, and the same cl_*speed limits, so a
	 * nub at full travel is exactly as fast as the player's +forward. */
	cmd->sidemove   += (short)(side * cl_sidespeed.value);
	cmd->forwardmove += (short)(fwd * (fwd < 0.0f ? cl_backspeed.value
	                                              : cl_forwardspeed.value));

	if (cmd->forwardmove >  cl_forwardspeed.value) cmd->forwardmove =  (short)cl_forwardspeed.value;
	if (cmd->forwardmove < -cl_backspeed.value)    cmd->forwardmove = -(short)cl_backspeed.value;
	if (cmd->sidemove    >  cl_sidespeed.value)    cmd->sidemove    =  (short)cl_sidespeed.value;
	if (cmd->sidemove    < -cl_sidespeed.value)    cmd->sidemove    = -(short)cl_sidespeed.value;
}
