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

int main (void)
{
	test_inside();
	test_behind();
	test_crossing();
	test_touch();
	test_guards();

	if (failures)
	{
		printf("%d check(s) failed\n", failures);
		return 1;
	}

	printf("all clipper checks passed\n");

	return 0;
}
