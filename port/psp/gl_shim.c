/*
 * port/psp/gl_shim.c -- the 2D part of the OpenGL -> sceGu shim (stage M4, step 2).
 *
 * The engine draws its interface with immediate-mode OpenGL: glOrtho() for the
 * projection, glMatrixMode/glLoadIdentity for the model view, glBegin(GL_QUADS)
 * ... glTexCoord2f/glVertex2f ... glEnd() for every picture.  There is no GL on
 * a PSP, so this file does the three things that matter:
 *
 *   1. keeps the matrix state (projection and model view, with push/pop) and
 *      applies it to every vertex on the CPU -- the GU is only told where the
 *      triangle ended up on the screen;
 *   2. turns a texture upload into memory the GU can sample, and remembers the
 *      filter mode each texture asked for;
 *   3. turns glBegin/glEnd into GU draw arrays, mapping the GL primitive onto
 *      the closest GU one.
 *
 * Coordinates: GL counts pixels from the bottom-left of the viewport, the GU
 * from the top-left of the screen, and glOrtho(0, w, h, 0) already flips the
 * y axis for the 2D canvases.  Everything below converts once, in glVertex.
 *
 * Textures live in main memory (malloc), not in EDRAM: the two frame buffers
 * and the depth buffer already use almost all of the 2 MB the GPU owns.  The
 * GU fetches them over the bus; it is slower than EDRAM, and it is what the
 * engine's texture sizes allow.
 */

#include <pspkernel.h>
#include <pspgu.h>

#include <stdlib.h>
#include <string.h>
#include <malloc.h>	/* memalign() */
#include <math.h>

#include "quakedef.h"
#include "gl_shim.h"
#include "gu_psp.h"
#include "psp_console.h"
#include "tex_arena.h"
#include "r_clip.h"

/* ------------------------------------------------------------------ */
/* state                                                              */
/* ------------------------------------------------------------------ */

/* The vertex layout, in the order the GU reads attributes: texture, colour,
 * normal, position.  Ours is texture + colour + position, in that same order,
 * which is what shim_vertex_t below declares. */
#define SHIM_VERTEX_FORMAT \
	(GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

/* The same vertex, transformed by the GE instead of by us.
 *
 * GU_TRANSFORM_2D is a promise that the position is already in screen
 * coordinates and the GE has nothing to do but rasterise it.  For the world
 * that promise is the whole cost of a frame: every vertex of every surface
 * would be multiplied by the model-view-projection matrix on the CPU, in
 * float, before anything is drawn.  Without the flag the GE takes the
 * position as a WORLD coordinate and runs it through its own transform stage
 * (sceGuSetMatrix, below), which is what the hardware is for -- this is what
 * Interstice's port does (source/psp/gu_main.cpp: sceGumPerspective() for the
 * projection and world-space vertices handed to sceGuDrawArray, and
 * source/psp/gu_draw.cpp for the 2D sprites that stay on GU_TRANSFORM_2D).
 *
 * The two formats are not mixed inside one draw: shim_batch_flush() picks the
 * one the batch was queued with. */
#define SHIM_VERTEX_FORMAT_3D \
	(GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF)

#define SHIM_MAX_MATRIX_STACK 8

/* Vertices accumulated between glBegin() and glEnd().  A whole line of text
 * arrives in one glBegin(GL_QUADS), so this has to hold a good few hundred
 * characters: 1024 vertices is 256 characters, and every buffer here is
 * static memory that the module's heap has to do without (24 bytes per
 * vertex, so the two arrays below cost 60 KB together). */
#define SHIM_MAX_VERTICES     1024

/* Room for the vertices the GE reads while it draws a frame.
 *
 * sceGuDrawArray() does not copy anything: it sends the GE the ADDRESS of the
 * vertex array (see pspsdk src/gu/sceGuDrawArray.c -- VADDR/PRIM and nothing
 * else) and the GE fetches those vertices over the bus while the CPU is
 * already building the next picture.  A single shared array is therefore
 * rewritten under the GE's feet, and the GE draws the *next* picture's
 * vertices, or a half-written mix of two -- which is what the full-screen
 * wedges, the diagonal edge and the flicker between frames are.  Every draw
 * now gets its own slice of this ring; the ring is reset at frame start, after
 * the previous frame has been synchronised.
 *
 * 16384 vertices is 384 KB.  The menu needs a small fraction of that (a screen
 * of text is a few thousand), but a frame of the WORLD does not: the GE draws
 * triangles straight out of this array, so a room with a thousand visible
 * surfaces is three or four thousand triangles, i.e. ten thousand vertices and
 * more when the player looks down a long corridor.  At 8192 the world would
 * have hit the ring limit inside the first frame and drawn whatever fit. */
#define SHIM_GE_VERTICES      16384

typedef struct
{
	float        m[16];
} shim_matrix_t;

typedef struct
{
	float        u, v;
	unsigned int color;
	float        x, y, z;
} shim_vertex_t;

typedef struct
{
	int          used;
	unsigned int psm;             /* GU_PSM_4444, GU_PSM_5650 or GU_PSM_T8 */
	int          width, height;   /* what the engine asked for          */
	int          potw, poth;      /* what the GE is told (power of two) */
	int          tbw;             /* row stride in pixels (power of two) */
	void        *pixels;          /* main-memory copy the GU samples     */
	/* For GU_PSM_T8 the 256-entry palette (GU_PSM_8888 words) sits at
	 * pixels + potw * poth, inside the same block -- potw * poth is a
	 * multiple of 64, so the palette is 16-byte aligned as
	 * sceGuClutLoad() requires, and one arena block per picture still. */
	size_t       bytes;           /* 1 or 2 bytes per pixel, plus a palette */
	int          filter;          /* GU_NEAREST or GU_LINEAR             */
	int          dirty;           /* needs a re-upload before drawing    */
	int          in_arena;        /* the block came from the texture arena */
	int          priority;        /* a lightmap: counted against its own
	                               * budget, not the picture cap (below) */
} shim_texture_t;

#define SHIM_MAX_TEXTURES 1024

static shim_texture_t shim_textures[SHIM_MAX_TEXTURES];
static unsigned int   shim_texture_count;
static size_t         shim_texture_bytes;
static unsigned int   shim_texture_bound;

/* What the GE has already been told about texturing.
 *
 * sceGuTexMode()/TexImage()/TexFilter()/TexFunc()/TexFlush() are GE commands,
 * not free-standing state: they cost about seven words of the frame's 32 KB
 * draw list.  glEnd() used to re-send all of them for every picture of every
 * frame -- on a 392-draw frame that is roughly 11 KB of the list spent saying
 * the same thing over and over, which is a third of the budget the frame
 * actually needs.  Nothing is sent now unless it changed. */
static unsigned int   shim_tex_announced_id;
static const void    *shim_tex_announced_pixels;
static unsigned int   shim_tex_announced_psm;
static int            shim_tex_announced_filter;
/* sceGuTexImage() stores the size (as logarithms) and the row stride; if a
 * picture is re-uploaded at a different size into a block that is still large
 * enough, the pointer and the format do not change -- so the size has to be
 * part of what "already announced" means, or the GE keeps sampling with the
 * old geometry and the picture comes out squeezed into the wrong rectangle. */
static int            shim_tex_announced_potw;
static int            shim_tex_announced_poth;
static int            shim_tex_announced_tbw;
/* An 8-bit picture is its indices plus a palette, and the palette is GE
 * state as much as the format is: two pictures can share nothing but the
 * address, so the palette that was loaded is part of "already announced". */
static const void    *shim_tex_announced_palette;
static int            shim_tex_state_valid;

/* matrix state */
static shim_matrix_t  shim_projection;
static shim_matrix_t  shim_modelview;
static shim_matrix_t  shim_projection_stack[SHIM_MAX_MATRIX_STACK];
static shim_matrix_t  shim_modelview_stack[SHIM_MAX_MATRIX_STACK];
static int            shim_projection_depth;
static int            shim_modelview_depth;
static GLenum         shim_matrix_mode = GL_MODELVIEW;

/* viewport, in GL coordinates (origin bottom-left) */
static int            shim_vp_x, shim_vp_y, shim_vp_w = PSP_SCREEN_WIDTH,
                      shim_vp_h = PSP_SCREEN_HEIGHT;

/* immediate mode */
static int            shim_primitive;
static int            shim_vertex_count;
static shim_vertex_t  shim_vertices[SHIM_MAX_VERTICES] __attribute__((aligned(16)));
/* Clip-space vertices, used instead of shim_vertices while a PERSPECTIVE
 * projection is in force.
 *
 * With an orthogonal projection (every 2D canvas in the engine) w is 1 for
 * every vertex and shim_emit_vertex() maps it straight onto the panel.  Under a
 * perspective one the vertex cannot be mapped yet: a polygon that crosses the
 * camera has a vertex with w <= 0, and dividing by it throws the polygon across
 * the screen in the wrong direction.  So the vertex is parked here in clip
 * space, and the primitive is clipped and projected together when glEnd()
 * closes it (shim_flush_3d).  Only one of the two arrays is in use at a time;
 * shim_vertex_count counts whichever it is. */
static r_clip_vertex_t shim_clip_vertices[SHIM_MAX_VERTICES];
/* Set in glBegin() from the projection in force, so a whole primitive goes
 * through one path. */
static int            shim_clipping;
static shim_vertex_t  shim_ge_vertices[SHIM_GE_VERTICES] __attribute__((aligned(16)));
static unsigned int   shim_ge_used;            /* vertices handed to the GE */
static unsigned int   shim_ge_peak;            /* ... and the frame's high mark */
static unsigned int   shim_ge_overflows;
static float          shim_current_u, shim_current_v;
/* How much of the power-of-two box the picture actually fills: a 240x112
 * picture lives in a 256x128 texture, so the vertex's u is multiplied by
 * 240/256 and its v by 112/128 on the way out.  Kept up to date by
 * shim_uv_scale_update() whenever the bound texture (or its size) changes. */
/* Texture-coordinate scale, per path.  The two paths need DIFFERENT units and
 * that difference cost the map its textures for several rounds -- see
 * shim_uv_scale_update(). */
static float          shim_u_scale = 1.0f, shim_v_scale = 1.0f;      /* 2D: texels */
static float          shim_u_scale3d = 1.0f, shim_v_scale3d = 1.0f;  /* 3D: fraction of the box */
static unsigned int   shim_current_color = 0xFFFFFFFFu;

/* pipeline switches */
static int            shim_texture_2d = 1;
static int            shim_blend;
static int            shim_blend_src = 0, shim_blend_dst = 0;   /* GL enums */
static int            shim_alpha_test;
static int            shim_alpha_func = GU_GREATER;   /* GU comparison, not GL */
static float          shim_alpha_ref = 0.1f;
static int            shim_depth_test;
static int            shim_depth_mask = 1;
static int            shim_scissor_on;
static int            shim_scr_x, shim_scr_y, shim_scr_w = PSP_SCREEN_WIDTH,
                      shim_scr_h = PSP_SCREEN_HEIGHT;

/* GU clear colour, set through glClearColor() */
static unsigned int   shim_clear_color = 0xFF000000u;

/* Non-zero while gu_psp.c has a draw list open: see the gate below. */
static int            shim_frame_active;

/* What the 2D path did this frame.  Printed for the first few frames and then
 * every second or so: "no draws at all" and "a thousand draws" are different
 * bugs, and the log cannot tell them apart otherwise. */
static unsigned int   shim_frames;
static unsigned int   shim_frame_draws;
static unsigned int   shim_frame_verts;
static unsigned int   shim_frame_batches;   /* sceGuDrawArray calls actually sent */

/* Did this frame put a 3D polygon of the world into the draw list?
 *
 * Counted because the screen's breadcrumbs (Quake/gl_screen.c, SCR_MARK) have
 * to know what a "world frame" is, and their old test was cls.state ==
 * ca_connected -- which is false in the attract loop, where a demo world is
 * drawn with the client disconnected.  The result was a gate that never
 * closed and sixteen card writes per frame; this counter is the fact, taken
 * from the draw itself rather than from client state. */
static unsigned int   shim_world_prims;

/* ---- where the frame's CPU time goes ------------------------------------
 *
 * The round-84 log leaves one number unexplained, and it is the only one that
 * matters:
 *
 *     gl: frame 600, 2223 draws in 528 batches, 11634 vertices ...
 *     run: f 640 state 2, ..., 185.5 ms/frame over 11
 *          (cpu 176.0, ge 0.1, vblank 9.4)
 *
 * 176 ms of "cpu" against 0.1 ms of "ge" cannot both be true of a frame that
 * submitted 2223 primitives: gu_t_cpu_us is measured from psp_gu_frame_begin()
 * to sceGuFinish(), and in GU_DIRECT mode a command write blocks when the GE's
 * FIFO is full -- so a slow GE shows up as cpu, not as ge.  "cpu 176" therefore
 * means "the frame took 176 ms somewhere between opening the list and finishing
 * it", and the heartbeat cannot say where.
 *
 * So each frame is now split into buckets, in microseconds.  Two of them are
 * ours (the shim's world flush and its draw array), two are the engine's
 * (R_RenderScene), and one is picture uploads.  Reset in psp_gl_shim_begin_frame,
 * printed once every 120 frames next to the draw counts.
 *
 * This is a measurement, not a guess about a fix: if the time is inside
 * sceGuDrawArray() it is the GE pushing back through the FIFO and the answer is
 * fewer, bigger primitives; if it is in the scene the answer is in R_*; if it is
 * in textures it is the announce path.  The next log says which. */
#define SHIM_PROF_BUCKETS   6
enum {
	SHIM_PROF_SCENE = 0,     /* R_RenderScene, added by gl_rmain.c            */
	SHIM_PROF_FLUSH,         /* shim_flush_3d: clip + emit for the world      */
	SHIM_PROF_TEX,           /* shim_upload_bound_texture: GE texture state   */
	SHIM_PROF_DRAW,          /* sceGuDrawArray + dcache writeback             */
	SHIM_PROF_PIXELS,        /* glTexImage2D/glTexSubImage2D: texel conversion */
	SHIM_PROF_SKY            /* Sky_DrawSky, added by gl_sky.c                 */
};

#if defined(PLATFORM_PSP) || defined(__psp__)
extern unsigned int sceKernelGetSystemTimeLow (void);
#define SHIM_PROF_NOW()	sceKernelGetSystemTimeLow ()
#else
#define SHIM_PROF_NOW()	0u
#endif

static unsigned int   shim_prof_us[SHIM_PROF_BUCKETS];
static unsigned int   shim_prof_texflush;   /* TEX_FLUSH+TEX_SYNC pairs a frame */

/* For the engine's own bucket: R_RenderScene cannot time itself into a static
 * of this file, so it hands the microseconds over. */
void psp_gl_shim_prof_add (int bucket, unsigned int us)
{
	if (bucket >= 0 && bucket < SHIM_PROF_BUCKETS)
		shim_prof_us[bucket] += us;
}

/* Draw tracer.
 *
 * The screen has been showing large pale wedges across the whole panel since
 * the 2D shim went in, and nothing in the log said *which* draw they come
 * from: "37 draws" a frame is not a name.  So every draw that ends up bigger
 * than a quarter of the panel is reported with its texture, its vertex count
 * and its bounding box, and the periodic frame line carries a running count.
 * A picture correctly drawn is 8x8 for a character or a few hundred pixels
 * wide for a menu bar; anything that covers the screen that was not asked to
 * is exactly what we are looking for. */
static unsigned int   shim_big_draws;          /* whole frame, reset each frame */
static unsigned int   shim_trace_frames = 4;   /* dump every draw this early on */
static unsigned int   shim_trace_early;
static int shim_trace_menu;    /* ... and the same for the settled menu */
static int shim_trace_text;    /* glyph-band lines for the current frame */
static unsigned int shim_trace_dump;   /* lines of the one full frame dump */


/* ------------------------------------------------------------------ */
/* batching                                                           */
/* ------------------------------------------------------------------ */

/* Consecutive draws that ask the GE for the same primitive with the same
 * state are one draw, not twenty.
 *
 * The engine draws text one character at a time: Draw_Character() is its own
 * glBegin(GL_QUADS)/glEnd() pair, and the boot console fills eighteen rows of
 * it.  Every glEnd() used to become its own sceGuDrawArray(), so a menu frame
 * was 392 GE commands, 382 of them a single 8x8 glyph.  Each command costs a
 * few words of the 32 KB draw list and, more importantly, a round of GE state
 * checks; the hardware is perfectly happy to take the same 2300 vertices in
 * one command.
 *
 * The batching is deliberately conservative: it only merges draws that are
 * already adjacent in the vertex ring (so the merged range is contiguous
 * memory) and only when the primitive matches.  Every shim_gu* wrapper below
 * flushes the pending batch before it sends a GE command, so a texture change,
 * a blend change or a scissor change can never be applied to a draw that was
 * started before it -- the batch cannot reorder anything. */
static shim_vertex_t *shim_batch_v;
static int            shim_batch_n;
static int            shim_batch_prim;
static unsigned int   shim_batch_tex;
/* 1 when the queued batch carries WORLD coordinates and has to be drawn with
 * the GE's own transform (SHIM_VERTEX_FORMAT_3D), 0 when the vertices are
 * already screen coordinates.  Kept per batch because the engine interleaves
 * the two: R_RenderScene draws the world, then the HUD is drawn in 2D over
 * it, and both go through this one queue. */
static int            shim_batch_3d;

static void shim_batch_flush (void);
static void shim_ge3d_matrices (void);
static void shim_clip_planes_update (void);

/* ------------------------------------------------------------------ */
/* the gate                                                           */
/* ------------------------------------------------------------------ */

/* THE RULE THIS FILE EXISTS TO RESPECT: a sceGu* call only does something
 * inside a draw list opened by sceGuStart().  The engine is happy to call
 * glEnable/glDisable/glBlendFunc/glTexImage2D during Host_Init, when gu_psp.c
 * has not opened a list yet, and sending GE commands then writes through a
 * null list pointer.  So every GE call below goes through one of these
 * wrappers: outside a frame the state is only *recorded*, and
 * psp_gl_shim_begin_frame() replays it into the next list. */
static void shim_guEnable (int cap)               { shim_batch_flush(); if (shim_frame_active) sceGuEnable(cap); }
static void shim_guDisable (int cap)              { shim_batch_flush(); if (shim_frame_active) sceGuDisable(cap); }
static void shim_guAlphaFunc (int f, unsigned int v, unsigned int m)
                                                  { shim_batch_flush(); if (shim_frame_active) sceGuAlphaFunc(f, v, m); }
static void shim_guBlendFunc (int op, int src, int dst, unsigned int sf, unsigned int df)
                                                  { shim_batch_flush(); if (shim_frame_active) sceGuBlendFunc(op, src, dst, sf, df); }
static void shim_guClear (unsigned int bits)      { shim_batch_flush(); if (shim_frame_active) sceGuClear(bits); }
static void shim_guClearColor (unsigned int c)    { shim_batch_flush(); if (shim_frame_active) sceGuClearColor(c); }
static void shim_guDepthFunc (int f)              { shim_batch_flush(); if (shim_frame_active) sceGuDepthFunc(f); }
static void shim_guDepthMask (int m)              { shim_batch_flush(); if (shim_frame_active) sceGuDepthMask(m); }
static void shim_guScissor (int x, int y, int w, int h)
                                                  { shim_batch_flush(); if (shim_frame_active) sceGuScissor(x, y, w, h); }
static void shim_guViewport (int cx, int cy, int w, int h)
                                                  { shim_batch_flush(); if (shim_frame_active) sceGuViewport(cx, cy, w, h); }
static void shim_guShadeModel (int m)             { shim_batch_flush(); if (shim_frame_active) sceGuShadeModel(m); }
static void shim_guTexFilter (int min, int mag)   { shim_batch_flush(); if (shim_frame_active) sceGuTexFilter(min, mag); }
static void shim_guTexFlush (void)                { shim_batch_flush(); if (shim_frame_active) sceGuTexFlush(); }
static void shim_guTexFunc (int tfx, int tcc)     { shim_batch_flush(); if (shim_frame_active) sceGuTexFunc(tfx, tcc); }
static void shim_guTexSync (void)              { shim_batch_flush(); if (shim_frame_active) sceGuTexSync(); }
static void shim_guTexImage (int mip, int w, int h, int tbw, const void *p)
                                                  { shim_batch_flush(); if (shim_frame_active) sceGuTexImage(mip, w, h, tbw, p); }
/* NOT "mips": psp-gcc predefines the macro `mips` (the target CPU family is
 * baked into the compiler), so a parameter of that name is replaced by a
 * number before it is even parsed.  The desktop syntax check cannot see it. */
/* Texturing on/off, sent only when it changes.
 *
 * glEnd() used to send GU_TEXTURE_2D unconditionally for every draw.  Each of
 * those is a GE command, and every GE command flushes the pending batch, so
 * the batching written to merge the console's 382 one-character draws never
 * merged a single pair: the log showed 391 batches for 392 draws.  The state
 * is what the GE remembers, so saying it twice is pure cost. */
static int shim_ge_tex2d = -1;

static void shim_guTexEnable (int on)
{
	if (shim_ge_tex2d == on)
		return;

	shim_ge_tex2d = on;

	if (on)
		shim_guEnable(GU_TEXTURE_2D);
	else
		shim_guDisable(GU_TEXTURE_2D);
}

static void shim_guTexMode (int psm, int levels, int a2, int swz)
                                                  { shim_batch_flush(); if (shim_frame_active) sceGuTexMode(psm, levels, a2, swz); }
/* The palette of an 8-bit picture.  Two GE commands, and the "16 colours per
 * block, 32 blocks = 256" arithmetic is straight out of pspgu.h:
 *
 *     sceGuClutLoad(num_blocks, cbp) -- "How many blocks of 8 entries to
 *     upload (32*8 is 256 colors)", palette 16-byte aligned;
 *     sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0) -- 8888 entries, no index shift,
 *     all 256 indices, no base.
 *
 * Both have to be sent before the draw that samples the picture, and only
 * when the picture changed: the CLUT lives in the GE's texture cache, so it
 * is exactly as much state as the format is. */
static void shim_guClutMode (unsigned int cpsm, unsigned int shift,
                             unsigned int mask, unsigned int csa)
                                                  { shim_batch_flush(); if (shim_frame_active) sceGuClutMode(cpsm, shift, mask, csa); }
static void shim_guClutLoad (int blocks, const void *p)
                                                  { shim_batch_flush(); if (shim_frame_active) sceGuClutLoad(blocks, p); }
/* Hand the GE's transform stage the two matrices a world-space draw needs.
 *
 * The GE multiplies its own three: a vertex goes through the model matrix,
 * then the view matrix, then the projection, and the result is divided by w
 * and mapped into the 4096x4096 coordinate space the viewport in gu_psp.c
 * describes.  The engine hands us exactly the two matrices that fit:
 *
 *   glMatrixMode(GL_PROJECTION); glLoadMatrixf(mat);   <- the projection
 *   glMatrixMode(GL_MODELVIEW);  glLoadMatrixf(mat);   <- THE VIEW (R_SetupGL)
 *
 * so the GL model-view slot holds the camera's view matrix, and the model slot
 * stays identity: proj * view * identity * vertex is proj * view * vertex,
 * which is what GL_SetupGL's own glLoadMatrixf pair means.
 *
 * The storage is the same on both sides.  GL's matrices are column-major
 * (mathlib.c: Matrix4_ProjectionMatrix writes m[11] = -1 and m[14] = ...,
 * exactly where a GL perspective puts them) and ScePspFMatrix4 is column-major
 * too (pspgu.h: the first ScePspFVector4 is the first COLUMN), so the 16 floats
 * are copied, not transposed.  pspsdk's sceGumPerspective (src/gum/pspgum.c)
 * builds the same numbers as glFrustum, minus the two minus signs that GL also
 * has -- i.e. the GE expects the GL convention.
 *
 * Re-sent only when the matrix changed: three sceGuSetMatrix calls are fifteen
 * words of the frame's draw list, and a frame of world surfaces changes them
 * once.  Below: the three buffers, in GE-aligned storage.
 *
 * ScePspFMatrix4 is `struct { ScePspFVector4 x, y, z, w; }` -- four COLUMNS,
 * not a flat float[16] (pspsdk src/base/psptypes.h), and it is declared
 * __attribute__((aligned(16))) because the GE path loads it with VFPU
 * quadword instructions.  So the shim's own float[16] cannot be handed over
 * by pointer: it is neither that type nor aligned.  These three buffers are. */
static float shim_ge3d_projection[16] __attribute__((aligned(16)));
static float shim_ge3d_model[16] __attribute__((aligned(16)));
static float shim_ge3d_view[16] __attribute__((aligned(16)));
static int   shim_ge3d_identity_ready;
static shim_matrix_t  shim_ge3d_sent_projection;
static shim_matrix_t  shim_ge3d_sent_modelview;
static int            shim_ge3d_sent_valid;

static void shim_ge3d_matrices (void)
{
	if (!shim_ge3d_identity_ready)
	{
		memset(shim_ge3d_view, 0, sizeof(shim_ge3d_view));
		shim_ge3d_view[0] = shim_ge3d_view[5] = 1.0f;
		shim_ge3d_view[10] = shim_ge3d_view[15] = 1.0f;
		shim_ge3d_identity_ready = 1;
	}

	if (shim_ge3d_sent_valid &&
	    !memcmp(shim_ge3d_sent_projection.m, shim_projection.m,
	            sizeof(shim_projection.m)) &&
	    !memcmp(shim_ge3d_sent_modelview.m, shim_modelview.m,
	            sizeof(shim_modelview.m)))
		return;

	memcpy(shim_ge3d_projection, shim_projection.m, sizeof(shim_ge3d_projection));
	memcpy(shim_ge3d_model, shim_modelview.m, sizeof(shim_ge3d_model));

	sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4 *)shim_ge3d_projection);
	sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4 *)shim_ge3d_view);
	sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4 *)shim_ge3d_model);

	shim_ge3d_sent_projection = shim_projection;
	shim_ge3d_sent_modelview = shim_modelview;
	shim_ge3d_sent_valid = 1;
}

/* Queue one draw.  Merged into the pending batch when the primitive matches
 * and the vertices are exactly where the pending batch ends, i.e. when the two
 * draws are already one contiguous run in the ring. */
static void shim_batch_add (int prim, unsigned int texture, shim_vertex_t *v,
                            int count, int is3d)
{
	if (!v || count <= 0)
		return;

	shim_frame_verts += (unsigned int)count;

	/* The merge key is primitive + texture + contiguity + which transform
	 * the vertices are in.  Contiguity alone would in practice be enough,
	 * because a texture change sends GE commands and those flush the batch
	 * -- but "in practice" is exactly what a batching bug looks like from
	 * the outside, so the texture is part of the condition rather than a
	 * side effect of another module.  The transform is part of it for a
	 * harder reason: a screen-space vertex list and a world-space one are
	 * drawn with two different vertex formats, and merging them would put
	 * one of the two into the GE with the wrong meaning -- a world surface
	 * drawn as pixels, or a picture drawn as a coordinate in the map. */
	if (shim_batch_n > 0 && shim_batch_prim == prim &&
	    shim_batch_tex == texture && shim_batch_3d == is3d &&
	    shim_batch_v + shim_batch_n == v)
	{
		shim_batch_n += count;
		return;
	}

	shim_batch_flush();

	shim_batch_v = v;
	shim_batch_n = count;
	shim_batch_prim = prim;
	shim_batch_tex = texture;
	shim_batch_3d = is3d;
}

static void shim_batch_flush (void)
{
	unsigned int prof_t0 = SHIM_PROF_NOW ();

	if (shim_batch_n > 0 && shim_batch_v)
	{
		/* THE LINE THAT WAS MISSING.
		 *
		 * sceGuDrawArray() hands the GE a bare ADDRESS (pspsdk
		 * src/gu/sceGuDrawArray.c: VADDR and PRIM, nothing else) and the GE
		 * then fetches those vertices over the bus, *bypassing the CPU's
		 * write-back data cache*.  A vertex the CPU has just written is
		 * therefore still in the cache and not yet in memory, so the GE reads
		 * whatever those cache lines last pushed there -- an older frame's
		 * vertices, or unrelated data that happens to live at that address.
		 *
		 * On the panel that is a picture whose one corner is dragged to a
		 * random place, and a picture that shows another picture's texels,
		 * because u and v travel in the same vertex.  It changes from frame to
		 * frame and from draw to draw because it depends on which cache lines
		 * the CPU happened to evict; it is not fixed by any amount of matrix
		 * or texture arithmetic, and it is invisible on an emulator that does
		 * not model the cache.
		 *
		 * pspsdk's own samples do exactly this before handing vertices to the
		 * GE (src/samples/gu/blend/blend.c: sceKernelDcacheWritebackAll()
		 * right after filling the vertex and texture arrays).  A range is
		 * better than "all": only this draw's slice has to reach memory. */
		sceKernelDcacheWritebackRange(shim_batch_v,
		                              (unsigned int)(shim_batch_n * (int)sizeof(shim_vertex_t)));

		if (shim_frame_active)
		{
			/* A world-space batch needs the GE's transform stage
			 * loaded before it can be drawn; the 2D one must not
			 * get those matrices, because it is the identity that
			 * keeps a pixel at the pixel it says it is. */
			if (shim_batch_3d)
				shim_ge3d_matrices();

			sceGuDrawArray(shim_batch_prim,
			               shim_batch_3d ? SHIM_VERTEX_FORMAT_3D
			                             : SHIM_VERTEX_FORMAT,
			               shim_batch_n, NULL, shim_batch_v);
			shim_frame_batches++;
		}
	}

	shim_batch_n = 0;
	shim_batch_v = NULL;

	/* Everything above -- the writeback, the matrices, the draw itself.  A
	 * batch that was empty costs the two clock reads and nothing else. */
	shim_prof_us[SHIM_PROF_DRAW] += SHIM_PROF_NOW () - prof_t0;
}

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static void shim_identity (shim_matrix_t *out)
{
	memset(out->m, 0, sizeof(out->m));
	out->m[0] = out->m[5] = out->m[10] = out->m[15] = 1.0f;
}

/* out = a * b, both column-major like GL */
static void shim_multiply (shim_matrix_t *out, const shim_matrix_t *a, const shim_matrix_t *b)
{
	shim_matrix_t t;
	int i, j;

	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			t.m[i * 4 + j] = a->m[0 * 4 + j] * b->m[i * 4 + 0] +
			                 a->m[1 * 4 + j] * b->m[i * 4 + 1] +
			                 a->m[2 * 4 + j] * b->m[i * 4 + 2] +
			                 a->m[3 * 4 + j] * b->m[i * 4 + 3];
	*out = t;
}

static shim_matrix_t *shim_current_matrix (void)
{
	return (shim_matrix_mode == GL_PROJECTION) ? &shim_projection : &shim_modelview;
}

/* ---- the GE vertex ring ----
 *
 * A draw gets a private slice of shim_ge_vertices and the GE keeps reading it
 * until it has finished that draw, so a slice must never be handed out twice
 * inside one frame.  When the ring is full the draw is dropped, loudly: a
 * picture that is missing is a bug report, a picture drawn from somebody
 * else's vertices is a wild goose chase. */
static shim_vertex_t *shim_ge_alloc (unsigned int count)
{
	shim_vertex_t *v;

	if (count == 0 || count > SHIM_GE_VERTICES - shim_ge_used)
	{
		if (shim_ge_overflows++ == 0)
			psp_tracef("ring: full at %u of %d vertices in frame %u, dropping draws",
			           shim_ge_used, SHIM_GE_VERTICES, shim_frames);
		return NULL;
	}

	v = &shim_ge_vertices[shim_ge_used];
	shim_ge_used += count;

	if (shim_ge_used > shim_ge_peak)
		shim_ge_peak = shim_ge_used;

	return v;
}

/* ------------------------------------------------------------------ */
/* textures                                                           */
/* ------------------------------------------------------------------ */

static shim_texture_t *shim_texture_for (unsigned int id, int create)
{
	if (id == 0)
		return NULL;

	if (id >= SHIM_MAX_TEXTURES)
		return NULL;

	if (!shim_textures[id].used && create)
	{
		shim_textures[id].used = 1;
		shim_textures[id].filter = GU_NEAREST;
		shim_texture_count++;
	}

	return shim_textures[id].used ? &shim_textures[id] : NULL;
}

/* Recomputed whenever a texture is bound or (re)sized: the vertex path needs
 * the current picture-to-box ratio, and it runs thousands of times a frame, so
 * it reads two floats instead of walking the texture table. */
static void shim_uv_scale_update (void)
{
	shim_texture_t *t = shim_texture_for(shim_texture_bound, 0);

	/* TEXELS, not fractions.
	 *
	 * GL hands the vertex stage a u in 0..1 and hands the hardware a u in
	 * 0..1 as well, so the obvious translation is to pass it straight
	 * through -- and that is what this did, with the picture scaled by
	 * width/potw to fit its power-of-two box.  The GE in through mode
	 * (GU_TRANSFORM_2D) does NOT take 0..1: it takes TEXEL coordinates
	 * directly.  pspsdk's own 2D texture sample says so plainly
	 * (src/samples/gu/font/font.c):
	 *
	 *	vertices[i*2].u   = 0;
	 *	vertices[i*2 + 1].u = widths[i];        // <- a texel count
	 *	vertices[i*2].v   = i * lineHeight;     // <- and so is this
	 *	vertices[i*2 + 1].v = (i + 1) * lineHeight;
	 *	sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 |
	 *	               GU_VERTEX_16BIT | GU_TRANSFORM_2D, ...)
	 *
	 * intraFont does the same (glyph->x, glyph->y straight into u and v)
	 * and never calls sceGuTexScale, which is what makes it work: the
	 * sampler divides by the size it was given in sceGuTexImage().  Pass
	 * 0..1 to it and it samples the 0..1 TEXEL box, i.e. the single texel
	 * in the top-left corner, stretched over the whole picture.  On the
	 * panel that is a flat field of one colour per picture -- the pale
	 * background, the blue bar, the cyan block -- and text made of blobs,
	 * because every glyph shows the colour of its own corner texel.
	 *
	 * So the picture's own size is the scale: u 0..1 becomes 0..width.
	 * Multiplying by the PICTURE size (not potw) is what keeps a 240x112
	 * picture inside the corner of its 256x128 box. */
	if (t && t->width > 0 && t->height > 0)
	{
		shim_u_scale = (float)t->width;
		shim_v_scale = (float)t->height;

		/* ---- and the 3D path needs the OPPOSITE unit ----------------
		 *
		 * Every world surface the engine draws divides its own texture
		 * coordinates by the miptex size (r_brush.c, Mod_PolyForUnlit/
		 * CalcSurfaceExtents: `s /= tex->width`), i.e. it hands
		 * glTexCoord2f a FRACTION of the picture.  GL takes fractions
		 * and the GE's transform pipe takes fractions too -- pspsdk
		 * src/gu/pspgu.h, sceGuTexScale():
		 *
		 *     Set texture scale
		 *     @note Only used by the 3D T&L pipe, renders with
		 *           GU_TRANSFORM_2D are not affected by this.
		 *     @param u - Scalar to multiply U coordinate with
		 *
		 * A *multiplier* of one and only for the 3D pipe means the 3D
		 * pipe's native unit is the same as GL's: 0..1 across the
		 * texture.  Through mode (2D) has no such multiplier and takes
		 * texels.  Passing the 2D scale to the 3D pipe is what the map
		 * showed for two rounds: u = 1.0 became u = 128.0 for a 128-wide
		 * miptex, so every wall showed the texture repeated 128 times
		 * -- "the textures have the wrong scale" -- and at that
		 * minification the result is a moire of texels, not a picture.
		 *
		 * Interstice-ProQuake draws map polys with the engine's own
		 * fractions and no scaling at all (source/psp/gu_surface.cpp,
		 * DrawGLPoly: GU_TEXTURE_32BITF | GU_VERTEX_32BITF over
		 * poly->verts), which is the same statement from the reference
		 * side.
		 *
		 * The one adjustment the 3D path does need is the box: a
		 * non-power-of-two picture lives in the top-left corner of a
		 * larger box, and 0..1 means the WHOLE box.  So the 3D scale is
		 * width/potw -- the same ratio the engine's own TEXPREF_PAD code
		 * uses for pictures (gl_draw.c: gl.sh = width /
		 * TexMgr_PadConditional(width)).  For a power-of-two texture,
		 * which is every miptex the GE will meet, that ratio is 1.0 and
		 * nothing changes. */
		shim_u_scale3d = (float)t->width / (float)t->potw;
		shim_v_scale3d = (float)t->height / (float)t->poth;
	}
	else
	{
		shim_u_scale = 1.0f;
		shim_v_scale = 1.0f;
		shim_u_scale3d = 1.0f;
		shim_v_scale3d = 1.0f;
	}
}

/* ---- 16-bit storage ----
 *
 * EDRAM belongs to the two frame buffers and the depth buffer, so the GE
 * samples textures straight out of main memory -- and main memory here is the
 * engine's few-megabyte malloc reserve, shared with picture decoding, map
 * loading and everything else.  At 32 bits a 512x512 picture costs a megabyte;
 * at 16 it costs half that, and the menu alone loads a dozen pictures before
 * the first frame is finished.  The 32-bit version ran the reserve dry while
 * Host_Init was still running (the log line "tex: no memory for a 256x256
 * picture"), and every picture after that was drawn as a white rectangle.
 *
 * GU_PSM_4444 keeps alpha (nibbles A,B,G,R); GU_PSM_5650 keeps 5-6-5 colour
 * with no alpha, which is exactly what an opaque picture needs -- one that
 * arrives as GL_RGB or GL_LUMINANCE has no alpha channel to lose.
 *
 * The pixels are converted straight into the memory the GE samples: no
 * intermediate RGBA8 buffer, because that would cost another megabyte. */

static unsigned short shim_px4444 (int r, int g, int b, int a)
{
	return (unsigned short)(((a >> 4) << 12) | ((b >> 4) << 8) |
	                        ((g >> 4) << 4) | (r >> 4));
}

/* GU_PSM_5650 keeps the same channel order as the 32-bit formats, only with
 * fewer bits: red in the low five, green in the middle six, blue in the top
 * five (this is the "ABGR1555/5650" layout the PSP uses for its own 16-bit
 * screen modes).  Written the other way round -- which it was -- every opaque
 * picture came out with its red and blue channels exchanged. */
static unsigned short shim_px5650 (int r, int g, int b)
{
	return (unsigned short)(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
}

/* ---- 8-bit pictures -----------------------------------------------------
 *
 * Half of everything the arena holds used to be twice as big as it needs to
 * be.  The round-83 log is the reason this exists at all:
 *
 *     tex: 240x112 picture (64 KB box) does not fit the pool and would take
 *          2112 KB of the engine's heap (cap 2048 KB): drawn with the null
 *          texture
 *     img: gfx/a_mmenu/mainmenu1.png ok 240x112 (3343 bytes)
 *
 * -- six lines apart, every frame, 240 ms a frame of which the panel shows
 * nothing (ge 0.1).  The arena is full (6140 KB of 6144) with the map's own
 * textures and the model skins, and the menu art it cannot fit is 100 KB.  No
 * eviction policy invents 100 KB: the pictures have to get smaller.
 *
 * A GE texture can be 8-bit indices plus a 256-entry palette (GU_PSM_T8 +
 * sceGuClutLoad, pspgu.h:"GU_PSM_T8 - Indexed, 8-bit").  That is exactly what
 * a Quake world texture already is -- an 8-bit miptex against a 256-colour
 * palette -- so for those pictures the conversion is lossless: the palette is
 * the distinct colours of the picture, and a miptex has at most 256 of them.
 *
 * The rule is deliberately narrow, because a wrong pixel format on the panel
 * is a black screen and this port cannot be debugged frame by frame:
 *
 *   - only when every texel is opaque (a == 255): alpha art keeps 4444, so
 *     not one blending path changes;
 *   - only when the picture has at most 256 distinct 15-bit colours, which is
 *     checked before anything is allocated.  A picture with more colours --
 *     every photo-like menu picture -- stays 16-bit rather than being
 *     quantised, because "the menu renders correctly" is worth more here than
 *     two megabytes;
 *   - never for a lightmap (shim_texture_priority): those are updated with
 *     glTexSubImage2D, and indices cannot be updated in place.
 */
static unsigned short shim_pal_lut[32768];       /* 15-bit key -> index+1 */
static unsigned int  shim_pal_color[256];

#define SHIM_PAL_KEY(r, g, b)  ((((r) >> 3) << 10) | (((g) >> 3) << 5) | ((b) >> 3))

/* The palette lives in the tail of the picture's own block. */
static unsigned int *shim_palette_of (shim_texture_t *t)
{
	return (unsigned int *)((unsigned char *)t->pixels +
	                        (size_t)t->potw * (size_t)t->poth);
}

/* Would this picture survive an 8-bit palette without changing a pixel?
 *
 * Runs before the allocation, so a picture that cannot is not even asked for
 * an arena block.  `distinct` stops counting past 256: the answer is already
 * no, and the rest of the picture still has to be scanned for alpha. */
#define SHIM_T8_MAX_COLORS 256

static int shim_picture_fits_8bit (const unsigned char *src, int row_bytes,
                                   int w, int h, GLenum format)
{
	static unsigned char seen[32768];
	int row, col, distinct = 0;

	memset(seen, 0, sizeof(seen));

	for (row = 0; row < h; row++)
	{
		const unsigned char *s = src + (size_t)row * (size_t)row_bytes;

		for (col = 0; col < w; col++)
		{
			int r, g, b, a;

			switch (format)
			{
			case GL_RGBA:  r = s[0]; g = s[1]; b = s[2]; a = s[3]; s += 4; break;
			case GL_BGRA:  b = s[0]; g = s[1]; r = s[2]; a = s[3]; s += 4; break;
			case GL_RGB:   r = s[0]; g = s[1]; b = s[2]; a = 255;  s += 3; break;
			case GL_LUMINANCE_ALPHA:
			               r = g = b = s[0]; a = s[1]; s += 2; break;
			case GL_LUMINANCE:
			               r = g = b = s[0]; a = 255;  s += 1; break;
			case GL_ALPHA: r = g = b = 255;  a = s[0]; s += 1; break;
			default:       r = s[0]; g = s[1]; b = s[2]; a = s[3]; s += 4; break;
			}

			/* Any transparency at all and this is not an 8-bit picture:
			 * alpha art keeps 4444, so not one blending path changes. */
			if (a != 255)
				return 0;

			if (distinct <= SHIM_T8_MAX_COLORS)
			{
				unsigned int key = SHIM_PAL_KEY(r, g, b);

				if (!seen[key])
				{
					seen[key] = 1;
					distinct++;
				}
			}
		}
	}

	return distinct <= SHIM_T8_MAX_COLORS;
}

static void shim_put_pixels (shim_texture_t *t, int x, int y, int w, int h,
                             const unsigned char *src, int row_bytes, GLenum format)
{
	int row, col, opaque = (t->psm == GU_PSM_5650);
	int t8 = (t->psm == GU_PSM_T8);
	int bpp = t8 ? 1 : 2;
	int pal_n = 0;
	unsigned char *base = (unsigned char *)t->pixels +
	                      (size_t)y * (size_t)t->tbw * (size_t)bpp +
	                      (size_t)x * (size_t)bpp;

	/* The table belongs to one picture: an index left over from the
	 * previous upload would put another picture's colour under this texel. */
	if (t8)
		memset(shim_pal_lut, 0, sizeof(shim_pal_lut));

	for (row = 0; row < h; row++)
	{
		const unsigned char *s = src + (size_t)row * (size_t)row_bytes;
		unsigned char *d8 = base + (size_t)row * (size_t)t->tbw;
		unsigned short *d16 = (unsigned short *)(base +
		                       (size_t)row * (size_t)t->tbw * (size_t)bpp);

		for (col = 0; col < w; col++)
		{
			int r, g, b, a;

			switch (format)
			{
			case GL_RGBA:  r = s[0]; g = s[1]; b = s[2]; a = s[3]; s += 4; break;
			case GL_BGRA:  b = s[0]; g = s[1]; r = s[2]; a = s[3]; s += 4; break;
			case GL_RGB:   r = s[0]; g = s[1]; b = s[2]; a = 255;  s += 3; break;
			case GL_LUMINANCE_ALPHA:
			               r = g = b = s[0]; a = s[1]; s += 2; break;
			case GL_LUMINANCE:
			               r = g = b = s[0]; a = 255;  s += 1; break;
			case GL_ALPHA: r = g = b = 255;  a = s[0]; s += 1; break;
			default:       r = s[0]; g = s[1]; b = s[2]; a = s[3]; s += 4; break;
			}

			if (t8)
			{
				unsigned int key = SHIM_PAL_KEY(r, g, b);
				unsigned int idx = shim_pal_lut[key];

				if (!idx)
				{
					/* glTexImage2D() has already checked that the whole
					 * picture has at most 256 of these, so this cannot
					 * run past the palette. */
					if (pal_n < 256)
					{
						shim_pal_color[pal_n] =
						    0xFF000000u | ((unsigned)r << 0) |
						    ((unsigned)g << 8) | ((unsigned)b << 16);
						pal_n++;
					}
					idx = (unsigned int)pal_n;
					shim_pal_lut[key] = idx;
				}

				d8[col] = (unsigned char)(idx - 1);
			}
			else
				d16[col] = opaque ? shim_px5650(r, g, b)
				                  : shim_px4444(r, g, b, a);
		}
	}

	if (t8)
	{
		unsigned int *pal = shim_palette_of(t);
		unsigned int i;

		for (i = 0; i < (unsigned int)pal_n; i++)
			pal[i] = shim_pal_color[i];
		for (; i < 256; i++)
			pal[i] = 0xFF000000u;   /* black, for an index no texel uses */

		t->dirty = 1;
	}
}

/* Repeat the picture's right and bottom edge into the margin of its
 * power-of-two box.
 *
 * The engine hands us pictures at their own size (gl_texture_NPOT is true in
 * vid_psp.c), so a 196x24 picture lives in the top-left corner of a 256x32
 * box whose margin shim_texture_alloc() cleared to zero.  u/v never point
 * into that margin -- they stop at 196 and 24 -- but a GU_LINEAR filter reads
 * the texels AROUND a sample point, so the last column and the last row are
 * blended with the cleared margin: a one-pixel dark (or, with alpha, a
 * see-through) seam down the right and bottom edge of every such picture,
 * which is what the engine's own TexMgr_PadEdgeFixW/H exists to prevent on a
 * desktop.
 *
 * Copying the edge outwards is exactly that fix, and it only has to happen
 * once per upload: shim_put_pixels() writes the picture itself, this writes
 * the border around it, and nothing samples the border after that. */
static void shim_edge_extend (shim_texture_t *t)
{
	int row, col;

	if (!t || !t->pixels || t->width <= 0 || t->height <= 0)
		return;

	if (t->potw <= t->width && t->poth <= t->height)
		return;	/* the box is the picture: nothing to extend into */

	if (t->psm == GU_PSM_T8)
	{
		/* One byte per texel: the same two passes, over indices.  Copying
		 * an index is copying the colour it names -- the palette is shared
		 * by the whole picture. */
		unsigned char *b8 = (unsigned char *)t->pixels;

		if (t->potw > t->width)
			for (row = 0; row < t->height; row++)
			{
				unsigned char *p = b8 + (size_t)row * (size_t)t->tbw;
				unsigned char edge = p[t->width - 1];

				for (col = t->width; col < t->potw; col++)
					p[col] = edge;
			}

		if (t->poth > t->height)
		{
			unsigned char *last = b8 + (size_t)(t->height - 1) * (size_t)t->tbw;

			for (row = t->height; row < t->poth; row++)
				memcpy(b8 + (size_t)row * (size_t)t->tbw, last,
				       (size_t)t->potw);
		}

		return;
	}

	{
	unsigned short *base = (unsigned short *)t->pixels;
	unsigned short *lastrow, edge;

	/* right edge: every row of the picture, from its last texel outwards */
	if (t->potw > t->width)
	{
		for (row = 0; row < t->height; row++)
		{
			unsigned short *p = base + (size_t)row * (size_t)t->tbw;
			edge = p[t->width - 1];
			for (col = t->width; col < t->potw; col++)
				p[col] = edge;
		}
	}

	/* bottom edge: the last row -- already extended to full width above --
	 * repeated down to the bottom of the box */
	if (t->poth > t->height)
	{
		lastrow = base + (size_t)(t->height - 1) * (size_t)t->tbw;
		for (row = t->height; row < t->poth; row++)
			memcpy(base + (size_t)row * (size_t)t->tbw, lastrow,
			       (size_t)t->potw * sizeof(unsigned short));
	}
	}
}

/* ---- the picture arena ----
 *
 * Every picture the engine draws costs twice its power-of-two box in main
 * memory (16 bits per pixel) and there is no VRAM to put them in: the GE
 * samples them straight out of the heap.  Each one used to be its own
 * memalign() block out of the general purpose pool the engine also decodes
 * pictures, demos and maps into, so the pool filled with a few dozen blocks of
 * every size from 8x8 to 512x512 -- which is how a run reports plenty of
 * memory free in total and still cannot provide one 256 KB picture.
 *
 * Pictures now have an address space of their own: tex_arena.c, a first-fit
 * allocator on a 16-byte grid that merges free neighbours, so it cannot be
 * shredded.  It takes its memory in CHUNKS, on demand:
 *
 *   * a chunk is created only when an allocation cannot be satisfied from the
 *     chunks that exist, so the footprint follows what is actually on screen;
 *   * a chunk that has become completely empty is handed back to the heap;
 *   * the total is capped, and when the cap is reached the port's eviction
 *     hook is asked to unload pictures the interface has stopped drawing
 *     before the allocation is reported as failed.
 *
 * Chunks rather than one big reservation because a reservation is charged at
 * the wrong moment.  The previous version took 4 MB before the interface had
 * loaded a single picture; the very next thing that happened was
 *
 *     img: gfx/conchars_ru.png FAILED: outofmem
 *
 * -- the PNG decoder could not get a few hundred KB while 4 MB sat empty, and
 * the Russian charset never appeared.  The arena has to grow into the memory a
 * picture needs, not take it in advance and hope nobody else wanted it.
 *
 * 16 bits per pixel is what the GE samples and there is no second copy
 * anywhere, so "used" is the true cost of the pictures on screen. */

#define SHIM_ARENA_CHUNK_MIN   (256u * 1024u)	/* smallest chunk we take    */
#define SHIM_ARENA_CHUNK_MAX   (1024u * 1024u)	/* largest single chunk      */
/* Pictures never exceed this.  It matches PSP_PICTURE_POOL in main_psp.c: the
 * port takes one block of that size at start-up and hands it to the arena
 * (psp_gl_shim_set_pool), so the cap and the block are the same number and the
 * arena can never need a chunk the block does not have. */
#define SHIM_ARENA_LIMIT       (6u * 1024u * 1024u)

/* How much of the *heap* the arena may borrow when it is full.
 *
 * A picture the arena cannot serve used to fall back to memalign() from the
 * same malloc heap the engine keeps its entity array, its lightmaps, its model
 * scratch and its progs in.  On the hardware that turned into
 *
 *     tex: arena short for 256 KB: ... retry STILL FAILED (6019 KB used of 6144 KB)
 *     tex: ... kept outside the pool, 17 pictures outside it now
 *     run: f 408 ..., malloc 10232 KB used, 1 KB left in the heap block, NULL 17
 *
 * -- seventeen textures borrowed the heap, the heap ran out, and from then on
 * seventeen allocations inside the *engine* failed.  A texture that is missing
 * draws as the engine's null texture, which is ugly and visible; an allocation
 * that fails inside the map loader ends the session.  So the borrowing is
 * capped: past this much, a picture is reported and left without a texture, and
 * the heap stays the engine's. */
#define SHIM_FALLBACK_LIMIT    (2u * 1024u * 1024u)

static tex_arena_t  shim_arena;
static int          shim_arena_ready;
static int          shim_arena_pooled;	/* holds one block, see below      */
static int          shim_arena_refused;	/* the cap, for the log           */
static int          shim_arena_fallback;	/* pictures that had to use memalign */
static size_t       shim_fallback_bytes;	/* and how much heap they took    */
/* ---- the lightmaps ----------------------------------------------------
 *
 * A lightmap is not a picture: the world is unlit without it, it exists for
 * the whole level, and there are only a few dozen of them.  The picture arena
 * is full by the time a map is entered -- the round-73 log has it at 6131 KB
 * of 6144 with 12 KB free, and the fallback path outside the pool at its
 * 2048 KB cap -- so every lightmap would be refused and every surface of the
 * map would be drawn with the null texture: a black world that is otherwise
 * rendering correctly, which is a far worse thing to debug than a crash.
 *
 * So the engine marks the texture it is about to upload as a lightmap
 * (TexMgr_LoadLightmap calls psp_gl_shim_priority_texture), and that one
 * allocation is allowed past the picture cap under a budget of its own.  A
 * 128x128 lightmap block is 32 KB here (port/psp/vid_psp.c sets the block
 * size), so this budget holds 48 of them.
 *
 * The flag is sticky until the next glTexImage2D, which is exactly the window
 * TexMgr_LoadLightmap needs (GL_Bind, then glTexImage2D). */
#define SHIM_PRIORITY_LIMIT    (1536u * 1024u)
static int          shim_texture_priority;
static size_t       shim_priority_bytes;
static int          shim_priority_logged;

void psp_gl_shim_priority_texture (int on)
{
	shim_texture_priority = on ? 1 : 0;
}
static int          shim_arena_short_logs;	/* "arena short" lines written  */

/* Called when the arena cannot satisfy a request.  The pictures the engine is
 * no longer asking for are the ones worth their memory; the port plugs its
 * cache purge in here (gu_psp.c -> Draw_EvictCachePics()) and we try again.
 * Returns how many pictures it released, so the log can say what happened. */
static int (*shim_evict_hook)(void);

void psp_gl_shim_set_evict_hook (int (*fn)(void))
{
	shim_evict_hook = fn;
}

/* Hand the arena one block for the whole run (main_psp.c sizes and takes it).
 *
 * Two things change when this is used.  The arena stops asking malloc for a
 * 256 KB-1 MB chunk every time it runs out -- one block, no fragmentation, no
 * calls into the allocator in the middle of drawing.  And it stops giving
 * chunks back: free() of a block this size does not return memory on this
 * platform (see the measurements in main_psp.c), so trimming a pooled arena
 * would be a one-way loss of picture memory.  The cap still bounds it, and a
 * picture the pool cannot hold still falls back to memalign(). */
void psp_gl_shim_set_pool (void *base, size_t bytes)
{
	if (!base || !bytes)
		return;

	if (!shim_arena_ready)
	{
		tex_arena_init(&shim_arena);
		shim_arena_ready = 1;
	}

	if (!tex_arena_add_chunk(&shim_arena, base, bytes))
		return;

	shim_arena_pooled = 1;
}

/* What a new picture could actually get right now: the biggest free run in the
 * arena, payload only.  The port reads it between frames to decide whether to
 * unload idle pictures while nothing is being drawn -- see the note in
 * gu_psp.c, which is where this number matters. */
size_t psp_gl_shim_texture_room (void)
{
	if (!shim_arena_ready)
		return 0;

	return tex_arena_largest_free(&shim_arena);
}

/* One line that says the truth about the picture pool, for the frame card. */
void psp_gl_shim_texture_report (void)
{
	if (!shim_arena_ready)
	{
		psp_tracef("tex: %d pictures, %d KB, no arena (one malloc each)",
		           (int)shim_texture_count, (int)(shim_texture_bytes / 1024));
		return;
	}

	psp_tracef("tex: %d pictures, %d KB in use of %d KB arena (%d chunks of "
	           "256 KB-1 MB, cap %d KB), largest free %d KB in %d runs, "
	           "%d outside the arena taking %d KB of the heap (cap %d KB), "
	           "%d refused",
	           (int)shim_texture_count, (int)(tex_arena_used(&shim_arena) / 1024),
	           (int)(tex_arena_size(&shim_arena) / 1024), shim_arena.chunks,
	           (int)(SHIM_ARENA_LIMIT / 1024),
	           (int)(tex_arena_largest_free(&shim_arena) / 1024),
	           tex_arena_free_runs(&shim_arena), shim_arena_fallback,
	           (int)(shim_fallback_bytes / 1024), (int)(SHIM_FALLBACK_LIMIT / 1024),
	           shim_arena_refused);
}

/* Walk the arena's block list and report the first thing that does not add
 * up.  This is a corruption detector, not a diagnostic: the block headers live
 * inside the picture memory the GE samples, so a picture saved with a box
 * bigger than the room it was given writes straight over them -- and the
 * symptom of that is a freeze somewhere else entirely, minutes later, in code
 * that has nothing to do with pictures.  Called once a second from the frame
 * loop; one failed check is printed once and not repeated.
 *
 * Returns 1 while the arena is intact. */
int psp_gl_shim_texture_sane (void)
{
	static int complained;
	const char *why = NULL;

	if (!shim_arena_ready)
		return 1;

	if (tex_arena_check(&shim_arena, &why))
		return 1;

	if (!complained)
	{
		complained = 1;
		psp_tracef("tex: ARENA BROKEN: %s -- picture memory has been written "
		           "past its block, expect a crash anywhere from here on",
		           why ? why : "?");
	}

	return 0;
}

/* How much room the arena keeps for itself even when it is empty.
 *
 * Trimming every chunk that happens to be empty was thrashing the arena.  The
 * hardware log of build 07:12 shows the shape of it:
 *
 *     tex: 67 pictures, 2399 KB in use of 2568 KB arena (9 chunks ...)
 *     ...evict 16 pictures, their chunk becomes empty, trim frees it...
 *     tex: arena short for 256 KB: 48 idle pictures unloaded, retry STILL
 *          FAILED (6 chunks, 1401 KB used of 1808 KB, largest free 127 KB)
 *
 * i.e. the arena had been shrunk to 1808 KB by trimming while the interface
 * still needed it, so the next picture had to come from a fresh 256 KB chunk
 * that nothing would ever grow again during that emergency path.  Chunks are
 * given back only above this much free room, which is more than any single
 * menu screen asks for. */
#define SHIM_ARENA_KEEP_FREE   (768u * 1024u)

/* Give back the chunks that hold nothing, down to the watermark above.  Called
 * after a picture is released: on a console with no VRAM, memory that is not
 * holding a picture should be back in the heap where the map and the decoder
 * can use it -- but not so eagerly that the interface has to take it back a
 * frame later. */
static void shim_arena_trim (void)
{
	int index;

	if (!shim_arena_ready)
		return;

	/* A pooled arena owns its block and nothing may take it back. */
	if (shim_arena_pooled)
		return;

	while ((index = tex_arena_idle_chunk(&shim_arena)) >= 0)
	{
		void *base;

		if (tex_arena_size(&shim_arena) - tex_arena_used(&shim_arena) <=
		    SHIM_ARENA_KEEP_FREE)
			break;

		base = shim_arena.chunk[index].base;
		tex_arena_remove_chunk(&shim_arena, index);
		free(base);
	}
}

/* The other half of the same budget: the picture *decoder*.
 *
 * A texture costs 16 bits per pixel out of the arena, but a picture under
 * construction is w*h*4 bytes of RGBA from plain malloc(), and both pools are
 * fed from the same heap -- the part of it the engine's zone does not take
 * (main_psp.c measures it; PSP_MALLOC_FLOOR is what the zone may not touch).
 * The hardware log of build 08:12 ends with
 *
 *     img: gfx/a_sgl/asgl3.bmp FAILED: outofmem (14166 bytes,
 *          sig 42 4D 56 37 00 00 00 00)
 *
 * "outofmem" is stb_image's word for "malloc() returned NULL".  Nothing was
 * wrong with the file: it was read completely, its own header says it is 14166
 * bytes long, and those bytes start with "BM".  What was missing was the
 * decode buffer -- while the picture cache was holding megabytes of pictures
 * that no frame had drawn for a long time (the same log has "tex 4022 KB" a
 * few dozen frames earlier).
 *
 * A cache that will not give memory back is not a cache, so the decoder asks
 * here: the port's own purge decides which pictures are really idle, and then
 * the chunks that became empty go back to the heap instead of sitting in the
 * arena's free list waiting for the interface to want them again.  This is the
 * same pair of operations shim_picture_alloc() already performs when a texture
 * does not fit; the only difference is who is short of memory. */
int psp_gl_shim_reclaim_pictures (void)
{
	int released = 0;

	if (shim_evict_hook)
		released = shim_evict_hook();

	shim_arena_trim();

	return released;
}

/* Take one more chunk big enough for `need` and try again.  Returns 1 when the
 * arena can now serve it. */
static int shim_arena_grow (size_t need)
{
	size_t chunk = need + TEX_ARENA_ALIGN + 4096u;
	void *base;

	/* A pooled arena (psp_gl_shim_set_pool) IS its block: it is exactly
	 * SHIM_ARENA_LIMIT big, so there is nothing to grow into and no reason to
	 * call memalign() while a frame is being drawn.  Growing is only for the
	 * fallback case where psp_mem.c could not get a block. */
	if (shim_arena_pooled)
		return 0;

	if (!shim_arena_ready)
	{
		tex_arena_init(&shim_arena);
		shim_arena_ready = 1;
	}

	if (chunk < SHIM_ARENA_CHUNK_MIN)
		chunk = SHIM_ARENA_CHUNK_MIN;

	if (chunk > SHIM_ARENA_CHUNK_MAX)
		chunk = SHIM_ARENA_CHUNK_MAX;

	/* The cap is the point of the cap: a request that the remaining 200 KB
	 * of it can serve must not be refused because the *minimum* chunk is
	 * 256 KB.  The log had exactly that shape --
	 *
	 *     tex: arena short for 256 KB: ... retry STILL FAILED
	 *          (17 chunks, 4664 KB used of 4904 KB)
	 *
	 * -- with 216 KB of the 5120 KB cap still unused and unusable, and the
	 * picture it wanted then fell out of the pool into a plain memalign(),
	 * where nothing counts it, trims it or ever gets it back. */
	if (tex_arena_size(&shim_arena) + chunk > SHIM_ARENA_LIMIT)
	{
		size_t left = SHIM_ARENA_LIMIT - tex_arena_size(&shim_arena);

		if (left < need + TEX_ARENA_ALIGN + 64u)
			return 0;

		chunk = left;
	}

	chunk = (chunk + 4095u) & ~(size_t)4095u;

	base = memalign(16, chunk);
	if (!base)
		return 0;

	if (!tex_arena_add_chunk(&shim_arena, base, chunk))
	{
		free(base);
		return 0;
	}

	return 1;
}

/* Allocate picture storage: from the arena if possible, growing it or evicting
 * what the interface has stopped drawing if not. */
static void *shim_picture_alloc (size_t need)
{
	void *p;

	if (!shim_arena_ready)
	{
		tex_arena_init(&shim_arena);
		shim_arena_ready = 1;
	}

	p = tex_arena_alloc(&shim_arena, need);
	if (p)
		return p;

	if (shim_arena_grow(need))
	{
		p = tex_arena_alloc(&shim_arena, need);
		if (p)
			return p;
	}

	/* Out of room: unload the pictures nobody is drawing any more and try
	 * once more.  The chunks stay (they hold the pictures that survived).
	 *
	 * This is the emergency path, and it is expensive: releasing a picture
	 * means the next frame that wants it tears it down and decodes the file
	 * again from the memory stick.  That is why the port also checks the
	 * arena's free room between frames (gu_psp.c) and unloads there, with the
	 * GPU idle -- doing this here, in the middle of building a frame, is how a
	 * menu that is merely full of pictures turns into a second-long stall. */
	if (shim_evict_hook)
	{
		int released = 0;
		int tries;
		int grew = 0;

		/* The hook releases a bounded batch (see CACHEPIC_MAX_PER_PASS in
		 * gl_draw.c), because emptying the whole cache behind a screen that
		 * is about to be drawn again costs a hundred re-decodes from the
		 * memory stick.  So ask it again while the batch it gave back was
		 * worth something: sixteen pictures at a time until either the
		 * picture fits or a pass releases nothing. */
		for (tries = 0; tries < 8; tries++)
		{
			int got = shim_evict_hook();

			released += got;

			shim_arena_trim();

			/* The freed blocks are coalesced by the arena, but a
			 * picture needs its box in ONE run: if the runs that came
			 * back are not big enough, take another chunk.  Without
			 * this the emergency path only ever freed memory and never
			 * grew, which is exactly what the log shows -- "48 idle
			 * pictures unloaded, retry STILL FAILED (6 chunks, 1401 KB
			 * used of 1808 KB)" -- and the picture went outside the
			 * pool. */
			p = tex_arena_alloc(&shim_arena, need);

			/* Two chunks at most per request: a run that is still too
			 * small after that is a layout problem, not a shortage, and
			 * the arena must not creep up to its cap one retry at a
			 * time. */
			if (!p && grew < 2 && tex_arena_largest_free(&shim_arena) < need &&
			    shim_arena_grow(need))
			{
				grew++;
				p = tex_arena_alloc(&shim_arena, need);
			}

			if (p)
				break;

			if (got == 0)
				break;
		}
		if (p)
		{
			/* Worth a line in the log: it says the pool was full, what
			 * it gave up to make room, and that the picture did load. */
			if (shim_arena_short_logs < 20)
			{
				shim_arena_short_logs++;
				psp_tracef("tex: arena short for %d KB: %d idle pictures "
				           "unloaded, retry ok (%d KB free now in one piece)",
				           (int)(need / 1024), released,
				           (int)(tex_arena_largest_free(&shim_arena) / 1024));
			}

			return p;
		}

		if (shim_arena_short_logs < 20)
		{
			shim_arena_short_logs++;
			psp_tracef("tex: arena short for %d KB: %d idle pictures unloaded, "
			           "retry STILL FAILED (%d chunks, %d KB used of %d KB, "
			           "largest free %d KB)",
			           (int)(need / 1024), released, shim_arena.chunks,
			           (int)(tex_arena_used(&shim_arena) / 1024),
			           (int)(tex_arena_size(&shim_arena) / 1024),
			           (int)(tex_arena_largest_free(&shim_arena) / 1024));
		}
	}

	if (tex_arena_size(&shim_arena) + need >= SHIM_ARENA_LIMIT)
		shim_arena_refused = 1;

	return NULL;
}

/* Make sure the texture has storage for width x height at the wanted depth,
 * and keep the memory if it already fits.
 *
 * "Fits" is the important word.  This used to free the block whenever the
 * picture changed size or changed between the two 16-bit formats -- and both
 * formats are exactly 2 bytes per pixel, so the replacement was the same size
 * as the block that had just been released.  Every such free/malloc pair
 * carves the pool into one more pair of pieces; a picture that alternates
 * between GL_RGB and GL_RGBA uploads then walks the heap down until malloc
 * cannot find a block at all, while the amount actually held never grows.
 * Reusing the block removes the churn entirely. */
/* The largest texture the GE can address in either direction.  This is not a
 * policy choice: pspsdk src/gu/sceGuTexImage.c asserts width and height 1..512
 * and encodes each as a 4-bit LOG2 field. */
#define SHIM_TEX_MAX 512

/* Round a picture size up to what the GE can sample.
 *
 * sceGuTexImage() does not store width and height, it stores their LOGARITHMS
 * (pspsdk src/gu/sceGuTexImage.c: getExp(height) << 8 | getExp(width)), and its
 * own assert says "Texture width must be power of 2".  A 240x112 menu picture
 * announced as 128x64 is sampled from the wrong place in memory, with the wrong
 * row stride: on the panel that is a stretched, striped mess, and it is exactly
 * what the non-power-of-two menu art looked like, while the power-of-two art
 * (charset 128x128, logos 256x64, plaque 32x256, conback2 512x512) was fine.
 *
 * So the picture is copied into the next power-of-two box and every texture
 * coordinate is scaled by width/potw at vertex time (shim_uv_scale below).
 * 8 is the floor: it keeps the row 16 bytes wide, which is what an unswizzled
 * 16-bit texture wants. */
static int shim_pot (int n)
{
	int p = 8;

	while (p < n && p < 1024)
		p <<= 1;

	return p;
}

#define SHIM_PALETTE_BYTES 1024	/* 256 GU_PSM_8888 entries, 16-byte aligned */

static int shim_texture_alloc (shim_texture_t *t, int width, int height,
                               int opaque, int t8)
{
	size_t need;
	int changed;
	unsigned int new_psm;
	int potw = shim_pot(width), poth = shim_pot(height);

	t->tbw = potw;
	need = (size_t)potw * (size_t)poth * (t8 ? 1 : 2) +
	       (t8 ? SHIM_PALETTE_BYTES : 0);

	/* Beyond what the GE can address, in either direction.
	 *
	 * Below the picture size the box would be smaller than the picture.  Above
	 * 512 it is worse than a wrong size: sceGuTexImage() stores the LOG2 of
	 * each dimension in a 4-bit field, and pspsdk derives it with
	 *
	 *     getExp(val) { return 31 - __builtin_clz(val & 0x3FF); }
	 *
	 * so a 1024-wide picture goes through the mask as 0, __builtin_clz(0) is
	 * 32, and getExp() returns -1.  (getExp(h) << 8) | -1 is 0xFFFF, i.e. the
	 * GE is told to sample a 32768x32768 texture and reads far outside the
	 * picture memory.  On hardware that is garbage on the panel in the best
	 * case and a GE that never finishes in the worst -- and the emulator does
	 * not reproduce it, because it clamps the size field.
	 *
	 * So the picture is not uploaded, glTexImage2D() says so once with its
	 * size, and the drawing code gets the engine's null texture. */
	if (potw < width || poth < height ||
	    potw > SHIM_TEX_MAX || poth > SHIM_TEX_MAX)
		return 0;	/* the caller logs it */

	new_psm = t8 ? GU_PSM_T8 : (opaque ? GU_PSM_5650 : GU_PSM_4444);

	changed = (t->width != width || t->height != height ||
	           t->potw != potw || t->poth != poth || t->psm != new_psm);

	/* The block is exactly the picture's box now, and the arena coalesces,
	 * so a block of the wrong size is always returned and taken again --
	 * that is what gives the memory of a picture that shrank back to the
	 * pool.  (Before the arena this had to be "keep it if it is big
	 * enough", because free + malloc of the same size shredded the pool.) */
	if (t->pixels && t->bytes != need)
	{
		if (t->in_arena)
		{
			tex_arena_free(&shim_arena, t->pixels);
			shim_arena_trim();
		}
		else
		{
			free(t->pixels);

			/* Give the borrowing back: a block that lived outside the
			 * pool must stop counting against SHIM_FALLBACK_LIMIT when it
			 * is released, or the cap would only ever be reached once. */
			if (t->priority)
			{
				if (shim_priority_bytes >= t->bytes)
					shim_priority_bytes -= t->bytes;
				else
					shim_priority_bytes = 0;
			}
			else if (shim_fallback_bytes >= t->bytes)
				shim_fallback_bytes -= t->bytes;
			else
				shim_fallback_bytes = 0;
			if (shim_arena_fallback > 0)
				shim_arena_fallback--;
		}

		shim_texture_bytes -= t->bytes;
		t->pixels = NULL;
		t->bytes = 0;
		t->in_arena = 0;
		changed = 1;
	}

	t->width = width;
	t->height = height;
	t->potw = potw;
	t->poth = poth;
	t->psm = new_psm;

	if (!t->pixels && need)
	{
		/* 16-byte alignment is not optional: sceGuTexImage() asserts
		 * "Texture buffer pointer must be 16-byte aligned".  The arena
		 * guarantees it; the memalign() fallback asks for it. */
		t->pixels = shim_picture_alloc(need);
		t->in_arena = (t->pixels != NULL);

		if (!t->pixels)
		{
			/* The arena is capped and still full after the eviction
			 * pass: rather than lose the picture, fall back to a plain
			 * memalign.  It is the old, fragmented path -- but a picture
			 * that fits is better than one that does not.  (What lives
			 * out here is invisible to every arena report and is never
			 * trimmed, which is why it is counted and printed.)
			 *
			 * Capped: see SHIM_FALLBACK_LIMIT.  A picture that would
			 * take the heap past the cap is left without one, and the
			 * engine keeps its memory. */
			if (shim_texture_priority &&
			    shim_priority_bytes + need <= SHIM_PRIORITY_LIMIT)
			{
				/* A lightmap: allowed past the picture cap, see
				 * SHIM_PRIORITY_LIMIT.  Counted separately and said once,
				 * so the log still shows where the heap went. */
				t->pixels = memalign(16, need);
				t->in_arena = 0;
				if (t->pixels)
				{
					t->priority = 1;
					shim_priority_bytes += need;
					if (shim_priority_logged++ == 0)
						psp_tracef("tex: lightmaps are kept outside the picture "
						           "pool: the first one took %d KB (budget %d KB)",
						           (int)(need / 1024),
						           (int)(SHIM_PRIORITY_LIMIT / 1024));
				}
			}
			else if (shim_fallback_bytes + need > SHIM_FALLBACK_LIMIT)
			{
				shim_arena_refused++;

				if (shim_arena_refused <= 4)
					psp_tracef("tex: %dx%d picture (%d KB box) does not fit the "
					           "pool and would take %d KB of the engine's heap "
					           "(cap %d KB): drawn with the null texture",
					           width, height, (int)(need / 1024),
					           (int)((shim_fallback_bytes + need) / 1024),
					           (int)(SHIM_FALLBACK_LIMIT / 1024));
			}
			else
			{
				t->pixels = memalign(16, need);
				t->in_arena = 0;
				if (t->pixels)
				{
					shim_fallback_bytes += need;
					shim_arena_fallback++;

					if (shim_arena_fallback <= 20)
						psp_tracef("tex: %dx%d picture (%d KB box) does not fit the "
						           "arena even after unloading: kept outside the "
						           "pool, %d pictures outside it now",
						           width, height, (int)(need / 1024),
						           shim_arena_fallback);
				}
			}
		}

		if (!t->pixels)
			return 0;

		t->bytes = need;
		shim_texture_bytes += need;
		changed = 1;
	}

	if (changed && t->pixels)
	{
		/* A linear filter reads one texel past the end of the picture at
		 * the right and bottom edge, and a reused block still holds the
		 * previous picture there.  The whole power-of-two box is cleared
		 * for the same reason: the space between the picture and the
		 * edge of the box is sampled by that filter. */
		memset(t->pixels, 0, t->bytes);
	}

	t->dirty = 1;
	return 1;
}

static void shim_texture_memory_short (int width, int height)
{
	static int warned, said;
	int potw = shim_pot(width), poth = shim_pot(height);

	/* Say it a few times, then stop.
	 *
	 * The picture arena being full is a *steady state* on this console, not
	 * an event: once the map and its models are in, a picture that does not
	 * fit is asked for again on the next frame, and the frame after that.
	 * The round-79 log has this line twenty-two times in one second of play
	 * -- each one a formatted string written to the memory stick, at a
	 * moment when the frame is already 155 ms long.  The first eight carry
	 * every number worth knowing (the box, the arena, the largest free run);
	 * the ninth says how many followed it and then this stays quiet until
	 * the run ends. */
	if (said >= 8)
	{
		if (said == 8)
		{
			said++;
			psp_tracef("tex: more pictures did not fit the arena; saying "
			           "so once here and not again (see the numbers above)");
		}
		return;
	}

	said++;

	/* sceKernelTotalFreeMemSize() is the SYSTEM partition, not our heap: it
	 * says 752 KB whether we hold 1 MB of pictures or 6, so it explained
	 * nothing.  What a picture that does not fit needs to know is how big
	 * the box it wanted was and what the picture arena looks like now. */
	if (shim_arena_ready)
		psp_tracef("tex: no room for a %dx%d picture (%dx%d box = %d KB); "
		           "%d KB of %d KB arena in use (%d chunks), largest free "
		           "%d KB in %d runs, %d pictures, cap %s",
		           width, height, potw, poth, (int)((potw * poth * 2) / 1024),
		           (int)(tex_arena_used(&shim_arena) / 1024),
		           (int)(tex_arena_size(&shim_arena) / 1024),
		           shim_arena.chunks,
		           (int)(tex_arena_largest_free(&shim_arena) / 1024),
		           tex_arena_free_runs(&shim_arena), (int)shim_texture_count,
		           shim_arena_refused ? "reached" : "not reached");
	else
		psp_tracef("tex: no room for a %dx%d picture and no arena (%d KB of "
		           "pictures, %d KB free in the system partition)",
		           width, height, (int)(shim_texture_bytes / 1024),
		           sceKernelTotalFreeMemSize() / 1024);

	if (warned)
		return;

	warned = 1;
	psp_tracef("tex: pictures that could not be uploaded are drawn as the "
	           "engine's null texture; the arena grows in memory, not on the "
	           "card, so the fix is to unload pictures the interface has left");
}

static void shim_upload_bound_texture (void)
{
	shim_texture_t *t = shim_texture_for(shim_texture_bound, 1);
	unsigned int prof_t0 = SHIM_PROF_NOW ();
	int data_changed = 0;

	if (!t || !t->pixels)
	{
		shim_prof_us[SHIM_PROF_TEX] += SHIM_PROF_NOW () - prof_t0;
		return;
	}

	if (t->dirty)
	{
		/* The GE reads main memory through the bus, past the CPU cache:
		 * without the writeback it samples stale bytes. */
		sceKernelDcacheWritebackRange(t->pixels, (unsigned int)t->bytes);
		t->dirty = 0;
		data_changed = 1;

		/* The bytes behind the address the GE is holding just changed,
		 * so whatever it cached about this texture is worthless. */
		shim_tex_state_valid = 0;
	}

	/* Already announced, and nothing about it changed: sending the same
	 * five commands again would only eat the draw list. */
	if (shim_tex_state_valid &&
	    shim_tex_announced_id == shim_texture_bound &&
	    shim_tex_announced_pixels == t->pixels &&
	    shim_tex_announced_psm == t->psm &&
	    shim_tex_announced_palette == (t->psm == GU_PSM_T8 ?
	                                   (const void *)shim_palette_of(t) : NULL) &&
	    shim_tex_announced_filter == t->filter &&
	    shim_tex_announced_potw == t->potw &&
	    shim_tex_announced_poth == t->poth &&
	    shim_tex_announced_tbw == t->tbw)
	{
		shim_prof_us[SHIM_PROF_TEX] += SHIM_PROF_NOW () - prof_t0;
		return;
	}

	/* Unswizzled: the engine's pictures are not power-of-two, and only
	 * swizzled textures have to be. */
	if (t->psm == GU_PSM_T8)
	{
		shim_guClutMode(GU_PSM_8888, 0, 0xFF, 0);
		shim_guClutLoad(32, shim_palette_of(t));   /* 32 blocks of 8 = 256 */
	}

	shim_guTexMode(t->psm, 0, 0, 0);
	shim_guTexImage(0, t->potw, t->poth, t->tbw, t->pixels);
	shim_guTexFilter(t->filter, t->filter);
	shim_guTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);

	/* TEX_FLUSH is what tells the GE's texture cache that the memory behind
	 * the address it was just given has been written by the CPU
	 * (pspsdk src/gu/sceGuTexFlush.c: one TEX_FLUSH command).  Without it
	 * the GE keeps sampling whatever it cached from that address, which is
	 * at least a frame of the previous picture and at worst the zeroes the
	 * block was allocated with.
	 *
	 * It is sent only when the texels really changed, and that is a
	 * correction of its own (round 84).  pspgu.h documents TEX_FLUSH as the
	 * command that invalidates the texture CACHE, and a frame of a map binds
	 * its textures several hundred times: the round-84 log counts 528
	 * batches for 2223 draws.  Sending it on every announcement threw the
	 * GE's texture cache away on every picture switch, so every surface
	 * refetched its texture from main memory.  A bind of a different
	 * picture needs TexMode/TexImage/TexFilter (the address and the format
	 * changed, which the pipeline already knows); it does not need the cache
	 * thrown away. */
	if (data_changed)
	{
		shim_guTexFlush();
		shim_guTexSync();
		shim_prof_texflush++;
	}

	shim_tex_announced_id = shim_texture_bound;
	shim_tex_announced_pixels = t->pixels;
	shim_tex_announced_psm = t->psm;
	shim_tex_announced_filter = t->filter;
	shim_tex_announced_potw = t->potw;
	shim_tex_announced_poth = t->poth;
	shim_tex_announced_tbw = t->tbw;
	shim_tex_announced_palette = (t->psm == GU_PSM_T8 ?
	                              (const void *)shim_palette_of(t) : NULL);
	shim_tex_state_valid = 1;

	shim_prof_us[SHIM_PROF_TEX] += SHIM_PROF_NOW () - prof_t0;
}

/* ------------------------------------------------------------------ */
/* vertices                                                           */
/* ------------------------------------------------------------------ */

/* Project one vertex through the current matrices and map it onto the screen.
 *
 * GL: clip -> ndc, then viewport with the origin at the bottom-left.
 * GU: pixels from the top-left corner of the panel. */
/* 1 while the projection in force is a perspective one.  The bottom row of the
 * matrix is the honest test: an orthogonal projection (glOrtho, and every 2D
 * canvas in the engine) leaves w == 1 for every vertex, which reads as
 * m15 == 1 and m3 == 0. */
static int shim_perspective (void)
{
	return shim_projection.m[15] != 1.0f || shim_projection.m[3] != 0.0f;
}

static void shim_emit_vertex (float x, float y, float z)
{
	shim_matrix_t mvp;
	float cx, cy, cw, sx, sy;

	if (shim_vertex_count >= SHIM_MAX_VERTICES)
		return;

	if (shim_clipping)
	{
		/* The only matrix arithmetic left on the CPU for the world: ONE
		 * row of the mvp, for the near-plane distance.  Everything the
		 * pipeline used to do here -- the full transform, the clip
		 * test on four components, the divide, the viewport map -- is
		 * the GE's job now (shim_emit_world, shim_ge3d_matrices). */
		mvp = shim_modelview;
		shim_multiply(&mvp, &shim_projection, &shim_modelview);

		cw = mvp.m[3] * x + mvp.m[7] * y + mvp.m[11] * z + mvp.m[15];

		/* Perspective: the vertex is parked as the WORLD coordinate the
		 * engine gave us, plus the one number the near-plane clipper
		 * needs -- w, the signed distance in front of the eye.
		 *
		 * For any GL perspective the bottom row of the projection is
		 * (0,0,-1,0) (mathlib.c: Matrix4_ProjectionMatrix writes
		 * out[11] = -1, i.e. -z_eye), so this w IS that distance, and
		 * it is a linear function of the world position -- which is
		 * what lets r_clip_near() cut the polygon in world space. */
		{
			r_clip_vertex_t *cv = &shim_clip_vertices[shim_vertex_count++];

			cv->x = x;
			cv->y = y;
			cv->z = z;
			cv->w = cw;
			cv->u = shim_current_u;
			cv->v = shim_current_v;
			cv->color = shim_current_color;
		}
		return;
	}

	mvp = shim_modelview;
	shim_multiply(&mvp, &shim_projection, &shim_modelview);

	cx = mvp.m[0] * x + mvp.m[4] * y + mvp.m[8]  * z + mvp.m[12];
	cy = mvp.m[1] * x + mvp.m[5] * y + mvp.m[9]  * z + mvp.m[13];

	/* Orthogonal: w is 1, and the depth of a 2D canvas is whatever the
	 * engine put in z (all of it is at the same depth, so the GE compares
	 * equal values). */
	sx = (float)shim_vp_x + (cx * 0.5f + 0.5f) * (float)shim_vp_w;
	sy = (float)PSP_SCREEN_HEIGHT -
	     ((float)shim_vp_y + (cy * 0.5f + 0.5f) * (float)shim_vp_h);

	/* GL's 0..1 becomes the GE's 0..width/0..height -- see
	 * shim_uv_scale_update(). */
	shim_vertices[shim_vertex_count].u = shim_current_u * shim_u_scale;
	shim_vertices[shim_vertex_count].v = shim_current_v * shim_v_scale;
	shim_vertices[shim_vertex_count].color = shim_current_color;
	shim_vertices[shim_vertex_count].x = sx;
	shim_vertices[shim_vertex_count].y = sy;
	shim_vertices[shim_vertex_count].z = z;
	shim_vertex_count++;
}

static int shim_gu_primitive (int gl_mode)
{
	switch (gl_mode)
	{
	case GL_POINTS:         return GU_POINTS;
	case GL_LINES:          return GU_LINES;
	case GL_LINE_STRIP:     return GU_LINE_STRIP;
	case GL_TRIANGLES:      return GU_TRIANGLES;
	case GL_TRIANGLE_STRIP: return GU_TRIANGLE_STRIP;
	case GL_TRIANGLE_FAN:   return GU_TRIANGLE_FAN;
	/* GL_QUADS is handled on its own in glEnd() (one list of six-vertex
	 * triangles per quad).  These used to be thrown in with it and come out
	 * as GU_SPRITES, which takes TWO vertices per primitive and draws the
	 * rectangle between them: a polygon of five vertices then became two
	 * overlapping rectangles drawn from vertices that are not opposite
	 * corners, i.e. the large crossing wedges over the picture.  A quad
	 * strip is a triangle strip, and a polygon is a triangle fan. */
	case GL_QUAD_STRIP:     return GU_TRIANGLE_STRIP;
	case GL_POLYGON:        return GU_TRIANGLE_FAN;
	case GL_LINE_LOOP:      return GU_LINE_STRIP;
	default:                return GU_TRIANGLES;
	}
}

/* Name a GL primitive for the log.  "array" told nobody anything: a wedge of
 * the wrong shape comes from a specific primitive, and which one it is was
 * never in the log. */
static const char *shim_prim_name (int mode)
{
	switch (mode)
	{
	case GL_POINTS:         return "points";
	case GL_LINES:          return "lines";
	case GL_LINE_STRIP:     return "linestrip";
	case GL_LINE_LOOP:      return "lineloop";
	case GL_TRIANGLES:      return "tris";
	case GL_TRIANGLE_STRIP: return "tristrip";
	case GL_TRIANGLE_FAN:   return "trifan";
	case GL_QUADS:          return "quads";
	case GL_QUAD_STRIP:     return "quadstrip";
	case GL_POLYGON:        return "polygon";
	default:                return "?";
	}
}

/* One letter per primitive, for the compact frame dump. */
static char shim_prim_letter (int mode)
{
	switch (mode)
	{
	case GL_POINTS:         return 'O';
	case GL_LINES:          return 'L';
	case GL_LINE_STRIP:     return 'l';
	case GL_LINE_LOOP:      return 'o';
	case GL_TRIANGLES:      return 'T';
	case GL_TRIANGLE_STRIP: return 'S';
	case GL_TRIANGLE_FAN:   return 'F';
	case GL_QUADS:          return 'Q';
	case GL_QUAD_STRIP:     return 's';
	case GL_POLYGON:        return 'P';
	default:                return '?';
	}
}

/* Do the vertices form axis-aligned rectangles, four at a time?
 *
 * Every 2D picture in the engine is an axis-aligned quad.  So a run of quads
 * that is NOT axis-aligned cannot be a picture: it is either a primitive we
 * are mis-mapping (a strip or a polygon arriving through the quad path) or a
 * quad whose corners were mixed up.  That is the shape of the large wedges on
 * the panel, so it is worth an unconditional log line rather than a guess. */
static int shim_quads_axis_aligned (int count)
{
	int i;

	if (count < 4 || (count % 4) != 0)
		return 0;

	for (i = 0; i + 3 < count; i += 4)
	{
		const shim_vertex_t *a = &shim_vertices[i];

		if (fabsf(a[0].y - a[1].y) > 0.75f) return 0;
		if (fabsf(a[2].y - a[3].y) > 0.75f) return 0;
		if (fabsf(a[0].x - a[3].x) > 0.75f) return 0;
		if (fabsf(a[1].x - a[2].x) > 0.75f) return 0;
	}

	return 1;
}

/* Report what the 2D path drew.
 *
 * Two jobs, deliberately separate:
 *
 *   1. A windowed dump of whole draws, for reading the frame in order.  The
 *      window skips short draws, because console text is hundreds of 8x8
 *      glyph draws and used to consume the entire line budget before the
 *      menu pictures were reached.
 *   2. An unconditional anomaly report, capped, for the two shapes that are
 *      certainly bugs wherever they appear: a non-axis-aligned run of quads
 *      and a draw several times the size of the panel.  Both carry the
 *      vertices, because "box 0,0 480,272" says nothing about which corner
 *      went where. */
static void shim_trace_vertices (int count, int maxv)
{
	int i, n = count < maxv ? count : maxv;

	for (i = 0; i < n; i++)
		psp_tracef("vert %d: %d,%d uv %d,%d", i,
		           (int)shim_vertices[i].x, (int)shim_vertices[i].y,
		           (int)shim_vertices[i].u, (int)shim_vertices[i].v);
}

static void shim_trace_draw (int count, const char *what, int quad_mode)
{
	float minx, maxx, miny, maxy, area;
	float minu, maxu, minv, maxv;
	int i, logging, skewed;
	static unsigned int anomaly_lines;
	static unsigned int window_frame = ~0u;

	if (count <= 0)
		return;

	minx = maxx = shim_vertices[0].x;
	miny = maxy = shim_vertices[0].y;
	minu = maxu = shim_vertices[0].u;
	minv = maxv = shim_vertices[0].v;

	for (i = 1; i < count; i++)
	{
		float x = shim_vertices[i].x, y = shim_vertices[i].y;
		float u = shim_vertices[i].u, v = shim_vertices[i].v;

		if (x < minx) minx = x;
		if (x > maxx) maxx = x;
		if (y < miny) miny = y;
		if (y > maxy) maxy = y;
		if (u < minu) minu = u;
		if (u > maxu) maxu = u;
		if (v < minv) minv = v;
		if (v > maxv) maxv = v;
	}

	area = (maxx - minx) * (maxy - miny);
	/* Only a run of quads is *supposed* to be made of rectangles: a
	 * strip or a line list is naturally any shape at all. */
	skewed = quad_mode && (count % 4 == 0) && !shim_quads_axis_aligned(count);

	/* ---- 2. anomalies, whenever they happen ---- */
	if (shim_frames >= 60 && anomaly_lines < 60)
	{
		if (skewed || area > 3.0f * (float)(PSP_SCREEN_WIDTH * PSP_SCREEN_HEIGHT))
		{
			anomaly_lines++;
			psp_tracef("%s: f %d #%d %s tex %d verts %d box %d,%d %d,%d uv %d,%d %d,%d",
			           skewed ? "skew" : "huge",
			           (int)shim_frames, (int)shim_frame_draws, what,
			           (int)shim_texture_bound, count,
			           (int)minx, (int)miny, (int)maxx, (int)maxy,
			           (int)minu, (int)minv, (int)maxu, (int)maxv);
			/* The vertices themselves are the evidence, but eight
			 * lines per anomaly times sixty anomalies is a log
			 * nobody reads: the first few carry the detail. */
			if (anomaly_lines <= 8)
				shim_trace_vertices(count, skewed ? 8 : 4);
		}
	}

	/* ---- 1. windowed dump ---- */
	if (window_frame != shim_frames)
	{
		window_frame = shim_frames;
		shim_trace_menu = 0;
		shim_trace_text = 0;
	}

	/* One frame in full, once: every draw in the order the engine made
	 * them, one short line each, nothing filtered.  It is the only way to
	 * answer "where did this wedge come from" without guessing, because a
	 * filtered window always leaves something out.
	 *
	 * One frame, not two.  The second one was there in case the first had a
	 * draw the second did not -- and it doubled four hundred lines to eight
	 * hundred to say that the same menu draws the same thing twice.  Half of
	 * the log of a menu session was this dump. */
	if (shim_frames == 120)
	{
		if (shim_trace_dump < 400)
		{
			shim_trace_dump++;
			psp_tracef("d %d %d %c t%d %dx%d %d,%d-%d,%d u%d,%d-%d,%d%s",
			           (int)shim_frames, (int)shim_frame_draws,
			           quad_mode ? 'Q' : shim_prim_letter(shim_primitive),
			           (int)shim_texture_bound,
			           (int)shim_tex_announced_potw, (int)shim_tex_announced_poth,
			           (int)minx, (int)miny, (int)maxx, (int)maxy,
			           (int)minu, (int)minv, (int)maxu, (int)maxv,
			           ((maxy > (float)PSP_SCREEN_HEIGHT + 1.0f ||
			             miny < -1.0f) ? " OFF" : ""));
		}
		return;
	}

	if (shim_frames < shim_trace_frames)
	{
		logging = shim_trace_early++ < 60;
		if (logging)
			shim_trace_menu = 0;   /* early window has its own counter */
	}
	else
	{
		/* The settled menu, plus one snapshot every two seconds: the boot
		 * screen, the menu and the in-game HUD are three different frames
		 * and the log has to be able to show more than the first of them. */
		logging = ((shim_frames == 118 || shim_frames == 119) ||
	           (shim_frames % 60) == 0) && shim_trace_menu < 30;
	}

	/* Pictures get the window's lines; a strip of glyphs does not, because
	 * console text is hundreds of 8 px draws and used to eat the whole
	 * budget before the menu was reached.  The text still gets two lines of
	 * its own kind, though: menu labels are reported as a band, and a band
	 * that lands on top of the previous one is marked, which is the only way
	 * the log can show "these two menu items are drawn over each other". */
	if (logging)
	{
		if ((maxy - miny) > 16.0f)
		{
			shim_trace_menu++;
			psp_tracef("draw: f %d #%d %s tex %d tsz %dx%d verts %d "
			           "box %d,%d %d,%d uv %d,%d %d,%d proj m15 %.3f m3 %.3f",
			           (int)shim_frames, (int)shim_frame_draws, what,
			           (int)shim_texture_bound,
			           (int)shim_tex_announced_potw, (int)shim_tex_announced_poth,
			           count,
			           (int)minx, (int)miny, (int)maxx, (int)maxy,
			           (int)minu, (int)minv, (int)maxu, (int)maxv,
			           (double)shim_projection.m[15], (double)shim_projection.m[3]);
		}
		else if (shim_trace_text < 12 && count >= 8)
		{
			/* One character is its own glBegin()/glEnd() pair, so the
			 * console's output is a stream of four-vertex draws and the
			 * menu's labels are not: a string is one draw of many quads.
			 * Requiring two glyphs keeps the console out of this list --
			 * which is what the whole window is for.  A band "overlaps the
			 * previous one" only if it does so in BOTH directions; the
			 * first version compared the y range alone, so every pair of
			 * consecutive characters on one line was reported as an
			 * overlap and the log said nothing useful at all. */
			static float last[4] = { -1000.0f, -1000.0f, -1000.0f, -1000.0f };
			int overlaps;

			shim_trace_text++;

			overlaps = (miny < last[3] && maxy > last[1] &&
			            minx < last[2] && maxx > last[0]);

			psp_tracef("text: f %d #%d box %d,%d %d,%d %s",
			           (int)shim_frames, (int)shim_frame_draws,
			           (int)minx, (int)miny, (int)maxx, (int)maxy,
			           overlaps ? "OVERLAPS PREVIOUS" : "");

			last[0] = minx; last[1] = miny; last[2] = maxx; last[3] = maxy;
		}
	}

	/* ---- the old quarterly-panel count, kept as a frame statistic ---- */
	if (area >= (float)(PSP_SCREEN_WIDTH * PSP_SCREEN_HEIGHT) / 4.0f)
		shim_big_draws++;
}

/* Report one texture that is drawn over a quarter of the panel, once per
 * texture.  Called from glEnd() so it covers every primitive, not just quads. */
static void shim_trace_big_once (int count, const char *what)
{
	static unsigned short named[16];
	static unsigned short over[16];
	static int named_n, over_n;
	float w = 0.0f, h = 0.0f;
	float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
	int i;

	if (count <= 0)
		return;

	{
		float minx, maxx, miny, maxy;

		minx = maxx = shim_vertices[0].x;
		miny = maxy = shim_vertices[0].y;

		for (i = 1; i < count; i++)
		{
			float x = shim_vertices[i].x, y = shim_vertices[i].y;

			if (x < minx) minx = x;
			if (x > maxx) maxx = x;
			if (y < miny) miny = y;
			if (y > maxy) maxy = y;
		}

		x0 = minx; y0 = miny; x1 = maxx; y1 = maxy;
		w = maxx - minx;
		h = maxy - miny;
	}

	/* A draw that leaves the panel is a bug in the caller's layout, and it
	 * used to be invisible: the panel only shows the part that fits, so the
	 * rest of the picture simply was not there and looked like a missing
	 * picture rather than a misplaced one.  The menu plaque (a 32x256
	 * picture asked to be 348 pixels tall on a 272 pixel panel) is the case
	 * that made this worth printing. */
	if ((y0 < -1.0f || x0 < -1.0f || y1 > (float)PSP_SCREEN_HEIGHT + 1.0f ||
	     x1 > (float)PSP_SCREEN_WIDTH + 1.0f) &&
	    over_n < (int)(sizeof(over) / sizeof(over[0])))
	{
		for (i = 0; i < over_n; i++)
			if (over[i] == (unsigned short)shim_texture_bound)
				return;
		over[over_n++] = (unsigned short)shim_texture_bound;
		psp_tracef("over: f %d %s tex %d verts %d box %d,%d %d,%d (%d)",
		           (int)shim_frames, what, (int)shim_texture_bound, count,
		           (int)x0, (int)y0, (int)x1, (int)y1, over_n);
		return;
	}

	if (w * h < (float)(PSP_SCREEN_WIDTH * PSP_SCREEN_HEIGHT) / 4.0f)
		return;

	if (named_n >= (int)(sizeof(named) / sizeof(named[0])))
		return;

	for (i = 0; i < named_n; i++)
		if (named[i] == (unsigned short)shim_texture_bound)
			return;

	named[named_n++] = (unsigned short)shim_texture_bound;

	psp_tracef("big: f %d %s tex %d verts %d %.0fx%.0f (%d of 16)",
	           (int)shim_frames, what, (int)shim_texture_bound, count,
	           (double)w, (double)h, named_n);
}

/* Turn the GL_QUADS accumulated since glBegin() into one triangle list.
 *
 * Every quad the engine draws arrives in the order
 *
 *      v0 = top-left    v1 = top-right
 *      v3 = bottom-left v2 = bottom-right        (clockwise, y downwards)
 *
 * so the two triangles are (v0, v1, v2) and (v0, v2, v3).  Triangles map the
 * texture exactly, which GU_SPRITES (an axis-aligned shortcut that stretches
 * one texel pair over the whole rectangle) does not for a quad that is not
 * square to the screen.
 *
 * The expanded list goes to the GE in as few command as the ring allows --
 * a line of text is one draw, not two hundred -- because every quad on its
 * own would fill the 32 KB draw list in a frame or two.
 *
 * (The alternative, GU_SPRITES in vertex pairs, stretches the texture across
 * the rectangle instead of interpolating it and needs two commands per quad;
 * it is only worth it for axis-aligned, unfiltered pictures.) */
static void shim_flush_quads (void)
{
	int quads = shim_vertex_count / 4;
	int done = 0;

	shim_frame_draws++;
	shim_trace_draw(shim_vertex_count, shim_prim_name(GL_QUADS), 1);
	shim_trace_big_once(shim_vertex_count, shim_prim_name(GL_QUADS));

	/* As many quads as the ring still has room for, in one command, then the
	 * next slice: a line of text usually fits in a single draw. */
	while (done < quads)
	{
		shim_vertex_t *v;
		int batch = quads - done;
		int k;

		if ((unsigned int)(batch * 6) > SHIM_GE_VERTICES - shim_ge_used)
			batch = (int)((SHIM_GE_VERTICES - shim_ge_used) / 6);

		if (batch <= 0)
		{
			shim_ge_alloc(0);   /* logs the overflow once */
			return;
		}

		v = shim_ge_alloc((unsigned int)(batch * 6));
		if (!v)
			return;

		for (k = 0; k < batch; k++)
		{
			int o = (done + k) * 4;

			v[k * 6 + 0] = shim_vertices[o + 0];
			v[k * 6 + 1] = shim_vertices[o + 1];
			v[k * 6 + 2] = shim_vertices[o + 2];

			v[k * 6 + 3] = shim_vertices[o + 0];
			v[k * 6 + 4] = shim_vertices[o + 2];
			v[k * 6 + 5] = shim_vertices[o + 3];
		}

		shim_batch_add(GU_TRIANGLES, shim_texture_bound, v, batch * 6, 0);

		done += batch;
	}
}

/* ------------------------------------------------------------------ */
/* the 3D path                                                        */
/* ------------------------------------------------------------------ */

/* How close to the eye a vertex may get.  Not zero: w lands in the denominator
 * of the divide, and a vertex at w == 0.0000001 is a screen coordinate in the
 * millions.  A thousandth of a unit in front of the eye is far inside anything
 * a near plane of half a unit ever shows. */
#define SHIM_NEAR_W 0.001f

static unsigned int shim_3d_triangles;   /* since the shim started */
static unsigned int shim_3d_clipped;
static int          shim_3d_logged;

/* Hand one clipped vertex to the GE as a WORLD coordinate.
 *
 * Nothing is transformed here any more.  The vertex keeps the position the
 * engine gave glVertex3f, and the GE's own transform stage multiplies it by the
 * projection and view matrices that shim_ge3d_matrices() loaded -- the divide,
 * the viewport mapping and the depth mapping all happen in the hardware, on
 * the geometry processor, for free.
 *
 * What the CPU still owes the pipeline is the ONE thing the GE does not do: the
 * near plane.  A vertex at or behind the eye has w <= 0, and the GE would put
 * it at a coordinate outside its 4096x4096 space and drop the whole primitive
 * (that is the hardware cull Interstice's clipping.cpp describes at length), so
 * a wall the player walks up to would vanish.  The polygon is therefore cut
 * against the near plane first, in world space -- which is exact, because w
 * (the distance in front of the eye) is a linear function of the world position,
 * so interpolating both with the same parameter gives a point that is on the
 * edge AND on the plane.  Clip, then let the GE transform and rasterise. */
static void shim_emit_world (shim_vertex_t *out, const r_clip_vertex_t *cv)
{
	/* The 3D scale, not the 2D one: the GE's transform pipe takes texture
	 * coordinates as a fraction of the picture (pspsdk sceGuTexScale is the
	 * multiplier that would convert texels into that unit, and it says it
	 * only applies to the 3D pipe).  See shim_uv_scale_update(). */
	out->u = cv->u * shim_u_scale3d;
	out->v = cv->v * shim_v_scale3d;
	out->color = cv->color;
	out->x = cv->x;
	out->y = cv->y;
	out->z = cv->z;
}

/* The five planes every world triangle is tested against, in WORLD space,
 * derived once per matrix change from the projection and the view (r_clip.h
 * spells out the extraction).  Four sides of the view and the near plane.
 *
 * WHY THE SIDES ARE CLIPPED AT ALL, on hardware that clips: the GE has no
 * per-pixel clip of its own beyond the scissor.  What it does have is a
 * hardware CULL: if any vertex of a primitive lands outside the 4096x4096
 * coordinate space (Interstice's source/psp/clipping.cpp calls it the VCS),
 * the whole primitive is thrown away.  Our viewport maps ndc +-1 onto
 * 2048 +- 240 pixels, so a vertex is safe while |ndc_x| < 8.5 and
 * |ndc_y| < 15 -- i.e. a vertex may be several screens off-axis before the
 * primitive dies.  A wall the player is standing against, or the floor under
 * their feet, has vertices dozens of screens off-axis, and would disappear
 * whole.  Cutting the polygon to the view rectangle keeps every vertex at
 * |ndc| <= 1 (and the near plane keeps w > 0, without which the divide puts
 * the vertex on the wrong side of the screen).
 *
 * The cost is only paid by polygons that actually cross the boundary: the BSP
 * walk has already rejected everything outside the view, so most arrive with
 * all three vertices inside and one pass over 5 planes says so
 * (r_clip_inside_all) without touching the geometry. */
#define SHIM_CLIP_PLANES 5

static r_clip_plane_t shim_clip_planes[SHIM_CLIP_PLANES];

/* Rebuild the planes from the matrices now in force.  Called from glBegin()
 * for every perspective primitive -- GL forbids a matrix change inside a
 * glBegin/glEnd pair, so once per primitive is once per matrix. */
static void shim_clip_planes_update (void)
{
	shim_matrix_t mvp;
	float *m;

	mvp = shim_modelview;
	shim_multiply(&mvp, &shim_projection, &shim_modelview);
	m = mvp.m;

	/* left: w + x >= 0, right: w - x >= 0, bottom: w + y, top: w - y.
	 * Column-major, so the row whose elements are used for the x coordinate
	 * of the result is (m[0], m[4], m[8], m[12]) -- that is r0 = 0; the y row
	 * is r0 = 1; the w row is r3 = 3. */
#define SHIM_PLANE(p, r0, s)                                        \
	do {                                                            \
		shim_clip_planes[p].a = m[3] + (s) * m[r0];                  \
		shim_clip_planes[p].b = m[7] + (s) * m[r0 + 4];              \
		shim_clip_planes[p].c = m[11] + (s) * m[r0 + 8];             \
		shim_clip_planes[p].d = m[15] + (s) * m[r0 + 12];            \
	} while (0)

	SHIM_PLANE(0, 0, +1.0f);      /* left   */
	SHIM_PLANE(1, 0, -1.0f);      /* right  */
	SHIM_PLANE(2, 1, +1.0f);      /* bottom */
	SHIM_PLANE(3, 1, -1.0f);      /* top    */
#undef SHIM_PLANE

	/* near: w >= SHIM_NEAR_W, i.e. row3 - SHIM_NEAR_W */
	shim_clip_planes[4].a = m[3];
	shim_clip_planes[4].b = m[7];
	shim_clip_planes[4].c = m[11];
	shim_clip_planes[4].d = m[15] - SHIM_NEAR_W;
}

/* One triangle, clipped to the view and handed to the GE.
 *
 * The result of clipping a triangle is a triangle or a convex polygon, drawn
 * as a fan around its first vertex -- which is why the loop below starts at 2,
 * not 3. */
static void shim_emit_3d_triangle (const r_clip_vertex_t *a,
                                   const r_clip_vertex_t *b,
                                   const r_clip_vertex_t *c)
{
	r_clip_vertex_t  buf[2][R_CLIP_MAX];
	r_clip_vertex_t  *cur, *nxt;
	int              m, i, p;

	shim_3d_triangles++;

	if (a->w < SHIM_NEAR_W && b->w < SHIM_NEAR_W && c->w < SHIM_NEAR_W)
		return;                      /* wholly behind: nothing to draw */

	buf[0][0] = *a;
	buf[0][1] = *b;
	buf[0][2] = *c;
	m = 3;
	cur = buf[0];
	nxt = buf[1];

	if (!r_clip_inside_all (cur, m, shim_clip_planes, SHIM_CLIP_PLANES))
	{
		/* One pass per plane, ping-ponging between the two buffers: a
		 * polygon clipped by five planes can gain five vertices, which
		 * R_CLIP_MAX holds. */
		shim_3d_clipped++;

		for (p = 0; p < SHIM_CLIP_PLANES; p++)
		{
			int k = r_clip_halfspace (cur, m, nxt, R_CLIP_MAX,
			                          &shim_clip_planes[p]);

			if (k < 3)
				return;              /* the whole polygon is outside */

			m = k;

			{
				r_clip_vertex_t *t = cur;
				cur = nxt;
				nxt = t;
			}
		}
	}

	for (i = 2; i < m; i++)
	{
		shim_vertex_t *v = shim_ge_alloc(3);

		if (!v)
			return;

		shim_emit_world(&v[0], &cur[0]);
		shim_emit_world(&v[1], &cur[i - 1]);
		shim_emit_world(&v[2], &cur[i]);

		/* Same primitive and texture as the previous triangle, so the
		 * batch merges them into one sceGuDrawArray: a surface of
		 * thirty polygons is still one command.  The 1 marks these
		 * vertices as world coordinates, i.e. drawn with the GE's
		 * transform stage. */
		shim_batch_add(GU_TRIANGLES, shim_texture_bound, v, 3, 1);
	}
}

/* Turn the primitive accumulated since glBegin() into clipped triangles.
 *
 * Everything becomes a triangle list here, including GL_QUADS and GL_POLYGON,
 * because the clipper works on a polygon and a triangle is the only shape a
 * clipped polygon can always be broken into without knowing more about it.  The
 * engine's 3D path sends GL_POLYGON for a BSP surface and GL_TRIANGLES for
 * models; both arrive here. */
static void shim_flush_3d (void)
{
	int n = shim_vertex_count;
	int i;

	shim_frame_draws++;
	shim_world_prims++;			/* see shim_world_prims: a world frame */
	shim_trace_draw(n, shim_prim_name(shim_primitive), 0);

	if (!shim_3d_logged)
	{
		shim_3d_logged = 1;
		/* The one line that says the world reached the hardware: a
		 * perspective primitive, in world coordinates, drawn with the
		 * GE's transform (sceGuSetMatrix) rather than ours.  Everything
		 * up to this point in the port's history drew 2D only, and the
		 * trace of a frame consisted of rectangles covering the panel. */
		/* The viewport matters as much as the matrices: the GE maps the
		 * projected vertex into the 4096-wide space gu_psp.c set up, which
		 * is the WHOLE panel, so a 3D viewport smaller than 480x272 would
		 * be stretched over it.  The engine computes that rectangle in
		 * R_SetupGL (glViewport), and if it is ever not the full panel the
		 * next round needs to know the numbers rather than the symptom. */
		psp_tracef("3d: first world primitive on the GE: %s of %d vertices, "
		           "near plane w %.4f, proj m11 %.3f m14 %.3f, viewport "
		           "%d,%d %dx%d (panel %dx%d)",
		           shim_prim_name(shim_primitive), n, (double)SHIM_NEAR_W,
		           (double)shim_projection.m[11],
		           (double)shim_projection.m[14],
		           shim_vp_x, shim_vp_y, shim_vp_w, shim_vp_h,
		           PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	}

	switch (shim_primitive)
	{
	case GL_QUADS:
		/* v0 v1
		 * v3 v2   -- the order the engine always uses (see
		 * shim_flush_quads, which relies on the same one). */
		for (i = 0; i + 3 < n; i += 4)
		{
			shim_emit_3d_triangle(&shim_clip_vertices[i + 0],
			                      &shim_clip_vertices[i + 1],
			                      &shim_clip_vertices[i + 2]);
			shim_emit_3d_triangle(&shim_clip_vertices[i + 0],
			                      &shim_clip_vertices[i + 2],
			                      &shim_clip_vertices[i + 3]);
		}
		break;

	case GL_TRIANGLES:
		for (i = 0; i + 2 < n; i += 3)
			shim_emit_3d_triangle(&shim_clip_vertices[i + 0],
			                      &shim_clip_vertices[i + 1],
			                      &shim_clip_vertices[i + 2]);
		break;

	case GL_TRIANGLE_STRIP:
		for (i = 0; i + 2 < n; i++)
		{
			/* A strip alternates its winding; the clipper does not
			 * care about winding, but a reader of this code does. */
			if (i & 1)
				shim_emit_3d_triangle(&shim_clip_vertices[i + 1],
				                      &shim_clip_vertices[i + 0],
				                      &shim_clip_vertices[i + 2]);
			else
				shim_emit_3d_triangle(&shim_clip_vertices[i + 0],
				                      &shim_clip_vertices[i + 1],
				                      &shim_clip_vertices[i + 2]);
		}
		break;

	case GL_TRIANGLE_FAN:
	case GL_POLYGON:      /* a convex polygon is a fan around its first vertex */
		for (i = 1; i + 1 < n; i++)
			shim_emit_3d_triangle(&shim_clip_vertices[0],
			                      &shim_clip_vertices[i],
			                      &shim_clip_vertices[i + 1]);
		break;

	default:
		/* Points and lines: the engine draws none of them in the world,
		 * and clipping a line is a different routine.  Named once in the
		 * log rather than silently dropped. */
		if (shim_3d_logged == 1)
		{
			shim_3d_logged = 2;
			psp_tracef("3d: primitive %s is not drawn in 3D yet",
			           shim_prim_name(shim_primitive));
		}
		break;
	}
}

/* ------------------------------------------------------------------ */
/* gl* entry points                                                   */
/* ------------------------------------------------------------------ */

void glBegin (GLenum mode)
{
	shim_primitive = (int)mode;
	shim_vertex_count = 0;
	/* Decided once per primitive, so every vertex of it takes the same
	 * path: half a triangle mapped in 2D and half clipped is nothing anyone
	 * wants. */
	shim_clipping = shim_perspective();

	/* The clipping planes are derived from the matrices, and a matrix can
	 * change between two primitives (R_SetupGL loads both of them once a
	 * frame, R_DrawViewModel multiplies the model-view per entity).  GL does
	 * not allow a matrix change INSIDE a glBegin/glEnd pair, so rebuilding
	 * them here -- once per primitive, not once per vertex -- is both
	 * correct and cheap. */
	if (shim_clipping)
		shim_clip_planes_update();
}

void glEnd (void)
{
	shim_texture_t *t;

	if (shim_vertex_count == 0)
		return;

	t = shim_texture_for(shim_texture_bound, 1);

	/* A texture whose upload ran out of memory has no pixels: drawing it
	 * untextured is ugly, but it is visible and it does not sample whatever
	 * the GE happened to have selected before. */
	if (shim_texture_2d && t && t->pixels)
	{
		shim_guTexEnable(1);
		shim_upload_bound_texture();
	}
	else
	{
		shim_guTexEnable(0);
	}

	if (shim_clipping)
	{
		/* The engine's own polygon work for the world, from the shim's
		 * side: clipping, emitting and the batch append.  See the note on
		 * SHIM_PROF_* -- a frame of this map is 176 ms of somebody's time
		 * and this bucket is one of the candidates. */
		unsigned int prof_t0 = SHIM_PROF_NOW ();

		shim_flush_3d();
		shim_prof_us[SHIM_PROF_FLUSH] += SHIM_PROF_NOW () - prof_t0;
		shim_vertex_count = 0;
		return;
	}

	if (shim_primitive == GL_QUADS)
	{
		shim_flush_quads();
		shim_vertex_count = 0;
		return;
	}

	/* GL_LINE_LOOP draws the closing segment, GU_LINE_STRIP does not: the
	 * loop is turned into a strip by repeating the first vertex, which is
	 * what GL's own definition of the loop says. */
	if (shim_primitive == GL_LINE_LOOP && shim_vertex_count >= 2 &&
	    shim_vertex_count < SHIM_MAX_VERTICES)
	{
		shim_vertices[shim_vertex_count] = shim_vertices[0];
		shim_vertex_count++;
	}

	{
		shim_vertex_t *v = shim_ge_alloc((unsigned int)shim_vertex_count);

		shim_frame_draws++;
		shim_trace_draw(shim_vertex_count, shim_prim_name(shim_primitive), 0);
		shim_trace_big_once(shim_vertex_count, shim_prim_name(shim_primitive));

		if (v)
		{
			memcpy(v, shim_vertices, sizeof(shim_vertex_t) * (size_t)shim_vertex_count);
			shim_batch_add(shim_gu_primitive(shim_primitive), shim_texture_bound, v,
			               shim_vertex_count, 0);
		}

		shim_vertex_count = 0;
	}
}

void glVertex2f (GLfloat x, GLfloat y)
{
	shim_emit_vertex(x, y, 0.0f);
}

void glVertex3f (GLfloat x, GLfloat y, GLfloat z)
{
	shim_emit_vertex(x, y, z);
}

void glVertex3fv (const GLfloat *v)
{
	shim_emit_vertex(v[0], v[1], v[2]);
}

void glTexCoord2f (GLfloat s, GLfloat t)
{
	shim_current_u = s;
	shim_current_v = t;
}

/* The GU stores a colour as 0xAABBGGRR -- red in the LOW byte (pspgu.h says
 * so for the pixel mask: "the representation of the mask is in the format
 * 0xAABBGGRR"), which is also the byte order a GL_RGBA array has on a
 * little-endian machine.  These three used to put red in bits 16..23 and blue
 * in bits 0..7, i.e. they swapped red and blue for every vertex of every
 * picture: a colour right in red came out blue.
 *
 * The order is not a matter of taste: a GL_RGBA array holds the bytes R,G,B,A
 * in memory, so on a little-endian machine the 32-bit word is A<<24 | B<<16 |
 * G<<8 | R, which is exactly 0xAABBGGRR.  glColor4ubv() -- the one the engine
 * uses for almost all 2D drawing, Draw_CharacterQuad() included -- unpacked
 * the bytes the other way round (v[0] into bits 16..23), so the two families
 * disagreed and every colour went through the pipeline depending on which
 * function the caller happened to use.  Both now store 0xAABBGGRR. */
void glColor3f (GLfloat r, GLfloat g, GLfloat b)
{
	shim_current_color = 0xFF000000u |
	                     ((unsigned int)(b * 255.0f + 0.5f) << 16) |
	                     ((unsigned int)(g * 255.0f + 0.5f) << 8) |
	                     ((unsigned int)(r * 255.0f + 0.5f));
}

void glColor4f (GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
	shim_current_color = ((unsigned int)(a * 255.0f + 0.5f) << 24) |
	                     ((unsigned int)(b * 255.0f + 0.5f) << 16) |
	                     ((unsigned int)(g * 255.0f + 0.5f) << 8) |
	                     ((unsigned int)(r * 255.0f + 0.5f));
}

void glColor3fv (const GLfloat *v)
{
	glColor3f(v[0], v[1], v[2]);
}

void glColor4fv (const GLfloat *v)
{
	glColor4f(v[0], v[1], v[2], v[3]);
}

void glColor4ubv (const GLubyte *v)
{
	/* v = { R, G, B, A } in memory -> 0xAABBGGRR */
	shim_current_color = ((unsigned int)v[3] << 24) | ((unsigned int)v[2] << 16) |
	                     ((unsigned int)v[1] << 8) | (unsigned int)v[0];
}

/* ---- matrices ---- */

void glMatrixMode (GLenum mode)
{
	shim_matrix_mode = mode;
}

void glLoadIdentity (void)
{
	/* A queued 3D batch belongs to the matrices that were in force when its
	 * vertices were produced, and the GE reads them from its own registers
	 * at draw time -- so changing a matrix has to close the batch.  Cheap:
	 * shim_batch_flush() returns immediately when there is nothing queued,
	 * and the engine changes matrices between draws, not inside them. */
	shim_batch_flush();
	shim_identity(shim_current_matrix());
}

void glLoadMatrixf (const GLfloat *m)
{
	shim_batch_flush();
	memcpy(shim_current_matrix()->m, m, sizeof(float) * 16);
}

void glMultMatrixf (const GLfloat *m)
{
	shim_matrix_t t, *cur = shim_current_matrix();

	shim_batch_flush();
	memcpy(t.m, m, sizeof(t.m));
	shim_multiply(cur, cur, &t);
}

void glPushMatrix (void)
{
	shim_batch_flush();
	if (shim_matrix_mode == GL_PROJECTION)
	{
		if (shim_projection_depth < SHIM_MAX_MATRIX_STACK)
			shim_projection_stack[shim_projection_depth++] = shim_projection;
	}
	else if (shim_modelview_depth < SHIM_MAX_MATRIX_STACK)
		shim_modelview_stack[shim_modelview_depth++] = shim_modelview;
}

void glPopMatrix (void)
{
	shim_batch_flush();
	if (shim_matrix_mode == GL_PROJECTION)
	{
		if (shim_projection_depth > 0)
			shim_projection = shim_projection_stack[--shim_projection_depth];
	}
	else if (shim_modelview_depth > 0)
		shim_modelview = shim_modelview_stack[--shim_modelview_depth];
}

void glOrtho (GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
              GLdouble zNear, GLdouble zFar)
{
	shim_matrix_t ortho, *cur = shim_current_matrix();

	shim_batch_flush();

	memset(ortho.m, 0, sizeof(ortho.m));
	ortho.m[0]  = (float)(2.0 / (right - left));
	ortho.m[5]  = (float)(2.0 / (top - bottom));
	ortho.m[10] = (float)(-2.0 / (zFar - zNear));
	ortho.m[12] = (float)(-(right + left) / (right - left));
	ortho.m[13] = (float)(-(top + bottom) / (top - bottom));
	ortho.m[14] = (float)(-(zFar + zNear) / (zFar - zNear));
	ortho.m[15] = 1.0f;

	shim_multiply(cur, cur, &ortho);
}

void glTranslatef (GLfloat x, GLfloat y, GLfloat z)
{
	shim_batch_flush();
	shim_matrix_t t, *cur = shim_current_matrix();

	shim_identity(&t);
	t.m[12] = x;
	t.m[13] = y;
	t.m[14] = z;
	shim_multiply(cur, cur, &t);
}

void glScalef (GLfloat x, GLfloat y, GLfloat z)
{
	shim_batch_flush();
	shim_matrix_t t, *cur = shim_current_matrix();

	shim_identity(&t);
	t.m[0] = x;
	t.m[5] = y;
	t.m[10] = z;
	shim_multiply(cur, cur, &t);
}

void glRotatef (GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
	shim_batch_flush();
	shim_matrix_t t, *cur = shim_current_matrix();
	float r = angle * (float)M_PI / 180.0f;
	float c = (float)cos(r), s = (float)sin(r);
	float len;

	len = (float)sqrt(x * x + y * y + z * z);
	if (len < 1e-6f)
		return;
	x /= len; y /= len; z /= len;

	shim_identity(&t);
	t.m[0] = c + x * x * (1 - c);
	t.m[1] = y * x * (1 - c) + z * s;
	t.m[2] = x * z * (1 - c) - y * s;
	t.m[4] = x * y * (1 - c) - z * s;
	t.m[5] = c + y * y * (1 - c);
	t.m[6] = y * z * (1 - c) + x * s;
	t.m[8] = x * z * (1 - c) + y * s;
	t.m[9] = y * z * (1 - c) - x * s;
	t.m[10] = c + z * z * (1 - c);
	shim_multiply(cur, cur, &t);
}

/* ---- viewport and scissor ---- */

void glViewport (GLint x, GLint y, GLsizei width, GLsizei height)
{
	shim_vp_x = x;
	shim_vp_y = y;
	shim_vp_w = width > 0 ? width : 1;
	shim_vp_h = height > 0 ? height : 1;

	/* The GE gets the same rectangle, and it needs it for real.
	 *
	 * The 2D path maps vertices to pixels itself (shim_emit_vertex), so for a
	 * menu this call was only a note.  The world is different: its vertices
	 * are handed to the GE in world space and the GE's transform stage does
	 * the divide and the viewport mapping with the numbers sceGuViewport()
	 * last sent -- which gu_psp.c sets once, to the whole panel.  The engine
	 * does NOT ask for the whole panel: R_SetupGL calls glViewport with
	 * r_refdef.vrect, and with scr_viewsize 100 and a status bar that is
	 * 480x248 at the top of the screen (gl_screen.c, R_SetVrect: sb_lines =
	 * 24 * scale).  Drawing 480x248 of world into a 480x272 viewport would
	 * stretch it ten percent vertically and push it down -- a small error
	 * that looks exactly like a broken field of view, and one that no amount
	 * of matrix arithmetic would explain.
	 *
	 * The mapping from GL's rectangle (origin bottom-left) to the GE's 4096
	 * coordinate space (origin top-left, centred by sceGuOffset):
	 *
	 *     offset = (2048 - W/2, 2048 - H/2)        set once, gu_psp.c
	 *     centre = offset + (x + w/2, H - y - h/2)
	 *
	 * which for a full-panel rectangle gives exactly (2048, 2048) -- the
	 * value gu_psp.c already uses -- so nothing changes for the 2D case. */
	if (shim_frame_active)
	{
		int vx = x, vy = y, vw = shim_vp_w, vh = shim_vp_h;
		int top, cx, cy;

		/* Clamped to the panel, like the scissor (shim_scissor_flush).
		 *
		 * The engine does not only call this for the world: a 2D canvas
		 * asks for its own rectangle, and the round-72 log has
		 * `viewport 22,0 870x272` in it -- a viewport 870 pixels wide on a
		 * 480-pixel panel, because the console's width comes from the
		 * font, not from the screen.  For a 2D draw the GE ignores the
		 * viewport altogether (GU_TRANSFORM_2D writes integer pixels and
		 * is bounded by the scissor), which is why it was never visible.
		 * But the viewport is GE state, and whatever the last 2D draw left
		 * there is what the next world draw would be mapped into: the GE
		 * turns centre +- (w/2, h/2) into its 4096-wide coordinate space,
		 * and a rectangle that hangs off the panel puts part of that space
		 * outside the frame buffer.  So the same clamp the scissor gets. */
		if (vw > PSP_SCREEN_WIDTH)  vw = PSP_SCREEN_WIDTH;
		if (vh > PSP_SCREEN_HEIGHT) vh = PSP_SCREEN_HEIGHT;
		if (vx < 0) { vw += vx; vx = 0; }
		if (vy < 0) { vh += vy; vy = 0; }
		if (vx + vw > PSP_SCREEN_WIDTH)  vw = PSP_SCREEN_WIDTH - vx;
		if (vy + vh > PSP_SCREEN_HEIGHT) vh = PSP_SCREEN_HEIGHT - vy;
		if (vw < 1) vw = 1;
		if (vh < 1) vh = 1;

		top = PSP_SCREEN_HEIGHT - vy - vh;
		cx = (2048 - PSP_SCREEN_WIDTH / 2) + vx + vw / 2;
		cy = (2048 - PSP_SCREEN_HEIGHT / 2) + top + vh / 2;

		shim_guViewport(cx, cy, vw, vh);
	}
}

/* Send the current rectangle to the GE, clamped to the panel.
 *
 * The scissor is not a nicety on this hardware: in through mode
 * (GU_TRANSFORM_2D) the GE does no transform or clipping at all -- the
 * scissor is the only render bound (ppsspp's GE overview says as much:
 * "through mode bypasses transform/clipping, positions are integer screen
 * pixels").  Our drawing buffer is 480x272 of a 512-pixel stride at EDRAM
 * 0x088000 and the buffer that is on screen starts exactly 0x88000 bytes
 * later, so a pixel outside the panel is not dropped: it is written into the
 * *displayed* buffer, or past both of them into the depth buffer.  That is
 * the one mechanism that can put a shape on the panel that no draw in the
 * tracer claims, and it stays there because it is in the buffer the GE is
 * showing while the next frame is drawn into the other one.
 *
 * So the GE scissor is always enabled and always inside the panel.  GL's
 * glDisable(GL_SCISSOR_TEST) means "no extra clipping" here, not "no
 * clipping": it becomes the full panel rectangle. */
static void shim_scissor_flush (void)
{
	int x = shim_scr_x, y = shim_scr_y;
	int w = shim_scr_w, h = shim_scr_h;

	if (w > PSP_SCREEN_WIDTH)  w = PSP_SCREEN_WIDTH;
	if (h > PSP_SCREEN_HEIGHT) h = PSP_SCREEN_HEIGHT;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > PSP_SCREEN_WIDTH)  w = PSP_SCREEN_WIDTH - x;
	if (y + h > PSP_SCREEN_HEIGHT) h = PSP_SCREEN_HEIGHT - y;
	if (w < 0) w = 0;
	if (h < 0) h = 0;

	shim_guEnable(GU_SCISSOR_TEST);
	shim_guScissor(x, y, w, h);
}

void glScissor (GLint x, GLint y, GLsizei width, GLsizei height)
{
	/* GL: origin bottom-left.  GU: origin top-left. */
	shim_scr_x = x;
	shim_scr_y = PSP_SCREEN_HEIGHT - (y + height);
	shim_scr_w = width > 0 ? width : 0;
	shim_scr_h = height > 0 ? height : 0;

	shim_scissor_flush();
}

/* ---- pipeline switches ---- */

void glEnable (GLenum cap)
{
	switch (cap)
	{
	case GL_TEXTURE_2D:   shim_texture_2d = 1; break;
	case GL_BLEND:
		shim_blend = 1;
		shim_guEnable(GU_BLEND);
		break;
	case GL_ALPHA_TEST:
		shim_alpha_test = 1;
		shim_guEnable(GU_ALPHA_TEST);
		break;
	case GL_DEPTH_TEST:
		shim_depth_test = 1;
		shim_guEnable(GU_DEPTH_TEST);
		break;
	case GL_SCISSOR_TEST:
		shim_scissor_on = 1;
		shim_scissor_flush();
		break;
	case GL_CULL_FACE:
		shim_guEnable(GU_CULL_FACE);
		break;
	default:
		break;
	}
}

void glDisable (GLenum cap)
{
	switch (cap)
	{
	case GL_TEXTURE_2D:   shim_texture_2d = 0; break;
	case GL_BLEND:
		shim_blend = 0;
		shim_guDisable(GU_BLEND);
		break;
	case GL_ALPHA_TEST:
		shim_alpha_test = 0;
		shim_guDisable(GU_ALPHA_TEST);
		break;
	case GL_DEPTH_TEST:
		shim_depth_test = 0;
		shim_guDisable(GU_DEPTH_TEST);
		break;
	case GL_SCISSOR_TEST:
		/* Not "no clipping": the GE's only bound is the scissor, so
		 * switching GL's scissor off restores the whole panel. */
		shim_scissor_on = 0;
		shim_scr_x = 0;
		shim_scr_y = 0;
		shim_scr_w = PSP_SCREEN_WIDTH;
		shim_scr_h = PSP_SCREEN_HEIGHT;
		shim_scissor_flush();
		break;
	case GL_CULL_FACE:
		shim_guDisable(GU_CULL_FACE);
		break;
	default:
		break;
	}
}

/* Turn GL's glBlendFunc() into a GE blend function.
 *
 * The GE computes     src * srcFactor  OP  dst * dstFactor
 * and the factors it has are (src/gu/pspgu.h, "Blending Factor"):
 *
 *     GU_OTHER_COLOR (0)   -- the *other* operand's colour: the frame buffer
 *                             when used as the source factor, the fragment's
 *                             own colour when used as the destination factor.
 *                             GU_SRC_COLOR and GU_DST_COLOR are both this same
 *                             value (the header marks them deprecated).
 *     GU_SRC_ALPHA / GU_ONE_MINUS_SRC_ALPHA
 *     GU_DST_ALPHA / GU_ONE_MINUS_DST_ALPHA
 *     GU_FIX             -- the constant passed as srcfix / destfix
 *
 * There is no "one" factor and no "zero" factor: one is GU_FIX 0xFFFFFFFF and
 * zero is GU_FIX 0.  (GU_ZERO exists in that header, but it is a STENCIL
 * operation, value 1 -- passing it here would ask for GU_ONE_MINUS_OTHER_COLOR.)
 *
 * THE BUG THIS FIXES.  Until round 79 the list below had no case for
 * GL_ZERO/GL_SRC_COLOR, so the world's lightmap pass fell through to "the
 * usual" and was drawn as ordinary alpha blending.  That pass is how the map
 * gets its lighting: r_world.c R_DrawTextureChains(), case 6, draws every
 * surface once with its miptex and then again like this --
 *
 *     glDepthMask (GL_FALSE);
 *     glEnable (GL_BLEND);
 *     glBlendFunc (GL_ZERO, GL_SRC_COLOR);        // modulate
 *     R_DrawLightmapChains ();
 *
 * -- the lightmap *multiplied into* the picture.  Alpha-blended instead, a
 * lightmap that should darken a corner paints a translucent grey over it and
 * a bright one paints white over it: every surface washes out towards the
 * lightmap's own colour, the map looks flat and pale, and the pattern of the
 * lightmap is on screen as if it were a picture.  It also costs a second
 * full-surface pass with a frame-buffer read for no visual gain.
 *
 * GU_OTHER_COLOR as the source factor is exactly GL_SRC_COLOR as the
 * destination factor: src * dst.  The overbright variant
 * (GL_DST_COLOR/GL_SRC_COLOR, "2x modulate") cannot be doubled by the GE --
 * there is no times-two factor -- so it is drawn as one modulate pass, which
 * is what the non-overbright path above does anyway. */
static void shim_apply_blend (void)
{
	int src = shim_blend_src;
	int dst = shim_blend_dst;

	if (src == GL_ONE && dst == GL_ONE)
		/* additive: src + dst */
		shim_guBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFFFF, 0xFFFFFFFF);
	else if (src == GL_ONE && dst == GL_ONE_MINUS_SRC_ALPHA)
		/* premultiplied: src + dst*(1-a) */
		shim_guBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_SRC_ALPHA, 0xFFFFFFFF, 0);
	else if (src == GL_SRC_ALPHA && dst == GL_ONE)
		/* additive, scaled by the fragment's alpha: src*a + dst.
		 * Sprites and particles ask for this and it used to land in
		 * "the usual" as well, i.e. they were alpha blended. */
		shim_guBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFF);
	else if ((src == GL_ZERO && dst == GL_SRC_COLOR) ||
	         (src == GL_DST_COLOR && dst == GL_SRC_COLOR))
		/* modulate: src * dst  (the world's lightmap pass) */
		shim_guBlendFunc(GU_ADD, GU_OTHER_COLOR, GU_FIX, 0, 0);
	else if (src == GL_ZERO && dst == GL_SRC_ALPHA)
		/* dst * a */
		shim_guBlendFunc(GU_ADD, GU_FIX, GU_SRC_ALPHA, 0, 0);
	else
		/* the usual: src*a + dst*(1-a) */
		shim_guBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
}

void glBlendFunc (GLenum sfactor, GLenum dfactor)
{
	shim_blend_src = (int)sfactor;
	shim_blend_dst = (int)dfactor;
	shim_apply_blend();
}

void glAlphaFunc (GLenum func, GLclampf ref)
{
	int gufunc = GU_GREATER;

	shim_alpha_ref = ref;

	switch (func)
	{
	case GL_NEVER:  gufunc = GU_NEVER;  break;
	case GL_LESS:   gufunc = GU_LESS;   break;
	case GL_EQUAL:  gufunc = GU_EQUAL;  break;
	case GL_LEQUAL: gufunc = GU_LEQUAL; break;
	case GL_GREATER: gufunc = GU_GREATER; break;
	case GL_NOTEQUAL: gufunc = GU_NOTEQUAL; break;
	case GL_GEQUAL: gufunc = GU_GEQUAL; break;
	case GL_ALWAYS: gufunc = GU_ALWAYS; break;
	default: break;
	}

	shim_alpha_func = gufunc;
	shim_guAlphaFunc(gufunc, (unsigned int)(ref * 255.0f), 0xFF);
}

void glDepthMask (GLboolean flag)
{
	shim_depth_mask = flag ? 1 : 0;
	/* GU thinks in the opposite direction: GU_TRUE means "do not write". */
	shim_guDepthMask(flag ? GU_FALSE : GU_TRUE);
}

void glDepthFunc (GLenum func)
{
	switch (func)
	{
	case GL_NEVER:  shim_guDepthFunc(GU_NEVER);  break;
	case GL_LESS:   shim_guDepthFunc(GU_LESS);   break;
	case GL_EQUAL:  shim_guDepthFunc(GU_EQUAL);  break;
	case GL_LEQUAL: shim_guDepthFunc(GU_LEQUAL); break;
	case GL_GREATER: shim_guDepthFunc(GU_GREATER); break;
	/* The depth convention is GL's now (gu_psp.c: sceGuDepthRange(0,
	 * 65535)), so these three are also the same way round as GL means
	 * them.  gl_sky.c uses GL_GEQUAL for the sky and would otherwise get
	 * whatever the previous draw left behind. */
	case GL_GEQUAL:  shim_guDepthFunc(GU_GEQUAL);  break;
	case GL_NOTEQUAL: shim_guDepthFunc(GU_NOTEQUAL); break;
	case GL_ALWAYS: shim_guDepthFunc(GU_ALWAYS); break;
	default: break;
	}
}

void glShadeModel (GLenum mode)
{
	shim_guShadeModel(mode == GL_FLAT ? GU_FLAT : GU_SMOOTH);
}

void glHint (GLenum target, GLenum mode)
{
	(void)target;
	(void)mode;
}

/* ---- clearing ---- */

void glClearColor (GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha)
{
	/* 0xAABBGGRR again -- see the note above glColor3f(). */
	shim_clear_color = ((unsigned int)(alpha * 255.0f + 0.5f) << 24) |
	                   ((unsigned int)(blue * 255.0f + 0.5f) << 16) |
	                   ((unsigned int)(green * 255.0f + 0.5f) << 8) |
	                   ((unsigned int)(red * 255.0f + 0.5f));
}

void glClear (GLbitfield mask)
{
	unsigned int bits = 0;

	if (mask & GL_COLOR_BUFFER_BIT)
	{
		bits |= GU_COLOR_BUFFER_BIT;
		shim_guClearColor(shim_clear_color);
	}
	if (mask & GL_DEPTH_BUFFER_BIT)
		bits |= GU_DEPTH_BUFFER_BIT;

	if (bits)
		shim_guClear(bits);
}

/* ---- textures ---- */

/* Texture names, with recycling.
 *
 * The table below is fixed size, so the names have to come back when a
 * texture dies.  They never did: the counter only ever went up, and once the
 * engine had created a thousand textures -- a few hundred menu visits now
 * that pictures are unloaded when nobody asks for them -- every new name
 * would have been past the end of the table and every new picture silently
 * dropped.  A freed name is pushed on a stack and handed out again first. */
static unsigned int shim_tex_id_next = 1;
static unsigned int shim_tex_id_free[SHIM_MAX_TEXTURES];
static unsigned int shim_tex_id_free_count;

static unsigned int shim_tex_id_take (void)
{
	if (shim_tex_id_free_count)
		return shim_tex_id_free[--shim_tex_id_free_count];

	return shim_tex_id_next++;
}

static void shim_tex_id_give (unsigned int id)
{
	if (id == 0 || id >= SHIM_MAX_TEXTURES)
		return;

	if (shim_tex_id_free_count < SHIM_MAX_TEXTURES)
		shim_tex_id_free[shim_tex_id_free_count++] = id;
}

void glGenTextures (GLsizei n, GLuint *textures)
{
	int i;

	for (i = 0; i < n; i++)
		textures[i] = shim_tex_id_take();
}

void glDeleteTextures (GLsizei n, const GLuint *textures)
{
	int i;

	for (i = 0; i < n; i++)
	{
		shim_texture_t *t = shim_texture_for(textures[i], 0);

		if (!t)
			continue;

		if (t->pixels)
		{
			if (t->in_arena)
			{
				tex_arena_free(&shim_arena, t->pixels);
				shim_arena_trim();
			}
			else
			{
				free(t->pixels);
				if (shim_fallback_bytes >= t->bytes)
					shim_fallback_bytes -= t->bytes;
				else
					shim_fallback_bytes = 0;
				if (shim_arena_fallback > 0)
					shim_arena_fallback--;
			}

			shim_texture_bytes -= t->bytes;
		}

		/* Whatever the GE was told about this texture is about bytes
		 * that are now free for the next picture; the next bind has to
		 * announce everything from scratch. */
		shim_tex_state_valid = 0;

		if (shim_texture_bound == textures[i])
			shim_texture_bound = 0;

		memset(t, 0, sizeof(*t));
		shim_texture_count--;
		shim_tex_id_give(textures[i]);
	}
}

void glBindTexture (GLenum target, GLuint texture)
{
	(void)target;
	shim_texture_bound = texture;
	shim_uv_scale_update();
}

void glTexImage2D (GLenum target, GLint level, GLint internalFormat,
                   GLsizei width, GLsizei height, GLint border,
                   GLenum format, GLenum type, const GLvoid *pixels)
{
	shim_texture_t *t;
	unsigned int prof_t0 = SHIM_PROF_NOW ();
	int row_bytes;
	int t8 = 0;

	(void)internalFormat;
	(void)border;

	if (target != GL_TEXTURE_2D || level != 0)
		return;	/* mip levels are not used by the 2D path */

	if (width <= 0 || height <= 0 || !pixels)
		return;

	if (type != GL_UNSIGNED_BYTE)
		return;	/* nothing in the 2D path asks for another type */

	t = shim_texture_for(shim_texture_bound, 1);
	if (!t)
		return;

	/* Say it in the log *before* the arena reports "no room": a picture this
	 * size is not a memory question, and the two used to look identical. */
	if (width > SHIM_TEX_MAX || height > SHIM_TEX_MAX)
	{
		static int too_big;

		if (too_big < 8)
		{
			too_big++;
			psp_tracef("tex: %dx%d picture is bigger than the GE's %d "
			           "limit in one direction -- not uploaded (this is "
			           "not a memory problem; the picture is drawn as the "
			           "null texture)", width, height, SHIM_TEX_MAX);
		}

		return;
	}

	switch (format)
	{
	case GL_RGBA: case GL_BGRA:       row_bytes = width * 4; break;
	case GL_RGB:                      row_bytes = width * 3; break;
	case GL_LUMINANCE_ALPHA:          row_bytes = width * 2; break;
	case GL_LUMINANCE: case GL_ALPHA: row_bytes = width;     break;
	default:                          row_bytes = width * 4; break;
	}

	/* Can this picture be 8-bit?  Decided here, before a single byte is
	 * taken from the arena: one pass over the texels answers both questions
	 * -- is every one of them opaque, and how many distinct 15-bit colours
	 * are there.  See the note above shim_put_pixels(). */
	t8 = (!shim_texture_priority &&
	      /* The palette costs a kilobyte, so the box has to be worth it:
	       * below 1024 texels 8-bit is *larger* than 16-bit. */
	      (size_t)shim_pot(width) * (size_t)shim_pot(height) >
	          (size_t)SHIM_PALETTE_BYTES &&
	      shim_picture_fits_8bit(pixels, row_bytes, width, height, format));

	if (!shim_texture_alloc(t, width, height,
	                        format == GL_RGB || format == GL_LUMINANCE,
	                        t8))
	{
		shim_texture_memory_short(width, height);
		return;
	}

	shim_put_pixels(t, 0, 0, width, height, (const unsigned char *)pixels,
	                row_bytes, format);
	shim_edge_extend(t);
	shim_uv_scale_update();

	/* The texels just changed, whether or not the block was reused: this is
	 * what makes the next bind write the CPU cache back and invalidate the
	 * GE's texture cache.  It used to be set only by the allocation path,
	 * which is enough for a block that is allocated fresh and wrong for one
	 * that is reused at the same size -- a reloaded picture or a lightmap
	 * block would have kept whatever the GE had cached. */
	t->dirty = 1;

	/* Converting texels is real work -- the 8-bit scan and the write of the
	 * box -- and it happens inside the frame when a lightmap or a menu page
	 * is reloaded.  Counted here so the next log can tell it apart from the
	 * drawing. */
	shim_prof_us[SHIM_PROF_PIXELS] += SHIM_PROF_NOW () - prof_t0;
}

/* Turn an 8-bit picture back into a 16-bit one.
 *
 * glTexSubImage2D() updates a rectangle of an existing texture from real
 * colours.  The indices of a GU_PSM_T8 picture cannot be updated that way --
 * a new colour would have to enter the palette and every other texel that
 * shares its index would change with it -- so the picture is expanded through
 * its own palette first and the update lands in the 16-bit copy.
 *
 * Nothing in the engine does this today: the only glTexSubImage2D() caller is
 * the dynamic lightmap path (Quake/gl_texmgr.c), and lightmaps are marked with
 * psp_gl_shim_priority_texture() and are never made 8-bit.  It exists so that
 * a future caller degrades into a slower picture rather than a wrong one. */
static int shim_texture_promote_to_16 (shim_texture_t *t)
{
	unsigned int pal[256];
	unsigned short *copy;
	size_t need;
	int row, col;

	if (!t || t->psm != GU_PSM_T8 || !t->pixels)
		return 1;

	need = (size_t)t->potw * (size_t)t->poth * 2;
	copy = (unsigned short *)malloc(need);
	if (!copy)
		return 0;

	memcpy(pal, shim_palette_of(t), sizeof(pal));

	for (row = 0; row < t->poth; row++)
	{
		const unsigned char *s = (const unsigned char *)t->pixels +
		                         (size_t)row * (size_t)t->tbw;
		unsigned short *d = (unsigned short *)((unsigned char *)copy +
		                     (size_t)row * (size_t)t->tbw * 2);

		for (col = 0; col < t->tbw; col++)
		{
			unsigned int e = pal[s[col]];

			d[col] = shim_px4444(e & 0xFF, (e >> 8) & 0xFF, (e >> 16) & 0xFF,
			                     (e >> 24) & 0xFF);
		}
	}

	if (!shim_texture_alloc(t, t->width, t->height, 1, 0) || !t->pixels)
	{
		free(copy);
		psp_tracef("tex: an 8-bit picture received a subimage and the "
		           "16-bit copy did not fit -- the picture is lost "
		           "(see the arena report above)");
		return 0;
	}

	memcpy(t->pixels, copy, need);
	free(copy);
	t->dirty = 1;
	shim_tex_state_valid = 0;
	return 1;
}

void glTexSubImage2D (GLenum target, GLint level, GLint xoffset, GLint yoffset,
                      GLsizei width, GLsizei height, GLenum format, GLenum type,
                      const GLvoid *pixels)
{
	shim_texture_t *t = shim_texture_for(shim_texture_bound, 0);
	int row_bytes;

	(void)target; (void)level; (void)type;

	if (!t || !t->pixels || width <= 0 || height <= 0 || !pixels)
		return;

	if (xoffset < 0 || yoffset < 0 ||
	    xoffset + width > t->width || yoffset + height > t->height)
		return;

	if (!shim_texture_promote_to_16(t))
		return;

	switch (format)
	{
	case GL_RGBA: case GL_BGRA:       row_bytes = width * 4; break;
	case GL_RGB:                      row_bytes = width * 3; break;
	case GL_LUMINANCE_ALPHA:          row_bytes = width * 2; break;
	case GL_LUMINANCE: case GL_ALPHA: row_bytes = width;     break;
	default:                          row_bytes = width * 4; break;
	}

	shim_put_pixels(t, xoffset, yoffset, width, height,
	                (const unsigned char *)pixels, row_bytes, format);
	t->dirty = 1;
	shim_uv_scale_update();
}

void glTexParameterf (GLenum target, GLenum pname, GLfloat param)
{
	shim_texture_t *t = shim_texture_for(shim_texture_bound, 1);

	(void)target;

	if (!t)
		return;

	if (pname == GL_TEXTURE_MAG_FILTER || pname == GL_TEXTURE_MIN_FILTER)
		t->filter = ((int)param == GL_LINEAR) ? GU_LINEAR : GU_NEAREST;
}

void glTexParameteri (GLenum target, GLenum pname, GLint param)
{
	glTexParameterf(target, pname, (GLfloat)param);
}

void glTexEnvf (GLenum target, GLenum pname, GLfloat param)
{
	(void)target; (void)pname; (void)param;
}

void glTexEnvi (GLenum target, GLenum pname, GLint param)
{
	(void)target; (void)pname; (void)param;
}

void glPixelStorei (GLenum pname, GLint param)
{
	(void)pname; (void)param;
}

void glFinish (void)
{
	/* The GU is synchronised at the end of every frame (gu_psp.c), and
	 * forcing a sync here would stall the pipeline for no benefit. */
}

void glColorMask (GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha)
{
	(void)red; (void)green; (void)blue; (void)alpha;
}

/* ---- capability queries ---- */

/* The engine does save/restore GL state around a sub-draw (menu.c's
 * M_DrawLogo does glIsEnabled(GL_SCISSOR_TEST) ... glGetIntegerv(GL_SCISSOR_BOX)
 * ... glEnable again), so both of these have to answer the truth.  The
 * scissor box is kept in GU coordinates -- top-left origin -- and has to come
 * back in GL's bottom-left ones. */
GLboolean glIsEnabled (GLenum cap)
{
	switch (cap)
	{
	case GL_TEXTURE_2D:   return shim_texture_2d ? GL_TRUE : GL_FALSE;
	case GL_BLEND:        return shim_blend      ? GL_TRUE : GL_FALSE;
	case GL_ALPHA_TEST:   return shim_alpha_test ? GL_TRUE : GL_FALSE;
	case GL_DEPTH_TEST:   return shim_depth_test ? GL_TRUE : GL_FALSE;
	case GL_SCISSOR_TEST: return shim_scissor_on ? GL_TRUE : GL_FALSE;
	default:              return GL_FALSE;
	}
}

void glGetIntegerv (GLenum pname, GLint *params)
{
	if (!params)
		return;

	switch (pname)
	{
	case GL_MAX_TEXTURE_SIZE:       *params = 512; break;
	case GL_MAX_TEXTURE_UNITS_ARB:  *params = 1;   break;
	case GL_VIEWPORT:               params[0] = shim_vp_x; params[1] = shim_vp_y;
	                                params[2] = shim_vp_w; params[3] = shim_vp_h;
	                                break;
	case GL_SCISSOR_BOX:            params[0] = shim_scr_x;
	                                params[1] = PSP_SCREEN_HEIGHT - (shim_scr_y + shim_scr_h);
	                                params[2] = shim_scr_w; params[3] = shim_scr_h;
	                                break;
	default:                        *params = 0; break;
	}
}

/* The matrices are kept column-major, exactly as glLoadMatrixf() reads them,
 * so handing one back is a straight copy. */
static void shim_copy_matrix (const shim_matrix_t *src, GLfloat *out)
{
	int i;

	for (i = 0; i < 16; i++)
		out[i] = src->m[i];
}

void glGetFloatv (GLenum pname, GLfloat *params)
{
	if (!params)
		return;

	switch (pname)
	{
	case GL_VIEWPORT:
		params[0] = (GLfloat)shim_vp_x; params[1] = (GLfloat)shim_vp_y;
		params[2] = (GLfloat)shim_vp_w; params[3] = (GLfloat)shim_vp_h;
		break;
	/* Needed twice over: the engine unprojects through them (gl_screen.c),
	 * and the menu saves and restores them around the 3D model preview so
	 * that the 2D drawing after it is not projected through a frustum. */
	case GL_PROJECTION_MATRIX: shim_copy_matrix(&shim_projection, params); break;
	case GL_MODELVIEW_MATRIX:  shim_copy_matrix(&shim_modelview, params); break;
	default:
		*params = 0.0f;
		break;
	}
}

/* How much main memory the textures are using: the PSP has 64 MB in total and
 * the engine's zone already takes 33 of them, so this number matters. */
size_t psp_gl_shim_texture_bytes (void)
{
	return shim_texture_bytes;
}

/* called from gu_psp.c                                               */
/* ------------------------------------------------------------------ */

/* GU commands only mean something inside a draw list, and the engine issues
 * most of its glEnable()/glBlendFunc()/glAlphaFunc() calls either during
 * Host_Init (no list at all) or outside the begin/end pair of the current
 * frame.  Those calls therefore only record state; this function sends the
 * recorded state into the list that psp_gu_frame_begin() has just opened.
 *
 * With this in place the shim never has to care whether a GL call lands
 * inside a frame or not -- the state it set is always in force by the time
 * the first picture of the frame is drawn. */
void psp_gl_shim_end_frame (void)
{
	shim_batch_flush();
	shim_frames++;

	/* What the picture pool actually looks like: the KB in the frame line
	 * below is the engine's own sum, the arena line is what the allocator
	 * really holds and what is left for the next picture.  Ten seconds
	 * apart, because a session that ends in a freeze lasts a few seconds,
	 * not the minute this used to wait for -- which is why not one of these
	 * lines was in the last log. */
	if (shim_frames == 60 || (shim_frames % 600) == 0)
		psp_gl_shim_texture_report();

	/* Where the frame went, in tenths of a millisecond -- see SHIM_PROF_*.
	 * Every frame of this port writes one of these lines only every 120th
	 * time, and the numbers are per frame, so it costs two clock reads per
	 * bucket and no card traffic unless the line is printed. */
	if (shim_frames <= 3 || (shim_frames % 120) == 0)
		psp_tracef("prof: frame %u, scene %u.%u ms (sky %u.%u), world flush "
		           "%u.%u, textures state %u.%u, draw %u.%u, texels %u.%u, "
		           "%u texture flushes",
		           shim_frames,
		           shim_prof_us[SHIM_PROF_SCENE] / 1000,
		           (shim_prof_us[SHIM_PROF_SCENE] % 1000) / 100,
		           shim_prof_us[SHIM_PROF_SKY] / 1000,
		           (shim_prof_us[SHIM_PROF_SKY] % 1000) / 100,
		           shim_prof_us[SHIM_PROF_FLUSH] / 1000,
		           (shim_prof_us[SHIM_PROF_FLUSH] % 1000) / 100,
		           shim_prof_us[SHIM_PROF_TEX] / 1000,
		           (shim_prof_us[SHIM_PROF_TEX] % 1000) / 100,
		           shim_prof_us[SHIM_PROF_DRAW] / 1000,
		           (shim_prof_us[SHIM_PROF_DRAW] % 1000) / 100,
		           shim_prof_us[SHIM_PROF_PIXELS] / 1000,
		           (shim_prof_us[SHIM_PROF_PIXELS] % 1000) / 100,
		           shim_prof_texflush);

	if (shim_frames <= 3 || (shim_frames % 120) == 0)
		psp_tracef("gl: frame %u, %u draws in %u batches, %u vertices, %u screen-size draws, "
		           "%d KB of textures, ring %u of %d, scissor %s %d,%d %dx%d, "
		           "viewport %d,%d %dx%d",
		           shim_frames, shim_frame_draws, shim_frame_batches, shim_frame_verts,
		           shim_big_draws,
		           (int)(shim_texture_bytes / 1024),
		           shim_ge_peak, SHIM_GE_VERTICES,
		           shim_scissor_on ? "on" : "off",
		           shim_scr_x, shim_scr_y, shim_scr_w, shim_scr_h,
		           shim_vp_x, shim_vp_y, shim_vp_w, shim_vp_h);

	/* The counters describe one frame; the textures already named above
	 * stay named, so the log keeps one line per offending picture. */
	shim_frame_draws = 0;
	shim_frame_verts = 0;
	shim_frame_batches = 0;
	shim_big_draws = 0;
}

void psp_gl_shim_set_frame_active (int active)
{
	shim_frame_active = active;
}

/* Whether a 3D polygon has already gone into this frame's draw list.
 *
 * Used by quakedef-side breadcrumbs only (Quake/gl_screen.c, SCR_MARK).  It is
 * deliberately a fact about the draw list and not about the client: at boot the
 * engine plays a demo world with cls.state == ca_disconnected, and every test
 * built on client state called that "no world" -- which is what kept the
 * breadcrumb gate open for a whole session (round-81 log: 1105 "scr: begin"
 * lines where three frames' worth were intended).
 *
 * The counter is cleared in psp_gl_shim_begin_frame() and NOT in end_frame(),
 * because the screen reads it after GL_EndRendering() has run. */
int psp_gl_world_drawn_this_frame (void)
{
	return shim_world_prims != 0;
}

/* Put the last accumulated batch into the draw list.
 *
 * The shim batches consecutive draws that only differ in position, and a batch
 * leaves for the GE when something else happens: a texture change, a state
 * change, or the batch flush inside glEnd().  A frame can therefore end with a
 * batch still in hand, and gu_psp.c calls sceGuFinish() right after this -- so
 * anything not flushed here is never drawn at all.  It used to be flushed as a
 * side effect of the reference card, which began with a flush; the card is
 * gone, so the flush is explicit. */
void psp_gl_shim_flush_batch (void)
{
	shim_batch_flush();
}

void psp_gl_shim_begin_frame (void)
{
	shim_frame_draws = 0;
	shim_frame_verts = 0;
	shim_frame_batches = 0;
	shim_world_prims = 0;

	{
		int i;

		for (i = 0; i < SHIM_PROF_BUCKETS; i++)
			shim_prof_us[i] = 0;
		shim_prof_texflush = 0;
	}

	/* gu_psp.c synced the previous frame before it swapped, so the GE is
	 * finished with every slice it was given last time. */
	shim_batch_n = 0;
	shim_batch_v = NULL;
	shim_ge_used = 0;
	shim_ge_peak = 0;

	/* A new draw list remembers nothing: whatever this process cached about
	 * what the GE had been told is void, so the state is sent again. */
	shim_ge_tex2d = -1;
	shim_ge3d_sent_valid = 0;
	shim_guTexEnable(shim_texture_2d ? 1 : 0);

	if (shim_blend)
	{
		shim_guEnable(GU_BLEND);
		shim_apply_blend();
	}
	else
		shim_guDisable(GU_BLEND);

	if (shim_alpha_test)
	{
		shim_guEnable(GU_ALPHA_TEST);
		shim_guAlphaFunc(shim_alpha_func, (unsigned int)(shim_alpha_ref * 255.0f), 0xFF);
	}
	else
		shim_guDisable(GU_ALPHA_TEST);

	if (shim_depth_test)
	{
		shim_guEnable(GU_DEPTH_TEST);
		shim_guDepthMask(shim_depth_mask ? GU_FALSE : GU_TRUE);
	}
	else
		shim_guDisable(GU_DEPTH_TEST);

	shim_scissor_flush();

	/* Whatever texture was current at the end of the last frame has to be
	 * re-announced to the GE: the engine binds gl-world textures by the
	 * dozen inside a frame, but the one left bound must still be valid.
	 *
	 * The end of a frame is also where anything drawn outside the shim
	 * would invalidate the record of what the GE has been told, so the
	 * cache is dropped unconditionally rather than reasoned about: it
	 * costs one texture announcement per frame and cannot go stale. */
	shim_tex_state_valid = 0;
	shim_uv_scale_update();
	shim_upload_bound_texture();
}
