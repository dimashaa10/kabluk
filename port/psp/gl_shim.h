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

/* Hand the pending vertex batch to the GE.  Called from psp_gu_frame_end()
 * before sceGuFinish(), because a batch left in hand when the list is closed
 * is a batch that is never drawn. */
void psp_gl_shim_flush_batch (void);

/* Main-memory bytes currently held by texture uploads.  The PSP has 64 MB
 * total and the engine's zone takes 33 of them, so this is worth logging. */
size_t psp_gl_shim_texture_bytes (void);

/* Tell the shim who to ask when the picture arena cannot satisfy a request.
 *
 * The shim owns the memory the GE samples; the engine owns the pictures (it
 * knows which ones are still wanted and how to load them again).  Neither can
 * free anything useful alone: a 512x256 help page does not fit next to a
 * menu's worth of background art, and the only memory worth having at that
 * moment is the memory of pictures nobody asked for in the last few seconds.
 * The port therefore plugs its own purge in here (gu_psp.c), the shim calls
 * it once and retries the allocation. */
/* The port's cache purge, called when a picture does not fit.  Returns the
 * number of pictures it released (0 if it could not free anything). */
void psp_gl_shim_set_evict_hook (int (*fn)(void));

/* Hand the picture arena one block for the whole run.  main_psp.c measures how
 * much malloc() can still reach and takes this block out of it, so the pictures
 * do not fragment the same free space the map and the decoder need, and the
 * arena never asks malloc() for another chunk.  A pooled arena also stops
 * handing chunks back (free() of a big block is a one-way loss here). */
void psp_gl_shim_set_pool (void *base, size_t bytes);

/* The biggest free run in the picture arena, payload only: what a new picture
 * could get right now.  Read between frames by the port. */
size_t psp_gl_shim_texture_room (void);

/* Give memory back to the heap for the picture decoder.
 *
 * Uploading a picture costs 16 bits per pixel out of the arena, but *decoding*
 * one costs w*h*4 bytes out of plain malloc() -- and both come from the same
 * ten megabytes.  The hardware log of build 08:12 ends with
 *
 *     img: gfx/a_sgl/asgl3.bmp FAILED: outofmem (14166 bytes, sig 42 4D ...)
 *
 * i.e. a file that arrived whole and intact could not get its decode buffer
 * while several megabytes of pictures that no frame had drawn for a long time
 * were still held.  This calls the same purge the arena uses when a picture
 * does not fit (the port's Draw_EvictCachePics) and then hands the arena
 * chunks that became empty back to the heap, which is what makes that memory
 * usable by malloc() again.  Returns the number of pictures released. */
int psp_gl_shim_reclaim_pictures (void);

/* The texture uploaded by the NEXT glTexImage2D is a lightmap: it may use
 * memory past the picture pool's fallback cap, under a budget of its own.
 * A map is unlit without its lightmaps, and the pool is always full by the
 * time a map is entered (gl_shim.c: SHIM_PRIORITY_LIMIT). */
void psp_gl_shim_priority_texture (int on);

/* One line stating what the picture pool actually holds, for the frame card. */
void psp_gl_shim_texture_report (void);

/* 1 while the picture arena's block list is intact.  Checks for a picture that
 * was written past the block it was given -- the corruption that turns into a
 * freeze somewhere unrelated.  Cheap enough for once a second. */
int psp_gl_shim_texture_sane (void);

#endif /* GL_SHIM_H */
