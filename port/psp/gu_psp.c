/*
 * port/psp/gu_psp.c -- sceGu bring-up and the frame loop (stage M4, step 1).
 *
 * Frame buffers live in the GPU's own 2 MB of EDRAM, not in the engine's 33 MB
 * zone, and the layout is deliberately packed after the debug console:
 *
 *     0x000000 .. 0x088000   the text console (pspDebugScreen), untouched
 *     0x088000 .. 0x110000   frame buffer 0
 *     0x110000 .. 0x198000   frame buffer 1
 *     0x198000 .. 0x1DC000   depth buffer
 *
 * 512 * 272 * 4 = 0x88000 per colour buffer and 512 * 272 * 2 = 0x44000 for
 * depth, so the whole thing ends at 0x1DC000 of the 0x200000 available.  The
 * console keeps its own memory, which is why it can still be read while the
 * game runs: without that separation the console would scribble text into
 * whichever buffer happened to be showing.
 *
 * The display is only handed over to the GU at the end of the first presented
 * frame -- everything printed during Host_Init stays on screen until then.
 */

#include <stddef.h>
#include <stdint.h>

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>

#include "gu_psp.h"
#include "psp_console.h"

/* One draw list for the whole frame.  A step-1 test pattern needs a handful of
 * vertices; the GL shim will want a good deal more, so the size is a named
 * constant that is easy to raise (and, later, to allocate from the heap). */
#define GU_LIST_SIZE  (16 * 1024)
static unsigned int gu_list[GU_LIST_SIZE] __attribute__((aligned(16)));

#define GU_FB0_OFFSET     0x088000
#define GU_FB1_OFFSET     0x110000
#define GU_DEPTH_OFFSET   0x198000

static int          gu_up;
static int          gu_in_frame;
static int          gu_display_on;
static unsigned int gu_frames;
static void        *gu_draw_buffer;
static unsigned int gu_clear_color;

/* 2D vertices for the step-1 test pattern.
 *
 * The stride must be a multiple of 4: the GPU reads the vertices back to back
 * and a 10-byte struct puts every second one on an odd address, which the
 * hardware does not tolerate -- the symptom is exactly "colours and positions
 * smeared into noise".  There is no padding field for its own sake: it makes
 * the struct 12 bytes. */
typedef struct
{
	unsigned int  color;
	short         x, y, z;
	unsigned int  pad;
} gu_vertex_t;

int psp_gu_ready (void)
{
	return gu_up;
}

unsigned int psp_gu_frame_count (void)
{
	return gu_frames;
}

int psp_gu_init (void)
{
	uintptr_t edram;

	if (gu_up)
		return 1;

	edram = (uintptr_t)sceGeEdramGetAddr();
	if (!edram)
		return 0;

	sceGuInit();

	sceGuStart(GU_DIRECT, gu_list);

	/* Draw into buffer 0, and show buffer 0 once the display is enabled. */
	gu_draw_buffer = (void *)(edram + GU_FB0_OFFSET);
	sceGuDrawBuffer(GU_PSM_8888, gu_draw_buffer, PSP_GU_STRIDE);
	sceGuDispBuffer(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT,
	                (void *)(edram + GU_FB0_OFFSET), PSP_GU_STRIDE);
	sceGuDepthBuffer((void *)(edram + GU_DEPTH_OFFSET), PSP_GU_STRIDE);

	/* The GU draws into a 4096x4096 coordinate space with the origin in the
	 * middle; these two calls centre a 480x272 window in it. */
	sceGuOffset(2048 - (PSP_SCREEN_WIDTH / 2), 2048 - (PSP_SCREEN_HEIGHT / 2));
	sceGuViewport(2048, 2048, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	sceGuDepthRange(65535, 0);

	sceGuScissor(0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	sceGuEnable(GU_SCISSOR_TEST);

	sceGuFrontFace(GU_CW);
	sceGuDisable(GU_CULL_FACE);
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_BLEND);
	sceGuDisable(GU_LIGHTING);

	sceGuClearColor(0xFF101820);
	sceGuClearDepth(0);

	sceGuFinish();
	sceGuSync(0, 0);

	/* The display stays with the debug console until the first real frame. */
	gu_display_on = 0;
	gu_clear_color = 0xFF101820;
	gu_frames = 0;
	gu_in_frame = 0;
	gu_up = 1;
	return 1;
}

void psp_gu_shutdown (void)
{
	if (!gu_up)
		return;

	if (gu_in_frame)
	{
		sceGuFinish();
		sceGuSync(0, 0);
		gu_in_frame = 0;
	}

	sceGuDisplay(GU_FALSE);
	sceGuTerm();
	gu_display_on = 0;
	gu_up = 0;
}

/* A slow colour breathing from navy to teal: if the screen is not a flat dead
 * panel any more and the tint moves, then the list, the clear, the swap and
 * the vsync wait are all working.  Replaced by the GL shim in step 2. */
static unsigned int psp_gu_test_clear_color (unsigned int frame)
{
	static const unsigned char navy[3] = { 0x10, 0x18, 0x20 };
	static const unsigned char teal[3] = { 0x12, 0x42, 0x3C };
	unsigned int phase = (frame / 4) & 63;
	unsigned int mix = (phase < 32) ? phase * 2 : (64 - phase) * 2;
	unsigned char r = (unsigned char)(navy[0] + ((teal[0] - navy[0]) * (int)mix) / 64);
	unsigned char g = (unsigned char)(navy[1] + ((teal[1] - navy[1]) * (int)mix) / 64);
	unsigned char b = (unsigned char)(navy[2] + ((teal[2] - navy[2]) * (int)mix) / 64);

	return 0xFF000000u | ((unsigned int)r << 16) | ((unsigned int)g << 8) | b;
}

/* The moving bar: 40x40 squares marching across the middle of the screen.
 * Pure geometry, no textures, so it says nothing about the texture path --
 * that is what the shim will be for. */
static void psp_gu_test_pattern (unsigned int frame)
{
	gu_vertex_t *v;

	sceGuDisable(GU_TEXTURE_2D);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_BLEND);

	v = (gu_vertex_t *)sceGuGetMemory(sizeof(gu_vertex_t) * 2 * 6);

	/* Two quads: a fixed frame-coloured band and the moving marker. */
	v[0].color = 0xFF2E4A57; v[0].x = 0;   v[0].y = 116; v[0].z = 0;
	v[1].color = 0xFF2E4A57; v[1].x = 480; v[1].y = 156; v[1].z = 0;
	v[2].color = 0xFFFFFFFF; v[2].x = (short)((frame * 3) % 480);
	v[2].y = 116; v[2].z = 0;
	v[3].color = 0xFFFFB030; v[3].x = (short)(((frame * 3) % 480) + 24);
	v[3].y = 156; v[3].z = 0;

	sceGuDrawArray(GU_SPRITES,
	               GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
	               4, NULL, v);
}

void psp_gu_frame_begin (void)
{
	if (!gu_up || gu_in_frame)
		return;

	sceGuStart(GU_DIRECT, gu_list);
	gu_in_frame = 1;

	/* sceGuSwapBuffers() returns the buffer that is now free to draw into;
	 * the list has to be told again every frame. */
	sceGuDrawBuffer(GU_PSM_8888, gu_draw_buffer, PSP_GU_STRIDE);

	gu_clear_color = psp_gu_test_clear_color(gu_frames);
	sceGuClearColor(gu_clear_color);
	sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

	psp_gu_test_pattern(gu_frames);
}

void psp_gu_frame_end (void)
{
	if (!gu_up || !gu_in_frame)
		return;

	sceGuFinish();
	sceGuSync(0, 0);
	gu_in_frame = 0;

	/* Wait for the panel before flipping, not after: this is what keeps the
	 * frame rate at 60 and stops a half-drawn buffer from being shown. */
	sceDisplayWaitVblankStart();
	gu_draw_buffer = sceGuSwapBuffers();
	gu_frames++;

	/* First presented frame: take the screen over from the debug console.
	 * The console draws with the same GPU, so it has to stop here -- both its
	 * characters and the game's triangles would share one command list and
	 * one framebuffer.  Its output continues in kabluk.log. */
	if (!gu_display_on)
	{
		psp_con_screen_off();
		sceGuDisplay(GU_TRUE);
		gu_display_on = 1;
	}
}
