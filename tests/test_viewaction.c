/* Asset-free tests for the timing of a "dynamic view action" (viewaction.h).
 *
 * The whole point of the design is that the frame comes from the clock and not
 * from a QC think, so the timing rules are the feature. They are also easy to
 * get subtly wrong in ways no gameplay test would catch:
 *
 *   - a frame that is shown one tick before the start time (the action would
 *     begin while the item is still being touched);
 *   - an action that never ends (the gun would be gone for good);
 *   - a frame past the model's own last frame (a "no such frame" warning every
 *     tick, from a mod that guessed the frame count);
 *   - fps of 0, or a negative time after a level change, dividing by zero.
 *
 * VA_FrameAt is pure arithmetic and header-only, so this file is the test.
 */
#include "../Quake/viewaction.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

/* A 20-frame action at 10 fps lasts exactly two seconds. */
#define FRAMES 20
#define FPS    10.0

static void TestNotStartedYet (void)
{
	/* Before the start time: no action. A mod may set dvastart to `time`
	 * and the server may then look at a clock that has moved backwards
	 * (qcvm->time is reset to 1.0 on a level change), so this must not
	 * produce a huge positive frame. */
	assert (VA_FrameAt (-1.0, 0.0, FPS, FRAMES, 20) == -1);
	assert (VA_FrameAt (0.0, 0.5, FPS, FRAMES, 20) == -1);
	assert (!VA_IsActive (0.0, 0.5, FPS, FRAMES, 20));
}

static void TestFrameZeroAtStart (void)
{
	/* At the exact start the first frame shows, not the second.
	 *
	 * The boundaries are checked in eighths, which are exact in binary: with
	 * fps 8 a frame lasts exactly 0.125, so 5.125 really is frame 1. Writing
	 * the same test as "5.1 - 5.0" would be testing double rounding -- that
	 * difference is 0.09999999999999964, and a frame boundary decided by
	 * the last bit of a double is not a rule anybody wants. */
	assert (VA_FrameAt (5.0, 5.0, 8.0, FRAMES, 20) == 0);
	assert (VA_FrameAt (5.0 + 0.125 * 0.5, 5.0, 8.0, FRAMES, 20) == 0);
	assert (VA_FrameAt (5.0 + 0.125, 5.0, 8.0, FRAMES, 20) == 1);
	assert (VA_IsActive (5.0, 5.0, FPS, FRAMES, 20));
}

static void TestFrameAdvancesWithTime (void)
{
	int i;

	/* Every frame shows for exactly 1/fps seconds and the sequence has no
	 * gaps and no repeats: frame N starts at start + N/fps. */
	for (i = 0; i < FRAMES; i++)
	{
		/* The middle of frame i, which no rounding can move to a
		 * neighbouring frame, plus the start of it. */
		assert (VA_FrameAt (10.0 + (i + 0.5) / FPS, 10.0, FPS, FRAMES, 20) == i);
		assert (VA_FrameAt (10.0 + (i + 0.01) / FPS, 10.0, FPS, FRAMES, 20) == i);
	}
}

static void TestEndsAfterTheLastFrame (void)
{
	/* Two seconds is exactly the end: frame 20 does not exist (0..19), so
	 * the action is over and the weapon is back. */
	assert (VA_FrameAt (10.0 + 2.0, 10.0, FPS, FRAMES, 20) == -1);
	assert (VA_FrameAt (10.0 + 2.0 + 100.0, 10.0, FPS, FRAMES, 20) == -1);
	assert (!VA_IsActive (10.0 + 2.0, 10.0, FPS, FRAMES, 20));
	/* ...and the last frame did show, just before the end. */
	assert (VA_FrameAt (10.0 + 2.0 - 0.001, 10.0, FPS, FRAMES, 20) == 19);
}

static void TestClampedToModelFrames (void)
{
	/* The mod asks for 100 frames, the model has 4: play 4 and stop, do not
	 * ask the renderer for frame 99 (it would warn every tick).  fps 8 makes
	 * a frame exactly 0.125 seconds, so these are exact comparisons. */
	assert (VA_FrameAt (0.0, 0.0, 8.0, 100, 4) == 0);
	assert (VA_FrameAt (0.375, 0.0, 8.0, 100, 4) == 3);
	assert (VA_FrameAt (0.5, 0.0, 8.0, 100, 4) == -1);

	/* And a mod that asks for fewer than the model has gets the shorter
	 * animation, not a clipped one. */
	assert (VA_FrameAt (0.125, 0.0, 8.0, 2, 40) == 1);
	assert (VA_FrameAt (0.25, 0.0, 8.0, 2, 40) == -1);
}

static void TestDegenerateInputs (void)
{
	/* No frames asked for, or a model with no frames: nothing to show. */
	assert (VA_FrameAt (1.0, 0.0, FPS, 0, 20) == -1);
	assert (VA_FrameAt (1.0, 0.0, FPS, -5, 20) == -1);
	assert (VA_FrameAt (1.0, 0.0, FPS, FRAMES, 0) == -1);
	assert (VA_FrameAt (1.0, 0.0, FPS, FRAMES, -1) == -1);

	/* fps 0 is "use the default", not a division by zero. */
	assert (VA_FrameAt (0.5, 0.0, 0.0, FRAMES, 20) == (int)(0.5 * DVA_DEFAULT_FPS));
	assert (VA_FrameAt (0.5, 0.0, -10.0, FRAMES, 20) == (int)(0.5 * DVA_DEFAULT_FPS));

	/* Absurd rates are clamped, so a typo cannot make the animation finish
	 * in the first tick and look like "nothing happened". */
	assert (VA_FrameAt (0.01, 0.0, 10000.0, 100, 100) == (int)(0.01 * DVA_MAX_FPS));

	/* NaN and infinity must not produce a frame number at all. */
	assert (VA_FrameAt (NAN, 0.0, FPS, FRAMES, 20) == -1);
	assert (VA_FrameAt (1.0, 0.0, NAN, FRAMES, 20) == (int)(1.0 * DVA_DEFAULT_FPS));
	assert (VA_FrameAt (INFINITY, 0.0, FPS, FRAMES, 20) == -1);
}

static void TestDefaultFpsIsUsable (void)
{
	/* A one-second action at the default rate plays DVA_DEFAULT_FPS frames,
	 * which has to be more than 1 for an animation to mean anything. */
	int n = (int)DVA_DEFAULT_FPS;

	assert (n >= 5);
	assert (VA_FrameAt (0.0, 0.0, 0.0, n, n) == 0);
	assert (VA_FrameAt ((n - 0.5) / DVA_DEFAULT_FPS, 0.0, 0.0, n, n) == n - 1);
	assert (VA_FrameAt ((double)n / DVA_DEFAULT_FPS, 0.0, 0.0, n, n) == -1);
}

/* ---- frame ranges in one shared model ------------------------------------
 *
 * progs/v_actions.mdl holds every action: the medkit is frames 1..11 (1-based),
 * so 0..10 for the engine, and the next action starts after it. VA_ActionFrameAt
 * is the offset, and the cases that matter are the two ends of the range.
 */
static void TestActionRange (void)
{
	/* The medkit: first frame 0, 11 frames at 8 fps (exact in binary). */
	assert (VA_ActionFrameAt (0.0, 0.0, 8.0, 11, 0, 11) == 0);
	assert (VA_ActionFrameAt (0.125, 0.0, 8.0, 11, 0, 11) == 1);
	assert (VA_ActionFrameAt (1.25, 0.0, 8.0, 11, 0, 11) == 10);   /* last */
	assert (VA_ActionFrameAt (1.375, 0.0, 8.0, 11, 0, 11) == -1);  /* done */

	/* A second action later in the same model starts at its own offset and
	 * must not run into the frames that are already over. */
	assert (VA_ActionFrameAt (0.0, 0.0, 8.0, 4, 20, 24) == 20);
	assert (VA_ActionFrameAt (0.375, 0.0, 8.0, 4, 20, 24) == 23);
	assert (VA_ActionFrameAt (0.5, 0.0, 8.0, 4, 20, 24) == -1);

	/* Asking for more frames than the model has left stops at the last frame
	 * of the model, not past it: an action starting at frame 8 of an 11-frame
	 * model has only 8, 9 and 10 to play, so it runs 3 frames and stops. */
	assert (VA_ActionFrameAt (0.25, 0.0, 8.0, 20, 8, 11) == 10);   /* last */
	assert (VA_ActionFrameAt (0.375, 0.0, 8.0, 20, 8, 11) == -1);  /* done */

	/* An action that starts past the end of the model shows nothing at all
	 * (a stale frame range from a mod against a shorter model). */
	assert (VA_ActionFrameAt (0.0, 0.0, 8.0, 5, 100, 11) == -1);

	/* A negative first frame is a mistake; frame 0 is the start, not -1. */
	assert (VA_ActionFrameAt (0.0, 0.0, 8.0, 5, -3, 11) == 0);

	/* Before the start, with an offset, still nothing. */
	assert (VA_ActionFrameAt (-1.0, 0.0, 8.0, 11, 5, 20) == -1);
}

int main (void)
{
	TestNotStartedYet ();
	TestFrameZeroAtStart ();
	TestFrameAdvancesWithTime ();
	TestEndsAfterTheLastFrame ();
	TestClampedToModelFrames ();
	TestDegenerateInputs ();
	TestDefaultFpsIsUsable ();
	TestActionRange ();
	printf ("viewaction: all tests passed\n");
	return 0;
}
