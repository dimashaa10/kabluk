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

/* Room for the triangles a quad list expands into (each quad becomes two
 * triangles, i.e. six vertices). */
#define SHIM_MAX_TRI_VERTICES ((SHIM_MAX_VERTICES / 4) * 6)

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
	int          tbw;             /* row stride in pixels (multiple of 4) */
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
static shim_vertex_t  shim_triangles[SHIM_MAX_TRI_VERTICES] __attribute__((aligned(16)));
static float          shim_current_u, shim_current_v;
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

static unsigned short shim_px5650 (int r, int g, int b)
{
	return (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
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
static int shim_texture_alloc (shim_texture_t *t, int width, int height, int opaque)
{
	size_t need;
	int changed;

	t->tbw = (width + 3) & ~3;		/* row stride the GU can live with */
	need = (size_t)t->tbw * (size_t)height * 2;

	changed = (t->width != width || t->height != height ||
	           (t->psm == GU_PSM_5650) != (opaque != 0));

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
	t->psm = opaque ? GU_PSM_5650 : GU_PSM_4444;

	if (!t->pixels)
	{
		t->pixels = malloc(need);
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
		 * previous picture there. */
		memset(t->pixels, 0, (size_t)t->tbw * (size_t)height * 2);
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
	}

	/* Unswizzled: the engine's pictures are not power-of-two, and only
	 * swizzled textures have to be. */
	shim_guTexMode(t->psm, 0, 0, 0);
	shim_guTexImage(0, t->width, t->height, t->tbw, t->pixels);
	shim_guTexFilter(t->filter, t->filter);
	shim_guTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	shim_guTexFlush();
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

	shim_vertices[shim_vertex_count].u = shim_current_u;
	shim_vertices[shim_vertex_count].v = shim_current_v;
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
	case GL_QUADS:
	case GL_QUAD_STRIP:
	case GL_POLYGON:        return GU_SPRITES;
	default:                return GU_TRIANGLES;
	}
}

/* Turn the GL_QUADS accumulated since glBegin() into one triangle list.
 *
 * Every quad the engine draws arrives in the order
 *
 *      v0 = top-left    v1 = top-right
 *      v3 = bottom-left v2 = bottom-right        (clockwise, y downwards)
 *
 * so the two triangles are (v0, v1, v2) and (v0, v2, v3).  Triangles map the
 * texture exactly, and -- just as important -- the whole list goes to the GE
 * in ONE sceGuDrawArray: a screen full of text is a few thousand vertices,
 * and one command per quad would fill the 64 KB draw list in a frame or two.
 *
 * (The alternative, GU_SPRITES in vertex pairs, stretches the texture across
 * the rectangle instead of interpolating it and needs two commands per quad;
 * it is only worth it for axis-aligned, unfiltered pictures.) */
static void shim_flush_quads (void)
{
	int i, n = 0;

	shim_frame_draws++;

	for (i = 0; i + 3 < shim_vertex_count; i += 4)
	{
		if (n + 6 > SHIM_MAX_TRI_VERTICES)
		{
			shim_guDrawArray(GU_TRIANGLES, SHIM_VERTEX_FORMAT, n, NULL, shim_triangles);
			n = 0;
		}

		shim_triangles[n++] = shim_vertices[i + 0];
		shim_triangles[n++] = shim_vertices[i + 1];
		shim_triangles[n++] = shim_vertices[i + 2];

		shim_triangles[n++] = shim_vertices[i + 0];
		shim_triangles[n++] = shim_vertices[i + 2];
		shim_triangles[n++] = shim_vertices[i + 3];
	}

	if (n)
		shim_guDrawArray(GU_TRIANGLES, SHIM_VERTEX_FORMAT, n, NULL, shim_triangles);
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

	shim_guDrawArray(shim_gu_primitive(shim_primitive), SHIM_VERTEX_FORMAT,
	               shim_vertex_count, NULL, shim_vertices);
	shim_frame_draws++;
	shim_vertex_count = 0;
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

void glColor3f (GLfloat r, GLfloat g, GLfloat b)
{
	shim_current_color = 0xFF000000u |
	                     ((unsigned int)(r * 255.0f + 0.5f) << 16) |
	                     ((unsigned int)(g * 255.0f + 0.5f) << 8) |
	                     ((unsigned int)(b * 255.0f + 0.5f));
}

void glColor4f (GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
	shim_current_color = ((unsigned int)(a * 255.0f + 0.5f) << 24) |
	                     ((unsigned int)(r * 255.0f + 0.5f) << 16) |
	                     ((unsigned int)(g * 255.0f + 0.5f) << 8) |
	                     ((unsigned int)(b * 255.0f + 0.5f));
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
	shim_current_color = ((unsigned int)v[3] << 24) | ((unsigned int)v[0] << 16) |
	                     ((unsigned int)v[1] << 8) | (unsigned int)v[2];
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
	shim_clear_color = ((unsigned int)(alpha * 255.0f + 0.5f) << 24) |
	                   ((unsigned int)(red * 255.0f + 0.5f) << 16) |
	                   ((unsigned int)(green * 255.0f + 0.5f) << 8) |
	                   ((unsigned int)(blue * 255.0f + 0.5f));
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
		psp_tracef("gl: frame %u, %u draws, %u vertices, %d KB of textures, "
		           "scissor %s %d,%d %dx%d, viewport %d,%d %dx%d",
		           shim_frames, shim_frame_draws, shim_frame_verts,
		           (int)(shim_texture_bytes / 1024),
		           shim_scissor_on ? "on" : "off",
		           shim_scr_x, shim_scr_y, shim_scr_w, shim_scr_h,
		           shim_vp_x, shim_vp_y, shim_vp_w, shim_vp_h);
}

void psp_gl_shim_set_frame_active (int active)
{
	shim_frame_active = active;
}

void psp_gl_shim_begin_frame (void)
{
	shim_frame_draws = 0;
	shim_frame_verts = 0;

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
	 * dozen inside a frame, but the one left bound must still be valid. */
	shim_upload_bound_texture();
}
