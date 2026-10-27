/*
 * port/psp/gl_shim.h -- public edge of the GL -> sceGu shim.
 *
 * The shim implements the OpenGL entry points the engine calls; this header is
 * only for the two things the video backend needs from it.
 */
#ifndef GL_SHIM_H
#define GL_SHIM_H

#include <stddef.h>

/* Re-apply the recorded pipeline state.
 *
 * The engine changes GL state (glEnable, glBlendFunc, glAlphaFunc, ...) both
 * inside a frame and, during Host_Init, long before there is a frame at all.
 * Sending GU commands outside a draw list writes into no list at all, so the
 * shim records the state and applies it here, at the start of every frame.
 * Called from psp_gu_frame_begin(). */
void psp_gl_shim_begin_frame (void);

/* Tell the shim whether gu_psp.c currently has a draw list open.  The engine
 * is free to call glEnable()/glBlendFunc()/... at any time, including before
 * the first frame; those calls may only record state.  Set to 1 right after
 * sceGuStart() and back to 0 right after sceGuFinish(). */
void psp_gl_shim_set_frame_active (int active);

/* Called from psp_gu_frame_end(), after the frame has been presented: prints
 * what the 2D path actually drew, for the first frames and then once a
 * second.  An empty frame and a frame full of untextured rectangles look the
 * same in kabluk.log without this line. */
void psp_gl_shim_end_frame (void);

/* Draw the reference card: three 64x64 squares in the top-left corner whose
 * correct appearance is described in gl_shim.c.  Called from
 * psp_gu_frame_end() so it lands on top of whatever the engine drew, and only
 * while a draw list is open. */
void psp_gl_shim_test_card (void);

/* Main-memory bytes currently held by texture uploads.  The PSP has 64 MB
 * total and the engine's zone takes 33 of them, so this is worth logging. */
size_t psp_gl_shim_texture_bytes (void);

#endif /* GL_SHIM_H */
