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
 * mallinfo() is referenced weakly on purpose: an SDK whose libc does not carry
 * it still links, and the numbers simply come out as zero.
 */

#include <stddef.h>
#include <string.h>

#include <malloc.h>
#include <sys/reent.h>	/* struct _reent and _REENT, for _mallinfo_r() */
#include <unistd.h>	/* sbrk(), the floor under the accounting */
#include <pspkernel.h>

#include "psp_console.h"

/* newlib exports the allocator's accounting under two names: mallinfo() when
 * the library was built for a single thread, and `_mallinfo_r(_REENT)` when it
 * was built reentrant -- which is what pspdev's newlib is.  The first build of
 * this file only asked for mallinfo(), the symbol was not there, and the
 * console printed "in use 0 KB, free 0 KB of 0 KB in 0 chunks" for the whole
 * run.  Both are referenced weakly so that neither the reentrant nor the
 * single-threaded libc can break the link. */
extern struct mallinfo mallinfo (void) __attribute__((weak));
extern struct mallinfo _mallinfo_r (struct _reent *) __attribute__((weak));

static unsigned int heap_read (int field)
{
	struct mallinfo mi;

	memset(&mi, 0, sizeof(mi));

	if (mallinfo)
		mi = mallinfo();
	else if (_mallinfo_r)
		mi = _mallinfo_r(_REENT);
	else
		return 0;

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
 * pspdev's newlib carries no mallinfo, and without it every number above is a
 * zero -- which is what the first hardware run printed for its whole length
 * ("malloc 0 KB in use + 0 KB free in 0 chunks"), leaving a hard stop in the
 * middle of the menu with nothing to blame.  The allocator still asks the
 * system for memory through sbrk(), and that answer is always there:
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

/* One line in kabluk.log (and on the screen while the console still owns it).
 * Called from the two-second heartbeat, from Sys_Error() paths and whenever an
 * allocation fails, so it stays cheap: no formatting of its own. */
void psp_heap_report (const char *where)
{
	if (!mallinfo && !_mallinfo_r)
	{
		/* Neither name exists: say so instead of printing four zeros that
		 * look like an empty heap, and give the one number that is always
		 * available -- how far the heap has grown since the first read. */
		psp_tracef("heap: %s: no mallinfo in this libc, heap grown %d KB "
		           "since start [system free %d KB]",
		           where ? where : "?",
		           (int)(psp_heap_sbrk_peak_bytes() / 1024),
		           sceKernelTotalFreeMemSize() / 1024);
		return;
	}

	psp_tracef("heap: %s: in use %d KB, free %d KB of %d KB in %d chunks [system free %d KB]",
	           where ? where : "?",
	           (int)(psp_heap_in_use() / 1024),
	           (int)(psp_heap_free() / 1024),
	           (int)(psp_heap_arena() / 1024),
	           (int)psp_heap_chunks(),
	           sceKernelTotalFreeMemSize() / 1024);
}
