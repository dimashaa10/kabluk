/*
 * port/psp/psp_console.c -- stdout replacement and assert handler for PSP.
 *
 * Line buffered on purpose: the debug screen is a fixed grid of characters,
 * and writing one character at a time through pspDebugScreenPutChar (a VRAM
 * poke each) is both slow and easy to get out of sync.  We collect a line and
 * emit it with a single pspDebugScreenPrintf, scrolling by clearing the screen
 * once the cursor reaches the bottom row.
 */

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "psp_console.h"

#define CON_COLS 68   /* characters per line on the 480x272 debug font */
#define CON_ROWS 33   /* visible text rows */

static int   con_ready;
static int   con_row;
static const char *con_log_path;	/* kabluk.log, once we know it can live there */
static int   con_log_err;	/* last failing sceIo call, 0 while writes work */
static int   con_log_err_said;	/* the error has been put on the screen once */
static char  con_line[CON_COLS + 1];
static int   con_len;

/* Watchdog state: bumped by every finished line, read by the watchdog thread. */
static volatile unsigned con_progress;
static char  con_last_step[64] = "(startup)";

static void con_ensure (void)
{
	if (con_ready)
		return;

	pspDebugScreenInit();
	pspDebugScreenClear();
	con_ready = 1;
	con_row = 0;
	con_len = 0;
	con_line[0] = '\0';
}

/*
 * The log has to survive a module that dies without warning, and that rules out
 * keeping a file open.
 *
 * sceIoWrite() only hands the bytes to the memory stick's FAT cache; the cache
 * reaches the card on close(), or later, or never if the process is killed by
 * an exception.  A long-lived descriptor therefore produced a kabluk.log that
 * existed and was zero bytes long -- created once, written to, and no content
 * ever reaching the card.  sceIoSync() was supposed to force it out and did not.
 *
 * So every line is written the durable way: open (append) -- write -- close.
 * close() is the operation the driver cannot skip, so the bytes are on the card
 * before we print the next line.  A handful of card operations per console line
 * is cheap next to losing the only evidence there is.
 */
static void con_log_sync (void)
{
	if (con_log_path)
		sceIoSync("ms0:", 0);
}

static int con_log_try_open (void);
static void con_write (const char *s);
static void con_putc (int c);

static void con_logfmt (const char *s)
{
	SceUID fd;
	int n;
	size_t len;

	if (!con_log_path || !s)
		return;

	len = strlen(s);
	if (!len)
		return;

	fd = sceIoOpen(con_log_path,
	               PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if (fd < 0)
	{
		con_log_err = (int)fd;
		return;
	}

	n = sceIoWrite(fd, s, (unsigned int)len);
	sceIoClose(fd);

	if (n < 0)
		con_log_err = n;
}

/* Give the log a few more chances: psp_con_init() runs before the memory
 * stick is necessarily ready, and a card that answers on the second try is
 * worth more than a clean error message. */
#define CON_LOG_OPEN_TRIES 100

static int con_log_tries;

static void con_flush (void)
{
	if (!con_len)
		return;

	if (!con_log_path && con_log_tries < CON_LOG_OPEN_TRIES)
	{
		con_log_tries++;
		con_log_try_open();
	}

	con_line[con_len] = '\0';

	if (con_row >= CON_ROWS) {
		pspDebugScreenClear();
		con_row = 0;
	}

	pspDebugScreenSetXY(0, con_row);
	pspDebugScreenPrintf("%s\n", con_line);
	con_row++;
	con_len = 0;
	con_progress++;

	con_logfmt(con_line);
	con_logfmt("\n");

	/* If a card write failed, say so on the screen -- once, so a broken card
	 * does not turn into an endless stream of messages.  Without this the log
	 * would just stay empty and look like a code problem. */
	if (con_log_err && !con_log_err_said)
	{
		char note[64];

		con_log_err_said = 1;
		snprintf(note, sizeof(note), "log: card write error %d", con_log_err);
		con_write(note);
		con_putc('\n');
	}
}

static void con_putc (int c)
{
	con_ensure();

	if (c == '\n') {
		con_flush();
		return;
	}
	if (c == '\r')
		return;
	if (c == '\t') {
		do
			con_putc(' ');
		while (con_len % 8);
		return;
	}

	con_line[con_len++] = (char)c;

	if (con_len >= CON_COLS)
		con_flush();
}

static void con_write (const char *s)
{
	while (s && *s)
		con_putc((unsigned char)*s++);
}

/*
 * There is deliberately no exception handler here.
 *
 * pspDebugInstallErrorHandler() lives in libpspdebug's exception.o, and that
 * object calls sceKernelRegisterDefaultExceptionHandler(), which is part of
 * ExceptionManagerForKernel -- a kernel library.  Linking it from a user-mode
 * EBOOT is the same mistake that produced 8002013C, only this time the linker
 * says it plainly ("undefined reference", because we no longer link
 * libpspkernel to provide the kernel stub).
 *
 * A crash therefore still takes the screen with it.  What survives is
 * kabluk.log: every console line is written to it through sceIo as it is
 * printed, so the last line before a fault says where the engine got to.
 * The register dump can come back later, either through psplink (which runs
 * the module under a host-side debugger) or by moving the handler into a
 * kernel PRX, not by pulling a kernel library into the game.
 */

/*
 * The relative path depends on the working directory the firmware happens to
 * set, which is not something to rely on when the whole point is to read this
 * file after a crash.  Try the usual install folder, then wherever we are,
 * then the root of the stick.  The first one that opens wins.
 *
 * Called again from con_flush() while nothing is open: the first attempt runs
 * very early, and a memory stick that is not ready yet must not cost us the
 * whole log.
 */
static int con_log_try_open (void)
{
	static const char *const paths[] = {
		"ms0:/PSP/GAME/kabluk/kabluk.log",
		"ms0:/PSP/GAME/KABLUK/kabluk.log",
		"kabluk.log",
		"ms0:/kabluk.log"
	};
	unsigned int i;

	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
	{
		SceUID fd = sceIoOpen(paths[i],
		                      PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);

		if (fd >= 0)
		{
			/* Empty the file once, here, and keep no descriptor: from now
			 * on every line is appended and closed. */
			sceIoClose(fd);
			con_log_path = paths[i];
			return 1;
		}

		con_log_err = (int)fd;
	}

	return 0;
}

void psp_con_init (void)
{
	static int said_where;

	con_ensure();

	if (!con_log_path)
		con_log_try_open();

	/* Say where the log went -- or that there is none.  Without this line,
	 * "the console died and there is no file anywhere" is indistinguishable
	 * from "the file is in a folder you did not look in".
	 *
	 * This one goes through con_write() on purpose: it is the first line
	 * that has to reach the card, so it doubles as the proof that the write
	 * path works. */
	if (!said_where)
	{
		char buf[128];
		SceIoStat st;

		said_where = 1;

		con_write("log: ");
		con_write(con_log_path ? con_log_path : "NONE - crash output will be lost");
		con_putc('\n');

		/* Proof, on the screen, that the line above actually reached the
		 * card: ask the filesystem how big the file is now.  A "0 bytes"
		 * here means writes are not landing and the log is worthless, so
		 * do not go looking for a file that was never written. */
		if (con_log_path && sceIoGetstat(con_log_path, &st) >= 0)
		{
			snprintf(buf, sizeof(buf), "log: %d bytes on the card",
			         (int)st.st_size);
			con_write(buf);
			con_putc('\n');
		}
		else if (con_log_path)
		{
			con_write("log: stat failed\n");
		}

		if (!con_log_path)
		{
			snprintf(buf, sizeof(buf), "log: open error %d", con_log_err);
			con_write(buf);
			con_putc('\n');
		}
	}
}

int psp_con_puts (const char *s)
{
	con_write(s);
	con_putc('\n');
	return 0;
}

int psp_con_putchar (int c)
{
	con_putc(c);
	return c;
}

int psp_con_fputs (const char *s, FILE *stream)
{
	(void)stream; /* stderr and stdout both end up on screen */
	con_write(s);
	return 0;
}

static int con_vprintf (const char *fmt, va_list args)
{
	char buf[512];
	int n;

	n = vsnprintf(buf, sizeof(buf), fmt, args);
	if (n < 0)
		return n;

	con_write(buf);
	return n;
}

int psp_con_printf (const char *fmt, ...)
{
	va_list args;
	int n;

	va_start(args, fmt);
	n = con_vprintf(fmt, args);
	va_end(args);

	return n;
}

int psp_con_fprintf (FILE *stream, const char *fmt, ...)
{
	va_list args;
	int n;

	(void)stream;

	va_start(args, fmt);
	n = con_vprintf(fmt, args);
	va_end(args);

	return n;
}

int psp_con_fflush (FILE *stream)
{
	(void)stream;
	con_ensure();
	con_flush(); /* nothing is buffered but the current partial line */
	return 0;
}

FUNC_NORETURN void psp_con_panic (const char *what, const char *file, int line,
                                  const char *func, const char *expr)
{
	char buf[256];

	con_ensure();
	con_flush();

	con_write("*** ");
	con_write(what ? what : "FAILED");
	con_write(" ***");
	con_putc('\n');

	if (file)
	{
		snprintf(buf, sizeof(buf), "  at %s:%d", file, line);
		con_write(buf);
		con_putc('\n');
	}
	if (func && func[0])
	{
		snprintf(buf, sizeof(buf), "  in %s()", func);
		con_write(buf);
		con_putc('\n');
	}
	if (expr)
	{
		con_write("  ");
		con_write(expr);
		con_putc('\n');
	}

	con_write("  (holding for 30s)");
	con_putc('\n');
	con_log_sync();

	/* Give the tester time to read it before the module dies. */
	sceKernelDelayThread(30 * 1000 * 1000);
	sceKernelExitGame();

	for (;;)
		sceKernelDelayThread(1000 * 1000);
}

/* newlib's assert() calls this.  Declared weak on purpose: libc.a carries its
 * own __assert_func, and if the linker ends up pulling that object in as well
 * a non-weak definition here would be a duplicate symbol error.  Weak lets
 * ours win when nothing else provides it and step aside when something does. */
void __assert_func (const char *file, int line, const char *func,
                    const char *expr) __attribute__((weak));

void __assert_func (const char *file, int line, const char *func,
                    const char *expr)
{
	psp_con_panic("ASSERTION FAILED", file, line, func, expr);
}

/*
 * psp_trace -- a breadcrumb the engine drops between the steps of something
 * long (right now: Host_Init).
 *
 * The engine's own Con_Printf is useless this early: Con_Init() has not run
 * yet, so it throws the text away.  This writes straight to the debug screen
 * and to kabluk.log, and forces the card write, so whatever the last
 * breadcrumb says is where the module died -- on screen while it is still
 * alive, and in the file afterwards.
 */
void psp_trace (const char *step)
{
	char buf[96];

	con_ensure();
	con_flush();		/* finish a half line so the step stands on its own */

	/* Free RAM on the same line: a step that fails only because the heap ran
	 * out is then visible without a second guess. */
	snprintf(buf, sizeof(buf), "  > %s  [free %d KB]",
	         step ? step : "?", sceKernelTotalFreeMemSize() / 1024);

	strncpy(con_last_step, step ? step : "?", sizeof(con_last_step) - 1);
	con_last_step[sizeof(con_last_step) - 1] = '\0';

	con_write(buf);
	con_putc('\n');		/* flushes, logs and syncs */

	/* The same text again, in row 0, where nothing else ever scrolls it
	 * away.  Row 0 is the line to read when the screen is about to die:
	 * it always names the last step that was reached. */
	if (con_row > 1)
	{
		pspDebugScreenSetXY(0, 0);
		pspDebugScreenPrintf("LAST: %-60.60s", step ? step : "?");
		pspDebugScreenSetXY(0, con_row);
	}
}

void psp_tracef (const char *fmt, ...)
{
	char buf[160];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	psp_trace(buf);
}

/*
 * psp_mark -- a *cheap* breadcrumb: it only records the name, it prints nothing
 * and touches no file.  For code that runs hundreds of times (every picture, every
 * texture) a psp_trace() per iteration would cost a memory-stick write each time;
 * this costs a string copy, and the watchdog prints the last one when the engine
 * stops making progress.
 */
void psp_mark (const char *what)
{
	strncpy(con_last_step, what ? what : "?", sizeof(con_last_step) - 1);
	con_last_step[sizeof(con_last_step) - 1] = '\0';

	/* Counts as progress: loading sixty pictures in a row is slow, not stuck,
	 * and the watchdog must not shout at it. */
	con_progress++;
}

void psp_con_logsync (void)
{
	con_log_sync();
}

/*
 * Hang watchdog.
 *
 * A fault at least takes the screen with it; a *hang* leaves it sitting there
 * looking healthy, and there is no way to tell "still working on something
 * slow" from "stuck in an endless loop" -- which is exactly how the console
 * behaved when SDL_CreateThread ran the URI worker on the main thread and the
 * engine never came back from URI_Init.
 *
 * So a second thread, at a lower priority than the game, watches the line
 * counter.  When nothing has been printed for eight seconds it says so, on the
 * last row of the screen and in kabluk.log, and repeats every ten seconds after
 * that.  The last breadcrumb is printed with it, so the answer is on the screen
 * even though nothing crashed.
 */
static int con_watchdog_thread (SceSize args, void *argp)
{
	unsigned seen;
	int stalled = 0;
	char logged[64];

	(void) args;
	(void) argp;

	seen = con_progress;
	logged[0] = '\0';

	for (;;)
	{
		char msg[160];

		sceKernelDelayThread(1000 * 1000);

		/* A crash takes the screen with it, so the last thing the engine was
		 * doing has to reach the card *while it is still alive*.  One line per
		 * change of step, once a second, is cheap and turns the log into a
		 * timeline: its last entry is where the engine died. */
		if (strcmp(logged, con_last_step) != 0)
		{
			strncpy(logged, con_last_step, sizeof(logged) - 1);
			logged[sizeof(logged) - 1] = '\0';

			con_logfmt("mark: ");
			con_logfmt(logged);
			con_logfmt("\n");
		}

		if (con_progress != seen)
		{
			seen = con_progress;
			stalled = 0;
			continue;
		}

		stalled++;

		if (stalled != 8 && (stalled % 10) != 8)
			continue;

		snprintf(msg, sizeof(msg), "watchdog: %d s without progress, last: %s",
		         stalled, con_last_step);

		pspDebugScreenSetXY(0, CON_ROWS - 1);
		pspDebugScreenPrintf("%-67.67s", msg);
		pspDebugScreenSetXY(0, con_row < CON_ROWS ? con_row : 0);

		con_logfmt("*** ");
		con_logfmt(msg);
		con_logfmt("\n");
	}

	return 0;
}

void psp_con_watchdog (void)
{
	static int started;
	SceUID th;

	if (started)
		return;
	started = 1;

	th = sceKernelCreateThread("kabluk_watchdog", con_watchdog_thread,
	                           0x20, 0x4000, THREAD_ATTR_USER, NULL);
	if (th < 0)
	{
		con_write("watchdog: could not be started\n");
		return;
	}

	sceKernelStartThread(th, 0, NULL);
}
