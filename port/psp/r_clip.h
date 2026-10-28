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
 * (a quad after clipping) never needs more than four, and the largest
 * primitive the engine sends in immediate mode -- a GL_POLYGON from a BSP
 * surface, up to 32 vertices -- never more than 33.  8 covers triangles,
 * quads and the small polygons; ask for more if a caller needs it. */
#define R_CLIP_MAX 8

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
