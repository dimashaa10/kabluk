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
 * THE ADDRESSES ARE OFFSETS FROM THE START OF EDRAM, NOT POINTERS.  This is
 * the one thing worth remembering about the GU: sceGuDrawBuffer(),
 * sceGuDispBuffer() and sceGuDepthBuffer() all take a byte offset into the
 * GPU's memory (what guGetStaticVramBuffer() hands back), while
 * sceGuTexImage() takes a real pointer (sceGeEdramGetAddr() + offset).
 * Adding the EDRAM base to a framebuffer offset -- which this file did at
 * first -- makes the GE read a framebuffer that does not exist and makes
 * sceDisplaySetFrameBuf() point at an unmapped address: the panel then shows
 * whatever is in that memory, i.e. rainbow noise across the whole screen.
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

#include "gl_shim.h"
#include "gu_psp.h"
#include "psp_console.h"

/* One draw list for the whole frame.  A step-1 test pattern needs a handful of
 * vertices; the GL shim will want a good deal more, so the size is a named
 * constant that is easy to raise (and, later, to allocate from the heap). */
#define GU_LIST_SIZE  (32 * 1024)
static unsigned int gu_list[GU_LIST_SIZE] __attribute__((aligned(16)));

/* sceGuDrawBuffer()/DispBuffer()/DepthBuffer() take byte offsets from the
 * start of EDRAM, so this is a cast to the void* they want -- NOT an address.
 * Adding the EDRAM base here is the bug described at the top of the file. */
#define GU_OFFSET(off)    ((void *)(uintptr_t)(off))

#define GU_FB0_OFFSET     0x088000
#define GU_FB1_OFFSET     0x110000
#define GU_DEPTH_OFFSET   0x198000

static int          gu_up;
static int          gu_in_frame;
static int          gu_display_on;
static unsigned int gu_frames;
static unsigned int gu_draw_buffer;	/* EDRAM offset of the buffer to draw into */
static unsigned int gu_disp_buffer = GU_FB1_OFFSET;	/* ...and of the one on screen */
static unsigned int gu_clear_color = 0xFF101820u;

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
	if (gu_up)
		return 1;

	/* Only a sanity check: everything below is an offset, not an address. */
	if (!sceGeEdramGetAddr())
		return 0;

	sceGuInit();

	sceGuStart(GU_DIRECT, gu_list);

	/* Draw into buffer 0 and present buffer 1: the first swap then presents
	 * exactly the frame that was just drawn, and from then on they alternate. */
	gu_draw_buffer = GU_FB0_OFFSET;
	sceGuDrawBuffer(GU_PSM_8888, GU_OFFSET(gu_draw_buffer), PSP_GU_STRIDE);
	sceGuDispBuffer(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT,
	                GU_OFFSET(GU_FB1_OFFSET), PSP_GU_STRIDE);
	sceGuDepthBuffer(GU_OFFSET(GU_DEPTH_OFFSET), PSP_GU_STRIDE);

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

/* Stage M4 step 2: the GL shim (gl_shim.c) draws the real interface, so the
 * step-1 test pattern is gone.  What is left here is the frame plumbing. */

void psp_gu_frame_begin (void)
{
	if (!gu_up || gu_in_frame)
		return;

	sceGuStart(GU_DIRECT, gu_list);
	gu_in_frame = 1;

	/* From here until sceGuFinish() there is a live draw list, and the GL
	 * shim may send GE commands; before this line it may only record. */
	psp_gl_shim_set_frame_active(1);

	/* sceGuSwapBuffers() only swaps its own bookkeeping and the display
	 * pointer -- it sends no GE command (see src/gu/sceGuSwapBuffers.c), so
	 * the new draw buffer has to be announced in every frame's list. */
	sceGuDrawBuffer(GU_PSM_8888, GU_OFFSET(gu_draw_buffer), PSP_GU_STRIDE);

	sceGuClearColor(gu_clear_color);
	sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);

	/* Everything the engine asked for with glEnable()/glBlendFunc()/...
	 * during Host_Init and in earlier frames is recorded by the shim and
	 * sent to the GE here, now that there is a draw list to send it into. */
	psp_gl_shim_begin_frame();
}

void psp_gu_frame_end (void)
{
	if (!gu_up || !gu_in_frame)
		return;

	/* The shim's reference card goes in last, so that nothing the engine
	 * draws can cover it: it is the picture that says which part of the 2D
	 * translation is wrong (see the note in gl_shim.c). */
	psp_gl_shim_test_card();

	psp_gl_shim_set_frame_active(0);
	sceGuFinish();
	sceGuSync(0, 0);
	gu_in_frame = 0;

	/* Wait for the panel before flipping, not after: this is what keeps the
	 * frame rate at 60 and stops a half-drawn buffer from being shown. */
	sceDisplayWaitVblankStart();
	gu_disp_buffer = gu_draw_buffer;		/* what we just drew becomes visible */
	gu_draw_buffer = (unsigned int)(uintptr_t)sceGuSwapBuffers();
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

		psp_tracef("gu: first frame presented, disp %06X draw %06X",
		           gu_disp_buffer, gu_draw_buffer);
	}
	else if (gu_frames == 60)
		psp_tracef("gu: 60 frames presented [%d KB of textures]",
		           (int)(psp_gl_shim_texture_bytes() / 1024));

	psp_gl_shim_end_frame();
}
