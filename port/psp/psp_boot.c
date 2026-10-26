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
 * The size is "as much as the kernel will give": with MEMSIZE=1 in PARAM.SFO
 * the user partition is around 52 MB on a 2000/3000/E1000, without it 24 MB,
 * and a hard-coded number would either waste one case or refuse to boot in the
 * other.
 *
 * A negative size is pspsdk's documented way of saying "whatever is left":
 * libcglue/glue.c, __psp_sbrk(), casts the value to int and, when it comes out
 * negative, sizes the block as sceKernelMaxFreeMemSize() minus the 512 KB
 * threshold it keeps for the rest of the SDK.  main_psp.c then finds the real
 * figure by trying.
 */
PSP_HEAP_SIZE_KB(-1);
