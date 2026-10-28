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
 * 10 MB holds what the engine asks malloc() for -- gfx.wad (~1 MB), IPLog's
 * Q_malloc(2 MB), the decoder's w*h*4 buffers (1.2 MB for 640x480), the model
 * loader's scratch and the .cfg/.rc files -- with room for the fragmentation a
 * long session creates.  The zone and the pictures do NOT come from here; see
 * psp_mem.c.
 */
PSP_HEAP_SIZE_KB(10 * 1024);
