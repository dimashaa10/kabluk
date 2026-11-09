/*
    viewaction.h -- "Dynamic view actions"

    A view action is a short animation shown in place of the weapon model: pick
    up a medkit and the gun disappears, the medkit model plays its frames, then
    the gun comes back. The name is the player-facing one; internally it is one
    idea:

        the server decides WHEN and WHICH MODEL, the client decides nothing.

    Why it is built this way, and not by the mod swapping .weaponmodel itself:

      * .weaponmodel/.weaponframe are the weapon. Reusing them for the medkit
        means that, for the duration of the animation, the player's "weapon" is
        a medkit -- the HUD weapon icon, the ammo display, and any QC code that
        reads self.weaponmodel (for example to decide whether the player may
        fire) all see the medkit. Every mod that does this ends up blocking
        firing and restoring the weapon by hand, and every one of them has a
        path where the restore is missed (death, level change, weapon switch).
      * Here the weapon stays the weapon. The action is a separate pair of
        stats, and the client substitutes the model only where it draws the
        view model. Nothing is restored afterwards, because nothing was taken:
        when the server stops sending an action, the client draws the weapon
        again by itself.

    The state lives in four entity fields on the player, written once by
    QuakeC (see Misc/qc/dynview.qc) and read by the server every frame:

        dvamodel   string  name of the precached model to show
        dvastart   float   QC `time` when the action started
        dvaframes  float   how many frames the action plays
        dvafps     float   frames per second (0 = DVA_DEFAULT_FPS)

    and reaches the client as two stats:

        STAT_VIEWACTION_MODEL  model index (0 = no action; draw the weapon)
        STAT_VIEWACTION_FRAME  frame index within that model

    The frame is derived here, on the server, from the clock -- not counted by
    a QC think. That is deliberate: a think belongs to the player edict, and
    the player edict is reset by respawn; a clock-based animation cannot be
    forgotten, cannot be double-started, and costs no timer. A mod that dies
    mid-animation leaves nothing behind either, because the next frame simply
    reports "no action".

    This header is pure arithmetic with no engine types, so the tests include it
    directly and check the timing rules without stubbing a server.
*/

#ifndef VIEWACTION_H
#define VIEWACTION_H

#define DVA_DEFAULT_FPS     10.0    /* frames per second when the mod passes 0 */
#define DVA_MAX_FPS         60.0    /* a faster rate than the server frame rate
                                       is a typo, not an animation */

/*
=================
VA_FrameAt

Which frame of a view-action animation should be shown at `now`?

Returns the frame index, or -1 when the action is not running (before it
starts, after it ends, or if it would point past the model's last frame).

`frames` is what the mod asked for and `numframes` is what the model actually
has; the smaller of the two wins, so a mod that guesses the frame count wrong
gets a shorter or clipped animation instead of a "no such frame" warning in the
console every tick. A model with no frames at all cannot be animated.
=================
*/
static inline int VA_FrameAt (double now, double start, double fps,
                              int frames, int numframes)
{
	int     frame;
	double  t;

	if (frames <= 0 || numframes <= 0)
		return -1;
	if (numframes < frames)
		frames = numframes;         /* never run past the model's own frames */

	if (!(fps > 0.0))               /* also catches NaN */
		fps = DVA_DEFAULT_FPS;
	if (fps > DVA_MAX_FPS)
		fps = DVA_MAX_FPS;

	t = now - start;
	if (!(t >= 0.0))                /* negative, or NaN: not started yet */
		return -1;

	t *= fps;

	/* The bound is tested in floating point, before the cast. Casting an
	 * infinite or absurd time to int is undefined behaviour, and a mod that
	 * leaves dvastart alone while the clock runs for an hour would hit it. */
	if (!(t < (double)frames))
		return -1;                  /* finished: back to the weapon */

	frame = (int)t;

	return frame;
}

/*
=================
VA_IsActive

The same question, as a yes/no, for callers that only need to know whether to
substitute the view model at all.
=================
*/
static inline int VA_IsActive (double now, double start, double fps,
                               int frames, int numframes)
{
	return VA_FrameAt (now, start, fps, frames, numframes) >= 0;
}

#endif /* VIEWACTION_H */
