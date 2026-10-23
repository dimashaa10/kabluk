/* Asset-free tests for the HUD speedometer feed (G11).
 *
 * The bug: SV_UpdateSpeedInfo only ran at the end of the classic engine
 * movement path, so any client moved by the shared pmove kept speed_info.speed
 * at its initial -1 and gl_screen never drew the speedometer. The fix feeds it
 * from the pmove path too. These tests pin the reporting rules themselves:
 * the speed comes from the velocity the caller hands over, and the "bad bunny
 * hop" sample is only recorded while jumping off the ground. */
#include "quakedef.h"
#include <assert.h>

/* The only engine state SV_UpdateSpeedInfo looks at besides its arguments. */
static edict_t player;

static void Reset (void)
{
	memset(&player, 0, sizeof(player));
	memset(&speed_info, 0, sizeof(speed_info));
	speed_info.speed = -1;
	speed_info.jump_fmove = speed_info.jump_smove = -1;
	sv_player = &player;
}

static void TestSpeedIsHorizontal (void)
{
	vec3_t vel = { 300, 400, 900 };	/* 3-4-5 triangle, vertex speed ignored */
	usercmd_t cmd = {0};

	Reset();
	SV_UpdateSpeedInfo(vel, &cmd, false);
	assert(fabs(speed_info.speed - 500.0f) < 0.01f);
}

static void TestSpeedUpdatesEveryFrame (void)
{
	vec3_t vel = { 0, 0, 0 };
	usercmd_t cmd = {0};

	Reset();
	SV_UpdateSpeedInfo(vel, &cmd, true);
	assert(speed_info.speed == 0.0f);	/* standing still is a real reading */
	vel[0] = 320; vel[1] = 0; vel[2] = 0;
	SV_UpdateSpeedInfo(vel, &cmd, true);
	assert(fabs(speed_info.speed - 320.0f) < 0.01f);

	/* Predicted moves arrive with arbitrary speeds, including Quake's overspeed
	 * states; the HUD must show them rather than clamp them. */
	vel[0] = 700; vel[1] = -700; vel[2] = 0;
	SV_UpdateSpeedInfo(vel, &cmd, true);
	assert(fabs(speed_info.speed - 989.949f) < 0.1f);
}

static void TestJumpSampleNeedsGroundAndJump (void)
{
	vec3_t vel = { 200, 0, 0 };
	usercmd_t cmd = {0};

	Reset();
	cmd.forwardmove = 200;
	cmd.sidemove = -200;

	/* In the air the jump sample must stay untouched. */
	SV_UpdateSpeedInfo(vel, &cmd, false);
	assert(speed_info.jump_fmove == -1 && speed_info.jump_smove == -1);

	/* On the ground without +jump neither. */
	SV_UpdateSpeedInfo(vel, &cmd, true);
	assert(speed_info.jump_fmove == -1 && speed_info.jump_smove == -1);

	/* On the ground with +jump the directional keys are captured. */
	player.v.button2 = 1;
	SV_UpdateSpeedInfo(vel, &cmd, true);
	assert(speed_info.jump_fmove == 200 && speed_info.jump_smove == -200);

	/* A pure forward bunny hop is a *good* one; the HUD reports it that way. */
	cmd.sidemove = 0;
	SV_UpdateSpeedInfo(vel, &cmd, true);
	assert(speed_info.jump_fmove == 200 && speed_info.jump_smove == 0);
}

static void TestUnrelatedInputDoesNotLeak (void)
{
	vec3_t vel = { 100, 0, 0 };
	usercmd_t cmd = {0};

	Reset();
	cmd.forwardmove = 64;
	player.v.button2 = 1;

	/* Jump sample is taken when the ground jump is reported... */
	SV_UpdateSpeedInfo(vel, &cmd, true);
	assert(speed_info.jump_fmove == 64);

	/* ...and a later airborne frame must not overwrite it with 0. */
	cmd.forwardmove = 0;
	SV_UpdateSpeedInfo(vel, &cmd, false);
	assert(speed_info.jump_fmove == 64);
}

int main (void)
{
	TestSpeedIsHorizontal();
	TestSpeedUpdatesEveryFrame();
	TestJumpSampleNeedsGroundAndJump();
	TestUnrelatedInputDoesNotLeak();
	puts("PASS: the HUD speedometer is fed horizontal speed on every path,");
	puts("PASS: and only records the directional keys of a real ground jump.");
	return 0;
}
