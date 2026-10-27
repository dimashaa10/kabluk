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

/* The engine's picture cache is Draw_EvictCachePics() in Quake/gl_draw.c.
 * Its header comes with quakedef.h, which this file deliberately does not
 * include -- that pulls in SDL and the whole engine into a file whose job is
 * the GPU.  One prototype is cheaper. */
int Draw_EvictCachePics (int min_idle_frames);

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
/* sceGuFinish() returns the size of the list it just closed -- in WORDS, not
 * bytes (pspsdk src/gu/sceGuFinishId.c: (current - start) on a unsigned int*,
 * so 131072 words = 512 KB is not what this list is).  That number
 * matters: pspsdk's sendCommandi() (src/gu/guInternal.h) is nothing but
 *
 *      *(gu_list->current++) = (cmd << 24) | argument;
 *
 * with no bounds check anywhere.  A frame that needs more than GU_LIST_SIZE
 * words therefore writes straight past the end of gu_list into whatever the
 * linker put next -- silently, with the draw list still being submitted, so
 * the GE runs a mixture of real commands and neighbouring variables.  The
 * symptom of that is not "one picture missing", it is "the panel shows
 * nonsense that changes with unrelated code".  Measured here every frame so
 * that it can never be an unknown again. */
static int          gu_list_words;
static int          gu_list_peak;
static int          gu_list_warned;
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

/* ---- picture memory ------------------------------------------------
 *
 * The GE samples pictures out of main memory, so every picture the interface
 * has ever asked for used to stay there for the whole run: a PSP-E1008 has
 * 64 MB and no VRAM to hide it in.  Two things keep it bounded now:
 *
 *   - the shim keeps them in an arena it owns (gl_shim.c), which coalesces
 *     and reports what it holds, instead of a malloc per picture;
 *   - the engine's picture cache can give a texture back and load it again
 *     on the next request (Draw_EvictCachePics, gl_draw.c).  Nothing else
 *     knows which pictures are still wanted, so the port asks it to drop the
 *     ones that have been idle for a while, and asks again immediately when
 *     the arena cannot satisfy a new picture.
 *
 * Ten seconds of idleness is long enough that flipping between two menu
 * pages does not reload anything, and short enough that walking through a
 * menu tree reclaims the pages already left behind. */
#define GU_TEX_IDLE_FRAMES 600

/* The shim calls this from inside glTexImage2D(), i.e. WITH a draw list open,
 * when a picture does not fit.  Only pictures that were not part of this
 * frame -- nor of the one before -- may go: sceGuDrawArray() hands the GE the
 * address of the vertices and the texture, and it reads them while the CPU
 * has moved on, so freeing memory that a command already submitted points at
 * would put a different picture under the GE's feet.  Anything idle for two
 * frames is out of every submitted batch (a frame is finished and synced in
 * psp_gu_frame_end before the next one starts), so two frames is the safe
 * threshold, and the timer below is free to be much lazier. */
static void gu_pictures_wanted (void)
{
	Draw_EvictCachePics(2);
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

	/* When the picture arena is full, the shim asks this: the engine is the
	 * only side that knows which pictures the interface still wants. */
	psp_gl_shim_set_evict_hook(gu_pictures_wanted);

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

	/* The clear has to cover the WHOLE buffer, so it cannot be left to
	 * whatever scissor the previous frame happened to end with.
	 *
	 * This was a real bug: sceGuClear() is a GE command and obeys the
	 * scissor like every other command, and the scissor in force at this
	 * point is the one the last frame's last glScissor() left behind -- the
	 * engine restores it around the logo and sets it again for the status
	 * bar's running line.  Any pixel outside that rectangle was therefore
	 * never cleared: whatever had been drawn there once stayed on the panel
	 * for the rest of the run, which is exactly why the dark wedges and the
	 * stray diagonal lines did not correspond to any draw in the tracer and
	 * did not move.  The scissor is now the full panel for the clear and the
	 * shim re-applies the engine's own rectangle immediately afterwards. */
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuScissor(0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
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
	gu_list_words = sceGuFinish();          /* words, see the note above */
	sceGuSync(0, 0);
	gu_in_frame = 0;

	if (gu_list_words > gu_list_peak)
		gu_list_peak = gu_list_words;

	/* Three quarters of the list is the point to shout: past it the next
	 * frame's extra pictures are what overflows it. */
	if (!gu_list_warned && gu_list_words > GU_LIST_SIZE * 3 / 4)
	{
		gu_list_warned = 1;
		psp_tracef("gu: draw list %d of %d words (%d of %d bytes) -- too "
		           "close to the end, frames beyond this overflow it",
		           gu_list_words, GU_LIST_SIZE,
		           gu_list_words * 4, GU_LIST_SIZE * 4);
	}

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
	/* Once a second, hand the pictures nobody has asked for back to the
	 * arena.  This runs between frames, with no draw list open, which is
	 * the only safe moment: a picture that is released may be the one the
	 * GE was last told to sample, and its memory is about to be somebody
	 * else's. */
	if (gu_frames > 0 && (gu_frames % 60) == 0)
	{
		int released = Draw_EvictCachePics(GU_TEX_IDLE_FRAMES);

		if (released > 0)
			psp_tracef("tex: %d idle pictures unloaded, %d KB of pictures left",
			           released, (int)(psp_gl_shim_texture_bytes() / 1024));
	}

	if (gu_frames == 60)
		psp_tracef("gu: 60 frames presented [%d KB of textures], "
		           "draw list %d of %d words, peak %d",
		           (int)(psp_gl_shim_texture_bytes() / 1024),
		           gu_list_words, GU_LIST_SIZE, gu_list_peak);

	psp_gl_shim_end_frame();
}
