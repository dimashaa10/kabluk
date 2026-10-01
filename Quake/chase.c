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
// chase.c -- chase camera code

#include "quakedef.h"
#include "skate.h"

cvar_t	chase_back = {"chase_back", "90", CVAR_ARCHIVE};
cvar_t	chase_up = {"chase_up", "30", CVAR_ARCHIVE };
cvar_t	chase_right = {"chase_right", "0", CVAR_ARCHIVE };
cvar_t	chase_active = {"chase_active", "0", CVAR_NONE};

/* Skate forces chase view without overwriting the user's chase_active cvar. */
qboolean CL_SkateActive (void)
{
	unsigned int state = (unsigned int)cl.stats[STAT_SKATE];
	return (state & SKATE_STAT_MASK) == SKATE_STAT_MAGIC &&
		(state & SKATE_HEIGHT_MASK) <= (unsigned int)(SKATE_MAX_LIFT * SKATE_HEIGHT_SCALE);
}

float CL_SkateLift (void)
{
	return CL_SkateActive() ? ((unsigned int)cl.stats[STAT_SKATE] & SKATE_HEIGHT_MASK) / SKATE_HEIGHT_SCALE : 0;
}

qboolean Chase_Active (void)
{
	return chase_active.value != 0 || CL_SkateActive();
}

float CL_EntitySkateLift (const entity_t *ent)
{
	uintptr_t base = (uintptr_t)cl.entities, address = (uintptr_t)ent;
	if (!base || cl.maxclients <= 0 || cl.num_entities <= 1 || address <= base ||
		address - base > (size_t)q_min(cl.maxclients, cl.num_entities - 1) * sizeof(*ent) ||
		(address - base) % sizeof(*ent))
		return 0;
	return ent->skate_lift;
}

qboolean CL_IsSkateBoard (const entity_t *ent)
{
	return ent->model && (!q_strcasecmp(ent->model->name, SKATE_MODEL) ||
		!q_strcasecmp(ent->model->name, SKATE_MODEL_ROOT));
}

void CL_UpdateSkateVisuals (void)
{
	int i, owner;
	if (!cl.entities)
		return;
	for (i = 0; i < cl.num_entities; i++)
		cl.entities[i].skate_lift = 0;
	if (CL_SkateActive() && cl.viewentity > 0 && cl.viewentity < cl.num_entities)
	{
		cl.entities[cl.viewentity].skate_lift = CL_SkateLift();
		cl.entities[cl.viewentity].lerpflags &= ~LERP_MOVESTEP;
	}
	for (i = 1; i < cl.num_entities; i++)
	{
		entity_t *board = &cl.entities[i], *rider;
		float lift;
		if (!CL_IsSkateBoard(board) || (board->netstate.eflags & EFLAGS_COLOURMAPPED))
			continue;
		owner = board->netstate.colormap;
		if (owner <= 0 || owner > cl.maxclients || owner >= cl.num_entities || owner == i ||
			(owner == cl.viewentity && !CL_SkateActive()))
			continue;
		rider = &cl.entities[owner];
		if (!rider->model || fabsf(board->msg_origins[0][0] - rider->msg_origins[0][0]) > SKATE_MAX_LIFT ||
			fabsf(board->msg_origins[0][1] - rider->msg_origins[0][1]) > SKATE_MAX_LIFT)
			continue;
		lift = owner == cl.viewentity ? CL_SkateLift() :
			CLAMP(0.0f, board->msg_origins[0][2] - rider->msg_origins[0][2], SKATE_MAX_LIFT);
		rider->skate_lift = lift;
		rider->lerpflags &= ~LERP_MOVESTEP;
		// Follow the same interpolated rider, not a separate delayed alias
		// transform. The exported relative positions of both models stay intact.
		VectorCopy(rider->origin, board->origin);
		board->origin[2] += lift;
		board->angles[PITCH] = board->angles[ROLL] = 0;
		board->angles[YAW] = rider->angles[YAW];
		board->lerpflags &= ~LERP_MOVESTEP;
	}
}

/*
==============
Chase_Init
==============
*/
void Chase_Init (void)
{
	Cvar_RegisterVariable (&chase_back);
	Cvar_RegisterVariable (&chase_up);
	Cvar_RegisterVariable (&chase_right);
	Cvar_RegisterVariable (&chase_active);
}

extern int SV_HullPointContents(hull_t* hull, int num, vec3_t p); // woods(Qrack) #betterchase

qboolean SV_RecursiveHullCheck2(hull_t* hull, int num, float p1f, float p2f, vec3_t p1, vec3_t p2, trace_t* trace) // woods (Qrack) #betterchase
{
	mclipnode_t* node; //johnfitz -- was dclipnode_t
	mplane_t* plane;
	float		t1, t2;
	float		frac;
	int			i;
	vec3_t		mid;
	int			side;
	float		midf;

	// LordHavoc: a goto!  everyone flee in terror... :)
loc0:
	// check for empty
	if (num < 0)
	{
		if (num != CONTENTS_SOLID)
		{
			trace->allsolid = false;
			if (num == CONTENTS_EMPTY)
				trace->inopen = true;
			else
				trace->inwater = true;
		}
		else
			trace->startsolid = true;
		return true;		// empty
	}

	// LordHavoc: this can be eliminated by validating in the loader...  but Mercury told me not to bother
	if (num < hull->firstclipnode || num > hull->lastclipnode)
		Sys_Error("SV_RecursiveHullCheck: bad node number");

	// find the point distances
	node = hull->clipnodes + num;
	plane = hull->planes + node->planenum;

	if (plane->type < 3)
	{
		t1 = p1[plane->type] - plane->dist;
		t2 = p2[plane->type] - plane->dist;
	}
	else
	{
		t1 = DotProduct(plane->normal, p1) - plane->dist;
		t2 = DotProduct(plane->normal, p2) - plane->dist;
	}

	// LordHavoc: recursion optimization
	if (t1 >= 0 && t2 >= 0)
	{
		num = node->children[0];
		goto loc0;
	}
	if (t1 < 0 && t2 < 0)
	{
		num = node->children[1];
		goto loc0;
	}

	// put the crosspoint DIST_EPSILON pixels on the near side
	side = (t1 < 0);
	if (side)
		frac = bound(0, (t1 + DIST_EPSILON) / (t1 - t2), 1);
	else
		frac = bound(0, (t1 - DIST_EPSILON) / (t1 - t2), 1);

	midf = p1f + (p2f - p1f) * frac;
	for (i = 0; i < 3; i++)
		mid[i] = p1[i] + frac * (p2[i] - p1[i]);

	// move up to the node
	if (!SV_RecursiveHullCheck2(hull, node->children[side], p1f, midf, p1, mid, trace))
		return false;
	/*
#ifdef PARANOID
	if (SV_HullPointContents(pm_hullmodel, mid, node->children[side]) == CONTENTS_SOLID)
	{
		Con_Printf("mid PointInHullSolid\n");
		return false;
	}
#endif
	*/

	// LordHavoc: warning to the clumsy, this recursion can not be optimized because mid would need to be duplicated on a stack
	if (SV_HullPointContents(hull, node->children[side ^ 1], mid) != CONTENTS_SOLID)
		// go past the node
		return SV_RecursiveHullCheck2(hull, node->children[side ^ 1], midf, p2f, mid, p2, trace);

	if (trace->allsolid)
		return false;		// never got out of the solid area

//==================
// the other side of the node is solid, this is the impact point
//==================
	if (!side)
	{
		VectorCopy(plane->normal, trace->plane.normal);
		trace->plane.dist = plane->dist;
	}
	else
	{
		// LordHavoc: vec3_origin is evil; the compiler can not rely on it being '0 0 0'
		// VectorSubtract (vec3_origin, plane->normal, trace->plane.normal);
		trace->plane.normal[0] = -plane->normal[0];
		trace->plane.normal[1] = -plane->normal[1];
		trace->plane.normal[2] = -plane->normal[2];
		trace->plane.dist = -plane->dist;
	}

	while (SV_HullPointContents(hull, hull->firstclipnode, mid) == CONTENTS_SOLID)
	{ // shouldn't really happen, but does occasionally
		frac -= 0.1;
		if (frac < 0)
		{
			trace->fraction = midf;
			VectorCopy(mid, trace->endpos);
			return false;
		}
		midf = p1f + (p2f - p1f) * frac;
		for (i = 0; i < 3; i++)
			mid[i] = p1[i] + frac * (p2[i] - p1[i]);
	}

	trace->fraction = midf;
	VectorCopy(mid, trace->endpos);

	return false;
}

void TraceLine2(vec3_t start, vec3_t end, vec3_t impact) // woods (Qrack) #betterchase
{
	trace_t	trace;
	qboolean result;

	memset(&trace, 0, sizeof(trace));
	trace.fraction = 1;

	//result is true if end is empty...
	result = SV_RecursiveHullCheck2(cl.worldmodel->hulls, 0, 0, 1, start, end, &trace);

	if (!result)//hit something
	{
		VectorCopy(trace.endpos, impact);
	}
	else
		VectorCopy(end, impact);
}

void LerpVector(const vec3_t from, const vec3_t to, float frac, vec3_t out) // woods (Qrack) #betterchase
{
	out[0] = from[0] + frac * (to[0] - from[0]);
	out[1] = from[1] + frac * (to[1] - from[1]);
	out[2] = from[2] + frac * (to[2] - from[2]);
}

/*
==============
TraceLine

TODO: impact on bmodels, monsters
==============
*/
void TraceLine (vec3_t start, vec3_t end, float pushoff, vec3_t impact)
{
	trace_t	trace;

	memset (&trace, 0, sizeof(trace));
	trace.fraction = 1;
	trace.allsolid = true;
	VectorCopy (end, trace.endpos);
	SV_RecursiveHullCheck (cl.worldmodel->hulls, start, end, &trace, CONTENTMASK_ANYSOLID);

	VectorCopy (trace.endpos, impact);

	if (pushoff && trace.fraction < 1)	//push away from the impact plane by the distance specified, so our camera's near clip plane does not intersect the wall.
	{
		vec3_t dir;
		VectorSubtract(start, end, dir);
		pushoff = pushoff / DotProduct(dir, trace.plane.normal);	//distance needs to be bigger if the trace is co-planar to the surface
		VectorMA(impact, pushoff, dir, impact);
	}
}

/*
==============
Chase_UpdateForClient -- johnfitz -- orient client based on camera. called after input
==============
*/
void Chase_UpdateForClient (void)
{
	//place camera

	//assign client angles to camera

	//see where camera points

	//adjust client angles to point at the same place
}

/*
==============
Chase_UpdateForDrawing -- johnfitz -- orient camera based on client. called before drawing

TODO: stay at least 8 units away from all walls in this leaf
==============
*/
void Chase_UpdateForDrawing (void)
{
	int		i;
	vec3_t	forward, up, right, camera_angles;
	qboolean skating = CL_SkateActive();
	float back = skating ? SKATE_CAMERA_BACK : chase_back.value;
	float camera_up = skating ? SKATE_CAMERA_UP : chase_up.value;
	float camera_right = skating ? 0 : chase_right.value;
	vec3_t	ideal, crosshair, temp;
	float	alpha = 1, alphadist = 1;
	float	absdist;

	VectorCopy(cl.lerpangles, camera_angles);
	if (skating)
		camera_angles[PITCH] = CLAMP(-10.0f, camera_angles[PITCH] + SKATE_CAMERA_PITCH, 65.0f);
	AngleVectors (camera_angles, forward, right, up);

	// calc ideal camera location before checking for walls
	for (i=0 ; i<3 ; i++)
		ideal[i] = r_refdef.vieworg[i]
		- forward[i]*back
		+ right[i]*camera_right;
		//+ up[i]*chase_up.value;
	ideal[2] = r_refdef.vieworg[2] + camera_up - (skating ? forward[2] * back : 0);

	// make sure camera is not in or behind a wall
	// TraceLine2(r_refdef.vieworg, ideal, temp); // woods (Qrack) #betterchase - change to 2

	// make sure camera is not in or behind a wall
	TraceLine(r_refdef.vieworg, ideal, NEARCLIP, ideal);

	// find the spot the player is looking at
	if (skating)
	{
		// Frame the complete rider/board rather than looking past the head
		// at the horizon (which clips the skateboard below the screen).
		VectorCopy(r_refdef.vieworg, crosshair);
		crosshair[2] -= SKATE_CAMERA_FOCUS_DROP;
		VectorCopy(crosshair, temp);
	}
	else
	{
		VectorMA (r_refdef.vieworg, 1<<20, forward, temp);
		TraceLine (r_refdef.vieworg, temp, 0, crosshair);
	}

	if (VectorLength(temp) != 0)
	{	alphadist = VecLength2(r_refdef.vieworg, ideal); // chase_transparent from Qrack
		absdist = fabsf(back);
		alpha = bound(0, (alphadist / absdist), 1);


		if ((alpha < 1 && alpha > 0.6) || alpha < 0.09)
			alpha = (alpha < 0.09) ? 0 : 1;

		cl.entities[cl.viewentity].alpha = ENTALPHA_ENCODE((q_min(alpha, 1)));
	}
	
	// place camera
	VectorCopy (ideal, r_refdef.vieworg);

	// calculate camera angles to look at the same spot
	VectorSubtract (crosshair, r_refdef.vieworg, temp);
	VectorAngles (temp, NULL, r_refdef.viewangles);
	if (r_refdef.viewangles[PITCH] >= 89.9 || r_refdef.viewangles[PITCH] <= -89.9)
		r_refdef.viewangles[YAW] = cl.viewangles[YAW];
}

