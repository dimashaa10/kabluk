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
qboolean	gl_texture_NPOT = false;
qboolean	gl_texture_s3tc = false;
qboolean	gl_texture_rgtc = false;
qboolean	gl_texture_bptc = false;
qboolean	gl_texture_etc2 = false;
qboolean	gl_texture_astc = false;
qboolean	gl_texture_e5bgr9 = false;

GLint		gl_hardware_maxsize = 0;
float		gl_max_anisotropy = 0.0f;
GLint		gl_max_texture_units = 0;
GLint		gl_max_texture_image_units = 0;
GLint		gl_max_combined_texture_image_units = 0;
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

	Con_Printf("Video: %dx%d, sceGu renderer not wired up yet (stage M4)\n",
	           vid.width, vid.height);
}

void VID_Shutdown (void)
{
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
}

void GL_EndRendering (void)
{
}
