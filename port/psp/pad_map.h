/*
 * port/psp/pad_map.h -- which engine key each pad button produces, and how a
 * held button repeats.
 *
 * The port used to give every pad button a private action of its own (Cross
 * was "Enter", Circle was "Escape", Triangle was the letter 'e', ...).  That
 * works, in the same way a keyboard with the keys soldered in place works: the
 * engine's Controls -> Keys screen cannot rebind any of it, because as far as
 * the engine knows the player has no controller at all.
 *
 * Two rules fix that, and they are the rules every other Quake port follows:
 *
 *   1. A pad button produces an ordinary KEY EVENT.
 *
 *      The engine already understands a controller -- it has key numbers for
 *      one (K_ABUTTON, K_BBUTTON, K_XBUTTON, K_YBUTTON, K_LSHOULDER,
 *      K_RSHOULDER, K_LTHUMB, K_RTHUMB) and its own menus use them: "confirm"
 *      is K_ABUTTON everywhere (Quake/menu.c, 40 of the 41 menu handlers), and
 *      "back" is K_BBUTTON.  Its default bindings already bind the shoulders
 *      to weapon change (Quake/default_cfg.h: LSHOULDER = impulse 12,
 *      RSHOULDER = impulse 10).  So a pad button that says "K_ABUTTON" gets
 *      every one of those behaviours for free, and the player can rebind it to
 *      anything in Controls -> Keys -- including +use, which is what the
 *      triangle button is for.
 *
 *   2. A held direction repeats, exactly the way the desktop controller code
 *      repeats it (Quake/in_sdl.c, IN_JoyKeyEvent): 0.5 s before the first
 *      repeat, then one every 0.1 s.  Without that, moving down a ten item
 *      menu is ten separate presses.
 *
 * Nothing here includes a PSP header or an engine header other than keys.h, so
 * the whole mapping can be tested on a desktop -- tests/test_pad_map.c.  The
 * one risk that leaves is a wrong button bit, and in_psp.c answers it with a
 * _Static_assert against the real <pspctrl.h>: a typo is a build error, not a
 * button that does the wrong thing on the console.
 */
#ifndef KABLUK_PAD_MAP_H
#define KABLUK_PAD_MAP_H

/* Self-sufficient on purpose: the desktop test (tests/test_pad_map.c) is not
 * an engine translation unit and does not include quakedef.h.  Both headers
 * have include guards, so inside the port these two lines do nothing. */
#include "q_stdinc.h"
#include "keys.h"

/* The buttons, by the bit values pspsdk's <pspctrl.h> gives them. */
#define PAD_BTN_SELECT      0x000001u
#define PAD_BTN_START       0x000008u
#define PAD_BTN_UP          0x000010u
#define PAD_BTN_RIGHT       0x000020u
#define PAD_BTN_DOWN        0x000040u
#define PAD_BTN_LEFT        0x000080u
#define PAD_BTN_LTRIGGER    0x000100u
#define PAD_BTN_RTRIGGER    0x000200u
#define PAD_BTN_TRIANGLE    0x001000u
#define PAD_BTN_CIRCLE      0x002000u
#define PAD_BTN_CROSS       0x004000u
#define PAD_BTN_SQUARE      0x008000u

/* One entry per button the port reports.  The table is fixed, so the array of
 * per-button state in in_psp.c can be a plain array; pad_map.c asserts that the
 * table still fits. */
#define PAD_KEY_MAX         16

typedef struct
{
	unsigned int	button;		/* PAD_BTN_*				*/
	int		key;		/* engine key number the button sends		*/
	int		repeat;		/* 1: hold to repeat (navigation only)		*/
	const char	*name;		/* for the log and for the manual		*/
} pad_key_t;

extern const pad_key_t	psp_pad_keys[];
extern const int	psp_pad_key_count;

/* Per-button repeat state.  One of these per entry of psp_pad_keys[]. */
typedef struct
{
	int		down;		/* the engine has been told it is down	*/
	double		next;		/* when the next repeat is due		*/
} pad_repeat_t;

/* What psp_pad_step() decided this frame. */
#define PAD_EVENT_NONE	0
#define PAD_EVENT_DOWN	1
#define PAD_EVENT_UP	2

#define PAD_REPEAT_DELAY	0.5	/* seconds held before the first repeat */
#define PAD_REPEAT_RATE		0.1	/* ... and between repeats after it  */

/*
 * Advance one button by one frame.
 *
 *   isdown	the button is down in the sample that was just read
 *   now	the frame's clock (Sys_DoubleTime())
 *   repeat_ok	1 when this button may repeat right now (a direction, and the
 *		player is not in the game -- see in_psp.c)
 *
 * Returns PAD_EVENT_DOWN (call Key_Event(key, true)), PAD_EVENT_UP (call
 * Key_Event(key, false)) or PAD_EVENT_NONE.
 */
int psp_pad_step (pad_repeat_t *b, int isdown, double now, int repeat_ok);

/*
 * The action the port gives a face button when the config file left it
 * unbound.  These are ordinary bindings, not code: Controls -> Keys changes
 * them.  See in_psp.c (IN_ApplyPadDefaults) for why they are applied after the
 * config rather than in Quake/default_cfg.h.
 */
typedef struct
{
	int		key;		/* K_ABUTTON, ...			*/
	const char	*binding;	/* console command, e.g. "+attack"	*/
	const char	*button;	/* which pad button, for the log		*/
} pad_default_t;

extern const pad_default_t	psp_pad_defaults[];
extern const int		psp_pad_default_count;

/*
 * One analog axis, -1.0 .. 1.0, with `deadzone` counted in the pad's own units
 * (128 is the centre).
 *
 * The dead zone is not simply cut away: the travel that is left is stretched
 * over the whole range.  Cutting it away -- which this did -- leaves the first
 * fifth of the stick doing nothing AND the last fifth unable to ask for more
 * speed than the clamp allows, so the usable range was the middle 60 % of a
 * 60 % stick.
 */
float psp_pad_axis (int raw, int deadzone);

#endif /* KABLUK_PAD_MAP_H */
