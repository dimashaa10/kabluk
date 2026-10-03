/* Asset-free tests of the real server skate command, motion and board hooks. */
#include "quakedef.h"
#include "skate.h"
#include <assert.h>

extern edict_t *sv_player;
extern cvar_t sv_maxspeed, sv_accelerate, sv_friction, sv_stopspeed;
server_t sv;
server_static_t svs;
client_t *host_client;
extern int SV_FlyMove (edict_t *ent, float time, trace_t *steptrace);
qcvm_t *qcvm;
globalvars_t *pr_global_struct;
static globalvars_t test_globals;
cmd_source_t cmd_source;
double host_frametime;
static client_t client;
static edict_t pool[64];
static qmodel_t model;
static int argc_value, allocations, frees, forwarded, links, traces;
static const char *argument;
static char printed[256];
static float slope, camera_ground;
static qboolean wall_collision, wall_support, top_support, wall_continues_above;
static float wall_top_z;
static vec3_t wall_normal, wall_plane_point;

int Cmd_Argc (void) { return argc_value; }
const char *Cmd_Argv (int n) { assert(n == 1); return argument; }
void Cmd_ForwardToServer (void) { forwarded++; }
void SV_ClientPrintf (const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vsnprintf(printed, sizeof(printed), fmt, args);
	va_end(args);
}
void PR_SwitchQCVM (qcvm_t *vm) { qcvm = vm; }
int NUM_FOR_EDICT (edict_t *ent) { return (int)(ent - pool); }
edict_t *EDICT_NUM (int n) { assert(n >= 0 && n < 64); return &pool[n]; }
int PR_SetEngineString (const char *text)
{
	if (!strcmp(text, SKATE_MODEL)) return 1;
	if (!strcmp(text, SKATE_CLASSNAME)) return 2;
	if (!strcmp(text, "progs/player.mdl")) return 3;
	if (!strcmp(text, SKATE_MODEL_ROOT)) return 4;
	assert(!"unexpected string"); return 0;
}
const char *PR_GetString (int n)
{
	switch (n)
	{
	case 0: return "";
	case 1: return SKATE_MODEL;
	case 2: return SKATE_CLASSNAME;
	case 3: return "progs/player.mdl";
	case 4: return SKATE_MODEL_ROOT;
	default: assert(!"unexpected string index"); return "";
	}
}
edict_t *ED_Alloc (void)
{
	int i;
	for (i = 2; i < qcvm->num_edicts; i++)
		if (pool[i].free && (pool[i].freetime < 2 || qcvm->time - pool[i].freetime > 0.5)) break;
	assert(i < qcvm->max_edicts);
	if (i == qcvm->num_edicts) qcvm->num_edicts++;
	memset(&pool[i], 0, sizeof(pool[i]));
	allocations++;
	return &pool[i];
}
void ED_Free (edict_t *ent)
{
	assert(ent > pool + 1 && ent < pool + 64 && !ent->free);
	ent->free = true;
	ent->freetime = qcvm->time;
	frees++;
}
void SV_LinkEdict (edict_t *ent, qboolean touch)
{
	assert(ent == client.skate_board && !touch);
	links++;
}
trace_t SV_Move (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end, int type, edict_t *skip)
{
	trace_t trace = {0};
	float floor_z = camera_ground + slope * (start[0] - client.edict->v.origin[0]);
	int i;
	(void)mins; (void)maxs;
	if (type == MOVE_NORMAL)
	{
		assert(skip == client.edict);
		if (wall_collision)
		{
			float radius;
			wall_collision = false; // model one collision, followed by free tangential travel
			trace.fraction = 0.5f;
			for (i = 0; i < 3; i++)
				trace.endpos[i] = start[i] + (end[i] - start[i]) * trace.fraction;
			trace.ent = &pool[0];
			VectorCopy(wall_normal, trace.plane.normal);
			radius = -(wall_normal[0] * (wall_normal[0] > 0 ? client.edict->v.mins[0] : client.edict->v.maxs[0]) +
				wall_normal[1] * (wall_normal[1] > 0 ? client.edict->v.mins[1] : client.edict->v.maxs[1]));
			wall_plane_point[0] = trace.endpos[0] - wall_normal[0] * radius;
			wall_plane_point[1] = trace.endpos[1] - wall_normal[1] * radius;
			wall_plane_point[2] = wall_top_z;
			return trace;
		}
		trace.fraction = 1;
		VectorCopy(end, trace.endpos);
		return trace;
	}

	assert(type == MOVE_NOMONSTERS && skip == client.edict);
	if (start[2] == end[2])
	{
		float denominator = wall_normal[0] * (end[0] - start[0]) +
			wall_normal[1] * (end[1] - start[1]);
		float plane = wall_normal[0] * wall_plane_point[0] +
			wall_normal[1] * wall_plane_point[1];
		float fraction = denominator ?
			(plane - wall_normal[0] * start[0] - wall_normal[1] * start[1]) / denominator : 1;
		if (wall_support && (start[2] < wall_top_z || wall_continues_above) &&
			fraction >= 0 && fraction < 1)
		{
			trace.fraction = fraction;
			for (i = 0; i < 3; i++)
				trace.endpos[i] = start[i] + (end[i] - start[i]) * fraction;
			trace.ent = &pool[0];
			VectorCopy(wall_normal, trace.plane.normal);
		}
		else
		{
			trace.fraction = 1;
			VectorCopy(end, trace.endpos);
		}
		return trace;
	}
	if (top_support && start[2] > end[2])
	{
		float fraction = (start[2] - wall_top_z) / (start[2] - end[2]);
		if (fraction >= 0 && fraction < 1)
		{
			trace.fraction = fraction;
			trace.endpos[0] = start[0];
			trace.endpos[1] = start[1];
			trace.endpos[2] = wall_top_z;
			trace.ent = &pool[0];
			trace.plane.normal[2] = 1;
			return trace;
		}
	}

	traces++;
	trace.fraction = (start[2] - floor_z) / (start[2] - end[2]);
	VectorCopy(end, trace.endpos);
	trace.endpos[2] = floor_z;
	return trace;
}
float V_CalcRoll (vec3_t angles, vec3_t velocity)
{
	(void)angles; (void)velocity; return 0;
}
void PR_ExecuteProgram (func_t fnum)
{
	(void)fnum;
	assert(!"unexpected QuakeC call in movement test");
}
void Sys_Error (const char *error, ...)
{
	(void)error;
	abort();
}

static void Reset (void)
{
	memset(&sv, 0, sizeof(sv));
	memset(&svs, 0, sizeof(svs));
	memset(&test_globals, 0, sizeof(test_globals));
	pr_global_struct = &test_globals;
	memset(&client, 0, sizeof(client));
	memset(pool, 0, sizeof(pool));
	svs.clients = &client;
	svs.maxclients = 1;
	wall_collision = wall_support = top_support = wall_continues_above = false;
	wall_top_z = 64;
	wall_normal[0] = wall_normal[1] = wall_normal[2] = 0;
	wall_plane_point[0] = wall_plane_point[1] = wall_plane_point[2] = 0;
	pool[0].v.solid = SOLID_BSP;
	memset(&model, 0, sizeof(model));
	host_client = &client;
	qcvm = &sv.qcvm;
	qcvm->edicts = pool;
	qcvm->edict_size = sizeof(edict_t);
	qcvm->num_edicts = qcvm->reserved_edicts = 2;
	qcvm->max_edicts = 64;
	qcvm->time = 10;
	client.active = client.spawned = client.knowntoqc = true;
	client.limit_models = MAX_MODELS;
	client.edict = sv_player = &pool[1];
	client.edict->v.health = 100;
	client.edict->v.movetype = MOVETYPE_WALK;
	client.edict->v.flags = FL_ONGROUND;
	client.edict->v.origin[0] = 100;
	client.edict->v.origin[2] = 24;
	client.edict->v.mins[0] = client.edict->v.mins[1] = -16;
	client.edict->v.mins[2] = -24;
	client.edict->v.maxs[0] = client.edict->v.maxs[1] = 16;
	client.edict->v.maxs[2] = 32;
	client.edict->v.model = 3;
	client.edict->v.modelindex = 3;
	client.edict->v.currentammo = client.edict->v.ammo_shells = 25;
	model.type = mod_alias;
	model.mins[0] = -32; model.mins[1] = -8; model.mins[2] = -30;
	model.maxs[0] = 32; model.maxs[1] = 8; model.maxs[2] = -24;
	sv.models[1] = &model;
	sv.model_precache[1] = SKATE_MODEL;
	sv.skate_modelindex = 1;
	sv_maxspeed.value = 320;
	sv_accelerate.value = 10;
	sv_friction.value = 4;
	sv_stopspeed.value = 100;
	cmd_source = src_client;
	argc_value = 1;
	argument = "";
	allocations = frees = forwarded = links = traces = 0;
	slope = camera_ground = 0;
	printed[0] = 0;
}
static void Command (const char *arg)
{
	argc_value = arg ? 2 : 1;
	argument = arg;
	SV_Skate_f();
}
static void Step (double dt)
{
	host_frametime = dt;
	SV_ClientThink(); // real routing, not just a test implementation of velocity
	SV_SkateUpdate(&client);
	qcvm->time += dt;
}
static void CheckPhysical (void)
{
	assert(client.edict->v.origin[0] == 100 && client.edict->v.origin[2] == 24);
	assert(client.edict->v.mins[2] == -24 && client.edict->v.maxs[2] == 32);
	assert(client.edict->v.modelindex == 3 && client.edict->v.currentammo == 25);
	assert(client.edict->v.ammo_shells == 25);
}

static void TestToggleAndPlacement (void)
{
	edict_t *board;
	Reset();
	client.usingpmove = true;
	Command(NULL);
	assert(client.skate_active && !client.usingpmove && allocations == 1);
	board = client.skate_board;
	assert(SV_IsSkateBoard(board));
	assert(board->v.solid == SOLID_NOT && board->v.owner == EDICT_TO_PROG(client.edict));
	assert(board->v.colormap == 1 && board->v.modelindex == 1);
	assert(client.skate_lift == 7 && board->v.origin[2] == 31);
	assert(board->v.origin[2] + model.mins[2] == 1); // wheels above ground
	assert(board->v.origin[2] + model.maxs[2] == 7); // board top meets raised feet
	assert((unsigned)SV_SkateStat(&client) == (SKATE_STAT_MAGIC | 7u * 256u));
	CheckPhysical();
	Command("1");
	assert(allocations == 1); // idempotent enable
	Command(NULL);
	assert(!client.skate_active && !client.skate_board && board->free && frees == 1);
	assert(SV_SkateStat(&client) == 0);
	CheckPhysical();
	Command("0"); assert(frees == 1);

	Reset();
	sv.model_precache[1] = SKATE_MODEL_ROOT;
	Command(NULL); assert(client.skate_board->v.model == 4);
}
static void TestMotion (void)
{
	int i;
	float slow, fast;
	Reset(); Command(NULL);
	Step(0.05);
	assert(client.edict->v.velocity[0] > 20); // stronger initial push than the old 240 u/s^2
	for (i = 0; i < 150; i++) Step(0.05);
	assert(fabsf(client.edict->v.velocity[0] - SKATE_MAX_SPEED) < 0.01f);
	CheckPhysical();
	client.cmd.forwardmove = -400;
	for (i = 0; i < 30; i++) Step(0.05);
	assert(client.edict->v.velocity[0] == 0); // brake suppresses auto drive
	client.cmd.forwardmove = 0;
	client.edict->v.velocity[1] = 100;
	Step(0.05);
	assert(client.edict->v.velocity[1] > 80 && client.edict->v.velocity[1] < 90); // less lateral drift
	client.cmd.sidemove = 400;
	Step(0.05);
	assert(client.skate_yaw > 350 && client.skate_yaw < 355); // D turns right gradually
	client.cmd.sidemove = 0;
	client.edict->v.v_angle[YAW] = 180;
	Step(0.05);
	assert(fabsf(client.skate_yaw - 180) > 100); // no instantaneous pivot

	Reset(); Command(NULL);
	for (i = 0; i < 100; i++) Step(0.01);
	slow = client.edict->v.velocity[0];
	Reset(); Command(NULL);
	for (i = 0; i < 20; i++) Step(0.05);
	fast = client.edict->v.velocity[0];
	assert(fabsf(slow - fast) < 6); // acceleration/drag is time based; allow discrete-step rounding

	Reset(); Command(NULL);
	client.edict->v.flags = 0;
	client.edict->v.velocity[0] = 123;
	client.edict->v.velocity[2] = 270;
	Step(0.05);
	assert(client.edict->v.velocity[0] == 123 && client.edict->v.velocity[2] == 270);
	assert(traces == 4); // only the initial grounded update traced
	Command("0");
	client.edict->v.flags = FL_ONGROUND;
	client.edict->v.velocity[0] = client.edict->v.velocity[2] = 0;
	Step(0.05);
	assert(client.edict->v.velocity[0] == 0); // normal walking does not auto drive
}
static void TestSkateAnimations (void)
{
	int i;
	Reset(); Command(NULL);
	assert(client.edict->v.frame == SKATE_ANIM_CRUISE_FIRST); // same loop while standing still
	client.edict->v.velocity[0] = 10;
	qcvm->time += 0.01;
	SV_SkateUpdate(&client);
	assert(client.edict->v.frame == SKATE_ANIM_ACCEL_FIRST);
	for (i = 1; i <= 10; i++)
	{
		qcvm->time += SKATE_ANIM_FRAME_TIME;
		SV_SkateUpdate(&client);
		assert(client.edict->v.frame == SKATE_ANIM_ACCEL_FIRST + (i % 10));
	}

	client.edict->v.velocity[0] = SKATE_MAX_SPEED;
	qcvm->time += 0.01;
	SV_SkateUpdate(&client);
	assert(client.edict->v.frame == SKATE_ANIM_CRUISE_FIRST);
	for (i = 1; i <= 11; i++)
	{
		qcvm->time += SKATE_ANIM_FRAME_TIME;
		SV_SkateUpdate(&client);
		assert(client.edict->v.frame == SKATE_ANIM_CRUISE_FIRST + (i % 11));
	}

	client.edict->v.button0 = 1;
	SV_SkateUpdate(&client);
	assert(client.edict->v.frame == SKATE_ANIM_SHOOT_FIRST);
	for (i = 1; i <= 4; i++)
	{
		qcvm->time += SKATE_ANIM_FRAME_TIME;
		SV_SkateUpdate(&client);
		assert(client.edict->v.frame == SKATE_ANIM_SHOOT_FIRST + (i % 4));
	}
	client.edict->v.button0 = 0;
	for (i = 1; i <= 3; i++)
	{
		qcvm->time += SKATE_ANIM_FRAME_TIME;
		SV_SkateUpdate(&client);
		assert(client.edict->v.frame == SKATE_ANIM_SHOOT_FIRST + (i % 4));
	}
	qcvm->time += SKATE_ANIM_FRAME_TIME;
	SV_SkateUpdate(&client);
	assert(client.edict->v.frame == SKATE_ANIM_CRUISE_FIRST);

	client.edict->v.flags = 0;
	client.edict->v.velocity[2] = 270;
	SV_SkateUpdate(&client);
	assert(client.edict->v.frame == SKATE_ANIM_JUMP_FIRST);
	for (i = 1; i <= SKATE_ANIM_JUMP_PEAK - SKATE_ANIM_JUMP_FIRST; i++)
	{
		qcvm->time += SKATE_ANIM_FRAME_TIME;
		SV_SkateUpdate(&client);
		assert(client.edict->v.frame == SKATE_ANIM_JUMP_FIRST + i);
	}
	client.edict->v.velocity[2] = -1;
	SV_SkateUpdate(&client);
	assert(client.edict->v.frame == SKATE_ANIM_JUMP_PEAK);
	for (i = 1; i <= 2; i++)
	{
		qcvm->time += SKATE_ANIM_FRAME_TIME;
		SV_SkateUpdate(&client);
		assert(client.edict->v.frame == (i == 1 ? SKATE_ANIM_JUMP_FALL_LAST : SKATE_ANIM_JUMP_PEAK));
	}

	client.edict->v.flags = FL_ONGROUND;
	client.edict->v.velocity[2] = 0;
	SV_SkateUpdate(&client);
	assert(client.edict->v.frame == SKATE_ANIM_JUMP_LAND_FIRST);
	for (i = 1; i <= SKATE_ANIM_JUMP_LAND_LAST - SKATE_ANIM_JUMP_LAND_FIRST; i++)
	{
		qcvm->time += SKATE_ANIM_FRAME_TIME;
		SV_SkateUpdate(&client);
		assert(client.edict->v.frame == SKATE_ANIM_JUMP_LAND_FIRST + i);
	}
	qcvm->time += SKATE_ANIM_FRAME_TIME;
	SV_SkateUpdate(&client);
	assert(client.edict->v.frame == SKATE_ANIM_CRUISE_FIRST);

	client.edict->v.velocity[0] = 0;
	client.cmd.forwardmove = -400; // standing/braking keeps the ride loop
	qcvm->time += 0.01;
	SV_SkateUpdate(&client);
	assert(client.edict->v.frame == SKATE_ANIM_CRUISE_FIRST);
}
static void TestSlideCommand (void)
{
	Reset(); Command(NULL);
	SV_SkateSlideDown_f();
	assert(client.skate_slide_held);
	client.skate_sliding = true;
	client.skate_slide_surface = &pool[0];
	client.skate_slide_edge_height = 64;
	SV_SkateSlideUp_f();
	assert(!client.skate_slide_held && !client.skate_sliding && !client.skate_slide_surface &&
		client.skate_slide_edge_height == 0);
	SV_SkateSlide_f();
	assert(client.skate_slide_held && strstr(printed, "ON"));
	client.skate_sliding = true;
	client.skate_slide_surface = &pool[0];
	SV_SkateSlide_f();
	assert(!client.skate_slide_held && !client.skate_sliding && !client.skate_slide_surface && strstr(printed, "OFF"));

	Reset();
	SV_SkateSlide_f();
	assert(!client.skate_slide_held && strstr(printed, "Enable skate mode"));
	cmd_source = src_command;
	SV_SkateSlide_f();
	assert(forwarded == 1);
}
static void TestWallSlide (void)
{
	int clip;
	float old_y;
	vec3_t incoming = {SKATE_SLIDE_MIN_SPEED + 50, 0, -40};
	Reset(); Command(NULL);
	client.skate_slide_held = true;
	client.edict->v.flags = 0;
	VectorCopy(incoming, client.edict->v.velocity);
	wall_normal[0] = -1;
	wall_support = top_support = true;
	wall_collision = true;
	clip = SV_FlyMove(client.edict, 0.05f, NULL);
	assert(!(clip & 2)); // a grind shouldn't be retried as a stair-step
	assert(fabsf(client.edict->v.velocity[0]) < 0.01f);
	assert(fabsf(client.edict->v.velocity[1] +
		(SKATE_SLIDE_MIN_SPEED + 50) * SKATE_SLIDE_SPEED_SCALE) < 0.01f); // forward impact redirected along the wall
	assert(client.edict->v.velocity[2] == 0 && ((int)client.edict->v.flags & FL_ONGROUND));
	assert(client.skate_sliding && client.skate_slide_surface == &pool[0]);
	SV_SkateUpdate(&client);
	assert(client.skate_board->v.angles[PITCH] == SKATE_SLIDE_BOARD_PITCH);
	assert(fabsf(client.skate_slide_edge_height - wall_top_z) < 0.01f);
	assert(fabsf(client.edict->v.origin[2] - (wall_top_z - client.edict->v.mins[2] + SKATE_SLIDE_SURFACE_GAP)) < 0.01f);
	assert(fabsf((client.edict->v.origin[0] - wall_plane_point[0]) * wall_normal[0] +
		(client.edict->v.origin[1] - wall_plane_point[1]) * wall_normal[1] - SKATE_SLIDE_EDGE_OFFSET) < 0.01f);
	assert(client.skate_yaw > 269 && client.skate_yaw < 271); // board aligns to grind direction

	// The latch removes motion away from the brush, preserves tangent speed,
	// and stays active while the same face is still under the rider.
	SV_SkateMove(&client, client.edict, &client.cmd, 0.05);
	assert(client.edict->v.velocity[1] < -250); // grind friction stays low
	client.edict->v.velocity[0] = 120;
	old_y = client.edict->v.origin[1];
	clip = SV_FlyMove(client.edict, 0.05f, NULL);
	assert(client.skate_sliding && fabsf(client.edict->v.velocity[0]) < 0.01f);
	assert(client.edict->v.velocity[1] < -250 && client.edict->v.origin[1] < old_y);

	// Losing support at the end of the face releases the latch without
	// clearing the held input (the player may latch onto another face).
	wall_support = top_support = false;
	assert(!SV_SkateWallSlideMove(client.edict));
	assert(!client.skate_sliding && client.skate_slide_held);
	SV_SkateUpdate(&client);
	assert(client.skate_board->v.angles[PITCH] == 0); // restore board pitch after leaving the grind
}
static void TestSlideNeedsUpperEdge (void)
{
	vec3_t incoming = {SKATE_SLIDE_MIN_SPEED + 50, 0, -40};
	Reset(); Command(NULL);
	client.skate_slide_held = true;
	client.edict->v.flags = 0;
	VectorCopy(incoming, client.edict->v.velocity);
	wall_normal[0] = -1;
	wall_support = true;
	top_support = false;
	wall_collision = true;
	SV_FlyMove(client.edict, 0.05f, NULL);
	assert(!client.skate_sliding); // a vertical face without a top lip is not a grind edge

	Reset(); Command(NULL);
	client.skate_slide_held = true;
	client.edict->v.flags = 0;
	VectorCopy(incoming, client.edict->v.velocity);
	wall_normal[0] = -1;
	wall_support = top_support = wall_continues_above = true;
	wall_collision = true;
	SV_FlyMove(client.edict, 0.05f, NULL);
	assert(!client.skate_sliding); // a wall that continues above its apparent cap has no upper edge
}
static void TestWallBounce (void)
{
	int clip;
	trace_t wall = {0};
	vec3_t incoming = {SKATE_WALL_BOUNCE_MIN_SPEED + 50, 20, -40};
	Reset(); Command(NULL);
	client.skate_jump_active = true;
	client.skate_grounded = false;
	client.edict->v.flags = 0;
	VectorCopy(incoming, client.edict->v.velocity);
	wall_normal[0] = -1;
	wall_collision = true;
	clip = SV_FlyMove(client.edict, 0.05f, NULL);
	wall_collision = false;
	assert(clip & 2);
	assert(client.edict->v.origin[0] > 100); // collision moved partway to the wall
	assert(fabsf(client.edict->v.velocity[0] +
		(SKATE_WALL_BOUNCE_MIN_SPEED + 50) * SKATE_WALL_BOUNCE_RESTITUTION) < 0.01f);
	assert(client.edict->v.velocity[1] == incoming[1]); // tangential motion is preserved
	assert(client.edict->v.velocity[2] == SKATE_WALL_BOUNCE_UPWARD_SPEED);

	Reset(); Command(NULL);
	client.skate_jump_active = true;
	client.skate_grounded = false;
	client.edict->v.flags = 0;
	wall.plane.normal[0] = -1;
	incoming[0] = SKATE_WALL_BOUNCE_MIN_SPEED - 1;
	incoming[1] = 0;
	incoming[2] = 270;
	assert(!SV_SkateWallBounce(client.edict, wall.plane.normal, incoming)); // not fast enough
	incoming[0] = SKATE_WALL_BOUNCE_MIN_SPEED + 50;
	wall.plane.normal[0] = 0;
	wall.plane.normal[1] = -1;
	assert(!SV_SkateWallBounce(client.edict, wall.plane.normal, incoming)); // glancing/tangential hit
	wall.plane.normal[1] = 0;
	wall.plane.normal[2] = 1;
	assert(!SV_SkateWallBounce(client.edict, wall.plane.normal, incoming)); // floor/ceiling isn't a wall
	wall.plane.normal[0] = -1;
	wall.plane.normal[2] = 0;
	client.skate_jump_active = false;
	client.skate_grounded = false;
	assert(!SV_SkateWallBounce(client.edict, wall.plane.normal, incoming)); // falling isn't a jump
	client.skate_grounded = true;
	client.edict->v.velocity[2] = 100;
	assert(SV_SkateWallBounce(client.edict, wall.plane.normal, incoming)); // first frame after takeoff
}
static void TestJumpFlip (void)
{
	edict_t *board;
	double start;
	float previous = 0, total = 0;
	int i;
	Reset(); Command(NULL);
	board = client.skate_board;
	assert(client.skate_grounded && !client.skate_jump_active && !client.skate_flip_active && board->v.angles[ROLL] == 0);
	client.edict->v.flags = 0; // simulate the actual upward takeoff after QC/physics
	client.edict->v.button2 = 1;
	client.edict->v.velocity[2] = 270;
	SV_SkateUpdate(&client);
	start = qcvm->time;
	assert(client.skate_jump_active && client.skate_flip_active && !client.skate_grounded);
	assert(client.skate_flip_start_time == start && board->v.angles[ROLL] == 0);

	for (i = 1; i <= 4; i++)
	{
		float delta;
		qcvm->time = start + SKATE_FLIP_DURATION * i / 4;
		if (i == 3) client.edict->v.velocity[2] = -135; // keep flipping while descending
		SV_SkateUpdate(&client);
		assert(board->v.angles[ROLL] == (i == 4 ? 0 : i * 90));
		delta = board->v.angles[ROLL] - previous;
		if (delta < 0) delta += 360;
		total += delta;
		previous = board->v.angles[ROLL];
		assert(client.edict->v.angles[ROLL] == 0 && client.edict->v.angles[PITCH] == 0);
		assert(board->v.angles[PITCH] == 0 && board->v.angles[YAW] == client.skate_yaw);
		assert(board->v.origin[2] == 31 && client.skate_lift == 7);
		assert(client.edict->v.velocity[2] == (i >= 3 ? -135 : 270));
		CheckPhysical();
		SV_SkateUpdate(&client); // same timestamp: no frame-dependent extra rotation
		assert(board->v.angles[ROLL] == previous);
	}
	assert(total == 360 && !client.skate_flip_active && client.skate_jump_active && traces == 4);
	qcvm->time += 1;
	client.edict->v.velocity[2] = 270; // no repeat/double flip until landing
	SV_SkateUpdate(&client);
	assert(!client.skate_flip_active && board->v.angles[ROLL] == 0);

	client.edict->v.flags = FL_ONGROUND;
	SV_SkateUpdate(&client);
	assert(client.skate_grounded && !client.skate_jump_active);
	client.edict->v.flags = 0;
	SV_SkateUpdate(&client);
	assert(client.skate_flip_active && client.skate_flip_start_time == qcvm->time);
	qcvm->time += SKATE_FLIP_DURATION / 4;
	SV_SkateUpdate(&client);
	assert(board->v.angles[ROLL] == 90); // next jump gets its own full turn
	Command("0");
	assert(!client.skate_flip_active && !client.skate_grounded && client.skate_flip_start_time == 0);
	Command("1"); // enabling while already airborne must not invent a jump
	assert(!client.skate_flip_active && client.skate_board->v.angles[ROLL] == 0);
}
static void TestJumpFlipGuards (void)
{
	int i;
	Reset(); Command(NULL);
	client.edict->v.button2 = 1;
	for (i = 0; i < 5; i++)
	{
		qcvm->time += 0.1;
		SV_SkateUpdate(&client); // jump held, but no actual takeoff
		assert(!client.skate_flip_active && client.skate_board->v.angles[ROLL] == 0);
	}
	client.edict->v.flags = 0;
	client.edict->v.velocity[2] = -100; // rolling off a ledge is not a jump
	SV_SkateUpdate(&client);
	assert(!client.skate_flip_active);
	client.edict->v.velocity[2] = 270;
	SV_SkateUpdate(&client);
	assert(!client.skate_flip_active); // an airborne velocity change is not another takeoff

	Reset(); client.edict->v.flags = 0; client.edict->v.velocity[2] = 270;
	Command(NULL);
	qcvm->time += 0.1;
	SV_SkateUpdate(&client);
	assert(!client.skate_flip_active && client.skate_board->v.angles[ROLL] == 0);

	Reset(); Command(NULL);
	client.edict->v.flags = FL_WATERJUMP;
	client.edict->v.velocity[2] = 270;
	SV_SkateUpdate(&client);
	assert(!client.skate_flip_active);

	Reset(); Command(NULL);
	client.edict->v.flags = 0; client.edict->v.velocity[2] = 270;
	SV_SkateUpdate(&client);
	qcvm->time += SKATE_FLIP_DURATION / 4;
	SV_SkateUpdate(&client);
	assert(client.skate_board->v.angles[ROLL] == 90);
	client.edict->v.flags = FL_ONGROUND; // short jump/early landing returns to flat
	SV_SkateUpdate(&client);
	assert(!client.skate_flip_active && client.skate_grounded);
	assert(client.skate_flip_start_time == 0 && client.skate_board->v.angles[ROLL] == 0);
}
static void TestShoveIts (void)
{
	double start;
	float base_yaw, expected_yaw;
	int i;

	Reset(); Command(NULL);
	SV_SkateShoveIt_f();
	assert(!client.skate_shoveit_active && strstr(printed, "airborne"));

	// A Pop Shove-It replaces the default air roll, spins the board 180
	// degrees, and carries that stance through the catch without a yaw snap.
	client.edict->v.flags = 0;
	client.edict->v.velocity[2] = 270;
	SV_SkateUpdate(&client);
	assert(client.skate_jump_active && client.skate_flip_active);
	base_yaw = client.skate_yaw;
	start = qcvm->time;
	client.cmd.sidemove = -400; // A selects the counterclockwise shove-it
	SV_SkateShoveIt_f();
	assert(client.skate_shoveit_active && client.skate_shoveit_degrees == -SKATE_SHOVEIT_POP_ANGLE);
	assert(!client.skate_flip_active && client.skate_board->v.angles[ROLL] == 0);

	for (i = 1; i <= 4; i++)
	{
		qcvm->time = start + SKATE_SHOVEIT_DURATION * i / 4;
		SV_SkateUpdate(&client);
		expected_yaw = anglemod(base_yaw - SKATE_SHOVEIT_POP_ANGLE * i / 4);
		assert(fabsf(client.skate_board->v.angles[YAW] - expected_yaw) < 0.01f);
		assert(client.edict->v.angles[YAW] == client.skate_yaw); // rider stays facing forward
	}
	assert(!client.skate_shoveit_active && client.skate_board_yaw_offset == 180);
	assert(client.skate_board->v.angles[YAW] == anglemod(base_yaw + 180));

	// Land and take off again. A 360 Shove-It makes a full turn and ends at
	// the same board heading, including from the post-Pop stance.
	client.edict->v.flags = FL_ONGROUND;
	client.edict->v.velocity[2] = 0;
	SV_SkateUpdate(&client);
	client.edict->v.flags = 0;
	client.edict->v.velocity[2] = 270;
	SV_SkateUpdate(&client);
	assert(client.skate_jump_active && client.skate_flip_active);
	client.cmd.sidemove = 400; // D selects the clockwise 360 spin
	start = qcvm->time;
	SV_Skate360ShoveIt_f();
	assert(client.skate_shoveit_active && client.skate_shoveit_degrees == SKATE_SHOVEIT_360_ANGLE);
	assert(!client.skate_flip_active);

	for (i = 1; i <= 4; i++)
	{
		qcvm->time = start + SKATE_SHOVEIT_DURATION * i / 4;
		SV_SkateUpdate(&client);
		expected_yaw = anglemod(base_yaw + 180 + SKATE_SHOVEIT_360_ANGLE * i / 4);
		assert(fabsf(client.skate_board->v.angles[YAW] - expected_yaw) < 0.01f);
	}
	assert(!client.skate_shoveit_active && client.skate_board_yaw_offset == 180);
	assert(client.skate_board->v.angles[YAW] == anglemod(base_yaw + 180));

	// Falling, grinding, and duplicate inputs cannot start/replace a trick.
	Reset(); Command(NULL);
	client.edict->v.flags = 0;
	client.edict->v.velocity[2] = -100;
	SV_SkateUpdate(&client);
	SV_SkateShoveIt_f();
	assert(!client.skate_shoveit_active && strstr(printed, "real jump"));

	Reset(); Command(NULL);
	client.edict->v.flags = 0;
	client.edict->v.velocity[2] = 270;
	SV_SkateUpdate(&client);
	client.skate_sliding = true;
	SV_SkateShoveIt_f();
	assert(!client.skate_shoveit_active && strstr(printed, "grind"));
	client.skate_sliding = false;
	SV_SkateShoveIt_f();
	assert(client.skate_shoveit_active);
	SV_Skate360ShoveIt_f();
	assert(client.skate_shoveit_degrees == SKATE_SHOVEIT_POP_ANGLE && strstr(printed, "current trick"));

	Reset();
	cmd_source = src_command;
	SV_Skate360ShoveIt_f();
	assert(forwarded == 1 && !client.skate_shoveit_active);
}
static void TestSlopeAndCleanup (void)
{
	int reason;
	Reset(); slope = 0.25f; Command(NULL);
	assert(client.skate_lift == 15); // front wheels clear the higher end of a slope
	assert(client.skate_board->v.origin[2] + model.mins[2] >= 8 + 1);
	CheckPhysical();
	client.edict->v.origin[0] += 100;
	client.edict->v.origin[2] += 40;
	client.skate_yaw = 90;
	SV_SkateUpdate(&client);
	assert(client.skate_board->v.origin[0] == 200);
	assert(client.skate_board->v.angles[YAW] == 90);
	for (reason = 0; reason < 4; reason++)
	{
		Reset(); Command(NULL);
		client.edict->v.flags = 0; client.edict->v.velocity[2] = 270;
		SV_SkateUpdate(&client);
		assert(client.skate_flip_active);
		if (reason == 0) client.edict->v.health = 0;
		if (reason == 1) client.edict->v.movetype = MOVETYPE_NOCLIP;
		if (reason == 2) client.edict->v.waterlevel = 2;
		if (reason == 3) client.edict->onladder = true;
		SV_SkateUpdate(&client);
		assert(!client.skate_active && frees == 1 && SV_SkateStat(&client) == 0);
		assert(!client.skate_jump_active && !client.skate_flip_active && !client.skate_grounded && client.skate_flip_start_time == 0);
	}
	Reset(); Command(NULL);
	client.skate_board->v.classname = 0; // slot was reused by unrelated QC
	SV_SkateStop(&client);
	assert(frees == 0 && !client.skate_board && !client.skate_active);
	Reset(); Command(NULL);
	PR_SwitchQCVM(NULL);
	SV_SkateStop(&client); // disconnect/map cleanup may be outside server VM
	assert(!qcvm && frees == 1);
}
static void TestGuards (void)
{
	Reset(); sv.skate_modelindex = 0; Command(NULL);
	assert(!client.skate_active && allocations == 0 && strstr(printed, "missing"));
	Reset(); client.edict->v.health = 0; Command(NULL); assert(!allocations);
	Reset(); client.limit_models = 1; Command(NULL); assert(!allocations);
	Reset(); qcvm->max_edicts = qcvm->num_edicts; Command(NULL); assert(!allocations);
	Reset(); model.mins[2] = -512; Command(NULL); assert(!allocations);
	Reset(); Command("invalid"); assert(!allocations && strstr(printed, "skate [0|1]"));
	Reset(); cmd_source = src_command; Command(NULL); assert(forwarded == 1 && !allocations);
	Reset(); client.spawned = false; Command(NULL); assert(!allocations);
}
int main (void)
{
	TestToggleAndPlacement(); TestMotion(); TestSkateAnimations(); TestSlideCommand(); TestWallSlide(); TestSlideNeedsUpperEdge(); TestWallBounce(); TestJumpFlip(); TestJumpFlipGuards(); TestShoveIts();
	TestSlopeAndCleanup(); TestGuards();
	puts("PASS: skate command, forward acceleration, cap, gradual steering, drift and braking.");
	puts("PASS: skate cruise/idle, acceleration, shooting, jump and landing frame sequences.");
	puts("PASS: held skate slide latches to brush upper edges, then releases at the edge or key-up.");
	puts("PASS: high-speed jump bounce from vertical walls; speed, glancing-hit, and non-jump guards.");
	puts("PASS: one time-based 360-degree jump flip, upright rider, landing/rearm and takeoff guards.");
	puts("PASS: 180/360 Shove-It yaw spins, stance carry-through, flip suppression and invalid-input guards.");
	puts("PASS: board placement/ground clearance, unchanged hull/jump/ammo, lifetime and guards.");
	return 0;
}
