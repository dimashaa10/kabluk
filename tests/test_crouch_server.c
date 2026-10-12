/* Asset-free tests of smooth crouch bounds and the player's seated animations. */
#include "quakedef.h"
#include <assert.h>

qcvm_t test_qcvm;
qcvm_t *qcvm = &test_qcvm;
double host_frametime;
qboolean rogue, hipnotic;

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
	memset(&test_qcvm, 0, sizeof(test_qcvm));
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
	host_frametime = 0.1;
	rogue = hipnotic = false;
}

static void StepCrouch (float upmove)
{
	test_qcvm.time += 0.1;
	host_frametime = 0.1;
	SV_UpdateCrouch(&client, &player, upmove);
}

static void StepAnimation (void)
{
	test_qcvm.time += SV_CROUCH_ANIM_FRAME_TIME;
	SV_UpdateCrouchAnimation(&client, &player);
}

static void FinishCrouch (void)
{
	int i;
	SV_UpdateCrouch(&client, &player, -200); // initialize the time baseline
	for (i = 0; i < 6; i++)
		StepCrouch(-200);
}

static void TestSmoothCrouchAndRelease (void)
{
	float previous_maxs = 32;
	int i;

	Reset();
	SV_UpdateCrouch(&client, &player, -200);
	assert(client.crouched && client.crouch_wanted);
	assert(fabsf(client.crouch_amount) < 0.001f);
	assert(player.v.maxs[2] == 32 && player.v.view_ofs[2] == 22);

	for (i = 0; i < 6; i++)
	{
		StepCrouch(-200);
		assert(player.v.maxs[2] < previous_maxs);
		assert(player.v.maxs[2] > 8 || i == 5);
		assert(player.v.view_ofs[2] < 22);
		previous_maxs = player.v.maxs[2];
	}
	assert(client.crouched && fabsf(client.crouch_amount - 1.0f) < 0.001f);
	assert(player.v.mins[2] == -24 && player.v.maxs[2] == 8);
	assert(player.v.size[2] == 32 && player.v.view_ofs[2] == 8);

	// Repeated down input must retain the original standing hull for release,
	// even if QuakeC tries to restore it while the animation is in progress.
	player.v.maxs[2] = 32;
	player.v.view_ofs[2] = 22;
	SV_UpdateCrouch(&client, &player, -200);
	assert(client.crouched && player.v.maxs[2] == 8 && player.v.view_ofs[2] == 8);

	for (i = 0; i < 5; i++)
	{
		StepCrouch(0);
		assert(client.crouched);
		assert(player.v.maxs[2] > 8 && player.v.maxs[2] < 32);
	}
	StepCrouch(0);
	assert(!client.crouched && !client.crouch_wanted);
	assert(player.v.mins[2] == -24 && player.v.maxs[2] == 32);
	assert(player.v.size[2] == 56 && player.v.view_ofs[2] == 22);
	assert(links >= 12 && traces == 6);
}

static void TestBlockedStand (void)
{
	int i;

	Reset();
	FinishCrouch();
	assert(player.v.maxs[2] == 8 && client.crouched);

	block_standing = true;
	StepCrouch(0);
	assert(client.crouched && !client.crouch_wanted && client.crouch_stand_blocked);
	assert(fabsf(client.crouch_amount - 1.0f) < 0.001f);
	assert(player.v.maxs[2] == 8 && player.v.view_ofs[2] == 8);
	assert(links == 6 && traces == 1);

	block_standing = false;
	for (i = 0; i < 5; i++)
	{
		StepCrouch(0);
		assert(client.crouched && player.v.maxs[2] > 8 && player.v.maxs[2] < 32);
	}
	StepCrouch(0);
	assert(!client.crouched && player.v.maxs[2] == 32);
	assert(player.v.view_ofs[2] == 22 && player.v.size[2] == 56);
	assert(traces == 7);
}

static void TestCrouchSpeed (void)
{
	float speed;
	int i;

	Reset();
	player.v.velocity[0] = 300;
	player.v.velocity[1] = 200;
	player.v.velocity[2] = -120;
	SV_UpdateCrouch(&client, &player, -200);
	for (i = 0; i < 6; i++)
		StepCrouch(-200);
	speed = sqrtf(player.v.velocity[0] * player.v.velocity[0] +
		player.v.velocity[1] * player.v.velocity[1]);
	assert(fabsf(speed - SV_CROUCH_MAX_SPEED) < 0.01f);
	assert(player.v.velocity[2] == -120); // crouch doesn't alter jump/fall velocity

	player.v.velocity[0] = 220;
	player.v.velocity[1] = 0;
	StepCrouch(0);
	assert(client.crouched && player.v.velocity[0] == SV_CROUCH_MAX_SPEED);
	for (i = 0; i < 4; i++)
		StepCrouch(0);
	player.v.velocity[0] = 220;
	StepCrouch(0);
	assert(!client.crouched && player.v.velocity[0] == 220);
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

static void BeginSeated (void)
{
	Reset();
	client.crouched = client.crouch_wanted = client.crouch_bounds_saved = true;
	client.crouch_amount = 1;
}

static void TestTransitionFrames (void)
{
	BeginSeated();
	client.crouch_amount = 0;
	SV_UpdateCrouchAnimation(&client, &player);
	assert(player.v.frame == SV_CROUCH_DOWN_FIRST_FRAME);
	client.crouch_amount = 0.5f;
	SV_UpdateCrouchAnimation(&client, &player);
	assert(player.v.frame == SV_CROUCH_DOWN_FIRST_FRAME + 3);
	client.crouch_amount = 0.99f;
	SV_UpdateCrouchAnimation(&client, &player);
	assert(player.v.frame == SV_CROUCH_DOWN_LAST_FRAME);

	client.crouch_wanted = false;
	client.crouch_amount = 1;
	SV_UpdateCrouchAnimation(&client, &player);
	assert(player.v.frame == SV_STAND_UP_FIRST_FRAME);
	client.crouch_amount = 0.01f;
	SV_UpdateCrouchAnimation(&client, &player);
	assert(player.v.frame == SV_STAND_UP_LAST_FRAME);
}

static void TestSeatedIdleAndWalk (void)
{
	int i;
	BeginSeated();
	SV_UpdateCrouchAnimation(&client, &player);
	assert(player.v.frame == SV_CROUCH_IDLE_FRAME);

	client.cmd.forwardmove = 200;
	StepAnimation();
	assert(player.v.frame == SV_CROUCH_WALK_FIRST_FRAME);
	for (i = 0; i < 5; i++)
		StepAnimation();
	assert(player.v.frame == SV_CROUCH_WALK_LAST_FRAME);
	StepAnimation();
	assert(player.v.frame == SV_CROUCH_WALK_FIRST_FRAME);

	client.cmd.forwardmove = 0;
	StepAnimation();
	assert(player.v.frame == SV_CROUCH_IDLE_FRAME);

	// Horizontal momentum also counts as movement when input is neutral.
	player.v.velocity[1] = 5;
	StepAnimation();
	assert(player.v.frame == SV_CROUCH_WALK_FIRST_FRAME);
	player.v.velocity[1] = 0;
	StepAnimation();
	assert(player.v.frame == SV_CROUCH_IDLE_FRAME);
}

static void TestSeatedAttacks (void)
{
	int i;
	BeginSeated();
	player.v.weapon = IT_AXE;
	player.v.button0 = 1;
	SV_UpdateCrouchAnimation(&client, &player);
	assert(player.v.frame == SV_CROUCH_MELEE_IDLE_FIRST);
	player.v.button0 = 0; // a tap finishes the current melee animation once
	for (i = 0; i < 5; i++)
	{
		StepAnimation();
		assert(player.v.frame == SV_CROUCH_MELEE_IDLE_FIRST + i + 1);
	}
	StepAnimation();
	assert(player.v.frame == SV_CROUCH_IDLE_FRAME);

	player.v.weapon = IT_SHOTGUN;
	player.v.button0 = 1;
	StepAnimation();
	assert(player.v.frame == SV_CROUCH_SHOOT_IDLE_FIRST);

	client.cmd.sidemove = 200;
	player.v.weapon = IT_AXE;
	StepAnimation();
	assert(player.v.frame == SV_CROUCH_MELEE_WALK_FIRST);
	player.v.weapon = IT_SHOTGUN;
	StepAnimation();
	assert(player.v.frame == SV_CROUCH_SHOOT_WALK_FIRST);
}

static void TestExpansionMeleeIds (void)
{
	BeginSeated();
	rogue = true;
	player.v.weapon = RIT_AXE;
	player.v.button0 = 1;
	SV_UpdateCrouchAnimation(&client, &player);
	assert(player.v.frame == SV_CROUCH_MELEE_IDLE_FIRST);

	BeginSeated();
	rogue = true;
	player.v.weapon = IT_AXE; // 4096 is Rogue's lava nailgun, not its axe
	player.v.button0 = 1;
	SV_UpdateCrouchAnimation(&client, &player);
	assert(player.v.frame == SV_CROUCH_SHOOT_IDLE_FIRST);

	BeginSeated();
	hipnotic = true;
	player.v.weapon = HIT_MJOLNIR;
	player.v.button0 = 1;
	SV_UpdateCrouchAnimation(&client, &player);
	assert(player.v.frame == SV_CROUCH_MELEE_IDLE_FIRST);

	BeginSeated();
	player.v.weapon = HIT_MJOLNIR; // base Quake treats bit 128 as a ranged weapon
	player.v.button0 = 1;
	SV_UpdateCrouchAnimation(&client, &player);
	assert(player.v.frame == SV_CROUCH_SHOOT_IDLE_FIRST);
}

int main (void)
{
	TestSmoothCrouchAndRelease();
	TestBlockedStand();
	TestCrouchSpeed();
	TestMovementModes();
	TestTransitionFrames();
	TestSeatedIdleAndWalk();
	TestSeatedAttacks();
	TestExpansionMeleeIds();
	puts("PASS: crouch/stand smoothly interpolate hull and view height, while standing remains collision-safe.");
	puts("PASS: seated idle, walk, melee and shooting use the requested frames and Rogue/Hipnotic melee IDs.");
	puts("PASS: crouch speed cap remains active through transitions without changing vertical velocity.");
	puts("PASS: swimming down, noclip, ladder and skate mode do not enter crouch.");
	return 0;
}
