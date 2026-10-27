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
#include <pspkernel.h>

#include "psp_console.h"

/* Weak, because mallocr.o may or may not export it; a missing symbol must not
 * stop the EBOOT from linking.  (MALLOC_PROVIDED / a libc built without the
 * SVID/XPG mallinfo would be such a case.) */
extern struct mallinfo mallinfo (void) __attribute__((weak));

static unsigned int heap_read (int field)
{
	struct mallinfo mi;

	if (!mallinfo)
		return 0;

	memset(&mi, 0, sizeof(mi));
	mi = mallinfo();

	switch (field)
	{
	case 0:  return (unsigned int)mi.uordblks;	/* in use */
	case 1:  return (unsigned int)mi.fordblks;	/* free inside the heap */
	case 2:  return (unsigned int)mi.arena;		/* heap grown so far */
	case 3:  return (unsigned int)mi.ordblks;	/* number of free chunks */
	default: return 0;
	}
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
	psp_tracef("heap: %s: in use %d KB, free %d KB of %d KB in %d chunks [system free %d KB]",
	           where ? where : "?",
	           (int)(psp_heap_in_use() / 1024),
	           (int)(psp_heap_free() / 1024),
	           (int)(psp_heap_arena() / 1024),
	           (int)psp_heap_chunks(),
	           sceKernelTotalFreeMemSize() / 1024);
}
