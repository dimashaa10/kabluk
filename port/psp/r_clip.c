/*
 * port/psp/r_clip.c -- Sutherland-Hodgman against a single plane.
 *
 * One plane, not six.  The other five are the GE's job: it clips a triangle to
 * the scissor rectangle for free, and the far plane costs nothing either --
 * a polygon beyond it still has w > 0, so the perspective divide keeps it on
 * the right side of the screen and the depth test throws it away.  The near
 * plane is the only one where the divide itself breaks, and that is what this
 * file exists for.
 *
 * The arithmetic is deliberately the plain kind: a linear interpolation of the
 * four clip-space components plus u, v and the colour.  Interpolating in clip
 * space and dividing afterwards is what GL does, and it is what keeps texture
 * coordinates correct on a face seen at a glancing angle -- interpolating the
 * screen-space u/v of a clipped edge instead is the classic affine-warping bug.
 */
#include "r_clip.h"

static int clip_in (const r_clip_vertex_t *v, float near_w)
{
	return v->w >= near_w;
}

/* Interpolate the vertex where a->b crosses the plane, at t from a to b. */
static void clip_lerp (const r_clip_vertex_t *a, const r_clip_vertex_t *b,
                       float t, r_clip_vertex_t *o)
{
	float ia = (float)(a->color & 0xffu);
	float ib = (float)(b->color & 0xffu);
	float ga = (float)((a->color >> 8) & 0xffu);
	float gb = (float)((b->color >> 8) & 0xffu);
	float ba = (float)((a->color >> 16) & 0xffu);
	float bb = (float)((b->color >> 16) & 0xffu);
	float aa = (float)((a->color >> 24) & 0xffu);
	float ab = (float)((b->color >> 24) & 0xffu);

	o->x = a->x + (b->x - a->x) * t;
	o->y = a->y + (b->y - a->y) * t;
	o->z = a->z + (b->z - a->z) * t;
	o->w = a->w + (b->w - a->w) * t;
	o->u = a->u + (b->u - a->u) * t;
	o->v = a->v + (b->v - a->v) * t;

	/* The colour is four bytes in one word, and the GE interpolates it
	 * across the triangle anyway -- but a clipped edge whose new vertex
	 * borrowed one neighbour's colour would put a visible seam exactly on
	 * the near plane, which is where the player is looking.  Rounding
	 * rather than truncating keeps a vertex that lands on an existing
	 * colour from drifting a level darker. */
#define CLIP_MIX(pa, pb) \
	((unsigned int)(pa + (pb - pa) * t + 0.5f) & 0xffu)

	o->color = (CLIP_MIX(ia, ib)       ) |
	           (CLIP_MIX(ga, gb) <<  8) |
	           (CLIP_MIX(ba, bb) << 16) |
	           (CLIP_MIX(aa, ab) << 24);

#undef CLIP_MIX
}

int r_clip_inside (const r_clip_vertex_t *in, int n, float near_w)
{
	int i;

	if (!in || n < 3)
		return 0;

	for (i = 0; i < n; i++)
		if (!clip_in(&in[i], near_w))
			return 0;

	return 1;
}

int r_clip_near (const r_clip_vertex_t *in, int n,
                 r_clip_vertex_t *out, int max_out, float near_w)
{
	int i, count = 0;

	if (!in || !out || in == out || n < 3 || max_out < 3)
		return 0;

	for (i = 0; i < n; i++)
	{
		const r_clip_vertex_t *a = &in[i];
		const r_clip_vertex_t *b = &in[(i + 1) % n];
		int ain = clip_in(a, near_w);
		int bin = clip_in(b, near_w);

		if (ain)
		{
			if (count >= max_out)
				return 0;
			out[count++] = *a;
		}

		/* One endpoint in, one out: the polygon gains the point where
		 * the edge crosses the plane.  Both endpoint cases in the two
		 * ifs above and below, so this runs exactly once per crossing
		 * edge, which keeps the output a proper polygon rather than a
		 * set of duplicated vertices. */
		if (ain != bin)
		{
			float t;

			if (count >= max_out)
				return 0;

			/* b->w != a->w here: one is >= near_w and the other
			 * is not, so the denominator cannot be zero. */
			t = (near_w - a->w) / (b->w - a->w);
			clip_lerp(a, b, t, &out[count++]);
		}
	}

	return (count >= 3) ? count : 0;
}

/* ------------------------------------------------------------------ */
/* an arbitrary plane, for the four sides of the view                 */
/* ------------------------------------------------------------------ */

/* A vertex that lands exactly on a plane is INSIDE, and "exactly" has to
 * survive the arithmetic: the point a clip invents lies on the plane by
 * construction, and the second clip of the same polygon then evaluates it as
 * -0.0000001 and cuts a sliver off.  That sliver is harmless on screen, but it
 * makes "is the result inside the planes it was clipped to?" false, and that
 * question is what tests/test_clip.c asks.  A tenth of a unit is far below
 * anything a map can show (a texel of a 512-wide surface is 0.5 units). */
#define CLIP_EPS 1e-1f

float r_clip_distance (const r_clip_plane_t *plane, const r_clip_vertex_t *v)
{
	return plane->a * v->x + plane->b * v->y + plane->c * v->z + plane->d;
}

int r_clip_halfspace (const r_clip_vertex_t *in, int n,
                      r_clip_vertex_t *out, int max_out,
                      const r_clip_plane_t *plane)
{
	int i, count = 0;

	if (!in || !out || !plane || in == out || n < 3 || max_out < 3)
		return 0;

	for (i = 0; i < n; i++)
	{
		const r_clip_vertex_t *a = &in[i];
		const r_clip_vertex_t *b = &in[(i + 1) % n];
		float da = r_clip_distance (plane, a);
		float db = r_clip_distance (plane, b);
		int ain = (da >= -CLIP_EPS);
		int bin = (db >= -CLIP_EPS);

		if (ain)
		{
			if (count >= max_out)
				return 0;
			out[count++] = *a;
		}

		if (ain != bin)
		{
			float t;

			/* da and db have opposite signs here, so the denominator
			 * cannot be zero. */
			t = da / (da - db);

			if (count >= max_out)
				return 0;

			clip_lerp (a, b, t, &out[count++]);
		}
	}

	return (count >= 3) ? count : 0;
}

int r_clip_inside_all (const r_clip_vertex_t *in, int n,
                       const r_clip_plane_t *planes, int numplanes)
{
	int i, p;

	if (!in || !planes || n < 3 || numplanes <= 0)
		return 0;

	for (p = 0; p < numplanes; p++)
		for (i = 0; i < n; i++)
			if (r_clip_distance (&planes[p], &in[i]) < -CLIP_EPS)
				return 0;

	return 1;
}
