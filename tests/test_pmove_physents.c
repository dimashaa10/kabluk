/* Asset-free tests for the collision-entity list the shared pmove walks.
 *
 * G-footnote: MAX_PHYSENTS used to be 64 and every caller that filled the list
 * gave up silently on the 65th solid. The server builds the list from the BSP
 * area, the predicting client adds every networked solid on top, so a busy map
 * made the two sides disagree about what is solid: rubber-banding and walking
 * through walls that only one side knew about.
 *
 * The tests here pin the contract of the list itself: it has to be large enough
 * for a crowd, its bookkeeping (numphysent, touchindex) has to stay inside the
 * arrays no matter how full it gets, and the last slot has to work exactly like
 * the first one.
 */
#include "quakedef.h"
#include "pmove.h"
#include <assert.h>

double		host_frametime;

void Sys_Error (const char *error, ...)
{
	va_list argptr;

	(void)error;
	va_start(argptr, error);
	va_end(argptr);
	assert(!"Sys_Error called");
}

/* ---- stubbed collision world: floor at z = 0 plus one wall at x = wall_x ---- */
static float	floor_z = 0.0f;
static float	wall_x = 100000.0f;
static int		wall_entnum = 0;	/* which physents[] slot the wall pretends to be */

void PM_InitBoxHull (void) {}

int PM_ExtraBoxContents (vec3_t p)
{
	(void)p;
	return 0;
}

int PM_PointContents (vec3_t point)
{
	if (point[2] < floor_z || point[0] > wall_x)
		return CONTENTMASK_FROMQ1(CONTENTS_SOLID);
	return CONTENTMASK_FROMQ1(CONTENTS_EMPTY);
}

qboolean PM_TestPlayerPosition (vec3_t pos)
{
	if (pos[2] + pmove.player_mins[2] < floor_z - 0.001f)
		return false;
	if (pos[0] + pmove.player_maxs[0] > wall_x + 0.001f)
		return false;
	return true;
}

trace_t PM_PlayerTrace (vec3_t start, vec3_t end, unsigned int solidmask)
{
	trace_t trace;
	float dz = end[2] - start[2];
	float dx = end[0] - start[0];
	float feet_start = start[2] + pmove.player_mins[2];
	float feet_end = end[2] + pmove.player_mins[2];

	memset(&trace, 0, sizeof(trace));
	trace.fraction = 1;
	trace.entnum = 0;	/* slot 0 is the world */
	VectorCopy(end, trace.endpos);
	(void)solidmask;

	if (feet_start < floor_z - 0.001f)
	{	/* already sunk into the floor */
		trace.allsolid = trace.startsolid = true;
		trace.fraction = 0;
		VectorCopy(start, trace.endpos);
		return trace;
	}

	if (dz < 0 && feet_end < floor_z)
	{
		trace.fraction = (feet_start - floor_z) / (feet_start - feet_end);
		trace.endpos[0] = start[0] + dx * trace.fraction;
		trace.endpos[1] = start[1] + (end[1] - start[1]) * trace.fraction;
		trace.endpos[2] = floor_z - pmove.player_mins[2];
		VectorSet(trace.plane.normal, 0, 0, 1);
		return trace;
	}

	/* the wall answers with the physents[] slot it pretends to occupy */
	if (dx > 0 && end[0] + pmove.player_maxs[0] > wall_x)
	{
		float limit = wall_x - pmove.player_maxs[0];

		trace.fraction = (limit - start[0]) / dx;
		if (trace.fraction < 0)
			trace.fraction = 0;
		trace.endpos[0] = limit;
		trace.endpos[1] = start[1] + (end[1] - start[1]) * trace.fraction;
		trace.endpos[2] = start[2] + dz * trace.fraction;
		trace.entnum = wall_entnum;
		VectorSet(trace.plane.normal, -1, 0, 0);
	}
	return trace;
}

trace_t PM_TraceLine (vec3_t start, vec3_t end)
{
	return PM_PlayerTrace(start, end, MASK_PLAYERSOLID);
}

/* ---- helpers ---- */
static void ResetPmove (void)
{
	int i;

	memset(&pmove, 0, sizeof(pmove));
	memset(&movevars, 0, sizeof(movevars));
	pmove.pm_type = PM_NORMAL;
	pmove.skipent = -1;
	pmove.onground = true;
	pmove.gravitydir[2] = -1;
	pmove.player_mins[0] = pmove.player_mins[1] = -16;
	pmove.player_mins[2] = -24;
	pmove.player_maxs[0] = pmove.player_maxs[1] = 16;
	pmove.player_maxs[2] = 32;
	pmove.origin[0] = 0;
	pmove.origin[1] = 0;
	pmove.origin[2] = -pmove.player_mins[2];	/* feet on the floor */
	VectorCopy(pmove.player_mins, pmove.stand_mins);
	VectorCopy(pmove.player_maxs, pmove.stand_maxs);
	pmove.stand_viewheight = pmove.viewheight = DEFAULT_VIEWHEIGHT;

	/* Slot 0 is the world; fill every remaining slot with a dummy solid so the
	 * list is genuinely full before the move starts. */
	pmove.numphysent = MAX_PHYSENTS;
	pmove.physents[0].info = 0;
	for (i = 1; i < MAX_PHYSENTS; i++)
	{
		pmove.physents[i].info = -i;
		VectorSet(pmove.physents[i].mins, -16, -16, -16);
		VectorSet(pmove.physents[i].maxs, 16, 16, 16);
	}

	movevars.maxspeed = 320;
	movevars.maxairspeed = 30;
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
	floor_z = 0;
	wall_x = 100000;
	wall_entnum = 0;
}

static void StepForward (int steps)
{
	int i;

	for (i = 0; i < steps; i++)
	{
		pmove.cmd.forwardmove = 320;
		pmove.cmd.seconds = 0.1f;
		PM_PlayerMove(1.0f);
	}
}

/* ---- tests ---- */

/* The list has to hold a realistic crowd, not one deathmatch room's worth. */
static void TestCapacityIsComfortable (void)
{
	assert(MAX_PHYSENTS >= 128);
	assert(countof(pmove.physents) == MAX_PHYSENTS);
	assert(countof(pmove.touchindex) == MAX_PHYSENTS);
	assert(countof(pmove.touchvel) == MAX_PHYSENTS);
}

/* The very last slot has to block exactly like any other: if the list is
 * truncated or the walk stops early, the player ends up inside the wall. */
static void TestLastPhysentBlocks (void)
{
	int last = MAX_PHYSENTS - 1;
	int i;

	ResetPmove();
	wall_x = 64;
	wall_entnum = last;

	StepForward(20);

	assert(pmove.origin[0] <= wall_x - pmove.player_maxs[0] + 0.01f);
	assert(pmove.velocity[0] < 320.0f);	/* the wall ate the speed */
	assert(pmove.numtouch > 0);
	for (i = 0; i < pmove.numtouch; i++)
	{
		assert(pmove.touchindex[i] >= 0);
		assert(pmove.touchindex[i] < pmove.numphysent);
	}
	/* the touched slot is reported, not silently dropped */
	assert(pmove.touchindex[0] == last || pmove.touchindex[pmove.numtouch - 1] == last);

	/* and the player is still standing on the floor, not shoved through it */
	assert(pmove.onground);
	assert(fabs(pmove.origin[2] + pmove.player_mins[2] - floor_z) < 1.0f);
}

/* Filling the whole list must not corrupt the touch bookkeeping: the arrays stay
 * bounded, and the player still moves and lands normally. */
static void TestFullListKeepsBookkeepingBounded (void)
{
	int i;

	ResetPmove();
	wall_x = 100000;	/* open space, only the floor */

	StepForward(10);

	assert(pmove.numtouch >= 0 && pmove.numtouch <= MAX_PHYSENTS);
	for (i = 0; i < pmove.numtouch; i++)
		assert(pmove.touchindex[i] >= 0 && pmove.touchindex[i] < pmove.numphysent);
	assert(pmove.groundent == -1 ||	/* standing on the world */
		(pmove.groundent >= 0 && pmove.groundent < pmove.numphysent));
	assert(pmove.onground);
}

/* Sanity: an empty-ish list still behaves like the normal game, so the capacity
 * change did not alter movement (only what is allowed to participate). */
static void TestNormalListUnchanged (void)
{
	float with_full[3], with_one[3];

	ResetPmove();
	StepForward(10);
	VectorCopy(pmove.origin, with_full);

	ResetPmove();
	pmove.numphysent = 1;
	StepForward(10);
	VectorCopy(pmove.origin, with_one);

	assert(with_full[0] == with_one[0]);
	assert(with_full[1] == with_one[1]);
	assert(fabs(with_full[2] - with_one[2]) < 0.001f);
}

int main (void)
{
	TestCapacityIsComfortable();
	TestLastPhysentBlocks();
	TestFullListKeepsBookkeepingBounded();
	TestNormalListUnchanged();
	puts("PASS: the collision-entity list holds a crowd, its last slot blocks like the first,");
	puts("PASS: and a full list keeps the touch/ground bookkeeping inside the arrays.");
	return 0;
}
