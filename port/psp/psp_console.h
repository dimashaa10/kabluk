/*
 * port/psp/psp_console.h -- text output on a PSP without a desktop terminal.
 *
 * The gameplay tests in tests/ talk to stdout with puts()/printf().  A PSP has
 * no stdout, so port/psp/Makefile compiles tests/*.c with
 *
 *     -Dputs=psp_con_puts -Dprintf=psp_con_printf ...
 *
 * which redirects those calls here, and this file writes to the debug screen
 * that PSPSDK puts on the framebuffer.  Nothing in the engine or in the tests
 * has to change for that.
 *
 * Initialisation is lazy: the first call sets the screen up, so a test can
 * start printing from the very first line of main().
 */

#ifndef KABLUK_PSP_CONSOLE_H
#define KABLUK_PSP_CONSOLE_H

#include <stdio.h>

void psp_con_init (void);
int  psp_con_puts (const char *s);
int  psp_con_printf (const char *fmt, ...);
int  psp_con_fputs (const char *s, FILE *stream);
int  psp_con_fprintf (FILE *stream, const char *fmt, ...);
int  psp_con_putchar (int c);
int  psp_con_fflush (FILE *stream);

/* Prints the failure, keeps it on screen and stops the module.  Newlib's
 * assert() lands here through __assert_func. */
void psp_con_panic (const char *what, const char *file, int line,
                    const char *func, const char *expr);

#endif /* KABLUK_PSP_CONSOLE_H */
