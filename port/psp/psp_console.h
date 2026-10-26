/*
 * port/psp/psp_console.h -- text output on a PSP without a desktop terminal.
 *
 * The gameplay tests in tests/ talk to stdout with puts()/printf().  A PSP has
 * no stdout, so port/psp/Makefile compiles the test sources with
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

/* FUNC_NORETURN comes from the engine headers; this one is included from
 * port files that may not have pulled quakedef.h in yet. */
#ifndef FUNC_NORETURN
#define FUNC_NORETURN
#endif

void psp_con_init (void);
int  psp_con_puts (const char *s);
int  psp_con_printf (const char *fmt, ...);
int  psp_con_fputs (const char *s, FILE *stream);
int  psp_con_fprintf (FILE *stream, const char *fmt, ...);
int  psp_con_putchar (int c);
int  psp_con_fflush (FILE *stream);

/* A breadcrumb for the steps of long initialisation code: screen + log, with
 * the card write forced.  The last one printed is where the module died. */
/* Starts a second thread that reports, on screen and in the log, when nothing
 * has been printed for eight seconds.  A hang then names the step it hangs in,
 * the same way a crash does.  Call once, after psp_con_init(). */
void psp_con_watchdog (void);

void psp_trace (const char *step);
void psp_tracef (const char *fmt, ...);

/* Force the log out of the memory stick's cache.  Called after every line
 * already; here so a fatal path can do it explicitly. */
void psp_con_logsync (void);

/* Prints the failure, keeps it on screen and stops the module.  Newlib's
 * assert() lands here through __assert_func.  It genuinely never returns, and
 * saying so keeps Sys_Error() (declared FUNC_NORETURN in sys.h) honest. */
FUNC_NORETURN void psp_con_panic (const char *what, const char *file, int line,
                    const char *func, const char *expr);

#endif /* KABLUK_PSP_CONSOLE_H */
