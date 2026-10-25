/* Asset-free tests for the engine action key (+use, E by default).
 *
 * The point of the feature is the *rule*, not the key: pressing use must press a
 * real button brush in front of the player, and must never touch anything else
 * that happens to carry a .use function - most importantly not a func_door,
 * because id's progs set door_use on every door, including the ones the map
 * wants opened only from a trigger or with a key.
 *
 * The tests below drive the real code from sv_user.c against a tiny world:
 * a wall at x = 64 (the trace stops there, as it does on a wall that has a
 * button set into it) plus a floor. Pressing is observed through a stubbed
 * PR_ExecuteProgram, so a test fails if the engine calls the wrong prog.
 */
#include "quakedef.h"
#include <assert.h>

/* ---- engine state the shared code touches ---- */
qcvm_t		test_qcvm;
qcvm_t		*qcvm = &test_qcvm;
globalvars_t	test_globals;
globalvars_t	*pr_global_struct;
double		host_frametime;

#define MAX_TEST_EDICTS 16
#define ACTIVATOR_SLOT	40

static edict_t	edicts[MAX_TEST_EDICTS];
static int		global_words[256];

/* classname strings, addressed by the string_t values used in the tests */
static const char *test_strings[] = {
	"", "player", "world", "func_button", "func_door", "func_door_secret",
	"func_lever", "trigger_multiple", "func_plat", "func_train", "func_wall",
	"button",
};

const char *PR_GetString (int num)
{
	assert(num >= 0 && num < (int)countof(test_strings));
	return test_strings[num];
}

static string_t String (const char *s)
{
	size_t i;

	for (i = 0; i < countof(test_strings); i++)
		if (!strcmp(test_strings[i], s))
			return (string_t)i;
	assert(!"unknown test string");
	return 0;
}

/* ---- observed prog calls ---- */
static int		executed;
static edict_t	*executed_self;
static edict_t	*executed_other;
static int		executed_activator;

void PR_ExecuteProgram (func_t fnum)
{
	if (!fnum)
		return;	/* a null function must never be called */

	executed++;
	executed_self = PROG_TO_EDICT (pr_global_struct->self);
	executed_other = PROG_TO_EDICT (pr_global_struct->other);
	executed_activator = ((int *)qcvm->globals)[ACTIVATOR_SLOT];
}

/* the progs' own `activator` global, as ED_FindGlobal would find it */
static ddef_t activator_def;

ddef_t *ED_FindGlobal (const char *name)
{
	if (!strcmp(name, "activator"))
	{
		activator_def.ofs = ACTIVATOR_SLOT;
		return &activator_def;
	}
	return NULL;
}

/* ---- stubbed collision world ---- */
static float	wall_x = 64.0f;		/* the wall the player aims at */
static edict_t	*wall_ent;			/* what the wall trace reports (NULL = world) */
static int		traces;

void SV_LinkEdict (edict_t *ent, qboolean touch_triggers)
{
	(void)ent;
	(void)touch_triggers;
}

trace_t SV_Move (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end,
	int type, edict_t *passedict)
{
	trace_t trace;
	float	dx = end[0] - start[0];

	memset(&trace, 0, sizeof(trace));
	trace.fraction = 1;
	trace.ent = edicts;	/* world */
	VectorCopy(end, trace.endpos);
	trace.plane.normal[0] = 0;
	trace.plane.normal[1] = 0;
	trace.plane.normal[2] = 1;
	traces++;
	(void)mins;
	(void)maxs;
	(void)passedict;

	if (type != MOVE_NOMONSTERS)	/* the action key must not be blocked by monsters */
		trace.ent = NULL;

	if (dx > 0 && end[0] > wall_x)
	{
		trace.fraction = (wall_x - start[0]) / dx;
		if (trace.fraction < 0)
			trace.fraction = 0;
		trace.endpos[0] = wall_x;
		trace.endpos[1] = start[1];
		trace.endpos[2] = start[2];
		trace.ent = wall_ent ? wall_ent : edicts;
		trace.plane.normal[0] = -1;
		trace.plane.normal[1] = 0;
		trace.plane.normal[2] = 0;
	}
	return trace;
}

/* ---- fixtures ---- */
static edict_t	*player;
static client_t	client;
static int		next_edict;

static void ResetWorld (void)
{
	int i;

	memset(edicts, 0, sizeof(edicts));
	memset(&test_qcvm, 0, sizeof(test_qcvm));
	memset(&test_globals, 0, sizeof(test_globals));
	memset(&client, 0, sizeof(client));
	memset(global_words, 0, sizeof(global_words));

	test_qcvm.edicts = edicts;
	test_qcvm.edict_size = sizeof(edict_t);
	test_qcvm.num_edicts = MAX_TEST_EDICTS;
	test_qcvm.globals = (float *)global_words;
	pr_global_struct = &test_globals;

	/* edict 0 is the world */
	edicts[0].v.classname = String("world");
	edicts[0].v.solid = SOLID_BSP;
	edicts[0].v.movetype = MOVETYPE_PUSH;

	/* edict 1 is the player, looking down +x from the origin, eyes at +22 */
	player = &edicts[1];
	player->v.classname = String("player");
	player->v.health = 100;
	player->v.movetype = MOVETYPE_WALK;
	player->v.solid = SOLID_SLIDEBOX;
	player->v.view_ofs[2] = 22;
	player->v.origin[0] = player->v.origin[1] = player->v.origin[2] = 0;
	player->v.v_angle[0] = player->v.v_angle[1] = player->v.v_angle[2] = 0;

	client.edict = player;
	client.spawned = true;

	wall_x = 48;		/* close enough that a 64 unit reach hits it */
	wall_ent = NULL;
	next_edict = 2;
	executed = 0;
	executed_self = executed_other = NULL;
	executed_activator = 0;
	traces = 0;

	/* the action key's rules are compile-time constants, nothing to reset */

	for (i = 0; i < MAX_TEST_EDICTS; i++)
		edicts[i].free = (i >= next_edict);
}

/* A brush entity of the given classname, sitting flush in the wall at x = wall_x,
 * a bit in front of it so that the trace stops on the brush itself unless told
 * otherwise. */
static edict_t *AddBrush (const char *classname)
{
	edict_t *e = &edicts[next_edict++];

	assert(next_edict <= MAX_TEST_EDICTS);
	e->free = false;
	e->v.classname = String(classname);
	e->v.solid = SOLID_BSP;
	e->v.movetype = MOVETYPE_PUSH;
	e->v.use = 1;			/* every brush in these tests has a use function */
	e->v.absmin[0] = wall_x - 16;
	e->v.absmin[1] = -8;
	e->v.absmin[2] = 0;
	e->v.absmax[0] = wall_x;
	e->v.absmax[1] = 8;
	e->v.absmax[2] = 32;
	return e;
}

/* ---- the classname whitelist ---- */

static void TestClassnameList (void)
{
	assert(SV_UseClassListed ("func_button", "func_button"));
	assert(SV_UseClassListed ("func_button", "FUNC_BUTTON"));	/* maps are sloppy */
	assert(SV_UseClassListed ("func_button func_lever", "func_lever"));
	assert(SV_UseClassListed ("func_button,func_lever", "func_lever"));
	assert(SV_UseClassListed ("func_button;func_lever", "func_button"));
	assert(SV_UseClassListed ("  func_button  ", "func_button"));

	/* near misses must not pass */
	assert(!SV_UseClassListed ("func_button", "func_butto"));
	assert(!SV_UseClassListed ("func_button", "func_button_2"));
	assert(!SV_UseClassListed ("func_button", "func_door"));
	assert(!SV_UseClassListed ("", "func_button"));
	assert(!SV_UseClassListed ("func_button", ""));
	assert(!SV_UseClassListed (NULL, "func_button"));
	assert(!SV_UseClassListed ("func_button", NULL));
}

/* ---- what may be pressed ---- */

/* The core requirement: a button is pressable, a door is not - even though both
 * have a .use function in id's progs (door_use opens the door). */
static void TestOnlyButtonsArePressable (void)
{
	ResetWorld();
	assert(SV_UseEntityAllowed (AddBrush("func_button")));

	ResetWorld();
	assert(!SV_UseEntityAllowed (AddBrush("func_door")));

	ResetWorld();
	assert(!SV_UseEntityAllowed (AddBrush("func_door_secret")));

	ResetWorld();
	assert(!SV_UseEntityAllowed (&edicts[0]));		/* the world is not a button */
	assert(!SV_UseEntityAllowed (NULL));
}

/* Structural requirements, one at a time. */
static void TestPressableShape (void)
{
	edict_t *e;

	ResetWorld();
	e = AddBrush("func_button");
	e->v.use = 0;						/* nothing to call */
	assert(!SV_UseEntityAllowed (e));

	ResetWorld();
	e = AddBrush("func_button");
	e->v.health = 1;					/* "must be killed instead of touched" */
	assert(!SV_UseEntityAllowed (e));

	ResetWorld();
	e = AddBrush("func_button");
	e->v.movetype = MOVETYPE_NONE;		/* a static brush is scenery */
	assert(!SV_UseEntityAllowed (e));

	ResetWorld();
	e = AddBrush("func_button");
	e->v.solid = SOLID_TRIGGER;			/* triggers are walked through, not pressed */
	assert(!SV_UseEntityAllowed (e));

	ResetWorld();
	e = AddBrush("func_button");
	e->v.classname = 0;					/* unnamed entity */
	assert(!SV_UseEntityAllowed (e));
}

/* ---- aiming ---- */

/* Standing in front of a button and looking at it: press it, with the player as
 * `other` and as the progs' `activator`, and restore the globals afterwards. */
static void TestPressButtonInFront (void)
{
	edict_t *button;

	ResetWorld();
	button = AddBrush("func_button");

	assert(SV_UsePress (player));
	assert(executed == 1);
	assert(executed_self == button);
	assert(executed_other == player);
	assert(executed_activator == EDICT_TO_PROG (player));

	/* the engine must not leak its own state into the rest of the server */
	assert(pr_global_struct->self == 0 && pr_global_struct->other == 0);
	assert(((int *)qcvm->globals)[ACTIVATOR_SLOT] == 0);
}

/* A door in the same place must be left alone: this is what stops the action key
 * from opening doors that only a map button or a key should open. */
static void TestDoorInFrontIsIgnored (void)
{
	ResetWorld();
	AddBrush("func_door");
	assert(!SV_UsePress (player));
	assert(executed == 0);

	ResetWorld();
	AddBrush("func_door_secret");
	assert(!SV_UsePress (player));
	assert(executed == 0);

	/* nor does aiming at nothing */
	ResetWorld();
	assert(!SV_UsePress (player));
	assert(executed == 0);
}

/* The reach is a fixed constant: a button beyond it is not pressed, and nothing in
 * the console can stretch it. */
static void TestRangeIsFixed (void)
{
	edict_t *button;

	assert(SV_USE_RANGE > 0.0f);

	ResetWorld();
	button = AddBrush("func_button");
	button->v.absmin[0] = 200;		/* well outside the reach */
	button->v.absmax[0] = 216;
	wall_x = 300;
	assert(!SV_UsePress (player));
	assert(executed == 0);

	/* just inside the reach the wall behind it is what the trace hits, and the
	 * button right next to that point is pressable */
	ResetWorld();
	button = AddBrush("func_button");
	wall_x = SV_USE_RANGE - 4.0f;			/* inside the reach, or the trace misses it */
	button->v.absmin[0] = wall_x - 16.0f;
	button->v.absmax[0] = wall_x;
	assert(SV_UsePress (player));
	assert(executed == 1);
	assert(executed_self == button);
}

/* The button brush has to be in front of the player. A thick brush that starts
 * behind him and reaches the aim point is not "the thing he is looking at", so
 * the switch must go by the brush center: behind -> refused, in front -> pressed. */
static void TestBrushMustBeInFront (void)
{
	edict_t *button;

	ResetWorld();
	button = AddBrush("func_button");
	button->v.absmin[0] = -64;		/* center x = -16, behind the player */
	button->v.absmax[0] = 32;
	wall_x = 32;
	assert(!SV_UsePress (player));
	assert(executed == 0);

	/* the same brush mirrored so that its center is in front: pressable */
	ResetWorld();
	button = AddBrush("func_button");
	button->v.absmin[0] = 0;
	button->v.absmax[0] = 32;
	wall_x = 32;
	assert(SV_UsePress (player));
	assert(executed == 1);
	assert(executed_self == button);
}

/* A button set into the wall usually shares its front plane with the wall, so the
 * trace can stop on the wall right next to it. Pressing the wall in the immediate
 * vicinity of a button still counts; pressing far away from it does not. */
static void TestFlushWithTheWall (void)
{
	edict_t *button;

	ResetWorld();
	button = AddBrush("func_button");

	/* the player aims at the wall 4 units above the button's top edge */
	player->v.v_angle[0] = -20;		/* pitch down is negative in Quake */
	player->v.origin[2] = 64;		/* so the trace lands above the button */
	assert(SV_UseFindTarget (player) == NULL);
	assert(!SV_UsePress (player));
	assert(executed == 0);

	/* now aim right at it: the trace stops on the wall, the button is 0 units away */
	player->v.v_angle[0] = 0;
	player->v.origin[2] = 16;
	assert(SV_UseFindTarget (player) == button);
	assert(SV_UsePress (player));
	assert(executed == 1);
}

/* The whitelist is fixed to plain func_button - not a cvar, so the console cannot
 * add doors to it. Nothing else that carries a .use function is pressable. */
static void TestBuiltinClassList (void)
{
	static const char *const rejected[] = {
		"func_door", "func_door_secret", "func_plat", "func_train",
		"func_wall", "trigger_multiple", "func_lever", "button",
	};
	int i;

	assert(!strcmp(SV_USE_CLASSES, "func_button"));
	assert(SV_UseClassListed (SV_USE_CLASSES, "func_button"));
	assert(SV_UseClassListed (SV_USE_CLASSES, "FUNC_BUTTON"));

	for (i = 0; i < (int)countof(rejected); i++)
	{
		edict_t *e;

		assert(!SV_UseClassListed (SV_USE_CLASSES, rejected[i]));

		ResetWorld();
		e = AddBrush (rejected[i]);
		assert(!SV_UseEntityAllowed (e));
		assert(!SV_UsePress (player));
		assert(executed == 0);
	}
}

/* Dead players and noclip players do not press anything. */
static void TestPlayerState (void)
{
	ResetWorld();
	AddBrush("func_button");

	player->v.health = 0;
	assert(!SV_UsePress (player));

	ResetWorld();
	AddBrush("func_button");
	player->v.movetype = MOVETYPE_NOCLIP;
	assert(!SV_UsePress (player));

	ResetWorld();
	AddBrush("func_button");
	player->v.movetype = MOVETYPE_NONE;
	assert(!SV_UsePress (player));
	assert(executed == 0);
}

/* ---- the key itself ---- */

/* Holding the key is one press, not one press per command. */
static void TestEdgeTrigger (void)
{
	int i;

	ResetWorld();
	AddBrush("func_button");
	test_qcvm.time = 10;

	assert(SV_UseInputEdge (&client, 4));	/* press */
	assert(client.use_held);
	for (i = 0; i < 10; i++)
	{
		test_qcvm.time += 0.01;
		assert(!SV_UseInputEdge (&client, 4));	/* still held: no repeats */
	}

	/* release, then press again after the cooldown */
	assert(!SV_UseInputEdge (&client, 0));
	assert(!client.use_held);

	test_qcvm.time += 0.001;
	assert(!SV_UseInputEdge (&client, 4));	/* inside the cooldown: dropped */
	assert(!SV_UseInputEdge (&client, 0));	/* release again */

	test_qcvm.time += 0.25;
	assert(SV_UseInputEdge (&client, 4));	/* after the cooldown: accepted */
	assert(!SV_UseInputEdge (&client, 4));	/* and holding it is still one press */
}

/* The bit the rest of the engine calls "use" is what must be watched: other
 * buttons (attack, jump) must not press anything. */
static void TestOtherButtonsDoNotPress (void)
{
	ResetWorld();
	test_qcvm.time = 5;

	assert(!SV_UseInputEdge (&client, 1));		/* +attack */
	assert(!SV_UseInputEdge (&client, 2));		/* +jump */
	assert(!SV_UseInputEdge (&client, 8));		/* +button4 */
	assert(!SV_UseInputEdge (&client, 0x80));	/* +button8 */
	assert(SV_UseInputEdge (&client, 4));		/* +use */
}

int main (void)
{
	TestClassnameList();
	TestOnlyButtonsArePressable();
	TestPressableShape();
	TestPressButtonInFront();
	TestDoorInFrontIsIgnored();
	TestRangeIsFixed();
	TestBrushMustBeInFront();
	TestFlushWithTheWall();
	TestBuiltinClassList();
	TestPlayerState();
	TestEdgeTrigger();
	TestOtherButtonsDoNotPress();

	puts("PASS: +use presses func_button and never a door, with the whitelist and reach");
	puts("PASS: fixed in the engine rather than tunable from the console;");
	puts("PASS: it reaches a button flush with the wall, ignores what is out of range,");
	puts("PASS: behind the player or shoot-only, and holding the key is one press.");
	return 0;
}
