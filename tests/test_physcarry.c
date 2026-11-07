/* Asset-free tests for picking a physdynamic prop up with the action key.
 *
 * These drive the real SV_UsePress/SV_Carry* code from sv_user.c against a tiny
 * world, and they exist because every step of the carry loop has a way to look
 * right in the source and be wrong in the game:
 *
 *   - the prop must actually leave the physics chain (SOLID_NOT, MOVETYPE_FOLLOW
 *     as the marker) or it fights pmove and SV_Impact every frame;
 *   - the hold point must be eye + forward * SV_CARRY_DIST and must be clamped
 *     by a hull trace, or the prop is dragged through walls;
 *   - the saved movetype must come back on release, or a TOSS prop becomes a
 *     bouncer (or stays glued) after one pickup;
 *   - the press must release before it grabs, or one press drops and re-grabs;
 *   - an entity without the tag must be untouched, or +use stops pressing
 *     buttons on every map that has crates.
 *
 * The QC fields are accessed through GetEdictFieldValue() with offsets cached in
 * qcvm->extfields, so this test owns a small field table and points the three
 * offsets at it.  Edict numbers go through EDICT_NUM/NUM_FOR_EDICT, stubbed over
 * a plain array.
 */
#include "quakedef.h"
#include <assert.h>
#include <math.h>

/* ---- engine state the shared code touches ---- */
qcvm_t		test_qcvm;
qcvm_t		*qcvm = &test_qcvm;
globalvars_t	test_globals;
globalvars_t	*pr_global_struct;
double		host_frametime;

#define MAX_TEST_EDICTS 8
#define FIELD_SLOTS	8
#define OFS_PHYSDYNAMIC	0
#define OFS_PHYSCARRY	1
#define OFS_PHYSMOVETYPE 2
#define OFS_PHYSSOLID	3

static edict_t	edicts[MAX_TEST_EDICTS];
static eval_t	fields[MAX_TEST_EDICTS][FIELD_SLOTS];

/* ---- entity numbering, as the server does it ---- */
edict_t *EDICT_NUM (int n)
{
	assert(n >= 0 && n < MAX_TEST_EDICTS);
	return &edicts[n];
}

int NUM_FOR_EDICT (edict_t *e)
{
	int n = (int)(e - edicts);

	assert(n >= 0 && n < MAX_TEST_EDICTS);
	return n;
}

/* ---- the QC entity fields ---- */
eval_t *GetEdictFieldValue (edict_t *ed, int ofs)
{
	int n = NUM_FOR_EDICT (ed);

	if (ofs < 0 || ofs >= FIELD_SLOTS)
		return NULL;	/* -1 is "the mod does not declare this field" */
	return &fields[n][ofs];
}

static float Field (edict_t *e, int ofs)
{
	return fields[NUM_FOR_EDICT (e)][ofs]._float;
}

static void SetField (edict_t *e, int ofs, float value)
{
	fields[NUM_FOR_EDICT (e)][ofs]._float = value;
}

/* ---- progs, stubbed: only reached if a press falls through to a button ---- */
static int	executed;

const char *PR_GetString (int num)
{
	(void)num;
	return "";
}

void PR_ExecuteProgram (func_t fnum)
{
	if (fnum)
		executed++;
}

ddef_t *ED_FindGlobal (const char *name)
{
	(void)name;
	return NULL;
}

/* ---- stubbed collision world ---- */
static int		traces;
static int		hull_traces;
static int		point_traces;
static int		last_type;
static edict_t	*last_passedict;
static float	hull_block;		/* 0 = the hold trace is clear, else fraction */
static edict_t	*aim_ent;		/* what the crosshair ray hits */
static float	los_block;		/* 0 = line of sight is clear */

void SV_LinkEdict (edict_t *ent, qboolean touch_triggers)
{
	int i;

	(void)touch_triggers;
	for (i = 0; i < 3; i++)
	{
		ent->v.absmin[i] = ent->v.origin[i] + ent->v.mins[i];
		ent->v.absmax[i] = ent->v.origin[i] + ent->v.maxs[i];
	}
}

trace_t SV_Move (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end,
	int type, edict_t *passedict)
{
	trace_t	trace;
	vec3_t	delta;
	float	fraction = 1;

	memset (&trace, 0, sizeof(trace));
	trace.fraction = 1;
	VectorCopy (end, trace.endpos);
	trace.plane.normal[2] = 1;
	traces++;
	last_type = type;
	last_passedict = passedict;

	if (maxs[2] > 0 || mins[2] < 0)
	{
		/* a hull trace: the hold point, or SV_TestEntityPosition */
		hull_traces++;
		if (hull_block > 0)
			fraction = hull_block;
	}
	else
	{
		point_traces++;
		if (type == MOVE_NORMAL)
			trace.ent = aim_ent;
		else if (los_block > 0)
			fraction = los_block;
	}

	if (fraction < 1)
	{
		trace.fraction = fraction;
		VectorSubtract (end, start, delta);
		VectorScale (delta, fraction, delta);
		VectorAdd (start, delta, trace.endpos);
		trace.plane.normal[0] = -1;
		trace.plane.normal[2] = 0;
	}
	return trace;
}

/* ---- fixtures ---- */
static edict_t	*player;
static edict_t	*prop;

static void ResetWorld (void)
{
	int i;

	memset (edicts, 0, sizeof(edicts));
	memset (fields, 0, sizeof(fields));
	memset (&test_qcvm, 0, sizeof(test_qcvm));
	memset (&test_globals, 0, sizeof(test_globals));

	test_qcvm.edicts = edicts;
	test_qcvm.edict_size = sizeof(edict_t);
	test_qcvm.num_edicts = MAX_TEST_EDICTS;
	test_qcvm.extfields.physdynamic = OFS_PHYSDYNAMIC;
	test_qcvm.extfields.physcarry = OFS_PHYSCARRY;
	test_qcvm.extfields.physmovetype = OFS_PHYSMOVETYPE;
	test_qcvm.extfields.physsolid = OFS_PHYSSOLID;
	pr_global_struct = &test_globals;

	for (i = 0; i < MAX_TEST_EDICTS; i++)
		edicts[i].free = true;

	/* edict 0 is the world */
	edicts[0].free = false;
	edicts[0].v.solid = SOLID_BSP;
	edicts[0].v.movetype = MOVETYPE_PUSH;

	/* the player stands at the origin, looking down +x, eyes at +22 */
	player = &edicts[1];
	player->free = false;
	player->v.health = 100;
	player->v.movetype = MOVETYPE_WALK;
	player->v.solid = SOLID_SLIDEBOX;
	player->v.mins[0] = player->v.mins[1] = -16;
	player->v.mins[2] = -24;
	player->v.maxs[0] = player->v.maxs[1] = 16;
	player->v.maxs[2] = 32;
	player->v.view_ofs[2] = 22;
	player->v.origin[0] = player->v.origin[1] = player->v.origin[2] = 0;
	player->v.v_angle[0] = player->v.v_angle[1] = player->v.v_angle[2] = 0;

	/* a tagged physics prop 40 units in front, sitting on the floor */
	prop = &edicts[2];
	prop->free = false;
	prop->v.mins[0] = prop->v.mins[1] = prop->v.mins[2] = -16;
	prop->v.maxs[0] = prop->v.maxs[1] = prop->v.maxs[2] = 16;
	prop->v.origin[0] = 40;
	prop->v.origin[1] = prop->v.origin[2] = 16;
	prop->v.solid = SOLID_BBOX;
	prop->v.movetype = MOVETYPE_BOUNCE;
	prop->v.modelindex = 1;
	prop->v.size[0] = prop->v.size[1] = prop->v.size[2] = 32;
	SetField (prop, OFS_PHYSDYNAMIC, 1);
	SV_LinkEdict (prop, false);

	traces = hull_traces = point_traces = 0;
	last_type = -1;
	last_passedict = NULL;
	hull_block = 0;
	aim_ent = NULL;
	los_block = 0;
	executed = 0;
	host_frametime = 0.1;
}

/* ---- 1. a press picks the prop up and takes it out of the physics chain ---- */
static void TestGrab (void)
{
	static const vec3_t hold = { SV_CARRY_DIST, 0, 22 };

	ResetWorld ();
	assert (SV_UsePress (player));
	assert (!executed);					/* no button was pressed */
	assert (Field (player, OFS_PHYSCARRY) == 2);
	assert (Field (prop, OFS_PHYSMOVETYPE) == MOVETYPE_BOUNCE);
	assert (prop->v.movetype == MOVETYPE_EXT_FOLLOW);
	assert (prop->v.solid == SOLID_NOT);
	assert (prop->v.owner == EDICT_TO_PROG (player));
	assert (prop->v.aiment == EDICT_TO_PROG (player));
	assert (!prop->v.velocity[0] && !prop->v.velocity[1] && !prop->v.velocity[2]);
	assert (!prop->v.avelocity[2]);
	/* held in front of the eyes, not at the prop's old place */
	assert (fabs (prop->v.origin[0] - hold[0]) < 0.01f);
	assert (fabs (prop->v.origin[2] - hold[2]) < 0.01f);
	/* the hold trace is the prop's own hull, and the prop is the one ignored */
	assert (last_type == MOVE_NOMONSTERS);
	assert (last_passedict == prop);
}

/* ---- 2. the hold point follows the view and stops at walls ---- */
static void TestHoldPoint (void)
{
	ResetWorld ();
	assert (SV_UsePress (player));

	/* look straight up: the prop goes above the eyes, still SV_CARRY_DIST away */
	player->v.v_angle[PITCH] = -90;
	SV_CarryUpdate (prop, player);
	assert (fabs (prop->v.origin[2] - (22 + SV_CARRY_DIST)) < 0.01f);
	assert (fabs (prop->v.origin[0]) < 0.01f);

	/* a wall halfway there clamps the prop to the trace result */
	ResetWorld ();
	assert (SV_UsePress (player));
	hull_block = 0.5f;
	SV_CarryUpdate (prop, player);
	assert (hull_traces > 0);
	assert (fabs (prop->v.origin[0] - SV_CARRY_DIST * 0.5f) < 0.01f);
	assert (fabs (prop->v.origin[2] - 22) < 0.01f);
}

/* ---- 3. releasing restores what the prop was, and keeps momentum ---- */
static void TestRelease (void)
{
	ResetWorld ();
	assert (SV_UsePress (player));
	assert (Field (player, OFS_PHYSCARRY) == 2);

	player->v.velocity[0] = 200;
	assert (SV_UsePress (player));			/* same key puts it down */
	assert (Field (player, OFS_PHYSCARRY) == 0);
	assert (Field (prop, OFS_PHYSMOVETYPE) == 0);
	assert (prop->v.movetype == MOVETYPE_BOUNCE);	/* what it was before */
	assert (prop->v.solid == SOLID_BBOX);
	assert (!prop->v.owner);
	assert (!prop->v.aiment);
	assert (prop->v.velocity[0] == 200);		/* keeps the player's momentum */
	assert (!((int)prop->v.flags & FL_ONGROUND));
	assert (!executed);
}

/* ---- 4. a TOSS prop stays a TOSS prop; a glued one becomes a bouncer ---- */
static void TestReleaseRestoresRealMovetype (void)
{
	ResetWorld ();
	prop->v.movetype = MOVETYPE_TOSS;
	assert (SV_UsePress (player));
	assert (Field (prop, OFS_PHYSMOVETYPE) == MOVETYPE_TOSS);
	assert (prop->v.movetype == MOVETYPE_EXT_FOLLOW);
	assert (SV_UsePress (player));
	assert (prop->v.movetype == MOVETYPE_TOSS);

	/* a prop that was MOVETYPE_NONE has no physics to give back */
	ResetWorld ();
	prop->v.movetype = MOVETYPE_NONE;
	assert (!SV_UsePress (player));			/* not carriable at all */
	assert (prop->v.movetype == MOVETYPE_NONE);
}

/* ---- 5. the tag is the whole permission ---- */
static void TestOnlyTaggedPropsAreGrabbed (void)
{
	ResetWorld ();
	SetField (prop, OFS_PHYSDYNAMIC, 0);
	assert (!SV_UsePress (player));
	assert (prop->v.movetype == MOVETYPE_BOUNCE);
	assert (prop->v.solid == SOLID_BBOX);
	assert (!Field (player, OFS_PHYSCARRY));

	/* a LIVE monster is never carried, even with the tag. Health matters:
	 * with health at zero the same flags mean "the head gib that ThrowHead()
	 * made out of a dead monster", and test 12 covers that. */
	ResetWorld ();
	prop->v.flags = FL_MONSTER;
	prop->v.health = 100;
	assert (!SV_UsePress (player));

	/* nor is a brush, a model without a box, or someone's missile. A bare
	 * trigger is NOT in this list any more: gibs are triggers, and test 11
	 * covers them. An untagged trigger is still refused, by the tag. */
	ResetWorld ();
	prop->v.solid = SOLID_BSP;
	assert (!SV_UsePress (player));
	ResetWorld ();
	prop->v.solid = SOLID_TRIGGER;
	SetField (prop, OFS_PHYSDYNAMIC, 0);
	assert (!SV_UsePress (player));
	ResetWorld ();
	prop->v.owner = EDICT_TO_PROG (player);
	assert (!SV_UsePress (player));
	ResetWorld ();
	prop->v.modelindex = 0;
	assert (!SV_UsePress (player));
}

/* ---- 6. what is out of sight is out of reach ---- */
static void TestReachAndVisibility (void)
{
	/* too far away: the scan must not teleport it into the hands */
	ResetWorld ();
	prop->v.origin[0] = SV_USE_RANGE + 50;
	SV_LinkEdict (prop, false);
	assert (!SV_UsePress (player));

	/* behind the player: never grabbed backwards */
	ResetWorld ();
	prop->v.origin[0] = -20;
	SV_LinkEdict (prop, false);
	assert (!SV_UsePress (player));

	/* in range and in front, but a wall between: no */
	ResetWorld ();
	los_block = 0.5f;
	assert (!SV_UsePress (player));

	/* in range, in front, visible, but the crosshair misses it: the scan
	 * still takes it, which is the crate-on-the-floor case */
	ResetWorld ();
	assert (SV_UsePress (player));
	assert (Field (player, OFS_PHYSCARRY) == 2);
}

/* ---- 7. the crosshair wins over the scan ---- */
static void TestAimedPropWins (void)
{
	edict_t	*other;

	ResetWorld ();
	other = &edicts[3];
	other->free = false;
	*other = *prop;
	other->v.origin[0] = 30;
	other->v.origin[1] = 0;
	fields[3][OFS_PHYSDYNAMIC]._float = 1;
	SV_LinkEdict (other, false);

	aim_ent = other;
	assert (SV_UsePress (player));
	assert (Field (player, OFS_PHYSCARRY) == 3);
}

/* ---- 8. dead or gone hands let go ---- */
static void TestDeadPlayerDrops (void)
{
	ResetWorld ();
	assert (SV_UsePress (player));
	player->v.health = 0;
	SV_CarryUpdate (prop, player);
	assert (prop->v.movetype == MOVETYPE_BOUNCE);
	assert (prop->v.solid == SOLID_BBOX);
	assert (!Field (player, OFS_PHYSCARRY));

	/* noclip drops it too: nothing may be dragged through walls */
	ResetWorld ();
	assert (SV_UsePress (player));
	player->v.movetype = MOVETYPE_NOCLIP;
	SV_CarryUpdate (prop, player);
	assert (prop->v.movetype == MOVETYPE_BOUNCE);
}

/* ---- 9. the hands hold one thing, and SV_CarryGrab says so ---- */
static void TestOneAtATime (void)
{
	edict_t	*other;

	ResetWorld ();

	/* a second, identical prop, snapped before the first one is touched */
	other = &edicts[3];
	other->free = false;
	*other = *prop;
	fields[3][OFS_PHYSDYNAMIC]._float = 1;
	SV_LinkEdict (other, false);

	assert (SV_CarryGrab (player, prop));
	assert (!SV_CarryGrab (player, prop));		/* already holding it */
	assert (!SV_CarryGrab (player, other));		/* hands full */
	assert (other->v.movetype == MOVETYPE_BOUNCE);
	assert (SV_CarryHeld (player) == prop);
}

/* ---- 10. a stale entity number is not a held prop ---- */
static void TestStaleNumberIsIgnored (void)
{
	ResetWorld ();
	assert (SV_UsePress (player));

	/* the prop is freed underneath the player, as a map change would */
	prop->free = true;
	assert (SV_CarryHeld (player) == NULL);
	assert (!SV_CarryRelease (player));

	/* ...and a number that points at something not held is refused too */
	ResetWorld ();
	SetField (player, OFS_PHYSCARRY, 2);
	assert (SV_CarryHeld (player) == NULL);
	assert (!SV_CarryRelease (player));
}


/* ---- 11. a gib is a trigger with a zero-size box, and it is carriable ----
 *
 * id's ThrowGib/ThrowHead spawn with `solid = SOLID_TRIGGER` and
 * `setsize ('0 0 0', '0 0 0')` on purpose (`// myach`), so the carry rule has
 * to accept triggers or the objects that most look pickable are the only ones
 * that cannot be picked up.  The box is degenerate, so every distance in the
 * scan is measured against absmin/absmax that SV_LinkEdict widens by one unit.
 */
static void MakeGib (void)
{
	prop->v.solid = SOLID_TRIGGER;
	prop->v.mins[0] = prop->v.mins[1] = prop->v.mins[2] = 0;
	prop->v.maxs[0] = prop->v.maxs[1] = prop->v.maxs[2] = 0;
	prop->v.size[0] = prop->v.size[1] = prop->v.size[2] = 0;
	prop->v.avelocity[1] = 300;			/* tumbling, as a gib does */
	SV_LinkEdict (prop, false);
}

static void TestGibIsCarriable (void)
{
	ResetWorld ();
	MakeGib ();

	assert (SV_UsePress (player));
	assert (Field (player, OFS_PHYSCARRY) == 2);
	assert (prop->v.movetype == MOVETYPE_EXT_FOLLOW);
	assert (prop->v.solid == SOLID_NOT);
	/* what it was is remembered: a gib is not a box */
	assert (Field (prop, OFS_PHYSSOLID) == SOLID_TRIGGER);
	assert (Field (prop, OFS_PHYSMOVETYPE) == MOVETYPE_BOUNCE);

	/* released: back to a trigger, not to the SOLID_BBOX default */
	assert (SV_UsePress (player));
	assert (prop->v.solid == SOLID_TRIGGER);
	assert (prop->v.movetype == MOVETYPE_BOUNCE);
	assert (!Field (prop, OFS_PHYSSOLID));

	/* a zero-size box must never come back as SOLID_BBOX: that would be an
	 * invisible solid obstacle a player can trip over on the floor */
	assert (prop->v.solid != SOLID_BBOX);
}

/* ---- 12. a corpse is not a monster, but a live monster is ---- */
static void TestDeadMonstersHeadIsCarriable (void)
{
	/* ThrowHead reuses the monster's own edict: FL_MONSTER still set, health
	 * already zeroed, solid switched to a trigger. */
	ResetWorld ();
	MakeGib ();
	prop->v.flags = FL_MONSTER;
	prop->v.health = 0;
	prop->v.takedamage = DAMAGE_NO;
	assert (SV_UsePress (player));
	assert (Field (player, OFS_PHYSCARRY) == 2);

	/* the same flags on something still alive is a monster: hands off */
	ResetWorld ();
	MakeGib ();
	prop->v.flags = FL_MONSTER;
	prop->v.health = 100;
	assert (!SV_UsePress (player));
	assert (prop->v.solid == SOLID_TRIGGER);
	assert (prop->v.movetype == MOVETYPE_BOUNCE);

	/* and a box prop with FL_MONSTER but no health is a corpse too */
	ResetWorld ();
	prop->v.flags = FL_MONSTER;
	prop->v.health = 0;
	assert (SV_UsePress (player));
}

/* ---- 13. brushes and abstract entities stay on the map ---- */
static void TestOnlyTwoSolidsAreCarriable (void)
{
	static const int refused[] = { SOLID_BSP, SOLID_NOT, SOLID_SLIDEBOX, SOLID_EXT_CORPSE };
	size_t i;

	for (i = 0; i < sizeof(refused)/sizeof(refused[0]); i++)
	{
		ResetWorld ();
		prop->v.solid = refused[i];
		assert (!SV_CarryCanGrab (prop));
		assert (!SV_UsePress (player));
		assert (prop->v.solid == refused[i]);
		assert (prop->v.movetype == MOVETYPE_BOUNCE);
	}
}

/* ---- 14. a gib lying in front is found by the scan, not by the crosshair ----
 *
 * No SV_Move() clips against a trigger, so the aim trace can never report a
 * gib; the scan has to. This pins the two shape rules of that scan that the
 * gib case depends on: the reach, and "not behind the player".
 */
static void TestGibScan (void)
{
	ResetWorld ();
	MakeGib ();
	assert (!aim_ent);				/* the crosshair sees nothing */
	assert (SV_UsePress (player));	/* the scan still does */

	/* a gib further than the reach is left alone */
	ResetWorld ();
	MakeGib ();
	prop->v.origin[0] = SV_USE_RANGE + 20;
	SV_LinkEdict (prop, false);
	assert (!SV_UsePress (player));

	/* a gib behind the player is left alone */
	ResetWorld ();
	MakeGib ();
	prop->v.origin[0] = -20;
	SV_LinkEdict (prop, false);
	assert (!SV_UsePress (player));
}

/* ---- 15. a gib that expires in the hands is dropped, not crashed -------- */
static void TestGibExpiresInHands (void)
{
	ResetWorld ();
	MakeGib ();
	assert (SV_UsePress (player));

	/* SUB_Remove fired: the edict is freed while the player still holds it */
	prop->free = true;
	assert (SV_CarryHeld (player) == NULL);
	assert (!SV_CarryRelease (player));

	/* and the press that follows is an ordinary press, not a crash */
	assert (SV_UsePress (player) == false);	/* nothing left to grab */
}

int main (void)
{
	TestGrab ();
	TestHoldPoint ();
	TestRelease ();
	TestReleaseRestoresRealMovetype ();
	TestOnlyTaggedPropsAreGrabbed ();
	TestReachAndVisibility ();
	TestAimedPropWins ();
	TestDeadPlayerDrops ();
	TestOneAtATime ();
	TestStaleNumberIsIgnored ();
	TestGibIsCarriable ();
	TestDeadMonstersHeadIsCarriable ();
	TestOnlyTwoSolidsAreCarriable ();
	TestGibScan ();
	TestGibExpiresInHands ();
	printf ("physcarry: all tests passed\n");
	return 0;
}
