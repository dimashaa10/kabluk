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
PSP_MAIN_THREAD_STACK_SIZE_KB(256);
/*
 * The newlib heap.  main_psp.c hands the engine one big block, and
 * PSP_HEAP_SIZE_KB is the ceiling malloc() can reach, so this has to be the
 * largest number the module can afford -- with 2048 the first malloc(24 MB)
 * failed and the engine died with "Not enough memory free".
 *
 * 20 MB: a PSP-E1000 has 64 MB of RAM in total, but homebrew gets the 32 MB
 * main partition by default (the second half needs a request through
 * PARAM.SFO), and of that the system, the module itself and 2 MB of VRAM take
 * their share.  The heap is claimed at startup, so if this is ever too big the
 * module refuses to boot instead of failing later -- which is at least an
 * honest failure.
 */
PSP_HEAP_SIZE_KB(20480);
