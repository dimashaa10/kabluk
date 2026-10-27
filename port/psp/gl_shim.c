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

/* ------------------------------------------------------------------ */
/* state                                                              */
/* ------------------------------------------------------------------ */

/* The vertex layout, in the order the GU reads attributes: texture, colour,
 * normal, position.  Ours is texture + colour + position, in that same order,
 * which is what shim_vertex_t below declares. */
#define SHIM_VERTEX_FORMAT \
	(GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D)

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
 * the previous frame has been synchronised.  8192 vertices is 192 KB and holds
 * 1365 quads, i.e. a screen and a half of 8x8 text. */
#define SHIM_GE_VERTICES      8192

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
	unsigned int psm;             /* GU_PSM_4444 or GU_PSM_5650          */
	int          width, height;   /* what the engine asked for          */
	int          potw, poth;      /* what the GE is told (power of two) */
	int          tbw;             /* row stride in pixels (power of two) */
	void        *pixels;          /* main-memory copy the GU samples     */
	size_t       bytes;           /* 2 bytes per pixel: tbw * height * 2 */
	int          filter;          /* GU_NEAREST or GU_LINEAR             */
	int          dirty;           /* needs a re-upload before drawing    */
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
static shim_vertex_t  shim_ge_vertices[SHIM_GE_VERTICES] __attribute__((aligned(16)));
static unsigned int   shim_ge_used;            /* vertices handed to the GE */
static unsigned int   shim_ge_peak;            /* ... and the frame's high mark */
static unsigned int   shim_ge_overflows;
static float          shim_current_u, shim_current_v;
/* How much of the power-of-two box the picture actually fills: a 240x112
 * picture lives in a 256x128 texture, so the vertex's u is multiplied by
 * 240/256 and its v by 112/128 on the way out.  Kept up to date by
 * shim_uv_scale_update() whenever the bound texture (or its size) changes. */
static float          shim_u_scale = 1.0f, shim_v_scale = 1.0f;
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
static unsigned int shim_3d_warned_frames;
static unsigned int shim_3d_warned_frame = ~0u;

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
static void shim_guEnable (int cap)               { if (shim_frame_active) sceGuEnable(cap); }
static void shim_guDisable (int cap)              { if (shim_frame_active) sceGuDisable(cap); }
static void shim_guAlphaFunc (int f, unsigned int v, unsigned int m)
                                                  { if (shim_frame_active) sceGuAlphaFunc(f, v, m); }
static void shim_guBlendFunc (int op, int src, int dst, unsigned int sf, unsigned int df)
                                                  { if (shim_frame_active) sceGuBlendFunc(op, src, dst, sf, df); }
static void shim_guClear (unsigned int bits)      { if (shim_frame_active) sceGuClear(bits); }
static void shim_guClearColor (unsigned int c)    { if (shim_frame_active) sceGuClearColor(c); }
static void shim_guDepthFunc (int f)              { if (shim_frame_active) sceGuDepthFunc(f); }
static void shim_guDepthMask (int m)              { if (shim_frame_active) sceGuDepthMask(m); }
static void shim_guScissor (int x, int y, int w, int h)
                                                  { if (shim_frame_active) sceGuScissor(x, y, w, h); }
static void shim_guShadeModel (int m)             { if (shim_frame_active) sceGuShadeModel(m); }
static void shim_guTexFilter (int min, int mag)   { if (shim_frame_active) sceGuTexFilter(min, mag); }
static void shim_guTexFlush (void)                { if (shim_frame_active) sceGuTexFlush(); }
static void shim_guTexFunc (int tfx, int tcc)     { if (shim_frame_active) sceGuTexFunc(tfx, tcc); }
static void shim_guTexImage (int mip, int w, int h, int tbw, const void *p)
                                                  { if (shim_frame_active) sceGuTexImage(mip, w, h, tbw, p); }
/* NOT "mips": psp-gcc predefines the macro `mips` (the target CPU family is
 * baked into the compiler), so a parameter of that name is replaced by a
 * number before it is even parsed.  The desktop syntax check cannot see it. */
static void shim_guTexMode (int psm, int levels, int a2, int swz)
                                                  { if (shim_frame_active) sceGuTexMode(psm, levels, a2, swz); }
static void shim_guDrawArray (int prim, unsigned int fmt, int count, const void *idx, const void *verts)
{
	shim_frame_verts += (unsigned int)count;
	if (shim_frame_active)
		sceGuDrawArray(prim, fmt, count, idx, verts);
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
	}
	else
	{
		shim_u_scale = 1.0f;
		shim_v_scale = 1.0f;
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

static void shim_put_pixels (shim_texture_t *t, int x, int y, int w, int h,
                             const unsigned char *src, int row_bytes, GLenum format)
{
	int row, col, opaque = (t->psm == GU_PSM_5650);
	unsigned char *base = (unsigned char *)t->pixels +
	                      (size_t)y * (size_t)t->tbw * 2 + (size_t)x * 2;

	for (row = 0; row < h; row++)
	{
		const unsigned char *s = src + (size_t)row * (size_t)row_bytes;
		unsigned short *d = (unsigned short *)(base + (size_t)row * (size_t)t->tbw * 2);

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

			d[col] = opaque ? shim_px5650(r, g, b) : shim_px4444(r, g, b, a);
		}
	}
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

static int shim_texture_alloc (shim_texture_t *t, int width, int height, int opaque)
{
	size_t need;
	int changed;
	int potw = shim_pot(width), poth = shim_pot(height);

	t->tbw = potw;
	need = (size_t)potw * (size_t)poth * 2;

	if (potw < width || poth < height)
		return 0;	/* beyond what the GE can address; the caller logs it */

	changed = (t->width != width || t->height != height ||
	           t->potw != potw || t->poth != poth ||
	           (t->psm == GU_PSM_5650) != (opaque != 0));

	/* Only a block that is too small is replaced.  The picture changing its
	 * FORMAT does not count: GU_PSM_4444 and GU_PSM_5650 are both two bytes
	 * per pixel, so the block that is already there fits exactly.  It used
	 * to be freed in that case, and the replacement malloc was the same
	 * size -- the classic way to shred a pool into pieces that are all too
	 * small for the next big request while the total held never grows. */
	if (t->pixels && t->bytes < need)
	{
		free(t->pixels);
		shim_texture_bytes -= t->bytes;
		t->pixels = NULL;
		t->bytes = 0;
		changed = 1;
	}

	t->width = width;
	t->height = height;
	t->potw = potw;
	t->poth = poth;
	t->psm = opaque ? GU_PSM_5650 : GU_PSM_4444;

	if (!t->pixels)
	{
		/* 16-byte alignment is not optional: sceGuTexImage() asserts
		 * "Texture buffer pointer must be 16-byte aligned", and a
		 * plain malloc() on this libc hands back 8.  A misaligned
		 * texture moves every row by four texels. */
		t->pixels = memalign(16, need);
		if (!t->pixels)
			return 0;

		t->bytes = need;
		shim_texture_bytes += need;
		changed = 1;
	}

	if (changed)
	{
		/* A linear filter reads one texel past the end of the picture at
		 * the right and bottom edge, and a reused block still holds the
		 * previous picture there.  The whole power-of-two box is cleared
		 * for the same reason: the space between the picture and the
		 * edge of the box is sampled by that filter. */
		memset(t->pixels, 0, need);
	}

	t->dirty = 1;
	return 1;
}

static void shim_texture_memory_short (int width, int height)
{
	static int warned;

	if (warned)
		return;

	warned = 1;
	psp_tracef("tex: no memory for a %dx%d picture (%d KB free, %d KB of textures)",
	           width, height, sceKernelTotalFreeMemSize() / 1024,
	           (int)(shim_texture_bytes / 1024));
}

static void shim_upload_bound_texture (void)
{
	shim_texture_t *t = shim_texture_for(shim_texture_bound, 1);

	if (!t || !t->pixels)
		return;

	if (t->dirty)
	{
		/* The GE reads main memory through the bus, past the CPU cache:
		 * without the writeback it samples stale bytes. */
		sceKernelDcacheWritebackRange(t->pixels, (unsigned int)t->bytes);
		t->dirty = 0;

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
	    shim_tex_announced_filter == t->filter &&
	    shim_tex_announced_potw == t->potw &&
	    shim_tex_announced_poth == t->poth &&
	    shim_tex_announced_tbw == t->tbw)
		return;

	/* Unswizzled: the engine's pictures are not power-of-two, and only
	 * swizzled textures have to be. */
	shim_guTexMode(t->psm, 0, 0, 0);
	shim_guTexImage(0, t->potw, t->poth, t->tbw, t->pixels);
	shim_guTexFilter(t->filter, t->filter);
	shim_guTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);

	/* TEX_FLUSH is what tells the GE's texture cache that the memory behind
	 * the address it was just given has been written by the CPU
	 * (pspsdk src/gu/sceGuTexFlush.c: one TEX_FLUSH command).  Without it
	 * the GE keeps sampling whatever it cached from that address, which is
	 * at least a frame of the previous picture and at worst the zeroes the
	 * block was allocated with. */
	shim_guTexFlush();

	shim_tex_announced_id = shim_texture_bound;
	shim_tex_announced_pixels = t->pixels;
	shim_tex_announced_psm = t->psm;
	shim_tex_announced_filter = t->filter;
	shim_tex_announced_potw = t->potw;
	shim_tex_announced_poth = t->poth;
	shim_tex_announced_tbw = t->tbw;
	shim_tex_state_valid = 1;
}

/* ------------------------------------------------------------------ */
/* vertices                                                           */
/* ------------------------------------------------------------------ */

/* Project one vertex through the current matrices and map it onto the screen.
 *
 * GL: clip -> ndc, then viewport with the origin at the bottom-left.
 * GU: pixels from the top-left corner of the panel. */
static void shim_emit_vertex (float x, float y, float z)
{
	shim_matrix_t mvp;
	float cx, cy, sx, sy;

	if (shim_vertex_count >= SHIM_MAX_VERTICES)
		return;

	/* The 3D pipeline is stage 3 and does not exist yet.  A perspective
	 * projection therefore reaches this vertex stage with no clipping at
	 * all: a vertex behind the camera comes out mirrored and enormously
	 * far away, and the GE fills the panel with triangles nobody asked
	 * for -- which is exactly the shape of the dark wedges that were
	 * crossing the menu.  So the case is named once per frame instead of
	 * being painted over: the next caller that draws in 3D shows up in the
	 * log by name instead of as a symptom. */
	if ((shim_projection.m[15] != 1.0f || shim_projection.m[3] != 0.0f) &&
	    shim_3d_warned_frames < 8 &&
	    shim_3d_warned_frame != shim_frames)
	{
		shim_3d_warned_frame = shim_frames;
		shim_3d_warned_frames++;
		psp_tracef("3d: f %d a vertex was drawn under a perspective projection "
		           "(m15 %.3f m3 %.3f); there is no 3D clipper yet",
		           (int)shim_frames, (double)shim_projection.m[15],
		           (double)shim_projection.m[3]);
	}

	mvp = shim_modelview;
	shim_multiply(&mvp, &shim_projection, &shim_modelview);

	cx = mvp.m[0] * x + mvp.m[4] * y + mvp.m[8]  * z + mvp.m[12];
	cy = mvp.m[1] * x + mvp.m[5] * y + mvp.m[9]  * z + mvp.m[13];

	/* The 2D canvases use an orthogonal projection with w == 1, so the
	 * perspective divide is a no-op; it is kept because the 3D shim (step 3)
	 * will need it and the maths is the same. */
	{
		float cw = mvp.m[3] * x + mvp.m[7] * y + mvp.m[11] * z + mvp.m[15];
		if (cw != 0.0f && cw != 1.0f)
		{
			cx /= cw;
			cy /= cw;
		}
	}

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

static void shim_trace_draw (int count, const char *what)
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
	skewed = (count % 4 == 0) && !shim_quads_axis_aligned(count);

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
		logging = ((shim_frames >= 118 && shim_frames < 122) ||
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
		else if (shim_trace_text < 12)
		{
			static float last_y0 = -1000.0f, last_y1 = -1000.0f;
			float overlap = 0.0f;

			shim_trace_text++;

			if (miny < last_y1 && maxy > last_y0)
				overlap = (maxy < last_y1 ? maxy : last_y1) -
				          (miny > last_y0 ? miny : last_y0);

			psp_tracef("text: f %d #%d box %d,%d %d,%d %s",
			           (int)shim_frames, (int)shim_frame_draws,
			           (int)minx, (int)miny, (int)maxx, (int)maxy,
			           (overlap > (maxy - miny) * 0.5f) ? "OVERLAPS PREVIOUS" : "");

			last_y0 = miny;
			last_y1 = maxy;
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
	static int named_n;
	float w = 0.0f, h = 0.0f;
	int i;

	if (count <= 0 || named_n >= (int)(sizeof(named) / sizeof(named[0])))
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

		w = maxx - minx;
		h = maxy - miny;
	}

	if (w * h < (float)(PSP_SCREEN_WIDTH * PSP_SCREEN_HEIGHT) / 4.0f)
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
	shim_trace_draw(shim_vertex_count, shim_prim_name(GL_QUADS));
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

		shim_guDrawArray(GU_TRIANGLES, SHIM_VERTEX_FORMAT, batch * 6, NULL, v);

		done += batch;
	}
}

/* ------------------------------------------------------------------ */
/* gl* entry points                                                   */
/* ------------------------------------------------------------------ */

void glBegin (GLenum mode)
{
	shim_primitive = (int)mode;
	shim_vertex_count = 0;
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
		shim_guEnable(GU_TEXTURE_2D);
		shim_upload_bound_texture();
	}
	else
	{
		shim_guDisable(GU_TEXTURE_2D);
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
		shim_trace_draw(shim_vertex_count, shim_prim_name(shim_primitive));
		shim_trace_big_once(shim_vertex_count, shim_prim_name(shim_primitive));

		if (v)
		{
			memcpy(v, shim_vertices, sizeof(shim_vertex_t) * (size_t)shim_vertex_count);
			shim_guDrawArray(shim_gu_primitive(shim_primitive), SHIM_VERTEX_FORMAT,
			                 shim_vertex_count, NULL, v);
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
	shim_identity(shim_current_matrix());
}

void glLoadMatrixf (const GLfloat *m)
{
	memcpy(shim_current_matrix()->m, m, sizeof(float) * 16);
}

void glMultMatrixf (const GLfloat *m)
{
	shim_matrix_t t, *cur = shim_current_matrix();

	memcpy(t.m, m, sizeof(t.m));
	shim_multiply(cur, cur, &t);
}

void glPushMatrix (void)
{
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
	shim_matrix_t t, *cur = shim_current_matrix();

	shim_identity(&t);
	t.m[12] = x;
	t.m[13] = y;
	t.m[14] = z;
	shim_multiply(cur, cur, &t);
}

void glScalef (GLfloat x, GLfloat y, GLfloat z)
{
	shim_matrix_t t, *cur = shim_current_matrix();

	shim_identity(&t);
	t.m[0] = x;
	t.m[5] = y;
	t.m[10] = z;
	shim_multiply(cur, cur, &t);
}

void glRotatef (GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
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
}

void glScissor (GLint x, GLint y, GLsizei width, GLsizei height)
{
	/* GL: origin bottom-left.  GU: origin top-left. */
	shim_scr_x = x;
	shim_scr_y = PSP_SCREEN_HEIGHT - (y + height);
	shim_scr_w = width > 0 ? width : 0;
	shim_scr_h = height > 0 ? height : 0;

	if (shim_scissor_on)
		shim_guScissor(shim_scr_x, shim_scr_y, shim_scr_w, shim_scr_h);
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
		shim_guEnable(GU_SCISSOR_TEST);
		shim_guScissor(shim_scr_x, shim_scr_y, shim_scr_w, shim_scr_h);
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
		shim_scissor_on = 0;
		shim_guDisable(GU_SCISSOR_TEST);
		break;
	case GL_CULL_FACE:
		shim_guDisable(GU_CULL_FACE);
		break;
	default:
		break;
	}
}

static void shim_apply_blend (void)
{
	int src = shim_blend_src;
	int dst = shim_blend_dst;

	/* The handful of combinations the engine actually asks for. */
	if (src == GL_ONE && dst == GL_ONE)
		shim_guBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFFFF, 0xFFFFFFFF);
	else if (src == GL_ONE && dst == GL_ONE_MINUS_SRC_ALPHA)   /* premultiplied */
		shim_guBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_SRC_ALPHA, 0xFFFFFFFF, 0);
	else if (src == GL_ZERO && dst == GL_SRC_ALPHA)             /* modulate */
		shim_guBlendFunc(GU_ADD, GU_FIX, GU_SRC_ALPHA, 0xFFFFFFFF, 0);
	else                                                        /* the usual */
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

void glGenTextures (GLsizei n, GLuint *textures)
{
	static unsigned int next = 1;
	int i;

	for (i = 0; i < n; i++)
		textures[i] = next++;
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
			free(t->pixels);
			shim_texture_bytes -= t->bytes;
		}
		memset(t, 0, sizeof(*t));
		shim_texture_count--;
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
	int row_bytes;

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

	switch (format)
	{
	case GL_RGBA: case GL_BGRA:       row_bytes = width * 4; break;
	case GL_RGB:                      row_bytes = width * 3; break;
	case GL_LUMINANCE_ALPHA:          row_bytes = width * 2; break;
	case GL_LUMINANCE: case GL_ALPHA: row_bytes = width;     break;
	default:                          row_bytes = width * 4; break;
	}

	if (!shim_texture_alloc(t, width, height,
	                        format == GL_RGB || format == GL_LUMINANCE))
	{
		shim_texture_memory_short(width, height);
		return;
	}

	shim_put_pixels(t, 0, 0, width, height, (const unsigned char *)pixels,
	                row_bytes, format);
	shim_uv_scale_update();
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

/* ------------------------------------------------------------------ */
/* the reference card                                                 */
/* ------------------------------------------------------------------ */

/* Everything above is a translation of GL state into GE state, and when the
 * panel shows "a grey field with a blue bar and a teal block" that says
 * nothing about *which* translation step is wrong: the geometry, the vertex
 * colour, the channel order of the texture, the power-of-two box, the alpha
 * test or the texture cache.
 *
 * So the shim draws a card of its own on top of the engine's frame, every
 * frame, in the top-left corner -- a picture whose correct appearance is
 * known in advance.  It is drawn with GE calls, not through the shim's own
 * gl* entry points, because testing the translation with the translation
 * would only prove it agrees with itself.
 *
 *      a black backdrop 160x72, so that "nothing was drawn" is black and
 *      not "white on a pale menu"
 *
 *      row 1 (y = 0)   the engine's alpha test left in force
 *      row 2 (y = 40)  the alpha test switched off
 *
 *      each row, five 32x32 squares left to right:
 *
 *        (0)    texture, GU_PSM_4444, GU_TFX_REPLACE  -- raw texels
 *        (32)   same texture, GU_TFX_MODULATE, white  -- what the menu does
 *        (64)   same picture as GU_PSM_8888, MODULATE -- rules out the nibbles
 *        (96)   no texture: magenta vertex colour
 *        (128)  no texture: white vertex colour
 *
 * The texture is red on its left half and blue on its right: two colours
 * that cannot hide against anything.  The correct result is two identical
 * rows of:
 *
 *      [ red | blue ][ red | blue ][ red | blue ][magenta][ white ]
 *
 * and what is actually there names the broken step:
 *
 *   - not even the black backdrop  -> the frame's draw list is not reaching
 *                                     the panel at all
 *   - backdrop but no magenta/white -> the vertex colour path (the GE is
 *                                     drawing, but not these vertices)
 *   - magenta and white, squares 1-3 black -> the textured path: the texture
 *                                     address, the cache (sceGuTexFlush), the
 *                                     format or the texture coordinates
 *   - squares 1-3 show red and blue swapped -> shim_px4444()'s nibble order
 *                                     for 4444, the 8888 square still right
 *   - 4444 in colour, 8888 black           -> GU_PSM_8888 set-up only
 *   - row 1 empty, row 2 fine              -> the engine's alpha test is
 *                                     discarding the frame
 *   - a striped or diagonal mess           -> the GE is reading vertices it
 *                                     should not (draw list overflow, or a
 *                                     vertex array reused before the GE has
 *                                     finished with it)
 *
 * The state the card changes is re-applied by psp_gl_shim_begin_frame() at
 * the start of the next frame. */

/* How many frames the reference card is drawn for, from the start of
 * rendering.  Forever is wrong (it covers the menu), never is wrong (there is
 * nothing to read on a photo), one second is the compromise. */
#define SHIM_CARD_FRAMES   60

#define SHIM_CARD_SIZE     32
#define SHIM_CARD_SQUARES  5
#define SHIM_CARD_ROWS     2
#define SHIM_CARD_ROW_GAP  (SHIM_CARD_SIZE + 8)

static unsigned short shim_card_tex4444[SHIM_CARD_SIZE * SHIM_CARD_SIZE] __attribute__((aligned(16)));
static unsigned int   shim_card_tex8888[SHIM_CARD_SIZE * SHIM_CARD_SIZE] __attribute__((aligned(16)));

/* Every draw gets its own vertices: sceGuDrawArray() only hands the GE the
 * address of the array and the GE keeps reading it after the CPU has moved
 * on, so one shared array would be rewritten under the card's own feet. */
static shim_vertex_t  shim_card_quads[SHIM_CARD_ROWS][SHIM_CARD_SQUARES][6] __attribute__((aligned(16)));
static shim_vertex_t  shim_card_backdrop[6] __attribute__((aligned(16)));
static int            shim_card_built;

static void shim_card_build (void)
{
	int x, y;

	for (y = 0; y < SHIM_CARD_SIZE; y++)
	{
		for (x = 0; x < SHIM_CARD_SIZE; x++)
		{
			int red_half = (x < SHIM_CARD_SIZE / 2);

			shim_card_tex4444[y * SHIM_CARD_SIZE + x] =
				red_half ? shim_px4444(255, 0, 0, 255)
				         : shim_px4444(0, 0, 255, 255);

			/* GU_PSM_8888 wants the byte order a GL_RGBA array has: R,G,B,A
			 * in memory, i.e. 0xAABBGGRR in the word (pspgl's format table
			 * lists GL_RGBA/GL_UNSIGNED_BYTE as TF_NATIVE on GE_RGBA_8888). */
			shim_card_tex8888[y * SHIM_CARD_SIZE + x] =
				red_half ? 0xFF0000FFu : 0xFFFF0000u;
		}
	}

	/* The GE reads main memory past the CPU cache: without the writeback it
	 * samples whatever was in those lines before. */
	sceKernelDcacheWritebackRange(shim_card_tex4444, sizeof(shim_card_tex4444));
	sceKernelDcacheWritebackRange(shim_card_tex8888, sizeof(shim_card_tex8888));

	shim_card_built = 1;
	psp_tracef("card: 2 rows x %d squares of %dx%d over a black backdrop %dx%d",
	           SHIM_CARD_SQUARES, SHIM_CARD_SIZE, SHIM_CARD_SIZE,
	           SHIM_CARD_SQUARES * SHIM_CARD_SIZE, SHIM_CARD_SIZE + 8 + SHIM_CARD_SIZE);
}

/* Six vertices of a screen-space quad: two triangles, wound like the quads the
 * engine itself hands over. */
static void shim_card_quad (shim_vertex_t *v, float x, float y, float w, float h,
                            unsigned int color)
{
	/* The texture coordinates are 0..SHIM_CARD_SIZE, not 0..1: the GE in
	 * through mode counts texels (the same thing that made every picture on
	 * the panel one stretched texel -- see shim_uv_scale_update()).  The
	 * squares that carry no texture ignore u and v. */
	const float m = (float)SHIM_CARD_SIZE;

	v[0].u = 0.0f; v[0].v = 0.0f; v[0].color = color; v[0].x = x;     v[0].y = y;     v[0].z = 0.0f;
	v[1].u = m;    v[1].v = 0.0f; v[1].color = color; v[1].x = x + w; v[1].y = y;     v[1].z = 0.0f;
	v[2].u = m;    v[2].v = m;    v[2].color = color; v[2].x = x + w; v[2].y = y + h; v[2].z = 0.0f;

	v[3] = v[0];
	v[4] = v[2];

	v[5].u = 0.0f; v[5].v = m;    v[5].color = color; v[5].x = x;     v[5].y = y + h; v[5].z = 0.0f;
}

/* One row of the card.  The five squares differ only in the texture state the
 * GE is in when they are drawn, so one photo of the panel says which of those
 * states the hardware is unhappy with. */
static void shim_card_row (int row, float y)
{
	shim_vertex_t (*q)[6] = shim_card_quads[row];

	/* (0) the picture with texturing left to do all the work */
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexMode(GU_PSM_4444, 0, 0, 0);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	sceGuTexImage(0, SHIM_CARD_SIZE, SHIM_CARD_SIZE, SHIM_CARD_SIZE, shim_card_tex4444);
	sceGuTexFlush();
	shim_card_quad(q[0], 0.0f, y, SHIM_CARD_SIZE, SHIM_CARD_SIZE, 0xFFFFFFFFu);
	sceGuDrawArray(GU_TRIANGLES, SHIM_VERTEX_FORMAT, 6, NULL, q[0]);

	/* (1) the same, modulated by a white vertex colour -- the menu's case */
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	shim_card_quad(q[1], (float)SHIM_CARD_SIZE, y, SHIM_CARD_SIZE, SHIM_CARD_SIZE, 0xFFFFFFFFu);
	sceGuDrawArray(GU_TRIANGLES, SHIM_VERTEX_FORMAT, 6, NULL, q[1]);

	/* (2) the same picture as a 32-bit texture: if this one draws and the
	 * two above do not, the 4-bit nibble packing is what is wrong. */
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, SHIM_CARD_SIZE, SHIM_CARD_SIZE, SHIM_CARD_SIZE, shim_card_tex8888);
	sceGuTexFlush();
	shim_card_quad(q[2], 2.0f * SHIM_CARD_SIZE, y, SHIM_CARD_SIZE, SHIM_CARD_SIZE, 0xFFFFFFFFu);
	sceGuDrawArray(GU_TRIANGLES, SHIM_VERTEX_FORMAT, 6, NULL, q[2]);

	/* (3) and (4) no texture at all: these test the geometry and the vertex
	 * colour path on their own. */
	sceGuDisable(GU_TEXTURE_2D);
	shim_card_quad(q[3], 3.0f * SHIM_CARD_SIZE, y, SHIM_CARD_SIZE, SHIM_CARD_SIZE,
	               0xFFFF00FFu);		/* 0xAABBGGRR: magenta */
	sceGuDrawArray(GU_TRIANGLES, SHIM_VERTEX_FORMAT, 6, NULL, q[3]);

	shim_card_quad(q[4], 4.0f * SHIM_CARD_SIZE, y, SHIM_CARD_SIZE, SHIM_CARD_SIZE,
	               0xFFFFFFFFu);		/* white */
	sceGuDrawArray(GU_TRIANGLES, SHIM_VERTEX_FORMAT, 6, NULL, q[4]);
}

void psp_gl_shim_test_card (void)
{
	if (!shim_frame_active)
		return;

	/* The card has done its job: it is what named the texel-coordinate bug
	 * (every picture was one stretched texel).  It now lives in the build
	 * for the first second only, so a screenshot taken after start-up shows
	 * the menu and not the diagnostic.  Raise SHIM_CARD_FRAMES when a
	 * picture on the panel needs explaining again. */
	if (shim_frames > SHIM_CARD_FRAMES)
		return;

	if (!shim_card_built)
		shim_card_build();

	/* Nothing may hide the card, and it must not be culled, faded or
	 * tested -- that is the whole point of a reference picture. */
	sceGuDisable(GU_SCISSOR_TEST);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_BLEND);
	sceGuDisable(GU_CULL_FACE);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_TEXTURE_2D);
	sceGuShadeModel(GU_FLAT);

	/* The backdrop first: the menu behind the card is pale, so a white
	 * square on it would be invisible, and "drawn in white" is exactly what
	 * this card must be able to tell apart from "not drawn". */
	shim_card_quad(shim_card_backdrop, 0.0f, 0.0f,
	               (float)(SHIM_CARD_SQUARES * SHIM_CARD_SIZE),
	               (float)(SHIM_CARD_SIZE + 8 + SHIM_CARD_SIZE), 0xFF000000u);
	sceGuDrawArray(GU_TRIANGLES, SHIM_VERTEX_FORMAT, 6, NULL, shim_card_backdrop);

	/* Row 1 keeps whatever alpha test the engine left in force, row 2 drops
	 * it, so "the picture is there but every pixel is discarded" cannot be
	 * mistaken for "the picture is not there". */
	if (shim_alpha_test)
	{
		sceGuEnable(GU_ALPHA_TEST);
		sceGuAlphaFunc(shim_alpha_func, (unsigned int)(shim_alpha_ref * 255.0f), 0xFF);
		shim_card_row(0, 0.0f);
		sceGuDisable(GU_ALPHA_TEST);
		shim_card_row(1, (float)SHIM_CARD_ROW_GAP);
	}
	else
	{
		shim_card_row(0, 0.0f);
		shim_card_row(1, (float)SHIM_CARD_ROW_GAP);
	}
}

/* ------------------------------------------------------------------ */
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
	shim_frames++;

	if (shim_frames <= 3 || (shim_frames % 120) == 0)
		psp_tracef("gl: frame %u, %u draws, %u vertices, %u screen-size draws, "
		           "%d KB of textures, ring %u of %d, scissor %s %d,%d %dx%d, "
		           "viewport %d,%d %dx%d",
		           shim_frames, shim_frame_draws, shim_frame_verts,
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
	shim_big_draws = 0;
}

void psp_gl_shim_set_frame_active (int active)
{
	shim_frame_active = active;
}

void psp_gl_shim_begin_frame (void)
{
	shim_frame_draws = 0;
	shim_frame_verts = 0;

	/* gu_psp.c synced the previous frame before it swapped, so the GE is
	 * finished with every slice it was given last time. */
	shim_ge_used = 0;
	shim_ge_peak = 0;

	if (shim_texture_2d)
		shim_guEnable(GU_TEXTURE_2D);
	else
		shim_guDisable(GU_TEXTURE_2D);

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

	if (shim_scissor_on)
	{
		shim_guEnable(GU_SCISSOR_TEST);
		shim_guScissor(shim_scr_x, shim_scr_y, shim_scr_w, shim_scr_h);
	}
	else
		shim_guDisable(GU_SCISSOR_TEST);

	/* Whatever texture was current at the end of the last frame has to be
	 * re-announced to the GE: the engine binds gl-world textures by the
	 * dozen inside a frame, but the one left bound must still be valid.
	 *
	 * The reference card of the previous frame drew with texture state of
	 * its own (GU_TFX_REPLACE, an 8888 texture, texturing switched off
	 * again), so nothing cached about texturing survives into a new
	 * frame. */
	shim_tex_state_valid = 0;
	shim_uv_scale_update();
	shim_upload_bound_texture();
}
