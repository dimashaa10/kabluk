/*
 * port/psp/r_clip.h -- clipping a polygon against the near plane.
 *
 * GL clips a primitive against six planes before it is projected; the GE only
 * clips in two dimensions, against the scissor.  The port draws 2D by
 * projecting vertices on the CPU (gl_shim.c, shim_emit_vertex) and the world
 * will be drawn the same way, so the CPU side owes the pipeline the one clip
 * that matters for a perspective projection: the near plane.
 *
 * Without it a vertex at or behind the camera has w <= 0, the perspective
 * divide flips its sign, and the polygon it belongs to stretches across the
 * panel in the wrong direction -- the wedges that used to cross the menu, and
 * the reason the menu itself was drawn through a frustum it never wanted.
 *
 * This file knows nothing about GL, the GE or the PSP: it takes clip-space
 * vertices in and writes a convex polygon out, which is what makes it
 * testable on the build machine (tests/test_clip.c) instead of only on the
 * console.
 */
#ifndef R_CLIP_H
#define R_CLIP_H

/* One vertex in clip space, with the two things a clipper has to interpolate
 * when it invents a vertex on the plane: the texture coordinates and the
 * colour.  Same layout for input and output. */
typedef struct r_clip_vertex_s
{
	float        x, y, z, w;   /* clip space: w = -z_eye for a GL projection */
	float        u, v;         /* texture coordinates                        */
	unsigned int color;        /* 0xAABBGGRR, as the GE wants it             */
} r_clip_vertex_t;

/* A convex polygon cut by ONE plane gains at most one vertex, so a triangle
 * becomes a quad.  The shim clips a triangle against FIVE planes (the four
 * sides of the view and the near plane), so it can grow to 3 + 5 = 8, and the
 * buffer has to hold the input as well when a call site runs the planes in
 * sequence.  16 is room for that with a margin -- and it used to be 8, which
 * was also the reason a world polygon with eight or more vertices was dropped
 * entirely: see r_clip_halfspace()'s note about max_out. */
#define R_CLIP_MAX 16

/* A plane the halfspace x on the KEEP side satisfies:
 *
 *     a*x + b*y + c*z + d >= 0
 *
 * The coefficients are the ones a GL projection gives you for free: with
 * M = projection * view (column-major, 16 floats) the rows of M are
 *
 *     row0 = (m[0], m[4], m[8],  m[12])      x_clip
 *     row1 = (m[1], m[5], m[9],  m[13])      y_clip
 *     row3 = (m[3], m[7], m[11], m[15])      w_clip
 *
 * and the six clip-space conditions |x| <= w, |y| <= w, 0 < z + w, z <= w
 * become, in WORLD coordinates,
 *
 *     left   row3 + row0      right  row3 - row0
 *     bottom row3 + row1      top    row3 - row1
 *     near   row3 (w >= eps)  far    row3 - row2
 *
 * which is the standard frustum-plane extraction.  It needs no matrix inverse
 * and no per-vertex transform: the same four dot products the GE is about to
 * do, done once per plane instead of once per vertex-per-draw. */
typedef struct r_clip_plane_s
{
	float a, b, c, d;
} r_clip_plane_t;

/*
 * Clip a convex polygon against one plane (keep the side where the signed
 * distance is >= 0).
 *
 * Same contract as r_clip_near(): in and out may not be the same buffer,
 * out needs room for n + 1 vertices, the result is convex and in the same
 * order, and 0 means nothing survived (or out was too small -- nothing is
 * written then, and the caller draws nothing, which is the safe answer).
 */
int r_clip_halfspace (const r_clip_vertex_t *in, int n,
                      r_clip_vertex_t *out, int max_out,
                      const r_clip_plane_t *plane);

/* Signed distance of a vertex to a plane, for the "is any of it outside?"
 * test a caller does before it pays for a clip. */
float r_clip_distance (const r_clip_plane_t *plane, const r_clip_vertex_t *v);

/* 1 when every vertex is on the keep side of every plane, i.e. the polygon
 * needs no clipping at all.  The common case by far: the BSP walk has already
 * rejected everything outside the view. */
int r_clip_inside_all (const r_clip_vertex_t *in, int n,
                       const r_clip_plane_t *planes, int numplanes);

/*
 * Clip a convex polygon against the plane w >= near_w.
 *
 * in   the polygon, n >= 3 vertices, in order (either winding; this is
 *      geometry, not a back-face test)
 * out  room for at least n + 1 vertices; the result is also convex and in the
 *      same order, so it can be drawn as a triangle fan
 *
 * Returns the number of vertices written, or 0 when nothing survives (the
 * whole polygon is behind, or in and out were the same buffer, or out is too
 * small -- in which case nothing is written and the caller draws nothing,
 * which is the safe answer: a polygon crossing the camera is exactly the one
 * that must not be drawn wrong).
 */
int r_clip_near (const r_clip_vertex_t *in, int n,
                 r_clip_vertex_t *out, int max_out, float near_w);

/* 1 when every vertex is in front, i.e. the polygon needs no clipping at all.
 * Lets the caller skip the copy on the common path -- most of a frame's
 * polygons are in front of the near plane.  n < 3 answers 0. */
int r_clip_inside (const r_clip_vertex_t *in, int n, float near_w);

#endif /* R_CLIP_H */
