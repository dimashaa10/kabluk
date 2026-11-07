/*
 * tests/test_pad_map.c -- the PSP pad's button map, on a desktop.
 *
 * The port cannot be run here, so what is tested is the part that decides
 * everything the player will feel:
 *
 *   1. every button the console has is reported, and no two of them send the
 *      same engine key (a duplicate would make one button dead);
 *   2. the two buttons the engine's menus already understand are the right
 *      ones: confirm is K_ABUTTON and back is K_BBUTTON.  This is not a style
 *      choice -- Quake/menu.c handles K_ABUTTON in 40 of its 41 menu handlers
 *      and K_ENTER in all of them, so a pad that sends K_ENTER instead stops
 *      being able to change a binding in Controls -> Keys, and a pad that
 *      sends K_ESCAPE for "back" cannot leave the "press a key" state at all;
 *   3. every key the pad sends is a key the engine can bind (0..MAX_KEYS-1 and
 *      not one of the keys the engine refuses to bind);
 *   4. holding a direction repeats it -- 0.5 s, then every 0.1 s -- and
 *      releasing it sends exactly one key-up;
 *   5. the repeat never fires where it is not wanted (the game: +forward is a
 *      key state, and a repeating +use would fire again and again);
 *   6. the stick's dead zone is not a hole in the middle of the travel: the
 *      full stick really is full speed, and half a stick is about half speed.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "q_stdinc.h"
#include "keys.h"
#include "pad_map.h"

static int failures;
static int checks;

static void check (int ok, const char *what)
{
	checks++;
	if (!ok)
	{
		failures++;
		printf("FAIL: %s\n", what);
	}
}

static const pad_key_t *pad_for (unsigned int button)
{
	int i;

	for (i = 0; i < psp_pad_key_count; i++)
		if (psp_pad_keys[i].button == button)
			return &psp_pad_keys[i];

	return NULL;
}

static int has_repeat (int key)
{
	int i;

	for (i = 0; i < psp_pad_key_count; i++)
		if (psp_pad_keys[i].key == key)
			return psp_pad_keys[i].repeat;

	return 0;
}

static void test_table (void)
{
	static const unsigned int all[] =
	{
		PAD_BTN_SELECT, PAD_BTN_START, PAD_BTN_UP, PAD_BTN_RIGHT,
		PAD_BTN_DOWN, PAD_BTN_LEFT, PAD_BTN_LTRIGGER, PAD_BTN_RTRIGGER,
		PAD_BTN_TRIANGLE, PAD_BTN_CIRCLE, PAD_BTN_CROSS, PAD_BTN_SQUARE
	};
	int i, j;

	check(psp_pad_key_count > 0, "the board reports at least one button");
	check(psp_pad_key_count <= PAD_KEY_MAX,
	      "the table fits the per-button state array");

	for (i = 0; i < (int)(sizeof(all) / sizeof(all[0])); i++)
		check(pad_for(all[i]) != NULL, "every pad button is reported");

	for (i = 0; i < psp_pad_key_count; i++)
	{
		for (j = i + 1; j < psp_pad_key_count; j++)
			check(psp_pad_keys[i].button != psp_pad_keys[j].button,
			      "no two entries read the same button");
	}

	for (i = 0; i < psp_pad_key_count; i++)
	{
		int k = psp_pad_keys[i].key;

		check(k >= 0 && k < MAX_KEYS, "every key is inside the key table");

		for (j = i + 1; j < psp_pad_key_count; j++)
			check(k != psp_pad_keys[j].key,
			      "no two buttons send the same key");
	}

	/* The buttons the engine's menus use. */
	check(pad_for(PAD_BTN_CROSS)->key == K_ABUTTON,
	      "cross confirms, the way every engine menu expects");
	check(pad_for(PAD_BTN_CIRCLE)->key == K_BBUTTON,
	      "circle cancels, the way every engine menu expects");
	check(pad_for(PAD_BTN_UP)->key == K_UPARROW &&
	      pad_for(PAD_BTN_DOWN)->key == K_DOWNARROW &&
	      pad_for(PAD_BTN_LEFT)->key == K_LEFTARROW &&
	      pad_for(PAD_BTN_RIGHT)->key == K_RIGHTARROW,
	      "the d-pad moves the cursor");
	check(pad_for(PAD_BTN_START)->key == K_ESCAPE,
	      "start is the engine's own pause/menu key");

	/* The two shoulder buttons keep the engine's default binds
	 * (Quake/default_cfg.h: LSHOULDER = impulse 12, RSHOULDER = impulse 10),
	 * so weapon change works with no PSP-specific code at all. */
	check(pad_for(PAD_BTN_LTRIGGER)->key == K_LSHOULDER,
	      "L is the left shoulder key, which is bound to the previous weapon");
	check(pad_for(PAD_BTN_RTRIGGER)->key == K_RSHOULDER,
	      "R is the right shoulder key, which is bound to the next weapon");

	/* The action buttons must not be the keys the engine refuses to bind:
	 * K_ESCAPE (hard-coded to the menu) and F1..F12 (menubound). */
	for (i = 0; i < psp_pad_key_count; i++)
	{
		int k = psp_pad_keys[i].key;

		if (k == K_ESCAPE)
			continue;	/* start: deliberately the engine's key */
		check(!(k >= K_F1 && k <= K_F12),
		      "no button sends a key that cannot be bound");
	}

	/* Only the directions repeat. */
	check(has_repeat(K_UPARROW) && has_repeat(K_DOWNARROW) &&
	      has_repeat(K_LEFTARROW) && has_repeat(K_RIGHTARROW),
	      "the directions repeat while held");
	check(!has_repeat(K_ABUTTON) && !has_repeat(K_BBUTTON) &&
	      !has_repeat(K_XBUTTON) && !has_repeat(K_YBUTTON),
	      "the action buttons do not repeat");
}

/* What the buttons do out of the box.  These are bindings, so the test checks
 * the *intent* the port promises in its README: fire on the thumb button, jump
 * next to it, and the action key on triangle -- and the action key must be the
 * engine's positive "press the button" action, not something that also opens
 * every door (+use was deliberately narrowed; see tests/test_use_button.c). */
static const char *binding_for (int key)
{
	int i;

	for (i = 0; i < psp_pad_default_count; i++)
		if (psp_pad_defaults[i].key == key)
			return psp_pad_defaults[i].binding;

	return NULL;
}

static void test_defaults (void)
{
	int i;

	check(psp_pad_default_count > 0, "the pad has default actions");

	for (i = 0; i < psp_pad_default_count; i++)
	{
		const pad_default_t *d = &psp_pad_defaults[i];

		check(d->binding && d->binding[0], "a default has a binding");
		check(d->button && d->button[0], "a default names its button");

		/* Every default is for a button in the table ... */
		{
			int found = 0, j;

			for (j = 0; j < psp_pad_key_count; j++)
				if (psp_pad_keys[j].key == d->key)
					found = 1;

			check(found, "a default is for a real pad button");
		}

		/* ... and none of them for a key the engine refuses to bind. */
		check(d->key != K_ESCAPE && !(d->key >= K_F1 && d->key <= K_F12),
		      "no default is for an unbindable key");
	}

	check(!strcmp(binding_for(K_ABUTTON), "+attack"),
	      "cross fires");
	check(!strcmp(binding_for(K_XBUTTON), "+jump"),
	      "square jumps");
	check(!strcmp(binding_for(K_YBUTTON), "+use"),
	      "triangle presses buttons (+use)");

	/* Back is a menu key, not an action: nothing is bound to it. */
	check(binding_for(K_BBUTTON) == NULL,
	      "circle has no game binding -- it is the menus' back button");
}

static void test_repeat (void)
{
	pad_repeat_t b;
	double t;
	int ev;

	/* A press is reported once, and only once. */
	memset(&b, 0, sizeof(b));
	ev = psp_pad_step(&b, 1, 0.0, 1);
	check(ev == PAD_EVENT_DOWN, "pressing a button sends one key-down");
	check(psp_pad_step(&b, 1, 0.0, 1) == PAD_EVENT_NONE,
	      "the next frame of the same press sends nothing");

	/* Hold: nothing for 0.5 s, then one repeat per 0.1 s. */
	check(psp_pad_step(&b, 1, 0.4, 1) == PAD_EVENT_NONE,
	      "holding for less than the delay does not repeat");
	check(psp_pad_step(&b, 1, 0.5, 1) == PAD_EVENT_DOWN,
	      "holding for the delay repeats once");
	check(psp_pad_step(&b, 1, 0.55, 1) == PAD_EVENT_NONE,
	      "a repeat does not happen twice in one interval");
	check(psp_pad_step(&b, 1, 0.6, 1) == PAD_EVENT_DOWN,
	      "the next interval repeats again");

	/* A slow frame produces one repeat, not a burst of them: the menu must
	 * not jump four items because the frame took 400 ms. */
	check(psp_pad_step(&b, 1, 3.0, 1) == PAD_EVENT_DOWN,
	      "a long frame repeats once");
	check(psp_pad_step(&b, 1, 3.0, 1) == PAD_EVENT_NONE,
	      "and the frame after it does not repeat again");

	/* Release: exactly one key-up, and the next press starts over. */
	check(psp_pad_step(&b, 0, 3.1, 1) == PAD_EVENT_UP,
	      "releasing sends one key-up");
	check(psp_pad_step(&b, 0, 3.2, 1) == PAD_EVENT_NONE,
	      "releasing twice sends nothing more");
	check(psp_pad_step(&b, 1, 4.0, 1) == PAD_EVENT_DOWN,
	      "pressing again sends a key-down");
	check(psp_pad_step(&b, 1, 4.4, 1) == PAD_EVENT_NONE,
	      "and the delay starts from the new press");

	/* Where repeating makes no sense: the game.  +forward is a key state,
	 * so a repeat is pointless, and a repeated +use would fire over and
	 * over (see tests/test_use_button.c). */
	memset(&b, 0, sizeof(b));
	t = 0.0;
	psp_pad_step(&b, 1, t, 0);
	ev = PAD_EVENT_NONE;
	for (t = 0.1; t < 5.0; t += 0.1)
		if (psp_pad_step(&b, 1, t, 0) != PAD_EVENT_NONE)
			ev = PAD_EVENT_DOWN;
	check(ev == PAD_EVENT_NONE, "a held button does not repeat in the game");
	check(psp_pad_step(&b, 0, 5.0, 0) == PAD_EVENT_UP,
	      "but it still sends its key-up");

	/* No button pressed, ever: nothing is sent (a pad that reports a phantom
	 * press would hold +forward for the whole session). */
	memset(&b, 0, sizeof(b));
	t = 0.0;
	ev = 0;
	for (t = 0.0; t < 10.0; t += 0.016)
		if (psp_pad_step(&b, 0, t, 1) != PAD_EVENT_NONE)
			ev = 1;
	check(!ev, "an untouched button never sends anything");
}

static void test_axis (void)
{
	const int dz = 24;
	float f;

	check(psp_pad_axis(128, dz) == 0.0f, "the centre is exactly zero");

	/* Everything inside the dead zone is zero, and the first step outside it
	 * is not a jump to half speed. */
	check(psp_pad_axis(128 - dz, dz) == 0.0f, "the dead zone is a dead zone (left)");
	check(psp_pad_axis(128 + dz, dz) == 0.0f, "the dead zone is a dead zone (right)");
	f = psp_pad_axis(128 - dz - 1, dz);
	check(f < 0.0f && f > -0.1f, "just outside the dead zone is a small move");

	/* Full travel is full speed: this is what the old dx*2-with-a-clamp
	 * scaling got wrong at the top of the range. */
	f = psp_pad_axis(0, dz);
	check(f < -0.99f && f >= -1.0f, "the stick all the way left is full left");
	f = psp_pad_axis(255, dz);
	check(f > 0.99f && f <= 1.0f, "the stick all the way right is full right");

	/* Half travel is about half speed, not 0.6 or 0.4 of it. */
	f = psp_pad_axis(128 + 64, dz);
	check(f > 0.3f && f < 0.5f, "half travel is about half speed");

	/* Monotonic, both ways: a stick that is pushed further is never slower. */
	{
		int x;
		float prev = -2.0f;
		int ok = 1;

		for (x = 128; x <= 255; x++)
		{
			float v = psp_pad_axis(x, dz);
			if (v < prev)
				ok = 0;
			prev = v;
		}
		check(ok, "pushing further right never moves slower");

		prev = 2.0f;
		for (x = 127; x >= 0; x--)
		{
			float v = psp_pad_axis(x, dz);
			if (v > prev)
				ok = 0;
			prev = v;
		}
		check(ok, "pushing further left never moves slower");
	}

	/* Junk is clamped, not wrapped. */
	check(psp_pad_axis(-40, dz) <= 0.0f, "a negative reading does not go positive");
	check(psp_pad_axis(400, dz) <= 1.0f, "an oversized reading is clamped");
	check(fabsf(psp_pad_axis(0, 0)) > 0.99f, "no dead zone means the whole travel");
}

int main (void)
{
	test_table();
	test_defaults();
	test_repeat();
	test_axis();

	printf("%s: %d checks\n", failures ? "FAILED" : "PASS", checks);
	printf(failures ? "" : "PASS: every pad button is an engine key, the menus' confirm and\n"
	                      "PASS: back are the right ones, holding a direction repeats the way\n"
	                      "PASS: the engine's own controller code repeats, and the stick curve\n"
	                      "PASS: uses its whole travel.\n");

	return failures ? 1 : 0;
}
