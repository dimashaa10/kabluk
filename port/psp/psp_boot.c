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
 * The size is 14 MB, and the number below says why it is not the 10 MB this
 * started at.
 *
 * The round-71 log ended with the heap full at the exact moment the map
 * started, and with three malloc()s returning NULL:
 *
 *     run: f 565 state 2, malloc 10234 KB used, 4 KB left, NULL 5
 *     malloc: NULL for 6029312 / 57344 / 851968 bytes -- last gfx/conback2.lmp
 *
 * while the engine's own hunk was healthy (8280 KB used of 19456, 11 MB
 * free) -- so this was never a shortage of memory, it was the wrong split.
 * The 5.75 MB request is the size of one thing being loaded at map start
 * (a file image or a decoded picture); it cannot fit in what is left of a
 * 10 MB heap that already holds the .cfg/.rc files, progs.dat, the client's
 * picture decoder buffers and every scratch copy the texture manager makes.
 *
 * The four megabytes come out of the zone, which psp_mem.c sizes from what is
 * left after this block: it goes from 19 MB to 15 MB, and the hunk still had
 * 7 MB free after the whole map was loaded.  The picture pool (6 MB) is
 * untouched -- it is a separate block and shrinking it is what broke the
 * menus in earlier rounds. */
PSP_HEAP_SIZE_KB(14 * 1024);
