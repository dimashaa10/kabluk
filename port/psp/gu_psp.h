/*
 * port/psp/gu_psp.h -- the sceGu side of the PSP video layer (stage M4).
 *
 * This is the frame plumbing only: bring the GU up, hand out a display list
 * per frame, clear, present with vsync.  What gets drawn inside the list is
 * the GL->sceGu shim's business (port/psp/gl_shim/), which lands in the next
 * step.
 *
 * Everything here is safe to call before psp_gu_init() has succeeded: the
 * functions either do nothing or return 0, so the engine can boot on a machine
 * (or an emulator) where the GPU is not there.
 */
#ifndef GU_PSP_H
#define GU_PSP_H

/* Screen geometry, in pixels.  The PSP-E1008 panel is 480x272. */
#define PSP_SCREEN_WIDTH   480
#define PSP_SCREEN_HEIGHT  272

/* Frame buffer stride, in PIXELS (the GU wants it in pixels, a multiple of
 * 64, and 480 is not one): 512 pixels * 4 bytes = 0x800 bytes per row. */
#define PSP_GU_STRIDE      512

/* Returns 1 once the GU is up and frames can be submitted. */
int  psp_gu_ready (void);

/* Bring the GU up and describe the framebuffers.  It does *not* take the
 * screen away from the debug console: the swap chain is only shown at the end
 * of the first presented frame, so the boot log stays visible until the engine
 * actually starts drawing. */
int  psp_gu_init (void);

/* Release the GU.  Safe to call twice, and safe before psp_gu_init(). */
void psp_gu_shutdown (void);

/* Start a frame: opens a draw list and clears the colour and depth buffers. */
void psp_gu_frame_begin (void);

/* Finish a frame: closes the list, waits for the GPU, then presents and waits
 * for the vertical blank so the frame rate is locked to the panel. */
void psp_gu_frame_end (void);

/* Frames presented so far -- used for the log line and for the step-1 test
 * pattern, which moves. */
unsigned int psp_gu_frame_count (void);

/* Average frame time since the last call, in milliseconds: the engine's own
 * drawing, the wait for the GE, the wait for the panel, and the whole frame.
 * Returns the number of frames averaged, or 0 when none were drawn.  See the
 * note above the timers in gu_psp.c for why these are three numbers and not
 * one. */
int psp_gu_timing_take (double *cpu_ms, double *ge_ms, double *vb_ms,
                        double *all_ms);

#endif /* GU_PSP_H */
