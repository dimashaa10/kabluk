/*
 * port/psp/psp_heap.c -- the malloc half of the memory budget, in numbers.
 *
 * The zone is one 30 MB block handed to the engine at start-up (main_psp.c),
 * and everything else the engine allocates comes out of newlib's heap: picture
 * decoding, the GL shim's 16-bit copy of every texture, the pak directory,
 * iplog, the model loader's scratch buffers, ...
 *
 * On the console the only hint that this second pool was running out used to be
 *
 *	*** FATAL ERROR ***
 *	Not enough memory free; check disk space
 *
 * which comes from Q_malloc() in Quake/zone.c -- that is, from plain malloc() --
 * and says nothing about *which* allocation failed, how big it was, or how much
 * of the reserve was actually gone.  Worse, the "[free N KB]" that psp_trace()
 * appends to every line is sceKernelTotalFreeMemSize(), the memory of the whole
 * console: newlib takes PSP_HEAP_SIZE_KB(-1) at the first malloc() and keeps it,
 * so that number sits at a few hundred KB for the entire run and can never show
 * a heap running dry.
 *
 * newlib's own allocator already keeps exactly that score (libc/stdlib/mallocr.c,
 * struct mallinfo), so this file reads it and puts it on the log line:
 *
 *	in use    bytes currently allocated and not freed
 *	free      bytes inside the heap, in the free list
 *	arena     bytes the heap has taken from the system (in use + free)
 *	free chunks   how many pieces "free" is split into -- a heap that is
 *	              99% free in 4000 small pieces cannot satisfy one big malloc,
 *	              and that looks identical to a leak in every other number
 *
 * These numbers were dead for the whole life of the port so far: the symbols
 * were referenced weakly, a weak undefined reference does not pull an object
 * out of libc.a, and every log line therefore read "in use 0 KB, free 0 KB of
 * 0 KB in 0 chunks".  See the note at the declaration below.
 *
 * The same file also reports the main thread's stack, because the other thing
 * that kills a PSP with no message at all is a stack that ran out: the engine
 * is recursive in places (the menu, the console, the entity chain) and the
 * thread gets a fixed 512 KB (psp_boot.c).
 */

#include <stddef.h>
#include <string.h>

#include <malloc.h>
#include <sys/reent.h>	/* struct _reent and _REENT, for _mallinfo_r() */
#include <unistd.h>	/* sbrk(), the floor under the accounting */
#include <pspkernel.h>

#include "psp_console.h"

/* newlib exports the allocator's accounting as `_mallinfo_r(_REENT)` in a
 * reentrant build (newlib/libc/stdlib/mallinfor.c: `#define DEFINE_MALLINFO`
 * plus _mallocr.c), and declares it in <malloc.h>.  pspdev's newlib is built
 * reentrant (psptoolchain-allegrex: --enable-newlib-retargetable-locking
 * --enable-newlib-multithread, and no --enable-newlib-nano-malloc, so the
 * full allocator is the one that is linked).
 *
 * This used to be a WEAK reference, and that is why every line of every
 * hardware log said "in use 0 KB, free 0 KB of 0 KB in 0 chunks" and "heap 0
 * KB grown", i.e. why the port was blind to the pool the pictures live in.
 * A weak UNDEFINED reference is not a reason for the linker to pull an object
 * out of an archive, and with -ffunction-sections/--gc-sections a section
 * nothing strongly references is thrown away as well -- so both weak symbols
 * resolved to 0 for the whole run and every reader of them reported zeros.
 * The reference is now strong, which is what keeps newlib's accounting in the
 * program.
 *
 * `sbrk` stays weak: it is only the floor under the numbers, and a libc that
 * answers through mallinfo does not need it at all. */
extern struct mallinfo _mallinfo_r (struct _reent *);

static unsigned int heap_read (int field)
{
	struct mallinfo mi = _mallinfo_r(_REENT);

	switch (field)
	{
	case 0:  return (unsigned int)mi.uordblks;	/* in use */
	case 1:  return (unsigned int)mi.fordblks;	/* free inside the heap */
	case 2:  return (unsigned int)mi.arena;		/* heap grown so far */
	case 3:  return (unsigned int)mi.ordblks;	/* number of free chunks */
	default: return 0;
	}
}

/* ---- the floor under mallinfo ------------------------------------------
 *
 * The allocator asks the system for memory through sbrk(), and that answer is
 * a number nobody can silence.  It is kept as a second opinion on `arena`
 * above: the two disagreeing means the accounting is not the one being read.
 *
 *      sbrk(0)   -> the current top of the heap, a plain address
 *
 * The distance from the top recorded the first time it is read is how much the
 * heap has taken from the console since then.  newlib never gives memory back,
 * so this is a high-water mark and not a live figure -- and that is the point:
 * a number that keeps climbing frame after frame is either a leak or a heap
 * shredded into pieces too small to satisfy the next big request, and both of
 * those are invisible in every other number the console can print. */
#pragma weak sbrk

static char        *heap_base;
static unsigned int heap_sbrk_peak;

unsigned int psp_heap_sbrk (void)
{
	void *brk;
	unsigned int grown;

	if (!sbrk)
		return 0;

	brk = sbrk(0);

	if (!heap_base)
	{
		heap_base = (char *)brk;
		return 0;
	}

	grown = (unsigned int)((char *)brk - heap_base);

	if (grown > heap_sbrk_peak)
		heap_sbrk_peak = grown;

	return grown;
}

unsigned int psp_heap_sbrk_peak_bytes (void)
{
	psp_heap_sbrk();
	return heap_sbrk_peak;
}

unsigned int psp_heap_in_use (void)  { return heap_read(0); }
unsigned int psp_heap_free (void)    { return heap_read(1); }
unsigned int psp_heap_arena (void)   { return heap_read(2); }
unsigned int psp_heap_chunks (void)  { return heap_read(3); }

/* ---- malloc failures ---------------------------------------------------
 *
 * The pool this file measures is the pool the engine, the picture decoder and
 * the GL shim allocate from.  On this console it was down to
 *
 *     heap: after Host_Init: in use 34430 KB, free 9 KB of 34439 KB in 1 chunks
 *
 * and the port has no way of telling an allocation that failed there from a
 * fault: Q_malloc() prints "Not enough memory free; check disk space" through
 * Con_Printf, and half the callers of malloc() in this engine do not check the
 * answer at all -- they store through it, and the console switches off.
 *
 * So every malloc() goes through here instead (port/psp/Makefile:
 * -Wl,--wrap=malloc).  ld rewrites the undefined references to `malloc` -- in
 * the engine, in the shim, in the model loader -- to `__wrap_malloc`, which
 * calls the real one and then, if the answer is NULL, writes one line to
 * kabluk.log naming the size and the breadcrumb of the caller.
 *
 * Two things this deliberately does NOT do:
 *
 *   - it does not abort.  A failing malloc is an expected, handled event in
 *     the picture arena: shim_arena_trim() gives memory back and the request is
 *     retried (gl_shim.c), and Quake/image.c retries "outofmem" once.  Making
 *     every NULL fatal would break those paths.
 *   - it does not change what malloc() returns or how much it returns.  It is
 *     accounting only, so a run with the hook and a run without it are the same
 *     run.
 *
 * The count goes on the two-second heartbeat line next to the free bytes, so
 * "the pool was empty" and "something asked for the impossible" can be told
 * apart without reading the whole log. */
void *__real_malloc (size_t size);

static unsigned int heap_mallocs;
static unsigned int heap_malloc_fails;

/* Set while the PORT ITSELF is asking for a block it expects may not fit.
 *
 * The zone size in main_psp.c is found by trying: "does 34 MB fit?" is a
 * question, not a failure, and the first hardware log with this hook spent one
 * of its eight lines on it:
 *
 *     malloc: NULL for 41943040 bytes -- last (startup), phase (none)
 *
 * A NULL that means "the probe found the ceiling" must not look like a crash in
 * the log, or the next real NULL hides among the probes.  Everything else is
 * still reported -- and still counted, because the count is what says whether
 * the pool is being starved. */
static int heap_quiet;

void psp_heap_expect_failure (int on)
{
	heap_quiet = on ? 1 : 0;
}

void *__wrap_malloc (size_t size)
{
	void *p = __real_malloc(size);

	heap_mallocs++;

	if (!p)
	{
		heap_malloc_fails++;

		if (!heap_quiet)
			psp_con_note_alloc_failure((unsigned int)size);
	}

	return p;
}

/* Calls that went through the hook.  Kept for the heartbeat line: a number
 * that stops rising while the game still runs would mean the wrap stopped
 * working (the linker quietly linked the plain symbol), which is exactly the
 * kind of silent instrument this port has been bitten by before. */
unsigned int psp_heap_mallocs (void)     { return heap_mallocs; }
unsigned int psp_heap_malloc_fails (void) { return heap_malloc_fails; }

/* One line in kabluk.log (and on the screen while the console still owns it).
 * Called from the two-second heartbeat, from Sys_Error() paths and whenever an
 * allocation fails, so it stays cheap: no formatting of its own. */
void psp_heap_report (const char *where)
{
	psp_tracef("heap: %s: in use %d KB, free %d KB of %d KB in %d chunks [system free %d KB]",
	           where ? where : "?",
	           (int)(psp_heap_in_use() / 1024),
	           (int)(psp_heap_free() / 1024),
	           (int)(psp_heap_arena() / 1024),
	           (int)psp_heap_chunks(),
	           sceKernelTotalFreeMemSize() / 1024);
}

/* ---- the other thing that runs out: the main thread's stack ------------
 *
 * A thread stack on the PSP is a fixed block (psp_boot.c asks for
 * PSP_MAIN_THREAD_STACK_SIZE_KB(512)), there is no guard page the firmware
 * reports through, and a fault from writing past the end is a plain address
 * error: the CPU stops, no user-mode code runs, nothing reaches kabluk.log.
 * For a crash that happens "anywhere, not always in the same place", the
 * question "how much stack was left" is worth as much as the memory numbers
 * above -- and pspsdk exposes it (src/user/pspthreadman.h):
 *
 *     sceKernelGetThreadStackFreeSize(thid)
 *
 * which returns the smallest amount of stack the thread has had left, i.e.
 * a high-water mark and not a snapshot.  Measured from the watchdog thread
 * once a second, about the main thread, which is the one that draws the
 * menu. */
#include <pspthreadman.h>

static SceUID heap_main_thread = -1;	/* -1: not known yet, or no API */
static int    heap_stack_free = -1;	/* last answer, bytes */

/* Called once, from the main thread, before the frame loop starts: the thread
 * whose stack the report is about is the one that runs the engine. */
void psp_heap_note_main_thread (void)
{
	if (heap_main_thread < 0)
		heap_main_thread = sceKernelGetThreadId();
}

/* Free stack of the main thread in bytes, or -1 when the firmware will not
 * say.  Safe to call from any thread. */
int psp_heap_stack_free (void)
{
	int free_bytes;

	if (heap_main_thread < 0)
		return heap_stack_free;

	free_bytes = sceKernelGetThreadStackFreeSize(heap_main_thread);

	/* A negative answer is an error from the kernel, not a small stack:
	 * keep whatever was measured last instead of reporting -1 KB. */
	if (free_bytes >= 0)
		heap_stack_free = free_bytes;

	return heap_stack_free;
}
