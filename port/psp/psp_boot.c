/*
 * port/psp/psp_boot.c -- module header for every PSP binary built here.
 *
 * PSP_MODULE_INFO has to live in exactly one translation unit of the module;
 * keeping it in its own file means the engine tests do not need to know
 * anything about the PSP (they keep their plain `int main(void)`).
 */

#include <pspkernel.h>

PSP_MODULE_INFO("kabluk", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
/* 512 KB, not 256: Host_Init descends through printf, the filesystem and the
 * model loader, and each of those keeps buffers on the stack.  A stack
 * overflow on the PSP does not say "stack overflow" -- it takes the screen. */
PSP_MAIN_THREAD_STACK_SIZE_KB(512);
/*
 * The newlib heap -- and it is a FIXED size, on purpose.
 *
 * pspsdk src/user/pspmoduleinfo.h:
 *
 *      #define PSP_HEAP_SIZE_KB(size_kb)  int sce_newlib_heap_kb_size = (size_kb)
 *
 * and src/libcglue/glue.c, _sbrk(): the first malloc() allocates exactly that
 * many kilobytes from PSP_MEMORY_PARTITION_USER and never asks for more.  So
 * this number IS the ceiling of everything the engine malloc()s, and the rest
 * of the partition is free for the two blocks psp_mem.c takes.
 *
 * It used to be -1, which glue.c reads as "sceKernelMaxFreeMemSize() minus
 * 512 KB" -- i.e. the heap took the whole partition at the first malloc (before
 * main()), and every later attempt to size anything from what was left read a
 * flat ~750 KB.  That is how three builds in a row were lost.  A fixed number
 * is the whole point of having a memory layout: nothing else may grow into this
 * region, and this may not grow into anything else.
 *
 * The size is 10 MB, and the two builds around it are what says so.
 *
 * The 14 MB build of round 71 removed the heap failures -- and the very next
 * log refused to load the map:
 *
 *     mem: zone 15360 KB, pictures 6144 KB, newlib heap 0 KB used of 14336 KB
 *     edicts: 8582 of 1160 bytes = 9721 KB
 *     hunk: Cache_Alloc 3536192 bytes does not fit: 15360 KB total,
 *           15030 KB used, 329 KB free
 *     *** FATAL ERROR *** Cache_TryAlloc: 3536192 is greater then free hunk
 *
 * The console has ONE user partition, about 37 MB of it, and this block, the
 * picture pool and the engine's zone are three slices of it:
 *
 *     pictures 6144 KB + heap 14336 KB + zone 15360 KB = 35840 KB
 *     pictures 6144 KB + heap 10240 KB + zone 19456 KB = 35840 KB
 *
 * Every megabyte given to malloc() is a megabyte the zone does not have, and
 * the zone is where the MAP lives: loading the world takes 8280 KB of it, and
 * an alias model needs its own size twice over (built in the low hunk, then
 * copied into the cache -- Cache_Alloc(total), gl_model.c).  That is what the
 * 3.5 MB request above is, and it needs a zone of at least 18.6 MB.  So the
 * heap goes back to 10 MB and the 4 MB returns to the zone.
 *
 * The three failures the 14 MB heap was meant to paper over are fixed where
 * they come from instead: the entity array was sized from the heap and took
 * 9.7 MB of it (Quake/pr_edict.c, PSP_MAX_EDICTS), the alias mesh builder was
 * building a vertex buffer for a GL API this port does not have (Quake/
 * gl_mesh.c), and the picture decoder's retries are what the 832 KB and 56 KB
 * requests were.  The picture pool (6 MB) is untouched: shrinking it is what
 * broke the menus in earlier rounds. */
PSP_HEAP_SIZE_KB(10 * 1024);
