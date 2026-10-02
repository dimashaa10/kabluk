/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// sv_user.c -- server code for moving users

#include "quakedef.h"
#include "cigarette.h"
#include "skate.h"
#include "pmove.h"

edict_t	*sv_player;

extern	cvar_t	sv_friction;
cvar_t	sv_edgefriction = {"edgefriction", "2", CVAR_NONE};
extern	cvar_t	sv_stopspeed;

static	vec3_t		forward, right, up;

// world
float	*angles;
float	*origin;
float	*velocity;

qboolean	onground;
qboolean	prevonground; // woods #qwbunnyhop
vec3_t velocitybeforethink; // woods #qwbunnyhop

usercmd_t	cmd;

cvar_t	sv_idealpitchscale = {"sv_idealpitchscale","0.8",CVAR_NONE};
cvar_t	sv_altnoclip = {"sv_altnoclip","1",CVAR_ARCHIVE}; //johnfitz
cvar_t	sv_nqplayerphysics = {"sv_nqplayerphysics", "1", CVAR_ARCHIVE}; //spike. set to 0 for prediction to work. name comes from fte.
cvar_t	sv_bunnyhopqw = {"sv_bunnyhopqw","0",CVAR_ARCHIVE}; // woods #qwbunnyhop quakespasm-shalrathy 76a1f96
cvar_t	sv_fullpitch = {"sv_fullpitch","1",CVAR_ARCHIVE| CVAR_SERVERINFO}; // woods -- 1 is qs default, 0 limits server to og nq default #pqfullpitch

qboolean SV_RunThink (edict_t *ent);

speed_info_t speed_info = { -1, -1, -1 }; // woods #speedometer

/*
===============
SV_SetIdealPitch
===============
*/
#define	MAX_FORWARD	6
void SV_SetIdealPitch (void)
{
	float	angleval, sinval, cosval;
	trace_t	tr;
	vec3_t	top, bottom;
	float	z[MAX_FORWARD];
	int		i, j;
	int		step, dir, steps;

	if (!((int)sv_player->v.flags & FL_ONGROUND))
		return;

	angleval = sv_player->v.angles[YAW] * M_PI*2 / 360;
	sinval = sin(angleval);
	cosval = cos(angleval);

	for (i=0 ; i<MAX_FORWARD ; i++)
	{
		top[0] = sv_player->v.origin[0] + cosval*(i+3)*12;
		top[1] = sv_player->v.origin[1] + sinval*(i+3)*12;
		top[2] = sv_player->v.origin[2] + sv_player->v.view_ofs[2];

		bottom[0] = top[0];
		bottom[1] = top[1];
		bottom[2] = top[2] - 160;

		tr = SV_Move (top, vec3_origin, vec3_origin, bottom, 1, sv_player);
		if (tr.allsolid)
			return;	// looking at a wall, leave ideal the way is was

		if (tr.fraction == 1)
			return;	// near a dropoff

		z[i] = top[2] + tr.fraction*(bottom[2]-top[2]);
	}

	dir = 0;
	steps = 0;
	for (j=1 ; j<i ; j++)
	{
		step = z[j] - z[j-1];
		if (step > -ON_EPSILON && step < ON_EPSILON)
			continue;

		if (dir && ( step-dir > ON_EPSILON || step-dir < -ON_EPSILON ) )
			return;		// mixed changes

		steps++;
		dir = step;
	}

	if (!dir)
	{
		sv_player->v.idealpitch = 0;
		return;
	}

	if (steps < 2)
		return;
	sv_player->v.idealpitch = -dir * sv_idealpitchscale.value;
}


/*
==================
SV_UserFriction

==================
*/
void SV_UserFriction (void)
{
	float	*vel;
	float	speed, newspeed, control;
	vec3_t	start, stop;
	float	friction;
	trace_t	trace;

	vel = velocity;

	speed = sqrt(vel[0]*vel[0] +vel[1]*vel[1]);
	if (!speed)
		return;

// if the leading edge is over a dropoff, increase friction
	start[0] = stop[0] = origin[0] + vel[0]/speed*16;
	start[1] = stop[1] = origin[1] + vel[1]/speed*16;
	start[2] = origin[2] + sv_player->v.mins[2];
	stop[2] = start[2] - 34;

	trace = SV_Move (start, vec3_origin, vec3_origin, stop, true, sv_player);

	if (trace.fraction == 1.0)
		friction = sv_friction.value*sv_edgefriction.value;
	else
		friction = sv_friction.value;

// apply friction
	control = speed < sv_stopspeed.value ? sv_stopspeed.value : speed;
	newspeed = speed - host_frametime*control*friction;

	if (newspeed < 0)
		newspeed = 0;
	newspeed /= speed;

	vel[0] = vel[0] * newspeed;
	vel[1] = vel[1] * newspeed;
	vel[2] = vel[2] * newspeed;
}

/*
==============
SV_Accelerate
==============
*/
cvar_t	sv_maxspeed = {"sv_maxspeed", "320", CVAR_NOTIFY|CVAR_SERVERINFO};
cvar_t	sv_accelerate = {"sv_accelerate", "10", CVAR_NONE};
void SV_Accelerate (float wishspeed, const vec3_t wishdir)
{
	int			i;
	float		addspeed, accelspeed, currentspeed;

	currentspeed = DotProduct (velocity, wishdir);
	addspeed = wishspeed - currentspeed;
	if (addspeed <= 0)
		return;
	accelspeed = sv_accelerate.value*host_frametime*wishspeed;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	for (i=0 ; i<3 ; i++)
		velocity[i] += accelspeed*wishdir[i];
}

void SV_AirAccelerate (float wishspeed, vec3_t wishveloc)
{
	int			i;
	float		addspeed, wishspd, accelspeed, currentspeed;

	wishspd = VectorNormalize (wishveloc);
	if (wishspd > 30)
		wishspd = 30;
	currentspeed = DotProduct (velocity, wishveloc);
	addspeed = wishspd - currentspeed;
	if (addspeed <= 0)
		return;
//	accelspeed = sv_accelerate.value * host_frametime;
	accelspeed = sv_accelerate.value*wishspeed * host_frametime;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	for (i=0 ; i<3 ; i++)
		velocity[i] += accelspeed*wishveloc[i];
}


void DropPunchAngle (void)
{
	float	len;

	len = VectorNormalize (sv_player->v.punchangle);

	len -= 10*host_frametime;
	if (len < 0)
		len = 0;
	VectorScale (sv_player->v.punchangle, len, sv_player->v.punchangle);
}

/*
===================
SV_WaterMove

===================
*/
void SV_WaterMove (void)
{
	int		i;
	vec3_t	wishvel;
	float	speed, newspeed, wishspeed, addspeed, accelspeed;

//
// user intentions
//
	AngleVectors (sv_player->v.v_angle, forward, right, up);

	for (i=0 ; i<3 ; i++)
		wishvel[i] = forward[i]*cmd.forwardmove + right[i]*cmd.sidemove;

	if (sv_player->onladder)
	{
		wishvel[2] *= 1+fabs(wishvel[2]/200)*9;	//exaggerate vertical movement.
		if (sv_player->v.button2)
			wishvel[2] += 400; //make jump climb (you can turn around and move off to fall)
	}

	if (!cmd.forwardmove && !cmd.sidemove && !cmd.upmove && !sv_player->onladder)
		wishvel[2] -= 60;		// drift towards bottom
	else
		wishvel[2] += cmd.upmove;

	wishspeed = VectorLength(wishvel);
	if (wishspeed > sv_maxspeed.value)
	{
		VectorScale (wishvel, sv_maxspeed.value/wishspeed, wishvel);
		wishspeed = sv_maxspeed.value;
	}
	wishspeed *= 0.7;

//
// water friction
//
	speed = VectorLength (velocity);
	if (speed)
	{
		newspeed = speed - host_frametime * speed * sv_friction.value;
		if (newspeed < 0)
			newspeed = 0;
		VectorScale (velocity, newspeed/speed, velocity);
	}
	else
		newspeed = 0;

//
// water acceleration
//
	if (!wishspeed)
		return;

	addspeed = wishspeed - newspeed;
	if (addspeed <= 0)
		return;

	VectorNormalize (wishvel);
	accelspeed = sv_accelerate.value * wishspeed * host_frametime;
	if (accelspeed > addspeed)
		accelspeed = addspeed;

	for (i=0 ; i<3 ; i++)
		velocity[i] += accelspeed * wishvel[i];
}

void SV_WaterJump (void)
{
	if (qcvm->time > sv_player->v.teleport_time
	|| !sv_player->v.waterlevel)
	{
		sv_player->v.flags = (int)sv_player->v.flags & ~FL_WATERJUMP;
		sv_player->v.teleport_time = 0;
	}
	sv_player->v.velocity[0] = sv_player->v.movedir[0];
	sv_player->v.velocity[1] = sv_player->v.movedir[1];
}

/*
===================
SV_NoclipMove -- johnfitz

new, alternate noclip. old noclip is still handled in SV_AirMove
===================
*/
void SV_NoclipMove (void)
{
	AngleVectors (sv_player->v.v_angle, forward, right, up);

	velocity[0] = forward[0]*cmd.forwardmove + right[0]*cmd.sidemove;
	velocity[1] = forward[1]*cmd.forwardmove + right[1]*cmd.sidemove;
	velocity[2] = forward[2]*cmd.forwardmove + right[2]*cmd.sidemove;
	velocity[2] += cmd.upmove*2; //doubled to match running speed

	if (VectorLength (velocity) > sv_maxspeed.value)
	{
		VectorNormalize (velocity);
		VectorScale (velocity, sv_maxspeed.value, velocity);
	}
}

static void SV_UpdateSpeedInfo (void) // woods #speedometer
{
	float fmove, smove;

	fmove = cmd.forwardmove;
	smove = cmd.sidemove;

	// If jumping record whether directional keys pressed, to indicate a bad power bunny hop.
	if (onground && sv_player->v.button2)
	{
 		speed_info.jump_fmove = fmove;
		speed_info.jump_smove = smove;
	}

	speed_info.speed = sqrt(velocity[0] * velocity[0] + velocity[1] * velocity[1]);
}

/*
===================
SV_AirMove
===================
*/
void SV_AirMove (void)
{
	int			i;
	vec3_t		wishvel, wishdir;
	float		wishspeed;
	float		fmove, smove;

	AngleVectors (sv_player->v.angles, forward, right, up);

	fmove = cmd.forwardmove;
	smove = cmd.sidemove;

// hack to not let you back into teleporter
	if (qcvm->time < sv_player->v.teleport_time && fmove < 0)
		fmove = 0;

	for (i=0 ; i<3 ; i++)
		wishvel[i] = forward[i]*fmove + right[i]*smove;

	if ( (int)sv_player->v.movetype != MOVETYPE_WALK)
		wishvel[2] = cmd.upmove;
	else
		wishvel[2] = 0;

	VectorCopy (wishvel, wishdir);
	wishspeed = VectorNormalize(wishdir);
	if (wishspeed > sv_maxspeed.value)
	{
		VectorScale (wishvel, sv_maxspeed.value/wishspeed, wishvel);
		wishspeed = sv_maxspeed.value;
	}

	if ( sv_player->v.movetype == MOVETYPE_NOCLIP)
	{	// noclip
		VectorCopy (wishvel, velocity);
	}
	else if (onground && !(sv_bunnyhopqw.value && !prevonground)) // woods #qwbunnyhop
	{
		SV_UserFriction ();
		SV_Accelerate (wishspeed, wishdir);
	}
	else
	{	// not on ground, so little effect on velocity
		SV_AirAccelerate (wishspeed, wishvel);
	}
}

/*
===================
SV_ClientThink

the move fields specify an intended velocity in pix/sec
the angle fields specify an exact angular motion in degrees
===================
*/
void SV_ClientThink (void)
{
	vec3_t		v_angle;

	if (sv_player->v.movetype == MOVETYPE_NONE)
		return;

	prevonground = onground; // woods #qwbunnyhop
	onground = (int)sv_player->v.flags & FL_ONGROUND;

	origin = sv_player->v.origin;
	velocity = sv_player->v.velocity;

	for (int i = 0;i < 3;i++) velocitybeforethink[i] = velocity[i]; // woods #qwbunnyhop

	DropPunchAngle ();

	if (host_client->usingpmove)
	{
		if (!sv_player->v.fixangle) // woods update self.angles engine-side
		{
			VectorAdd(sv_player->v.v_angle, sv_player->v.punchangle, v_angle);
			sv_player->v.angles[ROLL] = V_CalcRoll(sv_player->v.angles, sv_player->v.velocity) * 4;
			sv_player->v.angles[PITCH] = -v_angle[PITCH] / 3;
			sv_player->v.angles[YAW] = v_angle[YAW];
		}
		return;	//this stuff is handled on inputs. don't corrupt anything.
	}

//
// if dead, behave differently
//
	if (sv_player->v.health <= 0)
		return;

//
// angles
// show 1/3 the pitch angle and all the roll angle
	cmd = host_client->cmd;
	angles = sv_player->v.angles;

	VectorAdd (sv_player->v.v_angle, sv_player->v.punchangle, v_angle);
	angles[ROLL] = V_CalcRoll (sv_player->v.angles, sv_player->v.velocity)*4;
	if (!sv_player->v.fixangle)
	{
		angles[PITCH] = -v_angle[PITCH]/3;
		angles[YAW] = v_angle[YAW];
	}

	if ( (int)sv_player->v.flags & FL_WATERJUMP )
	{
		SV_WaterJump ();
		return;
	}
//
// walk
//
	//johnfitz -- alternate noclip
	if (host_client->skate_active && sv_player->v.movetype == MOVETYPE_WALK &&
		sv_player->v.waterlevel < 2 && !sv_player->onladder)
		SV_SkateMove(host_client, sv_player, &cmd, host_frametime);
	else if (sv_player->v.movetype == MOVETYPE_NOCLIP && sv_altnoclip.value)
		SV_NoclipMove ();
	else if ((sv_player->v.waterlevel >= 2||sv_player->onladder) && sv_player->v.movetype != MOVETYPE_NOCLIP)
		SV_WaterMove ();
	else
		SV_AirMove ();
	//johnfitz

	SV_UpdateSpeedInfo (); // woods #speedometer
}


/*
===================
Skating: server-authoritative horizontal motion and one non-solid board.
The player's origin/bounds stay untouched; only the visible models are lifted.
===================
*/
qboolean SV_IsSkateBoard (const edict_t *ent)
{
	return ent->v.classname && !strcmp(PR_GetString(ent->v.classname), SKATE_CLASSNAME);
}

static qboolean SV_SkateBoardValid (const client_t *client)
{
	uintptr_t base = (uintptr_t)sv.qcvm.edicts;
	uintptr_t board = (uintptr_t)client->skate_board;
	size_t bytes = sv.qcvm.num_edicts * sv.qcvm.edict_size;

	if (!base || !sv.qcvm.edict_size || board < base || board - base >= bytes ||
		(board - base) % sv.qcvm.edict_size)
		return false;
	return !client->skate_board->free && SV_IsSkateBoard(client->skate_board) &&
		client->edict && client->skate_board->v.owner == EDICT_TO_PROG(client->edict);
}

enum
{
	SKATE_ANIM_NONE,
	SKATE_ANIM_CRUISE,
	SKATE_ANIM_ACCELERATE,
	SKATE_ANIM_SHOOT,
	SKATE_ANIM_JUMP_ASCENT,
	SKATE_ANIM_JUMP_FALL,
	SKATE_ANIM_LANDING
};

static void SV_SkateAnimationReset (client_t *client)
{
	client->skate_anim_state = SKATE_ANIM_NONE;
	client->skate_anim_frame = 0;
	client->skate_anim_next_frame_time = 0;
}

static void SV_SkateAnimationBegin (client_t *client, int state, int frame, double now)
{
	client->skate_anim_state = state;
	client->skate_anim_frame = frame;
	client->skate_anim_next_frame_time = now + SKATE_ANIM_FRAME_TIME;
}

static int SV_SkateAnimationSteps (client_t *client, double now)
{
	int steps;
	double elapsed;

	if (client->skate_anim_next_frame_time <= 0)
	{
		client->skate_anim_next_frame_time = now + SKATE_ANIM_FRAME_TIME;
		return 0;
	}
	elapsed = now - client->skate_anim_next_frame_time;
	if (elapsed < -0.000001)
		return 0;
	steps = (int)(elapsed / SKATE_ANIM_FRAME_TIME) + 1;
	if (steps < 1)
		steps = 1;
	client->skate_anim_next_frame_time += steps * SKATE_ANIM_FRAME_TIME;
	return steps;
}

static int SV_SkateAnimationFrame (client_t *client, const edict_t *ent)
{
	qboolean grounded = !!((int)ent->v.flags & FL_ONGROUND);
	qboolean rising = ent->v.velocity[2] > 0;
	qboolean attack = !!ent->v.button0;
	float speed = sqrtf(ent->v.velocity[0] * ent->v.velocity[0] +
		ent->v.velocity[1] * ent->v.velocity[1]);
	double now = qcvm->time;
	int state = client->skate_anim_state;
	int steps, i, target_state, first, last;

	// Airborne poses take priority over firing and locomotion. A jump climbs
	// through 150-154, cycles 154-155 while falling, then lands on 156-158.
	if (!grounded || rising)
	{
		if (state != SKATE_ANIM_JUMP_ASCENT && state != SKATE_ANIM_JUMP_FALL)
			SV_SkateAnimationBegin(client,
				rising ? SKATE_ANIM_JUMP_ASCENT : SKATE_ANIM_JUMP_FALL,
				rising ? SKATE_ANIM_JUMP_FIRST : SKATE_ANIM_JUMP_PEAK, now);
		else if (state == SKATE_ANIM_JUMP_FALL && rising)
			SV_SkateAnimationBegin(client, SKATE_ANIM_JUMP_ASCENT,
				SKATE_ANIM_JUMP_FIRST, now);

		state = client->skate_anim_state;
		if (state == SKATE_ANIM_JUMP_ASCENT)
		{
			if (!rising)
				SV_SkateAnimationBegin(client, SKATE_ANIM_JUMP_FALL,
					SKATE_ANIM_JUMP_PEAK, now);
			else
			{
				steps = SV_SkateAnimationSteps(client, now);
				client->skate_anim_frame += steps;
				if (client->skate_anim_frame > SKATE_ANIM_JUMP_PEAK)
					client->skate_anim_frame = SKATE_ANIM_JUMP_PEAK;
			}
		}
		else if (state == SKATE_ANIM_JUMP_FALL)
		{
			steps = SV_SkateAnimationSteps(client, now);
			if (steps & 1)
				client->skate_anim_frame = client->skate_anim_frame == SKATE_ANIM_JUMP_PEAK ?
					SKATE_ANIM_JUMP_FALL_LAST : SKATE_ANIM_JUMP_PEAK;
		}
		return client->skate_anim_frame;
	}

	if (client->skate_anim_state == SKATE_ANIM_JUMP_ASCENT ||
		client->skate_anim_state == SKATE_ANIM_JUMP_FALL)
	{
		SV_SkateAnimationBegin(client, SKATE_ANIM_LANDING,
			SKATE_ANIM_JUMP_LAND_FIRST, now);
		return client->skate_anim_frame;
	}

	if (client->skate_anim_state == SKATE_ANIM_LANDING)
	{
		steps = SV_SkateAnimationSteps(client, now);
		for (i = 0; i < steps; i++)
		{
			if (client->skate_anim_frame < SKATE_ANIM_JUMP_LAND_LAST)
				client->skate_anim_frame++;
			else
			{
				SV_SkateAnimationReset(client);
				break;
			}
		}
		if (client->skate_anim_state == SKATE_ANIM_LANDING)
			return client->skate_anim_frame;
	}

	// A held attack loops the four firing poses. A tap finishes its current
	// 170-173 cycle before returning to the appropriate riding animation.
	if (client->skate_anim_state == SKATE_ANIM_SHOOT)
	{
		steps = SV_SkateAnimationSteps(client, now);
		for (i = 0; i < steps; i++)
		{
			if (client->skate_anim_frame < SKATE_ANIM_SHOOT_LAST)
				client->skate_anim_frame++;
			else if (attack)
				client->skate_anim_frame = SKATE_ANIM_SHOOT_FIRST;
			else
			{
				SV_SkateAnimationReset(client);
				break;
			}
		}
		if (client->skate_anim_state == SKATE_ANIM_SHOOT)
			return client->skate_anim_frame;
	}
	if (attack)
	{
		SV_SkateAnimationBegin(client, SKATE_ANIM_SHOOT,
			SKATE_ANIM_SHOOT_FIRST, now);
		return client->skate_anim_frame;
	}

	// Acceleration plays while the board is moving below top speed and the
	// rider is not braking. At rest, while braking or at top speed, use the
	// same cruising/idle loop (159-169).
	if (client->cmd.forwardmove >= -1 && speed > SKATE_ANIM_SPEED_EPSILON &&
		speed < SKATE_MAX_SPEED - SKATE_ANIM_SPEED_EPSILON)
	{
		target_state = SKATE_ANIM_ACCELERATE;
		first = SKATE_ANIM_ACCEL_FIRST;
		last = SKATE_ANIM_ACCEL_LAST;
	}
	else
	{
		target_state = SKATE_ANIM_CRUISE;
		first = SKATE_ANIM_CRUISE_FIRST;
		last = SKATE_ANIM_CRUISE_LAST;
	}

	if (client->skate_anim_state != target_state)
		SV_SkateAnimationBegin(client, target_state, first, now);
	else
	{
		steps = SV_SkateAnimationSteps(client, now);
		for (i = 0; i < steps; i++)
			client->skate_anim_frame = client->skate_anim_frame < last ?
				client->skate_anim_frame + 1 : first;
	}
	return client->skate_anim_frame;
}

static void SV_SkateSlideClear (client_t *client)
{
	client->skate_sliding = false;
	VectorClear(client->skate_slide_normal);
	client->skate_slide_surface = NULL;
	client->skate_slide_edge_height = 0;
}

void SV_SkateStop (client_t *client)
{
	if (client->skate_board && sv.qcvm.edicts)
	{
		qcvm_t *oldvm = qcvm;
		if (oldvm != &sv.qcvm)
		{
			PR_SwitchQCVM(NULL);
			PR_SwitchQCVM(&sv.qcvm);
		}
		if (SV_SkateBoardValid(client))
			ED_Free(client->skate_board);
		if (oldvm != &sv.qcvm)
		{
			PR_SwitchQCVM(NULL);
			PR_SwitchQCVM(oldvm);
		}
	}
	client->skate_active = false;
	client->skate_board = NULL;
	client->skate_lift = 0;
	client->skate_yaw = 0;
	client->skate_grounded = false;
	client->skate_jump_active = false;
	client->skate_slide_held = false;
	SV_SkateSlideClear(client);
	SV_SkateAnimationReset(client);
	client->skate_flip_active = false;
	client->skate_flip_start_time = 0;
}

int SV_SkateStat (const client_t *client)
{
	if (!client->skate_active || !(client->skate_lift >= 0 && client->skate_lift <= SKATE_MAX_LIFT))
		return 0;
	return (int)(SKATE_STAT_MAGIC | (unsigned int)(client->skate_lift * SKATE_HEIGHT_SCALE + 0.5f));
}

static qboolean SV_SkateCanRide (const edict_t *ent)
{
	return ent && !ent->free && ent->v.health > 0 && !ent->v.deadflag &&
		ent->v.movetype == MOVETYPE_WALK && ent->v.waterlevel < 2 && !ent->onladder;
}

static float SV_SkateGroundLift (edict_t *ent, const qmodel_t *model)
{
	float lift = q_max(SKATE_GROUND_CLEARANCE,
		ent->v.mins[2] - model->mins[2] + SKATE_GROUND_CLEARANCE);

	// Keep the entire board above steps/slopes, not just its center. Do not
	// trace in midair: a passing platform must not pull the board off the feet.
	if ((int)ent->v.flags & FL_ONGROUND)
	{
		vec3_t start, end, fwd, side, up, a = {0, 0, 0};
		int x, y;
		a[YAW] = ent->v.angles[YAW];
		AngleVectors(a, fwd, side, up);
		for (x = 0; x < 2; x++)
			for (y = 0; y < 2; y++)
			{
				float mx = x ? model->maxs[0] : model->mins[0];
				float my = y ? model->maxs[1] : model->mins[1];
				trace_t trace;
				VectorCopy(ent->v.origin, start);
				VectorMA(start, mx, fwd, start);
				VectorMA(start, -my, side, start);
				VectorCopy(start, end);
				start[2] += ent->v.maxs[2] + 8;
				end[2] += ent->v.mins[2] - SKATE_MAX_LIFT;
				trace = SV_Move(start, vec3_origin, vec3_origin, end, MOVE_NOMONSTERS, ent);
				if (!trace.startsolid && !trace.allsolid && trace.fraction < 1)
					lift = q_max(lift, trace.endpos[2] - ent->v.origin[2] - model->mins[2] + SKATE_GROUND_CLEARANCE);
			}
	}
	return CLAMP(SKATE_GROUND_CLEARANCE, lift, SKATE_MAX_LIFT);
}

static float SV_SkateJumpRoll (client_t *client, const edict_t *ent)
{
	qboolean grounded = !!((int)ent->v.flags & FL_ONGROUND);
	double elapsed;

	if (grounded)
	{
		client->skate_jump_active = false;
		client->skate_flip_active = false;
		client->skate_flip_start_time = 0;
	}
	else if (client->skate_grounded && ent->v.velocity[2] > 0 &&
		!((int)ent->v.flags & FL_WATERJUMP))
	{
		// An upward takeoff, not merely holding jump or falling off an edge.
		// Normal QuakeC/native jump physics have already run at this point.
		client->skate_jump_active = true;
		client->skate_flip_active = true;
		client->skate_flip_start_time = qcvm->time;
	}
	client->skate_grounded = grounded;
	if (!client->skate_flip_active)
		return 0;

	elapsed = qcvm->time - client->skate_flip_start_time;
	if (elapsed < 0 || elapsed >= SKATE_FLIP_DURATION)
	{
		// Zero is the same orientation as 360. Stay upright until the next
		// actual takeoff; landing (including a low ceiling) also resets us.
		client->skate_flip_active = false;
		client->skate_flip_start_time = 0;
		return 0;
	}
	return 360.0f * (elapsed / SKATE_FLIP_DURATION);
}

void SV_SkateUpdate (client_t *client)
{
	edict_t *ent = client->edict, *board = client->skate_board;
	qmodel_t *model;
	if (!client->skate_active)
		return;
	if (!client->active || !client->spawned || !SV_SkateCanRide(ent) || !SV_SkateBoardValid(client) ||
		sv.skate_modelindex <= 0 || sv.skate_modelindex >= MAX_MODELS || !(model = sv.models[sv.skate_modelindex]))
	{
		SV_SkateStop(client);
		return;
	}

	// Keep the rider upright; only the board rolls during a jump.
	// Both still share the prepared model origin and heading on the wire.
	ent->v.angles[PITCH] = ent->v.angles[ROLL] = 0;
	ent->v.angles[YAW] = client->skate_yaw;
	board->v.angles[ROLL] = SV_SkateJumpRoll(client, ent);
	ent->v.frame = SV_SkateAnimationFrame(client, ent);
	client->skate_lift = SV_SkateGroundLift(ent, model);
	VectorCopy(ent->v.origin, board->v.origin);
	board->v.origin[2] += client->skate_lift;
	board->v.angles[PITCH] = 0;
	board->v.angles[YAW] = client->skate_yaw;
	SV_LinkEdict(board, false);
}

void SV_SkateMove (client_t *client, edict_t *ent, const usercmd_t *move, double dt)
{
	vec3_t fwd, side, up, a = {0, 0, 0};
	float desired, delta, step, forwardspeed, sidespeed, speed;
	if (!client->skate_active || !SV_SkateCanRide(ent) || dt <= 0)
		return;
	dt = q_min(dt, 0.1);
	if (!client->skate_sliding)
	{
		desired = ent->v.v_angle[YAW] - CLAMP(-1.0f, move->sidemove / SKATE_INPUT_SCALE, 1.0f) * SKATE_STEER_ANGLE;
		delta = desired - client->skate_yaw;
		delta -= 360.0f * floorf((delta + 180.0f) / 360.0f);
		step = SKATE_TURN_RATE * dt;
		client->skate_yaw = anglemod(client->skate_yaw + CLAMP(-step, delta, step));
	}
	ent->v.angles[PITCH] = ent->v.angles[ROLL] = 0;
	ent->v.angles[YAW] = client->skate_yaw;

	a[YAW] = client->skate_yaw;
	AngleVectors(a, fwd, side, up);

	if (client->skate_sliding)
	{
		float tx = -client->skate_slide_normal[1];
		float ty = client->skate_slide_normal[0];
		float slide_speed = ent->v.velocity[0] * tx + ent->v.velocity[1] * ty;
		float forward_projection = fwd[0] * tx + fwd[1] * ty;

		// Keep momentum along the wall; ordinary lateral skate friction would
		// otherwise kill a grind in a few frames. The player can still brake or
		// add speed when their board points along the face.
		slide_speed *= expf(-SKATE_ROLLING_DRAG * dt);
		if (move->forwardmove < -1)
		{
			slide_speed *= expf(-SKATE_BRAKING * dt);
			if (fabsf(slide_speed) < 2)
				slide_speed = 0;
		}
		else
			slide_speed += SKATE_ACCELERATION *
				(1.0f + 0.3f * CLAMP(0.0f, move->forwardmove / SKATE_INPUT_SCALE, 1.0f)) *
				forward_projection * dt;

		slide_speed = CLAMP(-SKATE_MAX_SPEED, slide_speed, SKATE_MAX_SPEED);
		ent->v.velocity[0] = tx * slide_speed;
		ent->v.velocity[1] = ty * slide_speed;
		if (fabsf(slide_speed) > SKATE_SLIDE_DIRECTION_EPSILON)
		{
			client->skate_yaw = anglemod(atan2f(ent->v.velocity[1], ent->v.velocity[0]) / M_PI_DIV_180);
			ent->v.angles[YAW] = client->skate_yaw;
		}
		return;
	}

	if (!((int)ent->v.flags & FL_ONGROUND) || ((int)ent->v.flags & FL_WATERJUMP))
		return; // retain airborne momentum; native gravity/jump/collision still run

	forwardspeed = ent->v.velocity[0] * fwd[0] + ent->v.velocity[1] * fwd[1];
	sidespeed = ent->v.velocity[0] * side[0] + ent->v.velocity[1] * side[1];
	forwardspeed *= expf(-SKATE_ROLLING_DRAG * dt);
	sidespeed *= expf(-SKATE_SIDE_TRACTION * dt);
	if (move->forwardmove < -1)
	{
		forwardspeed *= expf(-SKATE_BRAKING * dt);
		sidespeed *= expf(-SKATE_BRAKING * dt);
		if (fabsf(forwardspeed) < 2) forwardspeed = 0;
		if (fabsf(sidespeed) < 2) sidespeed = 0;
	}
	else
		forwardspeed += SKATE_ACCELERATION * (1.0f + 0.3f * CLAMP(0.0f, move->forwardmove / SKATE_INPUT_SCALE, 1.0f)) * dt;
	ent->v.velocity[0] = fwd[0] * forwardspeed + side[0] * sidespeed;
	ent->v.velocity[1] = fwd[1] * forwardspeed + side[1] * sidespeed;
	speed = sqrtf(ent->v.velocity[0] * ent->v.velocity[0] + ent->v.velocity[1] * ent->v.velocity[1]);
	if (speed > SKATE_MAX_SPEED)
	{
		ent->v.velocity[0] *= SKATE_MAX_SPEED / speed;
		ent->v.velocity[1] *= SKATE_MAX_SPEED / speed;
	}
}

static client_t *SV_SkateClientForEnt (const edict_t *ent)
{
	int i;

	if (!ent || !svs.clients || svs.maxclients <= 0)
		return NULL;
	for (i = 0; i < svs.maxclients; i++)
		if (svs.clients[i].edict == ent)
			return &svs.clients[i];
	return NULL;
}

static qboolean SV_SkateFlatNormal (const vec3_t normal, float *nx, float *ny)
{
	float length;

	if (!normal || fabsf(normal[2]) > SKATE_WALL_BOUNCE_MAX_NORMAL_Z)
		return false;
	length = sqrtf(normal[0] * normal[0] + normal[1] * normal[1]);
	if (length < 0.9f)
		return false;
	*nx = normal[0] / length;
	*ny = normal[1] / length;
	return true;
}

static qboolean SV_SkateWallFaceProbe (edict_t *ent, edict_t *surface,
	float nx, float ny, float height, vec3_t hitpoint)
{
	vec3_t start, end, point, normal = {0, 0, 0}, zero = {0, 0, 0};
	trace_t trace;
	float hit_nx, hit_ny;

	normal[0] = nx;
	normal[1] = ny;
	VectorCopy(ent->v.origin, point);
	point[2] += height;
	VectorMA(point, SKATE_SLIDE_PROBE_DISTANCE, normal, start);
	VectorMA(point, -SKATE_SLIDE_PROBE_DISTANCE, normal, end);
	trace = SV_Move(start, zero, zero, end, MOVE_NOMONSTERS, ent);
	if (trace.fraction == 1 || trace.startsolid || trace.allsolid ||
		trace.ent != surface || !SV_SkateFlatNormal(trace.plane.normal, &hit_nx, &hit_ny))
		return false;
	if (hit_nx * nx + hit_ny * ny < SKATE_SLIDE_MIN_NORMAL_DOT)
		return false;
	if (hitpoint)
		VectorCopy(trace.endpos, hitpoint);
	return true;
}

static qboolean SV_SkateSlideFaceProbe (edict_t *ent, client_t *client,
	float height, vec3_t hitpoint)
{
	return SV_SkateWallFaceProbe(ent, client->skate_slide_surface,
		client->skate_slide_normal[0], client->skate_slide_normal[1], height, hitpoint);
}

static float SV_SkateWallRadius (const edict_t *ent, float nx, float ny)
{
	float radius = -(nx * (nx > 0 ? ent->v.mins[0] : ent->v.maxs[0]) +
		ny * (ny > 0 ? ent->v.mins[1] : ent->v.maxs[1]));
	return q_max(0.0f, radius);
}

static qboolean SV_SkateSlideTopProbe (edict_t *ent, edict_t *surface,
	float nx, float ny, float inset, float start_z, float end_z, float *top_z)
{
	vec3_t start, end, zero = {0, 0, 0};
	trace_t trace;

	start[0] = end[0] = ent->v.origin[0] - nx * inset;
	start[1] = end[1] = ent->v.origin[1] - ny * inset;
	start[2] = start_z;
	end[2] = end_z;
	trace = SV_Move(start, zero, zero, end, MOVE_NOMONSTERS, ent);
	if (trace.fraction == 1 || trace.startsolid || trace.allsolid ||
		trace.ent != surface || trace.plane.normal[2] < SKATE_SLIDE_EDGE_TOP_NORMAL_Z)
		return false;
	*top_z = trace.endpos[2];
	return true;
}

static qboolean SV_SkateFindTopEdge (edict_t *ent, edict_t *surface,
	float nx, float ny, float *edge_height)
{
	vec3_t wallpoint;
	float feet_z = ent->v.origin[2] + ent->v.mins[2];
	float radius = SV_SkateWallRadius(ent, nx, ny);
	float inset = radius + SKATE_SLIDE_EDGE_INSET;
	float height, wall_distance;

	if (!SV_SkateSlideTopProbe(ent, surface, nx, ny, inset,
		feet_z + SKATE_SLIDE_EDGE_SNAP_UP + SKATE_SLIDE_EDGE_SEARCH_PADDING,
		feet_z - SKATE_SLIDE_EDGE_SNAP_DOWN - SKATE_SLIDE_EDGE_SEARCH_PADDING,
		&height) || height < feet_z - SKATE_SLIDE_EDGE_SNAP_DOWN ||
		height > feet_z + SKATE_SLIDE_EDGE_SNAP_UP + SKATE_SLIDE_EDGE_SEARCH_PADDING)
		return false;

	// A real upper corner has the same nearby wall face below the cap, but
	// not above it. This rejects remote faces, floors and points halfway up a wall.
	if (!SV_SkateWallFaceProbe(ent, surface, nx, ny,
		height - SKATE_SLIDE_EDGE_SIDE_DEPTH - ent->v.origin[2], wallpoint))
		return false;
	wall_distance = (ent->v.origin[0] - wallpoint[0]) * nx +
		(ent->v.origin[1] - wallpoint[1]) * ny;
	if (fabsf(wall_distance - radius) > SKATE_SLIDE_MAX_ADHESION_DISTANCE ||
		SV_SkateWallFaceProbe(ent, surface, nx, ny,
		height + SKATE_SLIDE_EDGE_SIDE_DEPTH - ent->v.origin[2], NULL))
		return false;
	*edge_height = height;
	return true;
}

qboolean SV_SkateWallSlideMove (edict_t *ent)
{
	client_t *client = SV_SkateClientForEnt(ent);
	vec3_t point, start, end;
	float nx, ny, distance, correction, target_z, top_z, normal_speed;
	trace_t trace;

	if (!client || !client->skate_sliding)
		return false;
	if (!client->active || !client->skate_active || !client->skate_slide_held ||
		((int)ent->v.flags & FL_WATERJUMP) ||
		!SV_SkateFlatNormal(client->skate_slide_normal, &nx, &ny))
	{
		SV_SkateSlideClear(client);
		return false;
	}

	// The grind is attached to the wall's top corner, not anywhere on its side.
	// Require both the top cap and the vertical side just below its lip.
	if (!SV_SkateSlideTopProbe(ent, client->skate_slide_surface, nx, ny,
		SKATE_SLIDE_EDGE_INSET,
		client->skate_slide_edge_height + SKATE_SLIDE_EDGE_PROBE_DEPTH,
		client->skate_slide_edge_height - SKATE_SLIDE_EDGE_PROBE_DEPTH,
		&top_z) ||
		fabsf(top_z - client->skate_slide_edge_height) > SKATE_SLIDE_EDGE_HEIGHT_TOLERANCE ||
		!SV_SkateSlideFaceProbe(ent, client,
		top_z - SKATE_SLIDE_EDGE_SIDE_DEPTH - ent->v.origin[2], point) ||
		SV_SkateWallFaceProbe(ent, client->skate_slide_surface, nx, ny,
		top_z + SKATE_SLIDE_EDGE_SIDE_DEPTH - ent->v.origin[2], NULL))
	{
		SV_SkateSlideClear(client);
		return false;
	}
	client->skate_slide_edge_height = top_z;

	// Keep the player's center over the corner and the bottom just above the
	// top cap. The hull is now above the brush, so this can follow small
	// vertical/normal corrections without scraping along the side face.
	distance = (ent->v.origin[0] - point[0]) * nx +
		(ent->v.origin[1] - point[1]) * ny;
	correction = SKATE_SLIDE_EDGE_OFFSET - distance;
	target_z = top_z - ent->v.mins[2] + SKATE_SLIDE_SURFACE_GAP;
	if (fabsf(correction) > SKATE_SLIDE_MAX_ADHESION_DISTANCE ||
		fabsf(target_z - ent->v.origin[2]) > SKATE_SLIDE_MAX_ADHESION_DISTANCE)
	{
		SV_SkateSlideClear(client);
		return false;
	}
	if (fabsf(correction) > 0.01f || fabsf(target_z - ent->v.origin[2]) > 0.01f)
	{
		VectorCopy(ent->v.origin, start);
		VectorCopy(start, end);
		end[0] += nx * correction;
		end[1] += ny * correction;
		end[2] = target_z;
		trace = SV_Move(start, ent->v.mins, ent->v.maxs, end, MOVE_NORMAL, ent);
		if (trace.startsolid || trace.allsolid || trace.fraction != 1)
		{
			SV_SkateSlideClear(client);
			return false;
		}
		VectorCopy(trace.endpos, ent->v.origin);
	}

	ent->v.velocity[2] = 0;
	ent->v.flags = (int)ent->v.flags | FL_ONGROUND;
	ent->v.groundentity = EDICT_TO_PROG(client->skate_slide_surface);
	// Keep normal movement parallel to the corner; tangent speed is conserved.
	normal_speed = ent->v.velocity[0] * nx + ent->v.velocity[1] * ny;
	ent->v.velocity[0] -= nx * normal_speed;
	ent->v.velocity[1] -= ny * normal_speed;
	return true;
}

qboolean SV_SkateWallSlide (edict_t *ent, edict_t *surface,
	const vec3_t normal, const vec3_t incoming_velocity)
{
	client_t *client = SV_SkateClientForEnt(ent);
	vec3_t angles = {0, 0, 0}, forward, side, up;
	vec3_t start, edge_origin, target_origin;
	float nx, ny, speed, normal_speed, edge_height, radius;
	float tangent_speed, side_projection, forward_projection, slide_speed, direction;
	trace_t trace;

	if (!client || !ent || !normal || !incoming_velocity)
		return false;
	if (client->skate_sliding && (!client->skate_active || !client->skate_slide_held))
		SV_SkateSlideClear(client);
	if (!client->active || !client->skate_active || !client->skate_slide_held ||
		((int)ent->v.flags & FL_WATERJUMP) || !surface ||
		surface->v.solid != SOLID_BSP || !SV_SkateFlatNormal(normal, &nx, &ny))
		return false;

	if (client->skate_sliding)
	{
		// Keep the same edge latched through incidental contact at any speed.
		// A different face or brush ends the grind instead of wrapping a corner.
		if (surface != client->skate_slide_surface ||
			nx * client->skate_slide_normal[0] + ny * client->skate_slide_normal[1] <
				SKATE_SLIDE_MIN_NORMAL_DOT)
		{
			SV_SkateSlideClear(client);
			return false;
		}
		normal_speed = ent->v.velocity[0] * client->skate_slide_normal[0] +
			ent->v.velocity[1] * client->skate_slide_normal[1];
		ent->v.velocity[0] -= client->skate_slide_normal[0] * normal_speed;
		ent->v.velocity[1] -= client->skate_slide_normal[1] * normal_speed;
		ent->v.velocity[2] = 0;
		ent->v.flags = (int)ent->v.flags | FL_ONGROUND;
		ent->v.groundentity = EDICT_TO_PROG(surface);
		return true;
	}

	speed = sqrtf(incoming_velocity[0] * incoming_velocity[0] +
		incoming_velocity[1] * incoming_velocity[1]);
	normal_speed = incoming_velocity[0] * nx + incoming_velocity[1] * ny;
	if (speed < SKATE_SLIDE_MIN_SPEED ||
		normal_speed > -SKATE_SLIDE_MIN_IMPACT_SPEED)
		return false;
	if (!SV_SkateFindTopEdge(ent, surface, nx, ny, &edge_height))
		return false;

	// First lift clear of the wall, then move the rider's center onto the
	// upper corner. Doing this in two traces avoids sweeping the hull through
	// the brush side while climbing to the lip.
	radius = SV_SkateWallRadius(ent, nx, ny);
	VectorCopy(ent->v.origin, start);
	VectorCopy(start, edge_origin);
	edge_origin[0] += nx * SKATE_SLIDE_SURFACE_GAP;
	edge_origin[1] += ny * SKATE_SLIDE_SURFACE_GAP;
	edge_origin[2] = edge_height - ent->v.mins[2] + SKATE_SLIDE_SURFACE_GAP;
	trace = SV_Move(start, ent->v.mins, ent->v.maxs, edge_origin, MOVE_NORMAL, ent);
	if (trace.startsolid || trace.allsolid || trace.fraction != 1)
		return false;
	VectorCopy(trace.endpos, edge_origin);

	VectorCopy(edge_origin, target_origin);
	target_origin[0] += nx * (SKATE_SLIDE_EDGE_OFFSET - radius - SKATE_SLIDE_SURFACE_GAP);
	target_origin[1] += ny * (SKATE_SLIDE_EDGE_OFFSET - radius - SKATE_SLIDE_SURFACE_GAP);
	trace = SV_Move(edge_origin, ent->v.mins, ent->v.maxs, target_origin, MOVE_NORMAL, ent);
	if (trace.startsolid || trace.allsolid || trace.fraction != 1)
		return false;
	VectorCopy(trace.endpos, ent->v.origin);

	tangent_speed = incoming_velocity[0] * (-ny) + incoming_velocity[1] * nx;
	angles[YAW] = client->skate_yaw;
	AngleVectors(angles, forward, side, up);
	side_projection = side[0] * (-ny) + side[1] * nx;
	forward_projection = forward[0] * (-ny) + forward[1] * nx;
	if (fabsf(tangent_speed) > SKATE_SLIDE_DIRECTION_EPSILON)
		direction = tangent_speed > 0 ? 1 : -1;
	else if (fabsf(client->cmd.sidemove) > 1)
		direction = fabsf(side_projection) > 0.1f ?
			(client->cmd.sidemove * side_projection > 0 ? 1 : -1) :
			(client->cmd.sidemove > 0 ? 1 : -1);
	else if (fabsf(forward_projection) > 0.1f)
		direction = forward_projection > 0 ? 1 : -1;
	else
		direction = side_projection >= 0 ? 1 : -1;

	slide_speed = q_max(fabsf(tangent_speed), speed * SKATE_SLIDE_SPEED_SCALE);
	slide_speed = q_min(slide_speed, SKATE_MAX_SPEED);
	ent->v.velocity[0] = -ny * slide_speed * direction;
	ent->v.velocity[1] = nx * slide_speed * direction;
	client->skate_yaw = anglemod(atan2f(ent->v.velocity[1], ent->v.velocity[0]) / M_PI_DIV_180);
	ent->v.angles[YAW] = client->skate_yaw;
	client->skate_sliding = true;
	client->skate_slide_normal[0] = nx;
	client->skate_slide_normal[1] = ny;
	client->skate_slide_normal[2] = 0;
	client->skate_slide_surface = surface;
	client->skate_slide_edge_height = edge_height;
	ent->v.velocity[2] = 0;
	ent->v.flags = (int)ent->v.flags | FL_ONGROUND;
	ent->v.groundentity = EDICT_TO_PROG(surface);
	return true;
}

qboolean SV_SkateWallBounce (edict_t *ent, const vec3_t normal, const vec3_t incoming_velocity)
{
	client_t *client = SV_SkateClientForEnt(ent);
	float speed, normal_speed;

	if (!ent || !normal || !incoming_velocity || !client || !client->active ||
		!client->skate_active || client->edict != ent)
		return false;

	// Only rebound from vertical surfaces during a real jump, not from floors,
	// ceilings, walking into a wall, or simply falling off an edge.
	if (((int)ent->v.flags & (FL_ONGROUND | FL_WATERJUMP)) ||
		!(client->skate_jump_active || (client->skate_grounded && ent->v.velocity[2] > 0)))
		return false;

	if (fabsf(normal[2]) > SKATE_WALL_BOUNCE_MAX_NORMAL_Z)
		return false;

	speed = sqrtf(incoming_velocity[0] * incoming_velocity[0] +
		incoming_velocity[1] * incoming_velocity[1]);
	normal_speed = incoming_velocity[0] * normal[0] + incoming_velocity[1] * normal[1];
	if (speed < SKATE_WALL_BOUNCE_MIN_SPEED ||
		normal_speed > -SKATE_WALL_BOUNCE_MIN_IMPACT_SPEED)
		return false;

	// Reflect the horizontal impact velocity, losing a little energy. Preserve
	// some upward kick so a descending rider still pops away from the wall.
	ent->v.velocity[0] = incoming_velocity[0] -
		(1.0f + SKATE_WALL_BOUNCE_RESTITUTION) * normal_speed * normal[0];
	ent->v.velocity[1] = incoming_velocity[1] -
		(1.0f + SKATE_WALL_BOUNCE_RESTITUTION) * normal_speed * normal[1];
	if (ent->v.velocity[2] < SKATE_WALL_BOUNCE_UPWARD_SPEED)
		ent->v.velocity[2] = SKATE_WALL_BOUNCE_UPWARD_SPEED;
	return true;
}

static client_t *SV_SkateSlideCommandClient (qboolean require_skate)
{
	if (cmd_source != src_client)
	{
		Cmd_ForwardToServer();
		return NULL;
	}
	if (!host_client || !host_client->active || !host_client->spawned)
		return NULL;
	if (require_skate && !host_client->skate_active)
	{
		SV_ClientPrintf("Enable skate mode first with skate.\n");
		return NULL;
	}
	return host_client;
}

void SV_SkateSlide_f (void)
{
	client_t *client = SV_SkateSlideCommandClient(true);
	if (!client)
		return;
	client->skate_slide_held = !client->skate_slide_held;
	if (!client->skate_slide_held)
		SV_SkateSlideClear(client);
	SV_ClientPrintf("Skate slide %s\n", client->skate_slide_held ? "ON" : "OFF");
}

void SV_SkateSlideDown_f (void)
{
	client_t *client = SV_SkateSlideCommandClient(false);
	if (client && client->skate_active)
		client->skate_slide_held = true;
}

void SV_SkateSlideUp_f (void)
{
	if (cmd_source != src_client)
	{
		Cmd_ForwardToServer();
		return;
	}
	if (host_client)
	{
		host_client->skate_slide_held = false;
		SV_SkateSlideClear(host_client);
	}
}

void SV_Skate_f (void)
{
	edict_t *ent, *board;
	int i;
	qboolean enable;
	qmodel_t *model;
	if (cmd_source != src_client)
	{
		Cmd_ForwardToServer();
		return;
	}
	if (!host_client || !host_client->active || !host_client->spawned)
		return;
	if (Cmd_Argc() > 2 || (Cmd_Argc() == 2 && strcmp(Cmd_Argv(1), "0") && strcmp(Cmd_Argv(1), "1")))
	{
		SV_ClientPrintf("skate [0|1]: toggle skating, or explicitly disable/enable it\n");
		return;
	}
	enable = Cmd_Argc() == 1 ? !host_client->skate_active : !strcmp(Cmd_Argv(1), "1");
	if (!enable)
	{
		SV_SkateStop(host_client);
		SV_ClientPrintf("Skate OFF\n");
		return;
	}
	if (host_client->skate_active)
		return;
	ent = host_client->edict;
	if (!SV_SkateCanRide(ent))
	{
		SV_ClientPrintf("Skating requires a living, walking player outside deep water.\n");
		return;
	}
	if (sv.skate_modelindex <= 0 || sv.skate_modelindex >= MAX_MODELS ||
		!(model = sv.models[sv.skate_modelindex]) || model->type != mod_alias)
	{
		SV_ClientPrintf("Skate model missing: install progs/skate.mdl (or skate.mdl) and restart the map.\n");
		return;
	}
	if ((unsigned)sv.skate_modelindex >= host_client->limit_models)
	{
		SV_ClientPrintf("The skateboard exceeds this client's model limit.\n");
		return;
	}
	if (!(ent->v.mins[2] - model->mins[2] + SKATE_GROUND_CLEARANCE <= SKATE_MAX_LIFT))
	{
		SV_ClientPrintf("skate.mdl must use the same origin/scale as the player model.\n");
		return;
	}
	// Avoid a fatal ED_Alloc when a busy map has no reusable entity slots.
	if (qcvm->num_edicts >= qcvm->max_edicts)
	{
		for (i = qcvm->reserved_edicts; i < qcvm->num_edicts; i++)
		{
			edict_t *e = EDICT_NUM(i);
			if (e->free && (e->freetime < 2 || qcvm->time - e->freetime > 0.5))
				break;
		}
		if (i == qcvm->num_edicts)
		{
			SV_ClientPrintf("No free entity slot for the skateboard.\n");
			return;
		}
	}
	board = ED_Alloc();
	board->v.classname = PR_SetEngineString(SKATE_CLASSNAME);
	board->v.model = PR_SetEngineString(sv.model_precache[sv.skate_modelindex]);
	board->v.modelindex = sv.skate_modelindex;
	board->v.movetype = MOVETYPE_NOCLIP;
	board->v.solid = SOLID_NOT;
	board->v.owner = EDICT_TO_PROG(ent);
	// Stock colormap carries the player slot to all clients, including demos.
	board->v.colormap = NUM_FOR_EDICT(ent);
	VectorCopy(model->mins, board->v.mins);
	VectorCopy(model->maxs, board->v.maxs);
	VectorSubtract(board->v.maxs, board->v.mins, board->v.size);
	host_client->skate_board = board;
	host_client->skate_active = true;
	host_client->skate_yaw = ent->v.v_angle[YAW];
	host_client->skate_grounded = !!((int)ent->v.flags & FL_ONGROUND);
	host_client->skate_jump_active = false;
	host_client->skate_slide_held = false;
	SV_SkateSlideClear(host_client);
	SV_SkateAnimationReset(host_client);
	host_client->skate_flip_active = false;
	host_client->skate_flip_start_time = 0;
	host_client->usingpmove = false; // stock client prediction does not know skating
	SV_SkateUpdate(host_client);
	SV_ClientPrintf("Skate ON: mouse/A-D steer, S brakes, jump flips the board.\n");
}

/*
===================
SV_CigarettePreThink / SV_CigarettePostThink

Engine-side support for equipping progs/v_siga.mdl on impulse 9 without
firing a combat shot on +attack, and driving its smoking weaponframe
animation. Impulse 9 is consumed in SV_ReadClientMove before any QuakeC
entry point can run; impulse 101 is translated to the original QC cheat
impulse 9 without modifying progs.dat or depending on function names.
===================
*/
static void SV_ResetCigarette (client_t *client)
{
	client->siga_active = false;
	client->siga_smoking = false;
	client->siga_releasing = false;
	client->siga_attack_down = false;
	client->siga_frame = SIGA_IDLE_FRAME;
	client->siga_next_frame_time = 0;
}

void SV_CigarettePreThink (client_t *client, edict_t *ent, float *saved_button0, qboolean *equip_cigarette)
{
	*saved_button0 = ent->v.button0;
	*equip_cigarette = client->siga_pending;
	client->siga_pending = false;

	if (ent->v.health <= 0 || ent->v.deadflag)
	{
		*equip_cigarette = false;
		SV_ResetCigarette(client);
		return;
	}

	if (client->siga_active && q_strcasecmp(PR_GetString(ent->v.weaponmodel), SIGA_MODEL) != 0)
		SV_ResetCigarette(client);

	// Also suppress attack on the selection frame, before QC can fire the old weapon.
	if (client->siga_active || (*equip_cigarette && COM_FileExists(SIGA_MODEL, NULL)))
	{
		ent->v.button0 = 0;
		if (qcvm->extglobals.input_buttons)
			*qcvm->extglobals.input_buttons = (int)*qcvm->extglobals.input_buttons & ~1;
	}
}

void SV_CigarettePostThink (client_t *client, edict_t *ent, float saved_button0, qboolean equip_cigarette)
{
	int mod_idx, numframes, start_frame, hold_frame, end_frame;
	const double interval = SIGA_FRAME_INTERVAL;
	qmodel_t *mod;
	qboolean attack_down, attack_pressed;

	// The mask is only for QC execution. Retain held input for server ticks
	// without a new clc_move, otherwise they look like an accidental release.
	if (client->siga_active || equip_cigarette)
		ent->v.button0 = saved_button0;

	if (ent->v.health <= 0 || ent->v.deadflag)
	{
		SV_ResetCigarette(client);
		return;
	}

	if (equip_cigarette && COM_FileExists(SIGA_MODEL, NULL))
	{
		mod_idx = SV_Precache_Model(SIGA_MODEL);
		if (mod_idx > 0)
		{
			ent->v.weaponmodel = PR_SetEngineString(SIGA_MODEL);
			SV_ResetCigarette(client);
			client->siga_active = true;
		}
	}

	if (!client->siga_active)
		return;

	if (q_strcasecmp(PR_GetString(ent->v.weaponmodel), SIGA_MODEL) != 0)
	{
		SV_ResetCigarette(client);
		return;
	}

	mod_idx = SV_ModelIndex(SIGA_MODEL);
	mod = (mod_idx > 0 && mod_idx < MAX_MODELS) ? sv.models[mod_idx] : NULL;
	numframes = (mod && mod->numframes > 0) ? mod->numframes : 1;

	start_frame = CLAMP(0, SIGA_START_FRAME, numframes - 1);
	end_frame = numframes - 1;
	hold_frame = CLAMP(start_frame, SIGA_HOLD_FRAME, end_frame);

	attack_down = saved_button0 != 0;
	attack_pressed = attack_down && !client->siga_attack_down;
	client->siga_attack_down = attack_down;

	if (!client->siga_smoking)
	{
		client->siga_frame = SIGA_IDLE_FRAME;
		if (attack_pressed)
		{
			client->siga_smoking = true;
			client->siga_releasing = false;
			client->siga_frame = start_frame;
			client->siga_next_frame_time = qcvm->time + interval;
		}
	}

	if (client->siga_smoking)
	{
		client->siga_frame = CLAMP(start_frame, client->siga_frame, end_frame);
		if (!client->siga_releasing && !attack_down)
		{
			// Release latches the finish phase, even for a short tap. A new
			// press must not rewind it or freeze it again at the hold frame.
			client->siga_releasing = true;
			client->siga_next_frame_time = qcvm->time;
			if (client->siga_frame == end_frame)
				client->siga_next_frame_time += interval;
		}

		if (!client->siga_releasing && client->siga_frame > hold_frame)
			client->siga_frame = hold_frame;

		if (qcvm->time >= client->siga_next_frame_time)
		{
			if (client->siga_releasing)
			{
				if (client->siga_frame < end_frame)
				{
					client->siga_frame++;
					client->siga_next_frame_time = qcvm->time + interval;
				}
				else
				{
					client->siga_smoking = false;
					client->siga_releasing = false;
					client->siga_frame = SIGA_IDLE_FRAME;
					client->siga_next_frame_time = 0;
				}
			}
			else if (client->siga_frame < hold_frame)
			{
				client->siga_frame++;
				client->siga_next_frame_time = qcvm->time + interval;
			}
			// Otherwise keep the fixed hold frame until attack is released.
		}
	}

	ent->v.weaponframe = client->siga_frame;
	// Cosmetic HUD ammo only: never touch shells/nails/rockets/cells.
	// Reassert after QC so pickups or ammo refreshes cannot change the 1.
	ent->v.currentammo = SIGA_AMMO;
}


/*
===================
SV_ReadClientMove
===================
*/
void SV_ReadClientMove (usercmd_t *move)
{
	int		i;
	vec3_t	angle;
	int		buttonbits;
	int		newimpulse;
	unsigned int inweapon;
	float curs_screen[2];
	vec3_t curs_start, curs_impact;
	unsigned int curs_entity;
	eval_t *eval;
	qboolean drop = false;
	float timestamp;
	vec3_t movevalues;
	int sequence;
	eval_t *val;
	vec3_t savedvel;

	if (host_client->protocol_pext2 & PEXT2_PREDINFO)
	{
		i = (unsigned short)MSG_ReadShort();
		sequence = (host_client->lastmovemessage & 0xffff0000) | (i&0xffff);

		//tollerance of a few old frames, so we can have redundancy for packetloss
		if (sequence+0x100 < host_client->lastmovemessage)
			sequence += 0x10000;

		if (sequence <= host_client->lastmovemessage)
			drop = true;
	}
	else
		sequence = 0;

	//read the data
	timestamp = MSG_ReadFloat();
	for (i=0 ; i<3 ; i++)
	{
		if (sv.protocol == PROTOCOL_NETQUAKE && !(host_client->protocol_pext2 & PEXT2_PREDINFO) && !NET_QSocketGetProQuakeAngleHack(host_client->netconnection))
			angle[i] = MSG_ReadAngle (sv.protocolflags);
		else
			angle[i] = MSG_ReadAngle16 (sv.protocolflags);	//johnfitz -- 16-bit angles for PROTOCOL_FITZQUAKE
	}

	if (sv_fullpitch.value == 0) // woods -- force server #pqfullpitch
	{
		if (angle[PITCH] > 80.01 || angle[PITCH] < -70.01)
		{
			if (angle[PITCH] > 80.01)
				angle[PITCH] = 80.01;
			if (angle[PITCH] < -70.01)
				angle[PITCH] = -70.01;

			MSG_WriteByte(&host_client->message, svc_setangle);
			for (i = 0; i < 3; i++)
				MSG_WriteAngle(&host_client->message, angle[i], sv.protocolflags);
		}
	}

	movevalues[0] = MSG_ReadShort ();
	movevalues[1] = MSG_ReadShort ();
	movevalues[2] = MSG_ReadShort ();
	if (host_client->protocol_pext2 & PEXT2_PRYDONCURSOR)
		buttonbits = MSG_ReadLong();
	else
		buttonbits = MSG_ReadByte();
	newimpulse = MSG_ReadByte();

	inweapon = (buttonbits & (1u<<30))?MSG_ReadLong():0;
	curs_screen[0] = (buttonbits & (1u<<31))?MSG_ReadShort()/32767.0:0;
	curs_screen[1] = (buttonbits & (1u<<31))?MSG_ReadShort()/32767.0:0;
	curs_start[0] = (buttonbits & (1u<<31))?MSG_ReadFloat():0;
	curs_start[1] = (buttonbits & (1u<<31))?MSG_ReadFloat():0;
	curs_start[2] = (buttonbits & (1u<<31))?MSG_ReadFloat():0;
	curs_impact[0] = (buttonbits & (1u<<31))?MSG_ReadFloat():0;
	curs_impact[1] = (buttonbits & (1u<<31))?MSG_ReadFloat():0;
	curs_impact[2] = (buttonbits & (1u<<31))?MSG_ReadFloat():0;
	curs_entity = (buttonbits & (1u<<31))?MSG_ReadEntity(host_client->protocol_pext2):0;
	buttonbits &= ~((1u<<30)|(1u<<31));

	if (drop)
		return;	//okay, we don't care about that then

// calc ping times
	host_client->lastmovemessage = sequence; //so client can know which input frames still need predicting.
	if (!(host_client->protocol_pext2 & PEXT2_PREDINFO))
	{
		host_client->ping_times[host_client->num_pings%NUM_PING_TIMES]
			= qcvm->time - timestamp;
		host_client->num_pings++;
	}	//otherwise time is still useful for determining the input frame's time value

	// read movement
	VectorCopy (angle, host_client->edict->v.v_angle);
	move->forwardmove = movevalues[0];
	move->sidemove = movevalues[1];
	move->upmove = movevalues[2];

// read buttons
	host_client->edict->v.button0 = (buttonbits & 1)>>0;
	//button1 was meant to be 'use', but got reused by too many mods to get implemented now
	host_client->edict->v.button2 = (buttonbits & 2)>>1;
	if ((val = GetEdictFieldValue(host_client->edict, qcvm->extfields.button3)))
		val->_float = (buttonbits & 4)>>2;
	if ((val = GetEdictFieldValue(host_client->edict, qcvm->extfields.button4)))
		val->_float = (buttonbits & 8)>>3;
	if ((val = GetEdictFieldValue(host_client->edict, qcvm->extfields.button5)))
		val->_float = (buttonbits & 0x10)>>4;
	if ((val = GetEdictFieldValue(host_client->edict, qcvm->extfields.button6)))
		val->_float = (buttonbits & 0x20)>>5;
	if ((val = GetEdictFieldValue(host_client->edict, qcvm->extfields.button7)))
		val->_float = (buttonbits & 0x40)>>6;
	if ((val = GetEdictFieldValue(host_client->edict, qcvm->extfields.button8)))
		val->_float = (buttonbits & 0x80)>>7;

	if (newimpulse)
	{
		// Reserve the *incoming* impulse 9 for the cigarette. Never expose it to
		// QuakeC, even if the model is missing or the player is dead. Keep the
		// request separate from self.impulse so the cheat cannot run first.
		host_client->siga_pending = (newimpulse == 9);
		if (newimpulse == 9)
			newimpulse = 0;
		else if (newimpulse == 101)
			newimpulse = 9; // unchanged QC handles the cheat and its restrictions
		host_client->edict->v.impulse = newimpulse;
	}

	eval = GetEdictFieldValue(host_client->edict, qcvm->extfields.movement);
	if (eval)
	{
		eval->vector[0] = move->forwardmove;
		eval->vector[1] = move->sidemove;
		eval->vector[2] = move->upmove;
	}

	//decide if we're going independant or not
	host_client->usingpmove = !host_client->skate_active &&
		(!!qcvm->extfuncs.SV_RunClientCommand || (!sv_nqplayerphysics.value && (*sv_nqplayerphysics.string||deathmatch.value)));

	//and give them their independance here.
	if (host_client->usingpmove && host_client->knowntoqc)
	{
		if (timestamp > qcvm->time)
			timestamp = qcvm->time;					//don't let the client exceed the current time
		if (timestamp < qcvm->time-0.5)
			timestamp = qcvm->time-0.5;				//don't let the client bank too much time for bursts...
		if (timestamp < host_client->lastmovetime)
			timestamp = host_client->lastmovetime;	//don't let the client report times in the past (to get extra time with the next clc_move)
		if (qcvm->extglobals.input_timelength)
			*qcvm->extglobals.input_timelength = timestamp - host_client->lastmovetime;
		host_client->lastmovetime = timestamp;

		if (qcvm->extglobals.input_sequence)
			*qcvm->extglobals.input_sequence = sequence;
		if (qcvm->extglobals.input_servertime)
			*qcvm->extglobals.input_servertime = timestamp;
		if (qcvm->extglobals.input_buttons)
			*qcvm->extglobals.input_buttons = buttonbits;
		if (qcvm->extglobals.input_impulse)
			*qcvm->extglobals.input_impulse = newimpulse;
		if (qcvm->extglobals.input_movevalues)
			VectorCopy(movevalues, qcvm->extglobals.input_movevalues);
		if (qcvm->extglobals.input_angles)
			VectorCopy(angle, qcvm->extglobals.input_angles);
		if (qcvm->extglobals.input_weapon)
			*qcvm->extglobals.input_weapon = inweapon;
		if (qcvm->extglobals.input_cursor_screen)
			qcvm->extglobals.input_cursor_screen[0] = curs_screen[0], qcvm->extglobals.input_cursor_screen[1] = curs_screen[1];
		if (qcvm->extglobals.input_cursor_trace_start)
			VectorCopy(curs_start, qcvm->extglobals.input_cursor_trace_start);
		if (qcvm->extglobals.input_cursor_trace_endpos)
			VectorCopy(curs_impact, qcvm->extglobals.input_cursor_trace_endpos);
		if (qcvm->extglobals.input_cursor_entitynumber)
			*qcvm->extglobals.input_cursor_entitynumber = curs_entity;

		float saved_button0;
		qboolean equip_cigarette;

		SV_CigarettePreThink(host_client, host_client->edict, &saved_button0, &equip_cigarette);
		VectorCopy(host_client->edict->v.velocity, savedvel);
		pr_global_struct->self = EDICT_TO_PROG(host_client->edict);
		PR_ExecuteProgram(pr_global_struct->PlayerPreThink);
		if (!qcvm->extfuncs.SV_RunClientCommand)
		{
			//Undo damage caused by unaware mods... in vanilla this includes:
			//	PlayerJump has 4 velocity changes that might mess with prediction
			//	WaterMove imposes some additional drag that breaks things
			//	CheckWaterJump is entirely redundant.
			//(it should be noted that the (cs)qc should still track jumps for sounds, and landings for fall damage, and water for drowning, just not any of the velocity changes)
			VectorCopy(savedvel, host_client->edict->v.velocity);
		}

		if (!SV_RunThink (host_client->edict))
			return;	//ent was removed? o.O

		if (qcvm->extfuncs.SV_RunClientCommand)
		{
			pr_global_struct->self = EDICT_TO_PROG(host_client->edict);
			PR_ExecuteProgram(qcvm->extfuncs.SV_RunClientCommand);
		}
		else
		{	//the qc ain't helping us here. go direct to the runstandardplayerphysics builtin.
			G_INT(OFS_PARM0) = EDICT_TO_PROG(host_client->edict);
			PF_sv_pmove();
		}

		pr_global_struct->self = EDICT_TO_PROG(host_client->edict);
		PR_ExecuteProgram(pr_global_struct->PlayerPostThink);
		SV_CigarettePostThink(host_client, host_client->edict, saved_button0, equip_cigarette);
		SV_SkateUpdate(host_client);
	}
}

void SV_ReadQCRequest(void)
{
	int e;
	char args[8];
	const char *rname, *fname;
	func_t f;
	int i;
	client_t *cl = host_client;

	for (i = 0; ; )
	{
		byte ev = MSG_ReadByte();
		/*if (ev >= 200 && ev < 200+MAX_SPLITS)
		{
			ev -= 200;
			while (ev-- && cl)
				cl = cl->controlled;
			continue;
		}*/
		if (i >= sizeof(args)-1)
		{
			if (ev != ev_void)
			{
				msg_badread = true;
				return;
			}
			goto done;
		}
		switch(ev)
		{
		default:
			args[i] = '?';
			G_INT(OFS_PARM0+i*3) = MSG_ReadLong();
			break;
		case ev_void:
			goto done;
		case ev_float:
			args[i] = 'f';
			G_FLOAT(OFS_PARM0+i*3) = MSG_ReadFloat();
			break;
		case ev_vector:
			args[i] = 'v';
			G_FLOAT(OFS_PARM0+i*3+0) = MSG_ReadFloat();
			G_FLOAT(OFS_PARM0+i*3+1) = MSG_ReadFloat();
			G_FLOAT(OFS_PARM0+i*3+2) = MSG_ReadFloat();
			break;
		case ev_ext_integer:
			args[i] = 'i';
			G_INT(OFS_PARM0+i*3) = MSG_ReadLong();
			break;
		case ev_ext_uint32:
			args[i] = 'u';
			G_INT(OFS_PARM0+i*3) = MSG_ReadLong();
			break;
		case ev_ext_sint64:
			args[i] = 'I';
			G_INT64(OFS_PARM0+i*3) = MSG_ReadInt64();
			break;
		case ev_ext_uint64:
			args[i] = 'U';
			G_DOUBLE(OFS_PARM0+i*3) = MSG_ReadUInt64();
			break;
		case ev_ext_double:
			args[i] = 'F';
			G_DOUBLE(OFS_PARM0+i*3) = MSG_ReadDouble();
			break;
		case ev_string:
			args[i] = 's';
			G_INT(OFS_PARM0+i*3) = PR_MakeTempString(MSG_ReadString());
			break;
		case ev_entity:
			args[i] = 'e';
			e = MSG_ReadEntity(host_client->protocol_pext2);
			if (e < 0 || e >= qcvm->num_edicts)
				e = 0;
			G_INT(OFS_PARM0+i*3) = EDICT_TO_PROG(EDICT_NUM(e));
			break;
		}
		i++;
	}

done:
	args[i] = 0;
	rname = MSG_ReadString();
	if (i)
		fname = va("CSEv_%s_%s", rname, args);
	else
		fname = va("CSEv_%s", rname);
	f = PR_FindExtFunction(fname);
	/*if (!f)
	{
		if (i)
			rname = va("Cmd_%s_%s", rname, args);
		else
			rname = va("Cmd_%s", rname);
		f = PR_FindExtFunction(rname);
	}*/
	if (!cl)
		;	//bad seat! not going to warn as they might have been removed recently
	else if (f)
	{
		pr_global_struct->self = EDICT_TO_PROG(cl->edict);
		PR_ExecuteProgram(f);
	}
	else
		SV_ClientPrintf("qcrequest \"%s\" not supported\n", fname);
}

/*
===================
SV_ReadClientMessage

Returns false if the client should be killed
===================
*/
qboolean SV_ReadClientMessage (void)
{
	int		ccmd;
	const char	*s;

	MSG_BeginReading ();

	while (1)
	{
		if (!host_client->active)
			return false;	// a command caused an error

		if (msg_badread)
		{
			Sys_Printf ("SV_ReadClientMessage: badread\n");
			return false;
		}

		ccmd = MSG_ReadChar ();

		switch (ccmd)
		{
		case -1:
			return true;	//msg_badread, meaning we just hit eof.

		default:
			Sys_Printf ("SV_ReadClientMessage: unknown command char\n");
			return false;

		case clc_nop:
//			Sys_Printf ("clc_nop\n");
			break;

		case clc_stringcmd:
			s = MSG_ReadString ();
			if (!q_strncasecmp(s, "spawn", 5)) 
				SV_CheckDuplicateNames(host_client); // woods #dupnames
			if (q_strncasecmp(s, "spawn", 5) && q_strncasecmp(s, "begin", 5) && q_strncasecmp(s, "prespawn", 8) &&
				!(!q_strncasecmp(s, "skate", 5) && (!s[5] || s[5] == ' ' || s[5] == '\t')) &&
				!(!q_strncasecmp(s, "skate_slide", 11) && (!s[11] || s[11] == ' ' || s[11] == '\t')) &&
				!(!q_strncasecmp(s, "+skate_slide", 12) && (!s[12] || s[12] == ' ' || s[12] == '\t')) &&
				!(!q_strncasecmp(s, "-skate_slide", 12) && (!s[12] || s[12] == ' ' || s[12] == '\t')) &&
				qcvm->extfuncs.SV_ParseClientCommand)
			{	//the spawn/begin/prespawn are because of numerous mods that disobey the rules.
				//at a minimum, we must be able to join the server, so that we can see any sprints/bprints (because dprint sucks, yes there's proper ways to deal with this, but moders don't always know them).
				client_t *ohc = host_client;
				G_INT(OFS_PARM0) = PR_SetEngineString(s);
				pr_global_struct->time = qcvm->time;
				pr_global_struct->self = EDICT_TO_PROG(host_client->edict);
				PR_ExecuteProgram(qcvm->extfuncs.SV_ParseClientCommand);
				host_client = ohc;
			}
			else
				Cmd_ExecuteString (s, src_client);
			break;

		case clc_disconnect:
//			Sys_Printf ("SV_ReadClientMessage: client disconnected\n");
			return false;

		case clc_move:
			if (!host_client->spawned)
				return true;	//this is to suck up any stale moves on map changes, so we don't get confused (quite so easily) when protocols are changed between maps
			SV_ReadClientMove (&host_client->cmd);
			break;

		case clcdp_ackframe:
			SVFTE_Ack(host_client, MSG_ReadLong());
			break;

		case clcdp_ackdownloaddata:
			Host_DownloadAck(host_client);
			break;

		case clcfte_qcrequest:
			SV_ReadQCRequest();
			break;

		case clcfte_voicechat:
			SV_VoiceReadPacket(host_client);
			break;
		}
	}

	return true;
}


/*
==================
SV_RunClients
==================
*/
void SV_RunClients (void)
{
	int				i;

	//receive from clients first
	//Spike -- reworked this to query the network code for an active connection.
	//this allows the network code to serve multiple clients with the same listening port.
	//this solves server-side nats, which is important for coop etc.
	while(1)
	{
		struct qsocket_s *sock = NET_GetServerMessage();
		if (!sock)
			break;	//no more this frame

		for (i=0, host_client = svs.clients ; i<svs.maxclients ; i++, host_client++)
		{
			if (host_client->netconnection == sock)
			{
				sv_player = host_client->edict;
				if (!SV_ReadClientMessage ())
				{
					SV_DropClient (false);	// client misbehaved...
					break;
				}
			}
		}
	}

	//then do the per-frame stuff
	for (i=0, host_client = svs.clients ; i<svs.maxclients ; i++, host_client++)
	{
		if (!host_client->active)
			continue;

		sv_player = host_client->edict;

		if (host_client->skate_active && (!host_client->spawned ||
			!SV_SkateCanRide(sv_player) || !SV_SkateBoardValid(host_client)))
			SV_SkateStop(host_client);

		if (!host_client->spawned)
		{
		// clear client movement until a new packet is received
			memset (&host_client->cmd, 0, sizeof(host_client->cmd));
			continue;
		}

		if (!host_client->netconnection)
		{
			//botclients can't receive packets. don't even try.
			//not sure where to put this code, but here seems sane enough.
			//fill in the user's desired stuff according to a few things.
			eval_t *ev = GetEdictFieldValue(host_client->edict, qcvm->extfields.movement);
			if (ev)	//.movement normally works the other way around. oh well.
			{
				host_client->cmd.forwardmove = ev->vector[0];
				host_client->cmd.sidemove = ev->vector[1];
				host_client->cmd.upmove = ev->vector[2];
			}
			host_client->cmd.viewangles[0] = host_client->edict->v.v_angle[0];
			host_client->cmd.viewangles[1] = host_client->edict->v.v_angle[1];
			host_client->cmd.viewangles[2] = host_client->edict->v.v_angle[2];
		}

// always pause in single player if in console or menus
		if (!sv.paused && (svs.maxclients > 1 || key_dest == key_game) )
			SV_ClientThink ();
	}
}

