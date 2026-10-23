/* Asset-free tests for the engine-side crouch that lives in the shared pmove.
 *
 * The pmove runs on both the server and the predicting client, so the tests
 * below drive PM_PlayerMove twice from the same snapshot (one "server" run and
 * one "prediction" run) and require identical results: that is the property
 * G4 is about. The only world here is an endless floor at z = 0 plus an
 * optional ceiling, implemented by the trace stubs at the top. */
#include "quakedef.h"
#include "pmove.h"
#include <assert.h>

/* ---- globals the shared pmove expects from the rest of the engine ---- */
double			host_frametime;

void Sys_Error (const char *error, ...)
{
	va_list argptr;

	(void)error;
	va_start(argptr, error);
	va_end(argptr);
	assert(!"Sys_Error called");
}

/* ---- stubbed collision world ---- */
static float	floor_z = 0.0f;
static float	ceiling_z = 100000.0f;	/* nothing above the player by default */
static int		standing_tests;
static int		trace_calls;

void PM_InitBoxHull (void) {}

int PM_ExtraBoxContents (vec3_t p)
{
	(void)p;
	return 0;
}

int PM_PointContents (vec3_t point)
{
	if (point[2] < floor_z)
		return CONTENTMASK_FROMQ1(CONTENTS_SOLID);
	return CONTENTMASK_FROMQ1(CONTENTS_EMPTY);
}

qboolean PM_TestPlayerPosition (vec3_t pos)
{
	standing_tests++;
	/* Player is free unless the requested hull would poke into the ceiling. */
	return pos[2] + pmove.player_maxs[2] <= ceiling_z + 0.001f;
}

trace_t PM_PlayerTrace (vec3_t start, vec3_t end, unsigned int solidmask)
{
	trace_t trace;
	float dz = end[2] - start[2];
	float feet_start, feet_end;

	memset(&trace, 0, sizeof(trace));
	trace.fraction = 1;
	trace.entnum = -1;
	VectorCopy(end, trace.endpos);
	trace_calls++;
	(void)solidmask;

	/* PM_PlayerTrace sweeps the player's hull, not a point, so the floor is
	 * reached when the feet (origin + mins[2]) touch it. */
	feet_start = start[2] + pmove.player_mins[2];
	feet_end = end[2] + pmove.player_mins[2];

	if (feet_start < floor_z || start[2] + pmove.player_maxs[2] > ceiling_z)
	{	/* inside the floor, or the hull pokes into the ceiling */
		trace.allsolid = trace.startsolid = true;
		trace.fraction = 0;
		VectorCopy(start, trace.endpos);
		return trace;
	}

	if (dz < 0 && feet_end < floor_z)
	{
		trace.fraction = (feet_start - floor_z) / (feet_start - feet_end);
		trace.endpos[0] = start[0] + (end[0] - start[0]) * trace.fraction;
		trace.endpos[1] = start[1] + (end[1] - start[1]) * trace.fraction;
		trace.endpos[2] = floor_z - pmove.player_mins[2];
		VectorSet(trace.plane.normal, 0, 0, 1);
	}
	return trace;
}

trace_t PM_TraceLine (vec3_t start, vec3_t end)
{
	return PM_PlayerTrace(start, end, MASK_PLAYERSOLID);
}

/* ---- helpers ---- */
#define STAND_MINS_Z	(-24.0f)
#define STAND_MAXS_Z	32.0f
#define STAND_HEIGHT	(STAND_MAXS_Z - STAND_MINS_Z)

static void ResetPmove (float feet_z)
{
	memset(&pmove, 0, sizeof(pmove));
	memset(&movevars, 0, sizeof(movevars));
	pmove.pm_type = PM_NORMAL;
	pmove.numphysent = 0;
	pmove.skipent = -1;
	pmove.onground = true;
	pmove.gravitydir[2] = -1;
	pmove.origin[0] = 0;
	pmove.origin[1] = 0;
	pmove.origin[2] = feet_z - STAND_MINS_Z;	/* feet on the floor */
	pmove.player_mins[0] = pmove.player_mins[1] = -16;
	pmove.player_mins[2] = STAND_MINS_Z;
	pmove.player_maxs[0] = pmove.player_maxs[1] = 16;
	pmove.player_maxs[2] = STAND_MAXS_Z;

	/* what the snapshot seeds: the standing hull, possibly already crouched */
	pmove.stand_mins[0] = pmove.stand_mins[1] = -16;
	pmove.stand_mins[2] = STAND_MINS_Z;
	pmove.stand_maxs[0] = pmove.stand_maxs[1] = 16;
	pmove.stand_maxs[2] = STAND_MAXS_Z;
	pmove.stand_viewheight = DEFAULT_VIEWHEIGHT;
	pmove.crouched = false;
	pmove.crouch_amount = 0;
	pmove.viewheight = DEFAULT_VIEWHEIGHT;

	movevars.maxspeed = 320;
	movevars.stopspeed = 100;
	movevars.accelerate = 10;
	movevars.airaccelerate = 0.7f;
	movevars.friction = 4;
	movevars.edgefriction = 2;
	movevars.gravity = 800;
	movevars.stepheight = 18;
	movevars.jumpspeed = 270;
	movevars.bunnyspeedcap = 1;
	movevars.slidefix = 1;
	movevars.entgravity = 1;
	movevars.flags = MOVEFLAG_VALID;
	host_frametime = 0.1;

	floor_z = feet_z;
	ceiling_z = 100000.0f;
	standing_tests = trace_calls = 0;
}

static float HullHeight (void)
{
	return pmove.player_maxs[2] - pmove.player_mins[2];
}

/* ---- tests ---- */

/* G4: the hull snaps, the eye eases, and both sides see the same numbers. */
static void StepMove (float upmove)
{
	pmove.cmd.upmove = upmove;
	pmove.cmd.seconds = 0.1f;
	PM_PlayerMove(1.0f);
}

static void TestCrouchHullAndEye (void)
{
	int i;

	ResetPmove(0);
	for (i = 0; i < 4; i++)
		StepMove(-200);

	assert(pmove.crouched);
	assert(HullHeight() == STAND_HEIGHT * PM_CROUCH_HEIGHT_RATIO);
	assert(pmove.player_mins[2] == STAND_MINS_Z);	/* feet stay put */
	assert(pmove.player_maxs[2] == STAND_MINS_Z + 28);
	assert(pmove.crouch_amount == 1.0f);
	assert(fabsf(pmove.viewheight - pmove.player_maxs[2]) < 0.01f);
	assert(pmove.viewheight < DEFAULT_VIEWHEIGHT);

	/* standing back up in the open restores the exact hull and eye */
	for (i = 0; i < 4; i++)
		StepMove(0);
	assert(!pmove.crouched && !pmove.crouch_blocked);
	assert(HullHeight() == STAND_HEIGHT);
	assert(pmove.player_mins[2] == STAND_MINS_Z && pmove.player_maxs[2] == STAND_MAXS_Z);
	assert(pmove.crouch_amount == 0);
	assert(fabsf(pmove.viewheight - DEFAULT_VIEWHEIGHT) < 0.01f);
}

/* The eye has to ease between the two heights instead of teleporting. */
static void TestEyeTransitionIsSmooth (void)
{
	float first, second;

	ResetPmove(0);
	StepMove(-200);
	assert(pmove.crouched);
	assert(HullHeight() == STAND_HEIGHT * PM_CROUCH_HEIGHT_RATIO);	/* hull snaps */
	first = pmove.viewheight;
	assert(first < DEFAULT_VIEWHEIGHT);
	assert(first > pmove.player_maxs[2]);	/* still on its way down */

	StepMove(-200);
	second = pmove.viewheight;
	assert(second < first);
	assert(second > pmove.player_maxs[2] - 1);

	StepMove(-200);
	assert(fabsf(pmove.viewheight - pmove.player_maxs[2]) < 0.01f);
}

/* Crouching under a low ceiling must keep the crouch until there is room. */
static void TestUncrouchBlockedByCeiling (void)
{
	ResetPmove(0);
	StepMove(-200);
	StepMove(-200);
	assert(pmove.crouched);

	ceiling_z = 100000.0f;
	StepMove(0);
	assert(!pmove.crouched);

	ceiling_z = 20;	/* the crouched top (4) fits, the standing top (32) does not */
	StepMove(-200);
	assert(pmove.crouched);
	StepMove(0);
	assert(pmove.crouched && pmove.crouch_blocked);
	assert(pmove.player_maxs[2] == STAND_MINS_Z + 28);

	/* once the ceiling is gone, standing recovers on its own */
	ceiling_z = 100000.0f;
	StepMove(0);
	assert(!pmove.crouched && !pmove.crouch_blocked);
	assert(pmove.player_maxs[2] == STAND_MAXS_Z);
}

/* The seed used by the predicting client must rebuild the same hull. */
static void TestSeedFromSnapshot (void)
{
	ResetPmove(0);
	pmove.crouched = true;
	pmove.crouch_amount = 1.0f;
	PM_SeedCrouch();
	assert(pmove.player_maxs[2] == STAND_MINS_Z + 28);
	assert(HullHeight() == STAND_HEIGHT * PM_CROUCH_HEIGHT_RATIO);
	assert(fabsf(pmove.viewheight - pmove.player_maxs[2]) < 0.01f);

	/* a standing snapshot leaves the hull alone */
	pmove.crouched = false;
	pmove.crouch_amount = 0;
	PM_SeedCrouch();
	assert(pmove.player_maxs[2] == STAND_MAXS_Z);
	assert(HullHeight() == STAND_HEIGHT);
	assert(fabsf(pmove.viewheight - DEFAULT_VIEWHEIGHT) < 0.01f);
}

/* G3: the crouch cap limits the *wish* speed on the ground, so a crouched
 * player accelerates up to the crouch speed and no further. It never brakes
 * momentum the player already has (see the crouch-jump test below). */
static float RunForward (float upmove, int frames)
{
	int i;
	float speed;

	for (i = 0; i < frames; i++)
	{
		pmove.cmd.forwardmove = 400;
		pmove.cmd.sidemove = 0;
		StepMove(upmove);
	}
	speed = sqrtf(pmove.velocity[0] * pmove.velocity[0] +
		pmove.velocity[1] * pmove.velocity[1]);
	return speed;
}

static void TestCrouchSpeedCapInMove (void)
{
	float speed;

	ResetPmove(0);
	speed = RunForward(-200, 8);
	assert(pmove.crouched);
	assert(pmove.onground);
	assert(speed > PM_CROUCH_MAX_SPEED - 10);
	assert(speed <= PM_CROUCH_MAX_SPEED + 0.01f);

	/* standing back up lets the player accelerate to the full maxspeed again */
	speed = RunForward(0, 8);
	assert(!pmove.crouched);
	assert(speed > PM_CROUCH_MAX_SPEED);
	assert(speed <= movevars.maxspeed + 0.01f);

	/* a crouching player who already moves faster is not braked: the cap is a
	 * wish-speed limit, exactly like maxspeed itself */
	ResetPmove(0);
	pmove.velocity[0] = 400;
	StepMove(-200);
	assert(pmove.crouched && pmove.velocity[0] > PM_CROUCH_MAX_SPEED);
}

/* A crouch-jump must keep the horizontal speed it took off with: the cap only
 * applies on the ground, so holding crouch in the air is not a brake. */
static void TestCrouchJumpKeepsMomentum (void)
{
	float x;

	ResetPmove(0);
	floor_z = -1000;	/* airborne, well away from the floor */
	pmove.origin[2] = 120;
	pmove.velocity[0] = 400;
	pmove.velocity[1] = 0;
	pmove.velocity[2] = 100;
	pmove.onground = false;
	pmove.cmd.upmove = -200;
	pmove.cmd.forwardmove = 0;
	pmove.cmd.sidemove = 0;
	pmove.cmd.seconds = 0.1f;
	x = pmove.velocity[0];
	PM_PlayerMove(1.0f);

	assert(pmove.crouched);
	assert(!pmove.onground);
	assert(pmove.velocity[0] == x);
	assert(pmove.velocity[2] < 100);	/* gravity still works */
}

/* G4: replaying the same commands from the same snapshot must land in exactly
 * the same place - that is what makes client prediction agree with the server. */
static void TestPredictionMatchesServer (void)
{
	playermove_t snapshot, server, client;
	usercmd_t cmds[8];
	int i;

	ResetPmove(0);
	for (i = 0; i < 8; i++)
	{
		memset(&cmds[i], 0, sizeof(cmds[i]));
		cmds[i].forwardmove = (i < 4) ? 400 : -200;
		cmds[i].sidemove = (i & 1) ? 200 : -200;
		cmds[i].upmove = (i < 5) ? -200 : 0;
		cmds[i].seconds = 0.1f;
	}

	server = pmove;
	for (i = 0; i < 8; i++)
	{
		pmove.cmd = cmds[i];
		PM_PlayerMove(1.0f);
	}
	server = pmove;

	/* the client restarts from the same snapshot and replays the same commands */
	ResetPmove(0);
	snapshot = pmove;
	pmove = snapshot;
	for (i = 0; i < 8; i++)
	{
		pmove.cmd = cmds[i];
		PM_PlayerMove(1.0f);
	}
	client = pmove;

	assert(memcmp(server.origin, client.origin, sizeof(vec3_t)) == 0);
	assert(memcmp(server.velocity, client.velocity, sizeof(vec3_t)) == 0);
	assert(server.crouched == client.crouched);
	assert(server.crouch_amount == client.crouch_amount);
	assert(server.viewheight == client.viewheight);
	assert(server.player_maxs[2] == client.player_maxs[2]);
}

/* A crouched snapshot replays identically too (that is the case the old
 * prediction could not express at all, because the hull was not networked). */
static void TestCrouchedSnapshotReplay (void)
{
	playermove_t a, b;
	usercmd_t cmd;
	int i;

	ResetPmove(0);
	pmove.stand_viewheight = DEFAULT_VIEWHEIGHT;
	pmove.crouched = true;
	pmove.crouch_amount = 1.0f;
	PM_SeedCrouch();
	a = pmove;

	memset(&cmd, 0, sizeof(cmd));
	cmd.forwardmove = 200;
	cmd.upmove = 0;
	cmd.seconds = 0.1f;
	for (i = 0; i < 2; i++)
	{
		pmove.cmd = cmd;
		PM_PlayerMove(1.0f);
	}
	a = pmove;

	ResetPmove(0);
	pmove.crouched = true;
	pmove.crouch_amount = 1.0f;
	PM_SeedCrouch();
	for (i = 0; i < 2; i++)
	{
		pmove.cmd = cmd;
		PM_PlayerMove(1.0f);
	}
	b = pmove;

	assert(memcmp(a.origin, b.origin, sizeof(vec3_t)) == 0);
	assert(memcmp(a.velocity, b.velocity, sizeof(vec3_t)) == 0);
	assert(a.crouched == b.crouched);
	assert(a.player_maxs[2] == b.player_maxs[2]);
}

/* G5: a shot traced from a hard-coded standing eye must leave from the real
 * crouched eye, aimed exactly where the player aims. */
static void TestCrouchedAimRekeysTheShot (void)
{
	vec3_t origin = {0, 0, 24};
	vec3_t start, end, dir0, dir1;
	float len0, len1;

	/* prog traces from origin + 22 (standing eye) slightly downward */
	VectorSet(start, 10, 0, origin[2] + DEFAULT_VIEWHEIGHT);
	VectorSet(end, 410, 0, origin[2] + DEFAULT_VIEWHEIGHT - 40);
	VectorCopy(start, dir0);
	VectorSubtract(end, start, dir1);
	len1 = VectorNormalize(dir1);
	VectorSubtract(end, start, dir0);
	len0 = VectorNormalize(dir0);

	assert(PM_CrouchedAim(origin, 4, start, end));

	/* the trace now starts at the crouched eye, x/y untouched ... */
	assert(start[0] == 10 && start[1] == 0);
	assert(start[2] == origin[2] + 4);

	/* ... keeps the aim direction and the range ... */
	VectorSubtract(end, start, dir0);
	len0 = VectorNormalize(dir0);
	assert(fabsf(len0 - len1) < 0.01f);
	assert(fabsf(dir0[0] - dir1[0]) < 0.001f);
	assert(fabsf(dir0[2] - dir1[2]) < 0.001f);
}

static void TestCrouchedAimLeavesCorrectTracesAlone (void)
{
	vec3_t origin = {0, 0, 24};
	vec3_t start, end;

	/* a prog that already traces from view_ofs */
	VectorSet(start, 0, 0, origin[2] + 4);
	VectorSet(end, 100, 0, origin[2] + 4);
	assert(!PM_CrouchedAim(origin, 4, start, end));
	assert(start[2] == origin[2] + 4 && end[2] == origin[2] + 4);

	/* a floor probe well below the eye */
	VectorSet(start, 0, 0, origin[2] - 20);
	VectorSet(end, 0, 0, origin[2] - 40);
	assert(!PM_CrouchedAim(origin, 4, start, end));
	assert(start[2] == origin[2] - 20 && end[2] == origin[2] - 40);

	/* standing player: the hard-coded eye is already correct */
	VectorSet(start, 0, 0, origin[2] + DEFAULT_VIEWHEIGHT);
	VectorSet(end, 100, 0, origin[2] + DEFAULT_VIEWHEIGHT);
	assert(!PM_CrouchedAim(origin, DEFAULT_VIEWHEIGHT, start, end));
	assert(start[2] == origin[2] + DEFAULT_VIEWHEIGHT);
}

int main (void)
{
	TestCrouchHullAndEye();
	TestEyeTransitionIsSmooth();
	TestUncrouchBlockedByCeiling();
	TestSeedFromSnapshot();
	TestCrouchSpeedCapInMove();
	TestCrouchJumpKeepsMomentum();
	TestPredictionMatchesServer();
	TestCrouchedSnapshotReplay();
	TestCrouchedAimRekeysTheShot();
	TestCrouchedAimLeavesCorrectTracesAlone();

	puts("PASS: shared pmove crouch halves the hull, keeps the feet planted and eases the eye.");
	puts("PASS: uncrouching waits for head room and recovers once the ceiling is gone.");
	puts("PASS: the snapshot crouch flag seeds the same hull the server uses.");
	puts("PASS: crouch caps ground speed only; crouch-jumps keep their momentum.");
	puts("PASS: replaying commands from one snapshot is bit-identical for server and client.");
	puts("PASS: crouched shots leave from the real eye along the prog's aim, other traces untouched.");
	return 0;
}
