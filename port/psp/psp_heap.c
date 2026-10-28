/*
 * port/psp/psp_heap.c -- the malloc half of the memory budget, in numbers.
 *
 * The zone is one block handed to the engine at start-up (psp_mem.c takes it
 * from the firmware partition, not from here),
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

/* Forward: the arena number (what the allocator has taken so far) is defined
 * below with the other readers, and this file's own header is what other files
 * include; see psp_heap_block_left(). */
unsigned int psp_heap_arena (void);

/* ---- the heap's real ceiling -------------------------------------------
 *
 * mallinfo's `arena` is only what the allocator has taken from sbrk SO FAR; the
 * rest of newlib's heap block is invisible in it.  So a perfectly healthy run
 * reads
 *
 *     heap: after Host_Init: in use 2166 KB, free 4 KB of 2170 KB in 1 chunks
 *     run: f 1900 ... malloc 2166/0 KB in 1 chunks, NULL 0, tex 1494 KB
 *
 * -- zero kilobytes free, while nothing at all is wrong: the allocator simply
 * had not grown into the block yet, and can, up to its end.  Three rounds of
 * this port were spent chasing numbers like that, so it is worth fixing: the
 * size of the block is not a secret.  pspsdk defines it as a variable
 * (src/user/pspmoduleinfo.h: `PSP_HEAP_SIZE_KB(x)` expands to
 * `int sce_newlib_heap_kb_size = (x)`) and _sbrk() reads exactly that variable
 * (src/libcglue/glue.c).  psp_boot.c sets it to 10 MB, so "how much can malloc
 * still reach" is block - arena, known at run time.
 *
 * Weak on purpose: this file is also linked into test EBOOTs, and a test that
 * never sizes a heap must read "unknown" rather than fail to link. */

extern int sce_newlib_heap_kb_size __attribute__((weak));

unsigned int psp_heap_block_bytes (void)
{
	if (&sce_newlib_heap_kb_size == NULL || sce_newlib_heap_kb_size <= 0)
		return 0;

	return (unsigned int)sce_newlib_heap_kb_size * 1024u;
}

/* Bytes malloc() can still get before the block is exhausted.  0 when the size
 * is not known (see above) -- callers print that as "?" and not as "0". */
unsigned int psp_heap_block_left (void)
{
	unsigned int block = psp_heap_block_bytes();
	unsigned int arena = psp_heap_arena();

	return block > arena ? block - arena : 0;
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
 * So the three allocators the engine and the shim actually use go through here
 * instead (port/psp/Makefile: -Wl,--wrap=malloc,--wrap=realloc,
 * --wrap=memalign).  ld rewrites the undefined references -- in the engine, in
 * the shim, in the model loader, in stb_image -- to `__wrap_*`, which call the
 * real one and then, if the answer is NULL, write one line to kabluk.log
 * naming the size and the breadcrumb of the caller.
 *
 * memalign() matters as much as malloc(): every picture arena chunk
 * (gl_shim.c, 256 KB-1 MB, 16-byte aligned for the GE) comes from it, so
 * "no memory for a picture" was reported by the shim's own line while the
 * reason -- which request, how big -- stayed invisible.  realloc() is what
 * stb_image grows its decode buffers with.
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
 * apart without reading the whole log.
 *
 * And it names the line that asked.  The breadcrumb ring says which file was
 * being worked on, which narrowed round 65 to "the menu, after a picture", but
 * a menu is a thousand lines of code; $ra says which of them.  __wrap_malloc()
 * is called directly by the code that calls malloc(), so
 * __builtin_return_address(0) inside it IS the caller, and the failure line
 * carries it (`at 0x08...`).  One command turns it into a function and a line:
 *
 *     C:/pspdev/bin/psp-addr2line.exe -f -e C:/.../port/psp/build/kabluk.elf 0x08...
 *
 * (The linker puts a PSP module at 0x08804000 and the loader maps it there, so
 * the address in the log is an address in the .elf.  The .elf is not stripped:
 * data.psp is made from a copy, which is why a name is still available.) */
void *__real_malloc (size_t size);
void *__real_realloc (void *p, size_t size);
void *__real_memalign (size_t align, size_t size);

static unsigned int heap_mallocs;
static unsigned int heap_malloc_fails;

/* Counting and reporting for all three, so the number on the heartbeat line is
 * "allocations this program could not have" and not "malloc() failures". */
static void *heap_note (const char *what, void *p, size_t size,
                        void *caller)
{
	heap_mallocs++;

	if (!p)
	{
		heap_malloc_fails++;

		psp_con_note_alloc_failure(what, (unsigned int)size,
		                           (unsigned int)(unsigned long)caller);
	}

	return p;
}

void *__wrap_malloc (size_t size)
{
	/* Taken before anything else runs: the address of the instruction that
	 * called malloc(), which is the caller of *this* function.  Reading it
	 * first also keeps the answer away from the call below -- the register
	 * the return address arrives in is not something to leave to argument
	 * evaluation order. */
	void *caller = __builtin_return_address(0);

	return heap_note("malloc", __real_malloc(size), size, caller);
}

void *__wrap_realloc (void *p, size_t size)
{
	void *caller = __builtin_return_address(0);
	void *q = __real_realloc(p, size);

	/* realloc(p, 0) is a free() and answers NULL: not a failure. */
	if (size == 0)
		return q;

	return heap_note("realloc", q, size, caller);
}

void *__wrap_memalign (size_t align, size_t size)
{
	void *caller = __builtin_return_address(0);

	return heap_note("memalign", __real_memalign(align, size), size, caller);
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
	psp_tracef("heap: %s: in use %d KB, %d KB left in the heap block "
	           "(allocator holds %d KB in %d chunks) [system free %d KB]",
	           where ? where : "?",
	           (int)(psp_heap_in_use() / 1024),
	           (int)(psp_heap_block_left() / 1024),
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
