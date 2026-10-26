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
PSP_HEAP_SIZE_KB(2048);
