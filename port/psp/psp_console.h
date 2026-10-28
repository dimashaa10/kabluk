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

/* Records a name for the watchdog without printing or logging anything.  For
 * loops that run many times (every picture, every texture): the watchdog prints
 * the last mark when nothing has moved for eight seconds. */
void psp_mark (const char *what);

/* One line for an allocation that returned NULL (malloc, realloc, memalign:
 * `what` names which).  The size and the breadcrumb of whoever asked.  Called from the allocator hook in psp_heap.c, before the
 * caller can turn the NULL into a fault.  Never more than eight lines. */
void psp_con_note_alloc_failure (const char *what, unsigned int size);

/* Called once the frame loop starts: stops writing marks to the card.  The
 * boot trail is what matters, and the drawing code emits a mark per picture.
 * The watchdog still reports the last mark, and the last dozen of them, when
 * the engine stops making progress. */
void psp_con_boot_done (void);

/* Which part of the frame the engine is in ("gu: waiting for the GE", "host
 * frame", ...).  The breadcrumbs name a picture; this says whether the game
 * was drawing it or waiting for the GPU to finish, which are two very
 * different bugs.  Cheap: one string copy. */
void psp_phase (const char *what);

/* Hand the screen to the game.  pspDebugScreen* draws with the same GPU the
 * game uses, so from here on console text goes to kabluk.log only, and a
 * frame is never polluted with stray console triangles. */
void psp_con_screen_off (void);

/* One diagnostic line, formatted: screen plus the log buffer.  NOT a card write
 * -- the watchdog drains the batch once a second (see psp_tracef in
 * psp_console.c).  For anything that can fire many times inside one frame. */
void psp_tracef (const char *fmt, ...);

/* While this is on, every traced line and every mark goes to the card
 * immediately instead of waiting for the watchdog's once-a-second drain.  For
 * the one place the console dies without a message: see the note in
 * psp_console.c.  main_psp.c turns it on while key_dest is the menu. */
void psp_con_trace_sync (int on);

/* Force the log out of the memory stick's cache.  Called after every line
 * already; here so a fatal path can do it explicitly. */
void psp_con_logsync (void);

/* Prints the failure, keeps it on screen and stops the module.  Newlib's
 * assert() lands here through __assert_func.  It genuinely never returns, and
 * saying so keeps Sys_Error() (declared FUNC_NORETURN in sys.h) honest. */
FUNC_NORETURN void psp_con_panic (const char *what, const char *file, int line,
                    const char *func, const char *expr);

#endif /* KABLUK_PSP_CONSOLE_H */
