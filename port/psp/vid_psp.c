/*
 * port/psp/vid_psp.c -- video backend, stage M3: a working viddef_t and no
 * rendering yet.
 *
 * The engine keeps its video state in viddef_t and asks the backend to open a
 * window, flip buffers and hand back a GL context.  On the PSP there is no
 * window: sceGu draws straight into the frame buffer.  Until the sceGu shim
 * (stage M4) exists, this file gives the engine a valid viddef_t and honest
 * "no renderer" behaviour, and owns the GL capability globals the renderer
 * reads, all of which are set to "the hardware cannot do this".
 *
 * Nothing here calls the PSP SDK, so it also compiles on the host -- that is
 * how it was checked.
 */

#include "quakedef.h"
#include "glquake.h"
#include "gu_psp.h"

/* ---- the ones the renderer asks about before using an extension ---- */
qboolean	gl_vbo_able = false;
qboolean	gl_fbo_able = false;
qboolean	gl_glsl_able = false;
qboolean	gl_glsl_alias_able = false;
qboolean	gl_glsl_gamma_able = false;
qboolean	gl_glsl_water_able = false;
qboolean	gl_mtexable = false;
qboolean	gl_texture_env_add = false;
qboolean	gl_texture_env_combine = false;
/* Non-power-of-two pictures: YES, and the answer has to be yes.
 *
 * This flag does not mean "the hardware can sample any size".  The GE cannot:
 * sceGuTexImage() stores the LOGARITHM of the width and height, so the
 * hardware only knows power-of-two boxes.  What the flag tells the ENGINE is
 * whether the BACKEND can take a picture at its own size:
 *
 *   false -> gl_texmgr.c stretches every non-power-of-two picture up to the
 *            next power of two (TexMgr_LoadImage32 -> TexMgr_ResampleTexture)
 *            and gl_draw.c compensates by sampling only
 *            source_width/TexMgr_Pad(source_width) of the result.  Two
 *            compensations for one problem: the picture is resampled to fill
 *            the box and then a fraction of that stretched copy is shown, so
 *            the right and bottom edge of the artwork is cropped and the rest
 *            of it is squeezed.  A 240x112 menu picture is drawn from 240 of
 *            256 texels and 112 of 128 rows of a stretched copy -- 6% x 12%
 *            of the picture lost and the rest distorted.  It also costs a
 *            full extra 32-bit copy of the picture in the hunk, and a couple
 *            of hundred thousand bilinear samples while the menu loads.
 *
 *   true  -> the picture arrives at its own size, and the shim (gl_shim.c)
 *            copies it into the next power-of-two box itself, keeps the row
 *            stride right, clears the margin, and scales every u/v from 0..1
 *            to 0..width/0..height TEXELS (shim_uv_scale_update).  The GE then
 *            samples exactly the picture and nothing else, pixel for pixel,
 *            with no resample anywhere.
 *
 * So the backend does handle non-power-of-two *pictures*: it pads them in
 * video memory where padding belongs.  Saying false here made the engine pad
 * them in software first, and then we padded the padded copy again. */
qboolean	gl_texture_NPOT = true;
qboolean	gl_texture_s3tc = false;
qboolean	gl_texture_rgtc = false;
qboolean	gl_texture_bptc = false;
qboolean	gl_texture_etc2 = false;
qboolean	gl_texture_astc = false;
qboolean	gl_texture_e5bgr9 = false;

/* The largest texture sceGu can take.  This used to be 0, which is not "no
 * limit" but "no texture at all": TexMgr_SafeTextureSize() clamps every size
 * down to gl_hardware_maxsize, so every picture in the game was being shrunk
 * to 0x0 by the mip loop -- all the way to the last mip level, and a 0-wide
 * texture later divides by zero in the GL maths.  The PSP GPU tops out at
 * 512x512. */
GLint		gl_hardware_maxsize = 512;

/* ---- the lightmap block size, which nothing on this port was setting ------
 *
 * Quake/gl_vidsdl.c is the file that owns this number on the desktop:
 *
 *     glGetIntegerv (GL_MAX_TEXTURE_SIZE, &gl_hardware_maxsize);
 *     LMBLOCK_WIDTH  = q_min(gl_hardware_maxsize, 512);
 *     LMBLOCK_HEIGHT = q_min(gl_hardware_maxsize, 16384);
 *
 * and this port does not compile that file -- vid_psp.c replaces it, and
 * gl_vidsdl.c is not in the Makefile.  So the two variables kept the value
 * their definition gives them, which is zero:
 *
 *     Quake/r_brush.c:42   int LMBLOCK_WIDTH, LMBLOCK_HEIGHT;
 *
 * Nothing read them until a map loaded, and then two things happened at once.
 *
 * 1. CalcSurfaceExtents() throws away a surface whose lightmap is too big for
 *    a block --
 *
 *        if (!(tex->flags & TEX_SPECIAL) &&
 *            s->extents[i] >= (i?LMBLOCK_HEIGHT:LMBLOCK_WIDTH))
 *                s->extents[i] = 1;
 *
 *    -- and with a block of 0x0 that is EVERY surface: the whole map's
 *    lightmaps became 2x2 texels.
 *
 * 2. AllocBlock() could then never place one:
 *
 *        for (i = 0; i < LMBLOCK_WIDTH - w; i++)   // 0 - 2, never runs
 *        if (best + h > LMBLOCK_HEIGHT)            // 0 + 2 > 0, always true
 *            continue;
 *
 *    so the loop walked texnum up to MAX_SANITY_LIGHTMAPS (1<<20), realloc()ing
 *    the lightmap array every iteration.  That realloc eventually returned NULL
 *    and the very next line, memset(&lightmaps[texnum], 0, ...), wrote through
 *    it.  On this hardware that is an exception with no handler and no message
 *    (port/psp/psp_console.c), so the console simply stopped -- and the last
 *    line of kabluk.log was
 *
 *        r_newmap: 36 surfaces, lightmaps first
 *
 *    which is the line immediately before GL_BuildLightmaps() is called.
 *
 * 128x128 is GLQuake's own lightmap block and it fits this hardware twice
 * over: a block is 64 KB of engine memory (4 bytes a texel) and the GE
 * texture the shim builds from it is 128x128x2 = 32 KB, well inside the 512
 * texel limit of sceGuTexImage().  A taller block would only save lightmap
 * objects: the 16384 the desktop uses is 8 MB of bytes per block. */
#define PSP_LMBLOCK 128

static void psp_lmblock_init (void)
{
	if (LMBLOCK_WIDTH > 0 && LMBLOCK_HEIGHT > 0)
		return;

	LMBLOCK_WIDTH = LMBLOCK_HEIGHT = PSP_LMBLOCK;
	Con_Printf("Video: lightmap blocks are %dx%d\n",
	           LMBLOCK_WIDTH, LMBLOCK_HEIGHT);
}
float		gl_max_anisotropy = 0.0f;
GLint		gl_max_texture_units = 1;
GLint		gl_max_texture_image_units = 1;
GLint		gl_max_combined_texture_image_units = 1;
int		gl_stencilbits = 0;

/* ---- video state owned by the backend ---- */
viddef_t	vid;
qboolean	windowhasfocus = true;
int		videoc = 0;
int		videosetg = 0;

cvar_t		vid_contrast = {"vid_contrast", "1", CVAR_ARCHIVE};
cvar_t		vid_gamma = {"gamma", "1", CVAR_ARCHIVE};
cvar_t		vid_fsaa = {"vid_fsaa", "0", CVAR_ARCHIVE};
cvar_t		vid_fxaa = {"vid_fxaa", "0", CVAR_ARCHIVE};

/* PSP screen, in pixels. */
#define PSP_SCREEN_W 480
#define PSP_SCREEN_H 272

void VID_Init (void)
{
	/* No GL context yet: report the mode the engine should lay out for and
	 * leave the frame buffer alone.  M4 replaces this with sceGu. */
	vid.width = vid.conwidth = PSP_SCREEN_W;
	vid.height = vid.conheight = PSP_SCREEN_H;
	vid.rowbytes = vid.conrowbytes = PSP_SCREEN_W;
	vid.aspect = 480.0f / 272.0f;
	vid.numpages = 1;
	vid.recalc_refdef = 1;
	vid.fullbright = 0;

	/* Before anything can build a lightmap (see psp_lmblock_init above). */
	psp_lmblock_init ();

	/* Register the video cvars even though nothing renders yet: default.cfg
	 * sets 'gamma' and 'vid_fsaa', and an unregistered cvar turns every line
	 * of the player's config into 'Unknown command'. */
	Cvar_RegisterVariable(&vid_contrast);
	Cvar_RegisterVariable(&vid_gamma);
	Cvar_RegisterVariable(&vid_fsaa);
	Cvar_RegisterVariable(&vid_fxaa);

	/* Stage M4, step 1: the GU comes up and starts presenting frames.  What
	 * is drawn inside those frames is still nothing but a test pattern -- the
	 * GL shim that turns the engine's drawing calls into GU commands is the
	 * next step. */
	if (psp_gu_init())
		Con_Printf("Video: %dx%d, sceGu context ready (stage M4 step 1)\n",
		           vid.width, vid.height);
	else
		Con_Printf("Video: %dx%d, sceGu NOT available -- no frames will be shown\n",
		           vid.width, vid.height);
}

void VID_Shutdown (void)
{
	psp_gu_shutdown();
}

void VID_Update (vrect_t *rects)
{
	(void)rects;	/* nothing is drawn yet */
}

void VID_SyncCvars (void)
{
}

void VID_Toggle (void)
{
}

void *VID_GetWindow (void)
{
	return NULL;
}

qboolean VID_HasMouseOrInputFocus (void)
{
	return windowhasfocus;
}

qboolean VID_IsMinimized (void)
{
	return false;
}

void VID_Lock (void)
{
}

void VID_SetWindowCaption (const char *newcaption)
{
	(void)newcaption;
}

void VID_SetWindowTitle (const char *title)
{
	(void)title;
}

void VID_UpdateCursor (void)
{
}

void VID_SetCursor (qcvm_t *vm, const char *cursorname, float hotspot[2],
                    float cursorscale)
{
	(void)vm; (void)cursorname; (void)hotspot; (void)cursorscale;
}

void LoadCustomCursorImage (void)
{
}

float VID_GetCurrentDPI (void)
{
	return 96.0f;
}

void VID_ChangedRestart_f (void)
{
}

/* ---- frame hooks the renderer calls around a frame ---- */
void GL_BeginRendering (int *x, int *y, int *width, int *height)
{
	if (x) *x = 0;
	if (y) *y = 0;
	if (width) *width = vid.width;
	if (height) *height = vid.height;

	/* Opens the GU draw list and clears it.  Every drawing call the engine
	 * makes between here and GL_EndRendering() is meant to land in this
	 * list; for now the shim does not exist, so the list holds only the
	 * placeholder pattern from gu_psp.c. */
	psp_gu_frame_begin();
}

/* The two marks below name the only blocking pair in a frame: sceGuFinish()
 * and sceGuSync() inside psp_gu_frame_end().  A frame that dies there has no
 * line of its own otherwise, and a GE that never finishes looks exactly like a
 * crash from the outside.
 *
 * They are for the first PSP_FRAME_MARKS_MAX frames of the run, and how that
 * was decided is the round-81 finding.  The old gate asked
 *
 *     psp_view_frame_count () < 3        // a function in Quake/view.c
 *
 * and the round-81 log prints that mark 1112 times: the mark whose comment
 * says "first three world frames" ran in every frame of a 1500-frame session.
 * Two files, two counters, one of them in a function the other file calls --
 * and the number that came back never left zero, so the gate never shut.  Each
 * of the ~18 cards lines a frame cost a Memory Stick round trip, and the
 * heartbeat measured it: "card 23 writes 681 ms" over 39 frames of 36 ms.
 *
 * A breadcrumb gate is now data the printing file owns: a counter next to the
 * printf, incremented next to the printf.  Nothing else can leave it open. */
#define PSP_FRAME_MARKS_MAX	3

#if defined(PLATFORM_PSP) || defined(__psp__)
static int psp_frame_marks;

static int psp_frame_mark_here (void)
{
	return psp_frame_marks < PSP_FRAME_MARKS_MAX;
}
#endif

void GL_EndRendering (void)
{
	/* Closes the list, waits for the GPU and presents with vsync.
	 *
	 * The marks around psp_gu_frame_end() are the ones that name a death
	 * here: sceGuFinish() and sceGuSync() are the only two blocking calls
	 * in a frame, and a GE that never finishes looks exactly like a crash
	 * from the outside (the console stops; nothing is written).  The
	 * difference matters -- a crash is a bad pointer, a GE that never
	 * finishes is a bad command, and they are fixed in different files. */
#if defined(PLATFORM_PSP) || defined(__psp__)
	{
		extern void psp_trace_mark (const char *step);

		if (psp_frame_mark_here ())
			psp_trace_mark ("frame: close the draw list and wait for the GE");
	}
#endif

	psp_gu_frame_end();

#if defined(PLATFORM_PSP) || defined(__psp__)
	{
		extern void psp_trace_mark (const char *step);

		/* Reached means the GE finished the list and the frame was
		 * presented -- the list held nothing the hardware chokes on. */
		if (psp_frame_mark_here ())
			psp_trace_mark ("frame: presented");
	}

	/* One frame, one count, and it is counted here -- see
	 * psp_frame_mark_here(). */
	if (psp_frame_marks < PSP_FRAME_MARKS_MAX)
		psp_frame_marks++;
#endif
}
