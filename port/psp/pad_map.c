/*
 * port/psp/pad_map.c -- the pad's button table, the repeat rule and the stick
 * curve.  See pad_map.h for why the mapping is "engine keys, not actions".
 *
 * Kept apart from in_psp.c on purpose: in_psp.c talks to <pspctrl.h> and can
 * only be compiled for a PSP, while everything here is plain C that the
 * desktop tests can run -- tests/test_pad_map.c checks that every button the
 * console has is reported, that the two buttons the engine's menus use
 * (confirm and back) are the right ones, and that the repeat behaves like
 * Quake/in_sdl.c's.
 */

#include <math.h>

#include "pad_map.h"

/*
 * The table.
 *
 *   cross	A	confirm in every menu, +attack in the default PSP config
 *   circle	B	back in every menu
 *   square	X	+jump		(default, see default_cfg.h)
 *   triangle	Y	+use		(the positive "press the button" action)
 *   L / R		previous / next weapon (the engine's own default binds)
 *   up/down/left/right	navigation in the menus; the engine's defaults bind
 *			them to move/turn in the game
 *   start		the pause / menu key (K_ESCAPE is the engine's own)
 *   select		the console, exactly as the ` key is on a keyboard
 *
 * Which of these is "the action button" is a *binding*, not code: the defaults
 * live in Quake/default_cfg.h and the player changes them in Controls -> Keys.
 */
const pad_key_t psp_pad_keys[] =
{
	{ PAD_BTN_CROSS,    K_ABUTTON,    0, "cross"    },
	{ PAD_BTN_CIRCLE,   K_BBUTTON,    0, "circle"   },
	{ PAD_BTN_SQUARE,   K_XBUTTON,    0, "square"   },
	{ PAD_BTN_TRIANGLE, K_YBUTTON,    0, "triangle" },
	{ PAD_BTN_LTRIGGER, K_LSHOULDER,  0, "L"        },
	{ PAD_BTN_RTRIGGER, K_RSHOULDER,  0, "R"        },
	{ PAD_BTN_START,    K_ESCAPE,     0, "start"    },
	{ PAD_BTN_SELECT,   '`',          0, "select"   },
	{ PAD_BTN_UP,       K_UPARROW,    1, "up"       },
	{ PAD_BTN_DOWN,     K_DOWNARROW,  1, "down"     },
	{ PAD_BTN_LEFT,     K_LEFTARROW,  1, "left"     },
	{ PAD_BTN_RIGHT,    K_RIGHTARROW, 1, "right"    },
};

const int psp_pad_key_count =
	(int)(sizeof(psp_pad_keys) / sizeof(psp_pad_keys[0]));

/* The per-button state array in in_psp.c is a fixed size. */
_Static_assert(sizeof(psp_pad_keys) / sizeof(psp_pad_keys[0]) <= PAD_KEY_MAX,
               "PAD_KEY_MAX is smaller than the button table");

/*
 * What the face buttons do out of the box.
 *
 *   cross	A	fire: the button under the right thumb
 *   square	X	jump: next to it, so fire and jump are one movement
 *   triangle	Y	use: the engine's positive "press the button" action
 *   circle	B	nothing -- it is the menus' "back", and a player who wants
 *			something there binds it
 *
 * The d-pad and the shoulders need nothing: the d-pad sends the arrow keys and
 * LSHOULDER/RSHOULDER are already bound to the weapon change in
 * Quake/default_cfg.h.
 */
const pad_default_t psp_pad_defaults[] =
{
	{ K_ABUTTON, "+attack", "cross"    },
	{ K_XBUTTON, "+jump",   "square"   },
	{ K_YBUTTON, "+use",    "triangle" },
};

const int psp_pad_default_count =
	(int)(sizeof(psp_pad_defaults) / sizeof(psp_pad_defaults[0]));

int psp_pad_step (pad_repeat_t *b, int isdown, double now, int repeat_ok)
{
	if (!b)
		return PAD_EVENT_NONE;

	/* ---- released -> pressed: one event, and start the repeat timer ---- */
	if (!b->down)
	{
		if (!isdown)
			return PAD_EVENT_NONE;

		b->down = 1;
		b->next = now + PAD_REPEAT_DELAY;

		return PAD_EVENT_DOWN;
	}

	/* ---- pressed -> released ---- */
	if (!isdown)
	{
		b->down = 0;
		b->next = 0.0;

		return PAD_EVENT_UP;
	}

	/* ---- still held: repeat, if this button is one that may repeat ----
	 *
	 * The timer is not "when the press happened + n * rate": it is advanced
	 * from the clock each time it fires, so a frame that takes longer than
	 * the repeat rate produces one repeat and not a burst of them. */
	if (repeat_ok && now >= b->next)
	{
		b->next = now + PAD_REPEAT_RATE;

		return PAD_EVENT_DOWN;
	}

	return PAD_EVENT_NONE;
}

float psp_pad_axis (int raw, int deadzone)
{
	float	v, a, dz;

	if (raw < 0)
		raw = 0;
	if (raw > 255)
		raw = 255;

	/* 128 is the centre; the two halves are not the same size (0..127 and
	 * 128..255), so the divisor is the larger of them and the result is
	 * clamped. */
	v = ((float)raw - 128.0f) / 127.0f;

	if (v > 1.0f)
		v = 1.0f;
	if (v < -1.0f)
		v = -1.0f;

	a = fabsf(v);

	if (deadzone < 0)
		deadzone = 0;
	if (deadzone > 127)
		deadzone = 127;

	dz = (float)deadzone / 127.0f;

	if (a <= dz)
		return 0.0f;

	/* Stretch what is left of the travel over the whole range. */
	a = (a - dz) / (1.0f - dz);

	if (a > 1.0f)
		a = 1.0f;

	return (v < 0.0f) ? -a : a;
}
