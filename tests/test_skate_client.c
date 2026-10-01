/* Exercise the real alias transform, culling and skate/chase helpers without GL. */
#include "../Quake/r_alias.c"
#include "skate.h"
#include <assert.h>
#include <ctype.h>

client_state_t cl;
client_static_t cls;
extern cvar_t chase_back, chase_up, chase_right;
static entity_t entities[6];
static qmodel_t player_model, board_model, world_model;
static float trace_fraction;

int q_strcasecmp (const char *a, const char *b)
{
	while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
qboolean SV_RecursiveHullCheck (hull_t *hull, vec3_t start, vec3_t end, trace_t *trace, unsigned int contents)
{
	int i;
	(void)hull; (void)contents;
	trace->fraction = trace_fraction;
	trace->allsolid = trace->startsolid = false;
	for (i = 0; i < 3; i++) trace->endpos[i] = start[i] + trace_fraction * (end[i] - start[i]);
	trace->plane.normal[0] = 1;
	return true;
}
static void Reset (void)
{
	int i;
	memset(&cl, 0, sizeof(cl)); memset(&cls, 0, sizeof(cls));
	memset(entities, 0, sizeof(entities));
	memset(&player_model, 0, sizeof(player_model));
	memset(&board_model, 0, sizeof(board_model));
	memset(&world_model, 0, sizeof(world_model));
	memset(&r_refdef, 0, sizeof(r_refdef));
	cl.entities = entities;
	cl.num_entities = 6; cl.maxclients = 2; cl.viewentity = 1;
	cl.worldmodel = &world_model;
	cl.stats[STAT_SKATE] = (int)(SKATE_STAT_MAGIC | 7u * 256u);
	strcpy(board_model.name, SKATE_MODEL);
	board_model.mins[0] = -32; board_model.mins[1] = -8; board_model.mins[2] = -30;
	board_model.maxs[0] = 32; board_model.maxs[1] = 8; board_model.maxs[2] = -24;
	for (i = 0; i < 3; i++)
	{
		board_model.rmins[i] = board_model.ymins[i] = -50;
		board_model.rmaxs[i] = board_model.ymaxs[i] = 50;
	}
	player_model.mins[0] = player_model.mins[1] = -16; player_model.mins[2] = -24;
	player_model.maxs[0] = player_model.maxs[1] = 16; player_model.maxs[2] = 32;
	for (i = 1; i <= 2; i++)
	{
		entities[i].model = &player_model;
		entities[i].origin[2] = entities[i].msg_origins[0][2] = 24;
		entities[i].netstate.scale = ENTSCALE_DEFAULT;
	}
	entities[3].model = &board_model;
	entities[3].netstate.colormap = 1;
	entities[3].origin[2] = entities[3].msg_origins[0][2] = 31;
	entities[3].netstate.scale = ENTSCALE_DEFAULT;
	chase_active.value = 0;
	chase_back.value = 90; chase_up.value = 30; chase_right.value = 0;
	r_lerpmove.value = 0;
	trace_fraction = 1;
}
static void TestPresentation (void)
{
	lerpdata_t transform = {0};
	int i;
	Reset();
	assert(CL_SkateActive() && Chase_Active() && CL_SkateLift() == 7);
	CL_UpdateSkateVisuals();
	assert(entities[1].origin[2] == 24); // physical/prediction origin is untouched
	assert(entities[1].skate_lift == 7 && entities[3].origin[2] == 31);
	for (i = 0; i < 5; i++)
	{
		R_SetupEntityTransform(&entities[1], &transform);
		assert(transform.origin[2] == 31 && entities[1].origin[2] == 24); // no accumulated lift
	}
	R_SetupEntityTransform(&entities[3], &transform);
	assert(transform.origin[2] == 31); // no second lift on the board
	for (i = 0; i < 4; i++)
	{
		memset(&frustum[i], 0, sizeof(frustum[i]));
		frustum[i].normal[2] = 1; frustum[i].dist = 60;
	}
	assert(!R_CullModelForEntity(&entities[1])); // raised head reaches the visible frustum
	cl.stats[STAT_SKATE] = 0;
	entities[3].model = NULL;
	CL_UpdateSkateVisuals();
	R_SetupEntityTransform(&entities[1], &transform);
	assert(transform.origin[2] == 24 && !Chase_Active());
	assert(R_CullModelForEntity(&entities[1]));
}
static void TestJumpFlipPresentation (void)
{
	static const float rolls[] = {0, 90, 180, 270, 359, 0};
	static const float yaws[] = {0, 90, 135};
	int owner, scale, yaw, roll, i, repeat;
	for (owner = 1; owner <= 2; owner++)
		for (scale = 1; scale <= 2; scale++)
			for (yaw = 0; yaw < 3; yaw++)
			{
				entity_t *board, *rider;
				vec3_t upright_origin, upright_angles = {0, 0, 0};
				vec4_t pivot, expected, actual;
				mat4_t matrix;
				lerpdata_t transform = {0};
				Reset();
				board = &entities[3]; rider = &entities[owner];
				board->netstate.colormap = owner;
				board->netstate.scale = ENTSCALE_DEFAULT * scale;
				// Off-center prepared models must rotate around their own bounds,
				// not the player origin, even at another yaw or network scale.
				board_model.mins[0] += 5; board_model.maxs[0] += 5;
				board_model.mins[1] += 3; board_model.maxs[1] += 3;
				rider->angles[YAW] = upright_angles[YAW] = yaws[yaw];
				VectorCopy(rider->origin, upright_origin);
				upright_origin[2] += 7;
				for (i = 0; i < 3; i++) pivot[i] = (board_model.mins[i] + board_model.maxs[i]) * 0.5f;
				pivot[3] = 1;
				R_EntityMatrix(matrix, upright_origin, upright_angles, board->netstate.scale);
				Matrix4_Transform4(matrix, pivot, expected);

				for (roll = 0; roll < 6; roll++)
				{
					board->angles[ROLL] = rolls[roll]; // incoming, already interpolated network roll
					for (repeat = 0; repeat < 3; repeat++)
					{
						CL_UpdateSkateVisuals();
						R_SetupEntityTransform(board, &transform);
						assert(transform.angles[ROLL] == rolls[roll]); // never overwrite the flip
						assert(transform.angles[PITCH] == 0 && transform.angles[YAW] == yaws[yaw]);
						R_EntityMatrix(matrix, transform.origin, transform.angles, board->netstate.scale);
						Matrix4_Transform4(matrix, pivot, actual);
						for (i = 0; i < 3; i++) assert(fabsf(actual[i] - expected[i]) < 0.001f);
						assert(rider->origin[2] == 24 && CL_EntitySkateLift(rider) == 7);
						assert(rider->angles[ROLL] == 0 && rider->angles[PITCH] == 0);
						assert(board->msg_origins[0][2] == 31); // server lift metadata is untouched
					}
					for (i = 0; i < 4; i++)
					{
						memset(&frustum[i], 0, sizeof(frustum[i]));
						frustum[i].normal[2] = 1; frustum[i].dist = expected[2];
					}
					assert(!R_CullModelForEntity(board)); // the flipping board center stays visible
					R_SetupEntityTransform(rider, &transform);
					assert(transform.origin[2] == 31 && transform.angles[ROLL] == 0);
				}
				assert(VectorCompare(board->origin, upright_origin)); // lands with the original fit
			}
}
static void TestRemoteAndGuards (void)
{
	entity_t temporary = {0};
	Reset();
	cl.stats[STAT_SKATE] = 42; // another mod's ordinary stat is not a skate toggle
	assert(!CL_SkateActive());
	entities[3].netstate.colormap = 2;
	entities[3].msg_origins[0][2] = 39;
	strcpy(board_model.name, SKATE_MODEL_ROOT);
	CL_UpdateSkateVisuals();
	assert(entities[1].skate_lift == 0 && entities[2].skate_lift == 15);
	assert(entities[3].origin[2] == 39); // remote riders also see the raised model
	temporary.skate_lift = 999;
	assert(CL_EntitySkateLift(&temporary) == 0);
	entities[3].netstate.colormap = 250;
	CL_UpdateSkateVisuals(); assert(entities[2].skate_lift == 0);
	entities[3].netstate.colormap = 2;
	entities[3].msg_origins[0][0] = 1000;
	CL_UpdateSkateVisuals(); assert(entities[2].skate_lift == 0);
	entities[3].msg_origins[0][0] = 0;
	entities[3].netstate.eflags = EFLAGS_COLOURMAPPED;
	CL_UpdateSkateVisuals(); assert(entities[2].skate_lift == 0);
	cl.stats[STAT_SKATE] = (int)(SKATE_STAT_MAGIC | SKATE_HEIGHT_MASK);
	assert(!CL_SkateActive() && CL_SkateLift() == 0);
	chase_active.value = 1;
	assert(Chase_Active()); // user's pre-existing third person survives leaving skate
}
static void TestCamera (void)
{
	Reset();
	r_refdef.vieworg[2] = 53; // physical origin + eye height + visual lift
	Chase_UpdateForDrawing();
	assert(r_refdef.vieworg[0] < -130 && r_refdef.vieworg[0] > -140);
	assert(r_refdef.vieworg[2] > 90 && r_refdef.vieworg[2] < 100);
	assert(r_refdef.viewangles[PITCH] > 10 && r_refdef.viewangles[PITCH] < 30);
	assert(chase_active.value == 0 && chase_back.value == 90 && chase_up.value == 30);
	Reset(); trace_fraction = 0.5f;
	r_refdef.vieworg[2] = 53;
	Chase_UpdateForDrawing();
	assert(r_refdef.vieworg[0] > -70); // stock wall trace pushes camera out of the wall
}
int main (void)
{
	TestPresentation(); TestJumpFlipPresentation(); TestRemoteAndGuards(); TestCamera();
	puts("PASS: skate third-person camera, rider/board alignment, real alias transforms and culling.");
	puts("PASS: local/remote jump roll, stable board-center pivot, yaw/scale and no accumulated offsets.");
	puts("PASS: remote riders, unchanged prediction origins, camera restore, malformed/foreign-state guards.");
	return 0;
}
