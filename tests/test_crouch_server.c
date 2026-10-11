/* Asset-free tests of engine-managed crouch bounds and stand-up clearance. */
#include "quakedef.h"
#include <assert.h>

void SV_UpdateCrouch (client_t *client, edict_t *ent, float upmove);

static edict_t player;
static client_t client;
static qboolean block_standing;
static int links, traces;

void SV_LinkEdict (edict_t *ent, qboolean touch_triggers)
{
	assert(ent == &player);
	assert(!touch_triggers);
	links++;
}

trace_t SV_Move (vec3_t start, vec3_t mins, vec3_t maxs, vec3_t end,
	int type, edict_t *passedict)
{
	trace_t trace = {0};
	assert(type == MOVE_NORMAL);
	assert(passedict == &player);
	assert(!memcmp(start, end, sizeof(vec3_t)));
	traces++;

	if (block_standing && maxs[2] > 8)
	{
		trace.startsolid = true;
		trace.allsolid = true;
		trace.fraction = 0;
	}
	else
	{
		trace.fraction = 1;
		VectorCopy(end, trace.endpos);
	}
	(void)mins;
	return trace;
}

static void Reset (void)
{
	memset(&player, 0, sizeof(player));
	memset(&client, 0, sizeof(client));
	client.spawned = true;
	client.edict = &player;
	player.v.health = 100;
	player.v.movetype = MOVETYPE_WALK;
	player.v.solid = SOLID_SLIDEBOX;
	player.v.mins[0] = player.v.mins[1] = -16;
	player.v.mins[2] = -24;
	player.v.maxs[0] = player.v.maxs[1] = 16;
	player.v.maxs[2] = 32;
	player.v.size[0] = player.v.size[1] = 32;
	player.v.size[2] = 56;
	player.v.view_ofs[2] = 22;
	links = traces = 0;
	block_standing = false;
}

static void TestCrouchAndRelease (void)
{
	Reset();
	SV_UpdateCrouch(&client, &player, -200);
	assert(client.crouched);
	assert(player.v.mins[2] == -24 && player.v.maxs[2] == 8);
	assert(player.v.size[2] == 32 && player.v.view_ofs[2] == 8);
	assert(links == 1 && traces == 0);

	// Repeated down input must retain the original standing hull for release.
	player.v.maxs[2] = 32; // model/QC attempted to restore the default hull
	player.v.view_ofs[2] = 22;
	SV_UpdateCrouch(&client, &player, -200);
	assert(client.crouched && player.v.maxs[2] == 8 && player.v.view_ofs[2] == 8);
	assert(links == 2);

	SV_UpdateCrouch(&client, &player, 0);
	assert(!client.crouched);
	assert(player.v.mins[2] == -24 && player.v.maxs[2] == 32);
	assert(player.v.size[2] == 56 && player.v.view_ofs[2] == 22);
	assert(links == 3 && traces == 1);
}

static void TestBlockedStand (void)
{
	Reset();
	SV_UpdateCrouch(&client, &player, -200);
	block_standing = true;
	SV_UpdateCrouch(&client, &player, 0);
	assert(client.crouched);
	assert(player.v.maxs[2] == 8 && player.v.view_ofs[2] == 8);
	assert(links == 1 && traces == 1);

	block_standing = false;
	SV_UpdateCrouch(&client, &player, 0);
	assert(!client.crouched && player.v.maxs[2] == 32);
	assert(player.v.view_ofs[2] == 22 && player.v.size[2] == 56);
	assert(links == 2 && traces == 2);
}

static void TestCrouchSpeed (void)
{
	float speed;

	Reset();
	player.v.velocity[0] = 300;
	player.v.velocity[1] = 200;
	player.v.velocity[2] = -120;
	SV_UpdateCrouch(&client, &player, -200);
	speed = sqrtf(player.v.velocity[0] * player.v.velocity[0] +
		player.v.velocity[1] * player.v.velocity[1]);
	assert(fabsf(speed - SV_CROUCH_MAX_SPEED) < 0.01f);
	assert(player.v.velocity[2] == -120); // crouch doesn't alter jump/fall velocity

	player.v.velocity[0] = 220;
	player.v.velocity[1] = 0;
	SV_UpdateCrouch(&client, &player, 0);
	assert(!client.crouched && player.v.velocity[0] == 220); // normal speed returns on stand
}

static void TestMovementModes (void)
{
	Reset();
	player.v.waterlevel = 2;
	SV_UpdateCrouch(&client, &player, -200);
	assert(!client.crouched && player.v.maxs[2] == 32 && links == 0);

	player.v.waterlevel = 0;
	player.v.movetype = MOVETYPE_NOCLIP;
	SV_UpdateCrouch(&client, &player, -200);
	assert(!client.crouched && player.v.maxs[2] == 32 && links == 0);

	player.v.movetype = MOVETYPE_WALK;
	player.onladder = true;
	SV_UpdateCrouch(&client, &player, -200);
	assert(!client.crouched && player.v.maxs[2] == 32 && links == 0);

	player.onladder = false;
	client.skate_active = true;
	SV_UpdateCrouch(&client, &player, -200);
	assert(!client.crouched && player.v.maxs[2] == 32 && links == 0);
}

int main (void)
{
	TestCrouchAndRelease();
	TestBlockedStand();
	TestCrouchSpeed();
	TestMovementModes();
	puts("PASS: crouch shrinks the player hull and lowers view height until released.");
	puts("PASS: standing is deferred while blocked, then restores the original bounds.");
	puts("PASS: crouch caps horizontal velocity at 160 without changing vertical speed.");
	puts("PASS: swimming down, noclip, ladder and skate mode do not trigger crouch.");
	return 0;
}
