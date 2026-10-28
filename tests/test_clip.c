/*
 * tests/test_clip.c -- the near-plane clipper, checked on the build machine.
 *
 * The clipper (port/psp/r_clip.c) is plain arithmetic with no GL, no GE and no
 * PSP in it, which is the whole reason it lives in its own file: the one piece
 * of the 3D pipeline that can be wrong in a way the panel shows as "the world
 * is inside out" can be checked here, with numbers, before a build is ever
 * copied to the memory stick.
 *
 * Build and run:  cc -std=gnu11 -Wall -o /tmp/test_clip tests/test_clip.c \
 *                    port/psp/r_clip.c && /tmp/test_clip
 * (tests/run_gameplay_tests.sh does exactly that.)
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "../port/psp/r_clip.h"

static int failures;

static void check (int ok, const char *what)
{
	if (!ok)
	{
		printf("FAIL: %s\n", what);
		failures++;
	}
}

static int near_eq (float a, float b)
{
	return fabsf(a - b) < 1e-5f;
}

static r_clip_vertex_t vert (float x, float y, float z, float w,
                             float u, float v, unsigned int color)
{
	r_clip_vertex_t r;

	r.x = x;
	r.y = y;
	r.z = z;
	r.w = w;
	r.u = u;
	r.v = v;
	r.color = color;

	return r;
}

/* ---- nothing to do ------------------------------------------------------ */

static void test_inside (void)
{
	r_clip_vertex_t tri[3], out[R_CLIP_MAX];
	int n;

	tri[0] = vert(-0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 0.0f, 0xFFFFFFFFu);
	tri[1] = vert( 0.5f, -0.5f, 0.5f, 1.0f, 1.0f, 0.0f, 0xFFFFFFFFu);
	tri[2] = vert( 0.0f,  0.5f, 0.5f, 1.0f, 0.5f, 1.0f, 0xFFFFFFFFu);

	check(r_clip_inside(tri, 3, 0.001f) == 1, "a triangle in front reports inside");
	check(r_clip_inside(tri, 2, 0.001f) == 0, "fewer than three vertices is not a polygon");

	n = r_clip_near(tri, 3, out, R_CLIP_MAX, 0.001f);

	check(n == 3, "a triangle in front comes back unchanged");
	check(n == 3 && memcmp(tri, out, sizeof(tri)) == 0,
	      "a triangle in front is not disturbed");

	/* Exactly on the plane counts as in front: an edge lying in the plane is
	 * visible, and dropping it would leave a hairline hole at the near
	 * clip. */
	tri[0].w = 0.001f;
	check(r_clip_inside(tri, 3, 0.001f) == 1, "a vertex exactly on the plane is inside");
}

/* ---- everything behind -------------------------------------------------- */

static void test_behind (void)
{
	r_clip_vertex_t tri[3], out[R_CLIP_MAX];

	tri[0] = vert(-0.5f, -0.5f, 0.5f, -1.0f, 0.0f, 0.0f, 0xFFFFFFFFu);
	tri[1] = vert( 0.5f, -0.5f, 0.5f, -2.0f, 1.0f, 0.0f, 0xFFFFFFFFu);
	tri[2] = vert( 0.0f,  0.5f, 0.5f, -3.0f, 0.5f, 1.0f, 0xFFFFFFFFu);

	check(r_clip_inside(tri, 3, 0.001f) == 0, "a triangle behind reports outside");
	check(r_clip_near(tri, 3, out, R_CLIP_MAX, 0.001f) == 0,
	      "a triangle behind produces nothing");
}

/* ---- the case the whole file exists for --------------------------------- */

static void test_crossing (void)
{
	r_clip_vertex_t tri[3], out[R_CLIP_MAX];
	int n, i;

	/* w = 1, -1, 1 on the plane w = 0.  The two crossing edges are a->b and
	 * b->c, both exactly halfway, so every interpolated number below is a
	 * value that can be checked by hand. */
	tri[0] = vert(0.0f,  0.0f, 0.0f,  1.0f, 0.00f, 0.00f, 0x00000000u);
	tri[1] = vert(2.0f,  2.0f, 2.0f, -1.0f, 0.50f, 0.20f, 0x00000000u);
	tri[2] = vert(4.0f,  0.0f, 0.0f,  1.0f, 1.00f, 0.00f, 0xFFFFFFFFu);

	n = r_clip_near(tri, 3, out, R_CLIP_MAX, 0.0f);

	check(n == 4, "one vertex behind leaves a quad");

	if (n == 4)
	{
		for (i = 0; i < n; i++)
			check(out[i].w >= 0.0f, "every clipped vertex is in front");

		/* 0: the original first vertex */
		check(near_eq(out[0].x, 0.0f) && near_eq(out[0].w, 1.0f),
		      "the first vertex survives as it was");

		/* 1: halfway along a->b */
		check(near_eq(out[1].x, 1.0f) && near_eq(out[1].y, 1.0f) &&
		      near_eq(out[1].z, 1.0f) && near_eq(out[1].w, 0.0f),
		      "the crossing point is halfway in clip space");
		check(near_eq(out[1].u, 0.25f) && near_eq(out[1].v, 0.10f),
		      "texture coordinates are interpolated on the crossing point");
		check(out[1].color == 0x00000000u, "a black corner stays black");

		/* 2: halfway along b->c */
		check(near_eq(out[2].x, 3.0f) && near_eq(out[2].y, 1.0f) &&
		      near_eq(out[2].w, 0.0f), "the second crossing point is interpolated");
		check(near_eq(out[2].u, 0.75f), "its texture coordinate is interpolated too");
		check(out[2].color == 0x80808080u,
		      "its colour is halfway between black and white, not borrowed");

		/* 3: the original last vertex */
		check(near_eq(out[3].x, 4.0f) && near_eq(out[3].w, 1.0f),
		      "the last vertex survives as it was");
	}

	/* The order has to be the polygon's, not the input's per-edge order:
	 * drawing the result as a fan (0, i-1, i) is only correct if the
	 * vertices came back in sequence. */
	check(n == 4 && out[0].x == 0.0f && out[1].x == 1.0f &&
	      out[2].x == 3.0f && out[3].x == 4.0f,
	      "the clipped polygon keeps the vertex order");
}

/* ---- a polygon that only touches the plane at one vertex ---------------- */

static void test_touch (void)
{
	r_clip_vertex_t quad[4], out[R_CLIP_MAX];
	int n;

	quad[0] = vert(-1.0f, -1.0f, 0.0f,  1.0f, 0.0f, 0.0f, 0xFFFFFFFFu);
	quad[1] = vert( 1.0f, -1.0f, 0.0f,  1.0f, 1.0f, 0.0f, 0xFFFFFFFFu);
	quad[2] = vert( 1.0f,  1.0f, 0.0f, -1.0f, 1.0f, 1.0f, 0xFFFFFFFFu);
	quad[3] = vert(-1.0f,  1.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0xFFFFFFFFu);

	n = r_clip_near(quad, 4, out, R_CLIP_MAX, 0.0f);

	check(n == 4, "half a quad in front leaves a quad");
	check(n > 0 && near_eq(out[0].x, -1.0f) && near_eq(out[0].y, -1.0f) &&
	      near_eq(out[1].x, 1.0f) && near_eq(out[1].y, -1.0f),
	      "the two front corners are kept");

	/* Both cut edges land at y = 0 with x = +/-1: the clipped half of this
	 * quad is a rectangle, and anything else means the interpolation used
	 * the wrong axis. */
	if (n == 4)
	{
		check(near_eq(out[2].x, 1.0f) && near_eq(out[2].y, 0.0f),
		      "the cut along the right edge lands on the plane");
		check(near_eq(out[3].x, -1.0f) && near_eq(out[3].y, 0.0f),
		      "the cut along the left edge lands on the plane");
	}
}

/* ---- the guard rails ---------------------------------------------------- */

static void test_guards (void)
{
	r_clip_vertex_t tri[3], out[R_CLIP_MAX];
	r_clip_vertex_t tiny[2];

	tri[0] = vert(0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0xFFFFFFFFu);
	tri[1] = vert(1.0f, 0.0f, 0.0f,  1.0f, 1.0f, 0.0f, 0xFFFFFFFFu);
	tri[2] = vert(0.0f, 1.0f, 0.0f,  1.0f, 0.0f, 1.0f, 0xFFFFFFFFu);

	check(r_clip_near(tri, 3, out, 2, 0.0f) == 0,
	      "a buffer that cannot hold the result produces nothing, not a half polygon");
	check(r_clip_near(tri, 2, out, R_CLIP_MAX, 0.0f) == 0,
	      "a line is not a polygon");
	check(r_clip_inside(tiny, 2, 0.0f) == 0, "the inside test refuses a line too");

	/* In and out in the same buffer: refused rather than silently wrong. */
	check(r_clip_near(tri, 3, tri, R_CLIP_MAX, 0.0f) == 0,
	      "clipping in place is refused");
}

/* ---- the four sides of the view ----------------------------------------
 *
 * This is the check the world depends on.  The GE does not clip per pixel: a
 * primitive with ONE vertex outside the 4096x4096 coordinate space is thrown
 * away whole (Interstice's clipping.cpp, "the hardware cull happens when any
 * vert goes past the 0-4095 virtual coordinate space").  So after clipping, a
 * triangle has to be inside the view -- every vertex |ndc| <= 1 -- or a wall
 * the player stands next to disappears.
 *
 * The matrices here are the engine's own shapes (Quake/mathlib.c:
 * Matrix4_ProjectionMatrix writes m[11] = -1, i.e. a GL perspective), and the
 * planes are extracted with the same row arithmetic gl_shim.c uses, in world
 * space, exactly as the build does it.
 */
static void perspective (float fovx, float aspect, float n, float f, float *m)
{
	float xmax = n * tanf (fovx * (float)M_PI / 360.0f);
	float ymax = xmax / aspect;
	int i;

	for (i = 0; i < 16; i++)
		m[i] = 0.0f;

	m[0]  = n / xmax;
	m[5]  = n / ymax;
	m[10] = (f + n) / (n - f);
	m[11] = -1.0f;
	m[14] = (2.0f * f * n) / (n - f);
}

/* View matrix for a camera at the origin looking down -z, i.e. the identity of
 * the engine's Matrix4_ViewMatrix with zero angles and zero origin. */
static void identity4 (float *m)
{
	int i;

	for (i = 0; i < 16; i++)
		m[i] = 0.0f;

	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mul4 (float *out, const float *a, const float *b)
{
	int i, j;

	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			out[i * 4 + j] = a[j] * b[i * 4 + 0] + a[4 + j] * b[i * 4 + 1] +
			                 a[8 + j] * b[i * 4 + 2] + a[12 + j] * b[i * 4 + 3];
}

/* The same five planes gl_shim.c derives: four sides of the view, plus the near
 * plane (w >= 0.001). */
static void planes_of (const float *m, r_clip_plane_t *p)
{
	const float s[4] = { +1.0f, -1.0f, +1.0f, -1.0f };
	const int   r[4] = { 0, 0, 1, 1 };
	int i;

	for (i = 0; i < 4; i++)
	{
		p[i].a = m[3] + s[i] * m[r[i]];
		p[i].b = m[7] + s[i] * m[r[i] + 4];
		p[i].c = m[11] + s[i] * m[r[i] + 8];
		p[i].d = m[15] + s[i] * m[r[i] + 12];
	}

	p[4].a = m[3];
	p[4].b = m[7];
	p[4].c = m[11];
	p[4].d = m[15] - 0.001f;
}

/* Clip a world-space triangle the way shim_emit_3d_triangle() does, and report
 * the largest |ndc| any surviving vertex has.  Returns the vertex count. */
static int clip_triangle (const r_clip_vertex_t *tri,
                          r_clip_plane_t *planes, r_clip_vertex_t *out)
{
	r_clip_vertex_t a[R_CLIP_MAX], b[R_CLIP_MAX];
	r_clip_vertex_t *cur = a, *nxt = b;
	int n = 3, i;

	if (r_clip_inside_all (tri, 3, planes, 5))
	{
		for (i = 0; i < 3; i++)
			out[i] = tri[i];
		return 3;
	}

	memcpy (a, tri, sizeof (*tri) * 3);

	for (i = 0; i < 5; i++)
	{
		int k = r_clip_halfspace (cur, n, nxt, R_CLIP_MAX, &planes[i]);
		r_clip_vertex_t *t;

		if (k < 3)
			return 0;

		n = k;
		t = cur; cur = nxt; nxt = t;
	}

	memcpy (out, cur, sizeof (*out) * n);

	return n;
}

static float max_ndc (const r_clip_vertex_t *v, int n, const float *mvp)
{
	float worst = 0.0f;
	int i;

	for (i = 0; i < n; i++)
	{
		float cx = mvp[0] * v[i].x + mvp[4] * v[i].y + mvp[8]  * v[i].z + mvp[12];
		float cy = mvp[1] * v[i].x + mvp[5] * v[i].y + mvp[9]  * v[i].z + mvp[13];
		float cw = mvp[3] * v[i].x + mvp[7] * v[i].y + mvp[11] * v[i].z + mvp[15];
		float ax, ay;

		if (cw <= 0.0f)
			return 1e9f;        /* behind the eye: a broken vertex */

		ax = fabsf (cx / cw);
		ay = fabsf (cy / cw);

		if (ax > worst) worst = ax;
		if (ay > worst) worst = ay;
	}

	return worst;
}

static void test_planes (void)
{
	float proj[16], view[16], mvp[16];
	r_clip_plane_t planes[5];
	r_clip_vertex_t tri[3], out[R_CLIP_MAX];
	int n;

	perspective (90.0f, 480.0f / 248.0f, 4.0f, 4096.0f, proj);
	identity4 (view);
	mul4 (mvp, proj, view);
	planes_of (mvp, planes);

	/* A floor patch the player is standing on, seen from just above it: two
	 * vertices well off the right and bottom of the screen, one inside.  This
	 * is exactly the triangle the GE would have culled whole -- one vertex at
	 * |ndc| = 5 is already past the hardware's patience. */
	tri[0] = vert ( -1.0f,  -0.2f, -10.0f, 10.0f, 0.0f, 0.0f, 0xFFFFFFFFu);
	tri[1] = vert ( 50.0f,  -0.2f, -10.0f, 10.0f, 1.0f, 0.0f, 0xFFFFFFFFu);
	tri[2] = vert ( -1.0f, -20.0f, -10.0f, 10.0f, 0.0f, 1.0f, 0xFFFFFFFFu);

	check (max_ndc (tri, 3, mvp) > 1.0f,
	       "the input really does leave the view (otherwise this tests nothing)");

	n = clip_triangle (tri, planes, out);
	check (n >= 3, "a triangle that straddles the view survives clipping");
	check (max_ndc (out, n, mvp) <= 1.0005f,
	       "every clipped vertex is inside the view (|ndc| <= 1), so the GE's");
	check (r_clip_inside_all (out, n, planes, 5),
	       "the result satisfies the same planes it was clipped to");

	/* The same triangle moved entirely inside the view: no clipping at all,
	 * and the vertices come back untouched. */
	tri[0] = vert (-1.0f, -1.0f, -10.0f, 10.0f, 0.0f, 0.0f, 0xFFFFFFFFu);
	tri[1] = vert ( 1.0f, -1.0f, -10.0f, 10.0f, 1.0f, 0.0f, 0xFFFFFFFFu);
	tri[2] = vert ( 0.0f,  1.0f, -10.0f, 10.0f, 0.0f, 1.0f, 0xFFFFFFFFu);
	check (r_clip_inside_all (tri, 3, planes, 5),
	       "a triangle in the middle of the view needs no clip");
	check (clip_triangle (tri, planes, out) == 3,
	       "an unclipped triangle keeps its three vertices");

	/* Behind the camera entirely: nothing to draw. */
	tri[0] = vert (-1.0f, -1.0f, 10.0f, -10.0f, 0.0f, 0.0f, 0xFFFFFFFFu);
	tri[1] = vert ( 1.0f, -1.0f, 10.0f, -10.0f, 1.0f, 0.0f, 0xFFFFFFFFu);
	tri[2] = vert ( 0.0f,  1.0f, 10.0f, -10.0f, 0.0f, 1.0f, 0xFFFFFFFFu);
	check (clip_triangle (tri, planes, out) == 0,
	       "a triangle behind the eye is dropped");

	/* Crossing the near plane only: still in front, so it is clipped rather
	 * than dropped, and the new edge lies on the near plane. */
	tri[0] = vert (-1.0f, -1.0f,  1.0f, -1.0f, 0.0f, 0.0f, 0xFFFFFFFFu);
	tri[1] = vert ( 1.0f, -1.0f, -10.0f, 10.0f, 1.0f, 0.0f, 0xFFFFFFFFu);
	tri[2] = vert ( 0.0f,  1.0f, -10.0f, 10.0f, 0.0f, 1.0f, 0xFFFFFFFFu);

	n = clip_triangle (tri, planes, out);
	check (n >= 3, "a triangle crossing the near plane keeps its visible part");
	check (max_ndc (out, n, mvp) <= 1.0005f,
	       "and its vertices are inside the view too");
}

/* ---- and the plane clipper on its own ---------------------------------- */

static void test_halfspace (void)
{
	r_clip_vertex_t quad[4], out[R_CLIP_MAX];
	r_clip_plane_t keep_x;
	int n;

	quad[0] = vert (-2.0f, -2.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0xFFFFFFFFu);
	quad[1] = vert ( 2.0f, -2.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFFFFu);
	quad[2] = vert ( 2.0f,  2.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0xFFFFFFFFu);
	quad[3] = vert (-2.0f,  2.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0xFFFFFFFFu);

	/* keep x >= 0 */
	keep_x.a = 1.0f; keep_x.b = 0.0f; keep_x.c = 0.0f; keep_x.d = 0.0f;

	n = r_clip_halfspace (quad, 4, out, R_CLIP_MAX, &keep_x);
	check (n == 4, "a quad cut by one plane is a quad");
	/* The walk starts at v0 (outside), so the two invented vertices are the
	 * first and the last of the result: (0,-2) and (0,2). */
	check (near_eq (out[0].x, 0.0f) && near_eq (out[n - 1].x, 0.0f),
	       "the two new vertices lie on the plane");
	check (near_eq (out[0].u, 0.5f) && near_eq (out[n - 1].u, 0.5f),
	       "and their texture coordinates are interpolated with them");
	check (near_eq (out[0].y, -2.0f) && near_eq (out[n - 1].y, 2.0f),
	       "and their positions are interpolated along the edge");

	/* keep x >= 5: nothing left */
	keep_x.d = -5.0f;
	check (r_clip_halfspace (quad, 4, out, R_CLIP_MAX, &keep_x) == 0,
	       "a polygon entirely on the wrong side produces nothing");

	/* a buffer too small is refused instead of half-written */
	keep_x.d = 0.0f;
	check (r_clip_halfspace (quad, 4, out, 2, &keep_x) == 0,
	       "a buffer that cannot hold the result produces nothing");

	check (r_clip_halfspace (quad, 2, out, R_CLIP_MAX, &keep_x) == 0,
	       "the plane clipper refuses a line");
	check (r_clip_halfspace (quad, 4, quad, R_CLIP_MAX, &keep_x) == 0,
	       "clipping in place is refused");
}

int main (void)
{
	test_inside();
	test_behind();
	test_crossing();
	test_touch();
	test_guards();
	test_halfspace();
	test_planes();

	if (failures)
	{
		printf("%d check(s) failed\n", failures);
		return 1;
	}

	printf("all clipper checks passed\n");

	return 0;
}
