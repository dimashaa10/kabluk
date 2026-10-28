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
#include <psppower.h>
#include <pspthreadman.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "psp_console.h"

#define CON_COLS 68   /* characters per line on the 480x272 debug font */
#define CON_ROWS 33   /* visible text rows */
/* Longest line the log will hold.  This used to be CON_COLS, i.e. 68, and the
 * debug screen's 68 characters per row are a property of the *screen*, not of
 * the file.  Keeping one buffer for both meant every diagnostic line longer
 * than 68 characters was cut in half and logged as two lines with the second
 * half starting with the wrap -- which is why "gl: frame 1, ... ring 750 of"
 * and "draw: ... proj m15 1.00" arrive in kabluk.log without their tails.
 * The two outputs now have their own widths: the log gets the whole line, the
 * screen wraps it into as many 68-character rows as it needs. */
#define CON_LINE_MAX 320

/* Free stack of the main thread, for the watchdog's once-a-second status
 * line; psp_heap.c owns the measurement. */
extern int psp_heap_stack_free (void);
static int   con_ready;
static int   con_row;
static const char *con_log_path;	/* kabluk.log, once we know it can live there */
static int   con_log_err;	/* last failing sceIo call, 0 while writes work */
static int   con_log_err_said;	/* the error has been put on the screen once */
/* 0 once the game owns the display: pspDebugScreen* draws through the same
 * GPU the game is using, so printing there would inject the console's own
 * primitives into the game's frame.  Text keeps going to kabluk.log. */
static int   con_screen_on = 1;
static char  con_line[CON_LINE_MAX + 1];
static int   con_len;

/* Watchdog state: bumped by every finished line, read by the watchdog thread. */
static volatile unsigned con_progress;
static char  con_last_step[64] = "(startup)";
/* 1 when con_last_step came from psp_mark() (a breadcrumb worth naming again
 * from the watchdog), 0 when it came from a line that is already in the log. */
static volatile int con_last_step_is_mark;

/* Which part of the frame the engine was in when it stopped.  The breadcrumbs
 * answer "which picture", this answers "before or after the GPU": a hang while
 * waiting for the GE (sceGuSync) and a hang while loading a picture look
 * exactly the same from the log otherwise, and they have nothing in common. */
static char  con_phase[48] = "(none)";

/* The last few breadcrumbs, oldest first.  A single "last step" says where the
 * engine stopped; it does not say how it got there, and a fault that happens
 * sometimes and in different places is exactly the case where the previous
 * dozen steps are the only thing that turns a coincidence into a pattern. */
#define CON_STEPS 16
#define CON_STEP_LEN 48

static char  con_steps[CON_STEPS][CON_STEP_LEN];
static volatile int con_steps_next;	/* next slot to write */
static volatile int con_steps_full;

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
 * So the log is written the durable way -- open (append) -- write -- close --
 * and NOT through a descriptor that lives for the session.  close() is the
 * operation the driver cannot skip.
 *
 * What changed later is the granularity: one close() per *line* turned out to
 * cost more than the diagnostics were worth once the drawing code started
 * tracing (see the note on con_log_append() below).  Lines are collected in RAM
 * and reach the card as one write per buffer, still closed after every write.
 */
static int con_log_try_open (void);
static void con_write (const char *s);
static void con_putc (int c);
/* Give the log a few more chances: psp_con_init() runs before the memory
 * stick is necessarily ready, and a card that answers on the second try is
 * worth more than a clean error message. */
#define CON_LOG_OPEN_TRIES 100

static int con_log_tries;

/* Card write accounting, filled in by con_log_try() and read once per
 * heartbeat by psp_con_card_take_stats().  Declared here because the writer
 * comes before the reader in this file. */
static unsigned int con_card_writes;
static unsigned int con_card_us;

static void con_log_lock (void);
static void con_log_unlock (void);
static void con_log_drain_locked (void);
static void con_log_sync (void);
static void con_log_sync_at_most (unsigned int min_us);
static void con_log_sync_throttled (void);

/* One card write: open (append) -- write -- close, all of it in one place.
 *
 * close() is the operation the driver cannot skip, so whatever this function
 * was given is on the card when it returns.  Batch as much as possible into one
 * call (see con_log_append below): what is expensive here is the round trip,
 * not the bytes.  Returns 0 if the card did not take it. */
static int con_log_try (const char *buf)
{
	SceUID fd;

	{
		/* Timed as a whole: open, write and close are three syscalls and
		 * it is the round trip that costs, not the bytes.  See
		 * psp_con_card_take_stats(). */
		unsigned int t0 = (unsigned int)sceKernelGetSystemTimeWide();
		int ok = 1;

		fd = sceIoOpen(con_log_path,
		               PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
		if (fd < 0)
		{
			con_log_err = (int)fd;
			ok = 0;
		}
		else if (sceIoWrite(fd, buf, (unsigned int)strlen(buf)) < 0)
		{
			con_log_err = -1;
			sceIoClose(fd);
			ok = 0;
		}
		else
			sceIoClose(fd);

		con_card_writes++;
		con_card_us += (unsigned int)sceKernelGetSystemTimeWide() - t0;
		return ok;
	}
}

/*
 * The log is written in batches, one card write per buffer.
 *
 * Every line used to be its own open()/write()/close() -- two of them, in fact,
 * because con_flush() logged the text and the newline separately.  close() is
 * what actually pushes the FAT cache at the card, so a line cost a card round
 * trip: on real hardware several milliseconds, and much more when the card is
 * busy.  That is invisible while a handful of lines are printed at startup and
 * it is the whole frame budget once the drawing code starts tracing, where a
 * burst of 1200 lines (the full-frame dump in gl_shim.c) is 2400 card
 * operations: seconds of a black screen at frame 120, which is a freeze the
 * player feels and the log cannot explain.
 *
 * So lines are collected in RAM and handed to the card in one go.  The buffer
 * is drained when it fills, once a second by the watchdog thread, and at the
 * end of a fatal error.  The trade is deliberate: a crash can lose the lines
 * that were still in the buffer (at most the ones written in the last second),
 * and everything before them is on the card.
 *
 * The buffer is touched from two threads (the engine and the watchdog), so the
 * append and the drain take a mutex.  Without it a drain that copies while the
 * other thread appends produces a log with a torn line in the middle -- the
 * exact kind of hole this file exists to avoid.
 */
#define CON_LOGBUF_SIZE 4096

static char        con_logbuf[CON_LOGBUF_SIZE];
static size_t      con_logbuf_len;

/* A lightweight mutex (the LwMutex kind), not a full one.
 *
 * This is the API pspsdk puts in pspthreadman.h for user code -- the names
 * "sceKernelCreateMutex"/"LockMutex" belong to the kernel side and are not
 * declared there, so using them is an implicit declaration and a link error.
 * A lightweight mutex keeps its whole state in the workarea below (no kernel
 * object to create, leak or fail), which is also why it is the cheaper of the
 * two on a 333 MHz CPU. */
static SceLwMutexWorkarea con_log_mutex __attribute__((aligned(16)));
static int         con_log_mutex_state;	/* 0 = not made yet, 1 = usable, -1 = no */

static void con_log_lock (void)
{
	/* Created on the first line, which is always printed by the engine thread
	 * long before the watchdog starts.  If it cannot be made, the log keeps
	 * working unlocked: a torn line is better than no log at all -- and the
	 * state stops it from being retried on every line. */
	if (!con_log_mutex_state)
	{
		memset(&con_log_mutex, 0, sizeof(con_log_mutex));

		con_log_mutex_state =
			sceKernelCreateLwMutex(&con_log_mutex, "kabluk_log", 0, 0, NULL) < 0
				? -1 : 1;
	}

	if (con_log_mutex_state > 0)
		sceKernelLockLwMutex(&con_log_mutex, 1, NULL);
}

static void con_log_unlock (void)
{
	if (con_log_mutex_state > 0)
		sceKernelUnlockLwMutex(&con_log_mutex, 1);
}

/* Hand everything that has been collected to the card.  Called with the log
 * lock held; con_log_sync() is the version the rest of the file uses. */
static void con_log_drain_locked (void)
{
	int tries;

	if (!con_logbuf_len)
		return;

	if (!con_log_path && con_log_tries < CON_LOG_OPEN_TRIES)
	{
		con_log_tries++;
		con_log_try_open();
	}

	con_logbuf[con_logbuf_len] = '\0';

	/* Twice, because the trail is only worth anything if it has no holes in
	 * it: a single failed open (the card busy with another write, the FAT
	 * cache flushing) used to drop one line out of the middle and leave a
	 * log that looked like a crash in the wrong place. */
	for (tries = 0; tries < 2; tries++)
		if (con_log_try(con_logbuf))
		{
			con_logbuf_len = 0;
			con_logbuf[0] = '\0';
			return;
		}

	/* No card (or a broken one): the text is gone either way, and keeping it
	 * would only push the next line out of a full buffer. */
	con_logbuf_len = 0;
	con_logbuf[0] = '\0';
}

static void con_log_append (const char *text)
{
	size_t len;

	if (!text || !text[0])
		return;

	len = strlen(text);

	con_log_lock();

	if (len >= CON_LOGBUF_SIZE - 1)
	{
		/* A single line that does not fit the buffer at all.  Nothing in the
		 * port prints one (the longest is 384 bytes), and half a line in the
		 * log is worse than none, so it is dropped -- after the pending lines
		 * have been written out, so the log does not lose them instead. */
		con_log_drain_locked();
		con_log_unlock();
		return;
	}

	if (con_logbuf_len + len > CON_LOGBUF_SIZE - 1)
		con_log_drain_locked();

	memcpy(con_logbuf + con_logbuf_len, text, len);
	con_logbuf_len += len;
	con_logbuf[con_logbuf_len] = '\0';

	con_log_unlock();
}

/* The one operation that gets the collected lines onto the card. */
static void con_log_sync (void)
{
	con_log_lock();
	con_log_drain_locked();
	con_log_unlock();
}

/*
 * The same, but no more often than CON_TRACE_SYNC_MIN_US.
 *
 * Per-line sync is the only way to guarantee that the line which killed the
 * module is itself on the card.  It is also, measured on the hardware, about
 * 15 ms per line -- open-write-close on the Memory Stick is a round trip, and
 * the file is growing, so each append also touches the FAT.  The save/load
 * screen writes sixteen lines per frame (fourteen marks for the status bar and
 * the motif, plus the drawing dump), and sixteen times fifteen is the ~250 ms
 * a frame that screen took.  That is the diagnostic turning into the bug it is
 * supposed to find.
 *
 * So the trail keeps its order and its buffer, and reaches the card at most
 * once every CON_TRACE_SYNC_MIN_US.  Nothing is dropped: con_log_append()
 * already drains when the 4 KB buffer fills, so the card always holds every
 * line older than that window.  What a hard fault can take with it is the last
 * fraction of that window -- ten writes a second instead of sixteen per frame.
 *
 * The window was 15 ms first, which changed nothing at all (292 -> 257 ms is
 * noise): at sixteen lines a frame both settings write sixteen times a frame,
 * so the throttle has to be coarser than a frame, not finer.  psp_con_card_
 * take_stats() now counts the writes and the milliseconds they cost, and the
 * heartbeat prints them, so the next log says outright whether the card is
 * this cost or whether the menu is.
 */
#define CON_TRACE_SYNC_MIN_US 100000

static unsigned int con_sync_last_us;

/* The same bound, with the window passed in: the boot trail and the menu trail
 * want different ones (see CON_BOOT_SYNC_MIN_US below).
 *
 * One timestamp is shared on purpose.  Both callers are asking the same
 * question -- "when did the card last hear from us" -- and a crash loses the
 * lines written since that moment whichever trail they belong to. */
static void con_log_sync_at_most (unsigned int min_us)
{
	unsigned int now = (unsigned int)(sceKernelGetSystemTimeWide() & 0xffffffffu);
	unsigned int since = now - con_sync_last_us;	/* wraps cleanly */

	if (con_sync_last_us && since < min_us)
		return;

	con_sync_last_us = now;
	con_log_sync();
}

static void con_log_sync_throttled (void)
{
	con_log_sync_at_most(CON_TRACE_SYNC_MIN_US);
}

/* ---- what the log costs, measured ---------------------------------------
 *
 * The throttle above is a guess about a number nobody had measured: how long
 * one open-write-close round trip on the Memory Stick actually takes here.
 * The frame times leave room for the guess to be wrong -- sixteen marks in a
 * frame at 250 ms is 15 ms per write, which is far more than the 5 ms the
 * comment assumed -- so the number is counted instead of assumed, and the
 * heartbeat prints it:
 *
 *     run: f 296 ... , card 4 writes 61 ms
 *
 * If that figure is most of the frame, the trail is the cost.  If it is near
 * zero, the cost is the menu itself and the trail may stay switched on.
 * psp_con_card_take_stats() reads and clears, so each heartbeat covers the
 * frames since the previous one, like the frame average beside it. */
void psp_con_card_take_stats (unsigned int *writes, unsigned int *us)
{
	if (writes)
		*writes = con_card_writes;
	if (us)
		*us = con_card_us;

	con_card_writes = 0;
	con_card_us = 0;
}

static void con_log_line (const char *text)
{
	char buf[CON_LINE_MAX + 2];

	if (!text)
		return;

	snprintf(buf, sizeof(buf), "%s\n", text);
	con_log_append(buf);
}

static void con_logfmt (const char *s)
{
	con_log_append(s);
}

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

	if (con_screen_on)
	{
		int off;

		/* Wrap the one long line onto the screen's 68-character rows. */
		for (off = 0; off < con_len; off += CON_COLS)
		{
			if (con_row >= CON_ROWS) {
				pspDebugScreenClear();
				con_row = 0;
			}

			pspDebugScreenSetXY(0, con_row);
			pspDebugScreenPrintf("%-*.*s\n", CON_COLS,
			                     con_len - off < CON_COLS ? con_len - off : CON_COLS,
			                     con_line + off);
			con_row++;
		}
	}

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

	/* Only a full buffer ends a line now; the screen's row width is applied
	 * when the line is printed, not while it is being collected. */
	if (con_len >= CON_LINE_MAX)
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
 * Checked again later, from pspsdk's side, and the same answer comes back:
 * pspDebugInstallErrorHandler() lives in src/debug/exception.c, which calls
 * sceKernelRegisterDefaultExceptionHandler() -- NID 0x565C0B0E of
 * ExceptionManagerForKernel (pspsdk src/kernel/ExceptionManagerForKernel.S),
 * i.e. a kernel library, i.e. 8002013C for a user-mode EBOOT.  This is the
 * same rule the Makefile's link rule enforces by grepping the ELF for
 * "*ForKernel" and refusing to produce the EBOOT at all.  So there is no
 * register dump here, and the dump has to be replaced by evidence that is
 * reachable from user mode: the per-second status line (watchdog), and the
 * malloc failures.
 *
 * A crash therefore still takes the screen with it.  What survives is
 * kabluk.log -- and, for the two cases the port can still see for itself (a
 * Sys_Error/assert and a stall the watchdog notices), the text console put
 * back on the panel by con_screen_take_panel().  Every console line is
 * written to kabluk.log through sceIo as it is
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

		/* The line above is flushed on purpose: the very next thing this
		 * function does is ask the filesystem how big the file is, and a
		 * buffered line would make that answer a lie (always 0). */
		con_log_sync();

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
			con_log_sync();
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

/*
 * con_screen_take_panel -- put the text console back on the display.
 *
 * The console and the game do not share a framebuffer.  pspDebugScreen* draws
 * with the CPU straight into EDRAM page 0 (pspsdk src/debug/scr_printf.c,
 * debug_put_char_32(): vram = g_vram_base + y * PSP_LINE_SIZE), and the game
 * draws into its own two buffers starting at 0x088000 (see the layout note at
 * the top of gu_psp.c).  Once the GU presents its first frame, sceDisplay
 * frame buffer points at the game's buffer and page 0 is no longer scanned out.
 *
 * That made every message printed after handover invisible: text went into a
 * page nobody was showing, and kabluk.log was the only copy.  The consequences
 * were exactly the symptom reported from the hardware -- "the game crashes and
 * switches the console off, with no message":
 *
 *   - a Sys_Error()/assert() in the middle of a game wrote its reason to page 0,
 *     the panel kept the last game frame (the freeze), and the log line was all
 *     there was;
 *   - the watchdog's stall report had the same problem, so "5 s without
 *     progress, last step ..." only ever existed in the file.
 *
 * pspDebugScreenInit() is pspsdk's own way of doing this: it points
 * sceDisplaySetFrameBuf back at page 0 and clears that page.  The clear is
 * bounded by the page itself (512 x 272 x 4 = 0x88000 bytes), i.e. it stops
 * exactly where the game's frame buffers begin, so nothing the GE is drawing
 * into is touched.
 *
 * It is reversible by construction: the next frame the game presents calls
 * sceGuSwapBuffers(), which re-points the display at the game's buffer.  So a
 * stall that recovers goes back to the game by itself, and a real failure keeps
 * the text on screen instead of a dead frame.
 */
static void con_screen_take_panel (void)
{
	pspDebugScreenInit();
	con_ready = 1;
	con_row = 0;
	con_len = 0;
	con_line[0] = '\0';
	con_screen_on = 1;
}

FUNC_NORETURN void psp_con_panic (const char *what, const char *file, int line,
                                  const char *func, const char *expr)
{
	char buf[256];

	/* A fatal message is worth taking the display for: otherwise nobody
	 * can read it on the console itself. */
	con_screen_take_panel();
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
	char buf[384];

	con_ensure();
	con_flush();		/* finish a half line so the step stands on its own */

	/* Free RAM on the same line: a step that fails only because the heap ran
	 * out is then visible without a second guess. */
	snprintf(buf, sizeof(buf), "  > %s  [free %d KB]",
	         step ? step : "?", sceKernelTotalFreeMemSize() / 1024);

	strncpy(con_last_step, step ? step : "?", sizeof(con_last_step) - 1);
	con_last_step[sizeof(con_last_step) - 1] = '\0';
	/* This line is on the card by now, so the watchdog must not echo it back
	 * as a "mark:" entry (that is where the hundreds of
	 * "mark: over: f 50 quads tex 69 ..." lines came from). */
	con_last_step_is_mark = 0;

	con_write(buf);
	con_putc('\n');		/* flushes into the log buffer */

	/* This one is a boot breadcrumb, and the whole point of it is that it is
	 * already on the card when the very next step kills the module.  So it is
	 * not batched: a card write here, none per frame later. */
	con_log_sync();

	/* The same text again, in row 0, where nothing else ever scrolls it
	 * away.  Row 0 is the line to read when the screen is about to die:
	 * it always names the last step that was reached. */
	if (con_screen_on && con_row > 1)
	{
		pspDebugScreenSetXY(0, 0);
		pspDebugScreenPrintf("LAST: %-60.60s", step ? step : "?");
		pspDebugScreenSetXY(0, con_row);
	}
}

/*
 * psp_tracef -- one diagnostic line: the screen, and the log buffer.  Not the
 * card.
 *
 * This used to be psp_trace() with a format string, and psp_trace() ends in
 * con_log_sync(), which is an open/write/close on the memory stick.  For a boot
 * step that is the point (see psp_trace above): the very next instruction may
 * kill the module, so the line has to be on the card before it runs.
 *
 * For the drawing diagnostics it is exactly backwards, and the log of a menu
 * session shows why: gl_shim.c reports a window of up to thirty lines inside a
 * single frame every sixty frames, and two frames in full (four hundred draws
 * each).  Eight hundred and thirty memory-stick round trips, every one of them
 * in the middle of a frame, is not a diagnostic of the frame -- it is a
 * bottleneck the diagnostic invented, and it is the one number a stalled
 * frame does not need.
 *
 * The console already batches: con_flush() appends to a 4 KB log buffer, and
 * the watchdog thread drains it once a second (see "Once a second, whatever the
 * engine traced goes to the card" below), so a hard death still leaves at most
 * one second of lines behind.  That is the same granularity the status line
 * has, and it is what the batch is for.
 *
 * The `[free N KB]` suffix and the LAST: row on the screen belong to psp_trace
 * and stay there: this side is cadence output, not a breadcrumb, and a
 * "LAST: d 120 37 Q t18 512x512 ..." would overwrite the one line on the screen
 * that says where the module died.
 */
/* ---- the crash in the menu needs the last line, not the last second ------
 *
 * Outside the menu the diagnostics are batched: a line goes into the log buffer
 * and the watchdog drains it once a second.  That granularity is fine for a
 * picture that loads slowly; it is useless for a fault, because the fault does
 * not leave anything behind -- the console switches off and whatever was still
 * in RAM never reaches the card.
 *
 * Both hardware logs of this port end the same way -- the last line is an
 * image load from the single-player menu, and there is nothing after it.  The
 * crash is somewhere in that menu, and one second of lines is exactly the
 * second we need.
 *
 * So while the engine's key destination is the menu (main_psp.c sets this
 * before every frame), every line is written to the card as it is produced.
 * The menu is a few hundred lines a second at worst and a couple after it
 * settles, so the cost is worth it; outside the menu nothing changes. */
static int con_trace_sync;

void psp_con_trace_sync (int on)
{
	con_trace_sync = on ? 1 : 0;
}

void psp_tracef (const char *fmt, ...)
{
	/* Long enough for the frame diagnostics, which carry a dozen numbers
	 * (draw list words, heap, ring, viewport, scissor).  vsnprintf() below
	 * truncates rather than overruns, so the size is only about not losing
	 * the end of a line -- and the end of these lines is the part that
	 * changes. */
	char buf[320];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	con_write(buf);
	con_putc('\n');

	/* Hunting the menu crash: get the line onto the card before the next
	 * instruction can take the module down.  See the note above -- and on
	 * con_log_sync_throttled(), because one round trip per line is what made
	 * the save/load screen take a third of a second per frame. */
	if (con_trace_sync)
		con_log_sync_throttled();
}

/*
 * psp_mark -- a *cheap* breadcrumb: it only records the name, it prints nothing
 * and touches no file.  For code that runs hundreds of times (every picture, every
 * texture) a psp_trace() per iteration would cost a memory-stick write each time;
 * this costs a string copy, and the watchdog prints the last one when the engine
 * stops making progress.
 */
/* While the engine is starting up, every changed mark goes to the card: that
 * is how a crash inside an initialiser gets a name.  Once the frame loop is
 * running there is no "stuck initialiser" left to find, and the drawing code
 * marks a picture per draw call -- several hundred a second, each one an
 * open/write/close on the memory stick.  That is the difference between a
 * game and a slideshow, so the card writes are switched off at that point
 * (psp_con_boot_done()).  The marks themselves keep working: the watchdog
 * still names the last step it saw.*/
static int con_marks_live = 1;

/* How often a boot mark may reach the card: four times a second.
 *
 * The rule used to be "every mark, every time", on the argument that a mark is
 * one per picture load -- tens of milliseconds apart -- so a round trip per
 * mark costs a few milliseconds and buys an exact trail.  The hardware says
 * otherwise, and the heartbeat is where it says it:
 *
 *     run: f 6 ..., card 1345 writes 24749 ms
 *
 * That is start-up: 1345 open/write/close round trips, 24.7 seconds of the
 * boot, for 1345 lines nobody reads unless the module dies in one of them.
 * The round trip measured here is 18 ms, not the 5 ms the argument assumed
 * (the file is growing, so every append touches the FAT as well), and start-up
 * marks a picture per lump: a texture, a sprite, a scrap entry, every one of
 * them paid.
 *
 * So the boot trail keeps its content and loses only its resolution: a quarter
 * of a second of marks can be in RAM when the module dies, and everything
 * before that is on the card.  What that costs in diagnosis is small next to
 * what it costs in boot time, and the two lines that must be exact are still
 * exact -- psp_trace() syncs on every call (they are the `> X_Init` steps, one
 * per subsystem, and the crash is named by the last one), and the crash paths
 * and the watchdog write on their own, without this buffer.
 *
 * 100 ms was the other candidate; at 18 ms per write it is still a write every
 * six marks, and start-up would pay ~4.5 s of card time.  250 ms is the point
 * where the boot becomes loading again instead of logging. */
#define CON_BOOT_SYNC_MIN_US 250000

void psp_con_boot_done (void)
{
	char line[96];

	if (!con_marks_live)
		return;

	snprintf(line, sizeof(line),
	         "mark: boot done -- breadcrumbs to the card are off");
	con_log_line(line);

	/* Everything the boot trail left in RAM goes out now.  From here on the
	 * marks no longer sync on their own (a menu marks a picture per draw
	 * call), so this is the last chance to hand over a complete start-up
	 * trail; after it the watchdog's once-a-second drain owns the buffer. */
	con_log_sync();

	con_marks_live = 0;
}

/* The game is about to take the display over.
 *
 * pspDebugScreen* draws with the CPU into EDRAM page 0 -- pspsdk
 * src/debug/scr_printf.c, debug_put_char_32(): vram = g_vram_base + y *
 * PSP_LINE_SIZE, no GE involved.  Once the game presents its own frames,
 * sceDisplay's frame buffer points at the game's page (0x088000 upwards, see
 * gu_psp.c) and page 0 is no longer scanned out.
 *
 * So everything printed after this point is invisible: it lands in a page
 * nobody is showing.  That is fine while the game runs -- the text is in
 * kabluk.log either way -- but it is not fine for a failure or a freeze, so
 * those two paths call con_screen_take_panel(), which points the display back
 * at page 0 before printing.  (This note used to claim the console drew with
 * sceGu into the current framebuffer and left stray triangles in the game's
 * frame; that is not what pspsdk does.  The reason to release the screen is
 * that page 0 is not on the panel any more, not that the characters would
 * pollute the frame.) */
void psp_con_screen_off (void)
{
	if (!con_screen_on)
		return;

	con_screen_on = 0;
}

void psp_mark (const char *what)
{
	char line[96];
	static char written[64];
	int slot;

	const char *step = what ? what : "?";

	if (!strcmp(step, written))
		return;		/* same as last time: nothing to record */

	strncpy(written, step, sizeof(written) - 1);
	written[sizeof(written) - 1] = '\0';

	strncpy(con_last_step, step, sizeof(con_last_step) - 1);
	con_last_step[sizeof(con_last_step) - 1] = '\0';
	con_last_step_is_mark = 1;

	/* Remember it: the watchdog prints the whole tail of this ring when the
	 * engine stops, which is the difference between "died in a picture" and
	 * "died in this picture, after those twelve". */
	slot = con_steps_next;
	strncpy(con_steps[slot], step, CON_STEP_LEN - 1);
	con_steps[slot][CON_STEP_LEN - 1] = '\0';
	slot++;
	if (slot >= CON_STEPS)
	{
		slot = 0;
		con_steps_full = 1;
	}
	con_steps_next = slot;

	/* Counts as progress: loading sixty pictures in a row is slow, not stuck,
	 * and the watchdog must not shout at it. */
	con_progress++;

	/* And it goes straight to the card.
	 *
	 * This is the difference between "the console went off and the last log
	 * line is '> Sbar_Init'" and knowing which picture did it: a crash takes
	 * the screen instantly and the watchdog only speaks once a second, so a
	 * fast death leaves nothing behind.  One small write per changed step
	 * costs a few milliseconds per picture and turns the log into an exact
	 * trail. */
	/* A boot mark is the trail a crash needs, and so is a mark inside the
	 * menu while that is where the crash is (con_trace_sync, above).
	 *
	 * During boot every mark still pays the round trip: they are one-time
	 * (a picture load each, tens of milliseconds apart) and a crash in an
	 * initialiser has nothing else.  Inside the menu there are sixty of
	 * them per frame, so that trail is throttled. */
	if (con_marks_live)
	{
		snprintf(line, sizeof(line), "mark: %s", step);
		con_log_line(line);
		con_log_sync_at_most(CON_BOOT_SYNC_MIN_US);
	}
	else if (con_trace_sync)
	{
		snprintf(line, sizeof(line), "mark: %s", step);
		con_log_line(line);
		con_log_sync_throttled();
	}
}

void psp_phase (const char *what)
{
	strncpy(con_phase, what ? what : "(none)", sizeof(con_phase) - 1);
	con_phase[sizeof(con_phase) - 1] = '\0';
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
/* A line from the watchdog that does not go through the log buffer.
 *
 * The engine writes that buffer under a lock.  If it died while holding it --
 * which is exactly what a hang in the card driver or a fault inside
 * con_log_sync() looks like -- then a watchdog that queues behind the lock says
 * nothing at all, and the log ends in the middle of the engine's last line with
 * no explanation.  Two independent writes with no lock between them are what
 * makes the watchdog's report survive that case.
 */
static void con_log_direct (const char *text)
{
	char buf[CON_LINE_MAX + 2];

	if (!text)
		return;

	snprintf(buf, sizeof(buf), "%s\n", text);

	if (!con_log_path && con_log_tries < CON_LOG_OPEN_TRIES)
	{
		con_log_tries++;
		con_log_try_open();
	}

	if (con_log_path)
		con_log_try(buf);
}

/* psp_con_note_alloc_failure -- one line for a malloc() that came back NULL.
 *
 * Called from the --wrap=malloc hook in psp_heap.c (port/psp/Makefile passes
 * -Wl,--wrap=malloc), i.e. from inside the allocator, on whatever thread asked
 * for the memory and at whatever point in the frame.
 *
 * Why this is worth a card write of its own: a NULL that nothing checks is a
 * write through address 0, and on the PSP that is not an error message -- the
 * CPU faults, the module dies, and the console switches itself off.  The log
 * then ends in the middle of a frame with no reason.  This line is the reason,
 * printed *before* the dereference: the size that could not be served and the
 * breadcrumb of the step that asked for it.
 *
 * Lock-free on purpose (con_log_direct, the same path the watchdog uses): the
 * engine may be holding its log lock, or may be about to die while holding it.
 * Capped at eight lines -- some failures are expected and recovered from (the
 * picture arena trims itself and retries), and a run with hundreds of them
 * must not turn the log into a list of them. */
void psp_con_note_alloc_failure (const char *what, unsigned int size)
{
	static int noted;
	char line[CON_LINE_MAX + 2];

	if (noted >= 8)
		return;

	noted++;

	/* The two strings are written by the engine without a lock; the
	 * precision caps what a torn copy can make the line read. */
	snprintf(line, sizeof(line),
	         "%.8s: NULL for %u bytes -- last %.55s, phase %.39s",
	         what ? what : "alloc", size, con_last_step, con_phase);

	con_log_direct(line);
	con_log_sync();
}

/* The tail of the breadcrumb ring: the last dozen steps, oldest first.  Printed
 * once per stall, because the point of a stall report is to say where the
 * engine was and what it had just been doing. */
static void con_watchdog_history (int to_screen)
{
	char line[CON_LINE_MAX + 2];
	int i, count, start, len;

	count = con_steps_full ? CON_STEPS : con_steps_next;
	if (count <= 0)
		return;

	start = con_steps_next - count;
	if (start < 0)
		start += CON_STEPS;

	line[0] = '\0';
	len = 0;
	for (i = 0; i < count; i++)
	{
		const char *step = con_steps[(start + i) % CON_STEPS];
		int n = snprintf(line + len, sizeof(line) - (size_t)len,
		                 "%s%s", i ? " <- " : "hist: ", step);

		if (n <= 0 || (size_t)(len + n) >= sizeof(line) - 1)
			break;

		len += n;
	}

	if (!len)
		return;

	con_log_direct(line);

	/* The same trail on the panel when the panel has been taken for the
	 * report.  One step per row, oldest first: the file is what gets sent
	 * back, but on the console itself this is the only way to see what led
	 * to the stall without a PC in front of it. */
	if (to_screen)
		for (i = 0; i < count; i++)
			pspDebugScreenPrintf("%c %-66.66s\n", i ? ' ' : '>',
			                     con_steps[(start + i) % CON_STEPS]);
}

static int con_watchdog_thread (SceSize args, void *argp)
{
	unsigned seen;
	int stalled = 0;
	int reported = 0;
	char logged[64];

	(void) args;
	(void) argp;

	seen = con_progress;
	logged[0] = '\0';

	for (;;)
	{
		char msg[192];
		char line[CON_LINE_MAX + 2];

		sceKernelDelayThread(1000 * 1000);

		/* pspsdk src/power/psppower.h: "Generate a power tick, preventing
		 * unit from powering off and turning off display."  Nothing in the
		 * port called it (checked: no scePowerTick anywhere), so the one
		 * timer that would keep the unit awake depended on the game still
		 * feeding the display.  The moment the engine stops -- the freeze
		 * this port has been chasing -- nobody ticks any more, and the
		 * firmware is free to switch the console off under the frozen
		 * picture.  That is the "and then it turns itself off" half of the
		 * report; a tick a second from this thread, which keeps running
		 * while the engine is stuck, removes it. */
		scePowerTick(PSP_POWER_TICK_ALL);

		/* A crash takes the screen with it, so the last breadcrumb has to
		 * reach the card *while the engine is still alive*.  Only real
		 * breadcrumbs are echoed here: a traced line ("> img: ...") is
		 * already in the log, and echoing it used to double the size of the
		 * file for nothing. */
		if (con_last_step_is_mark && strcmp(logged, con_last_step) != 0)
		{
			strncpy(logged, con_last_step, sizeof(logged) - 1);
			logged[sizeof(logged) - 1] = '\0';

			snprintf(line, sizeof(line), "mark: %s", logged);
			con_log_direct(line);
		}

		/* Once a second, one line that survives the engine: the frame step
		 * it was in, how much stack the main thread has left and what the
		 * picture pool holds.  A crash takes the screen and the log ends in
		 * the middle of a frame, so the last second before it is the only
		 * evidence there is -- and on a console with no exception handler
		 * the fault address is not available at all.  Whatever this line
		 * says is what the engine was doing when it stopped:
		 *
		 *   status: phase <phase>, last <breadcrumb>, stack free N KB
		 *
		 * `stack free` is a high-water mark (sceKernelGetThreadStackFreeSize
		 * reports the least the thread has ever had left), so it never rises
		 * again: a number that keeps falling across a session is the engine
		 * going deeper and deeper, and a number near zero is a crash waiting
		 * for one more call. */
		{
			char status[CON_LINE_MAX + 2];
			int stack_free = psp_heap_stack_free();

			/* The two strings are written by the engine, without a
			 * lock; the precision caps what a torn copy can make this
			 * line read, exactly as con_watchdog_history() does. */
			if (stack_free >= 0)
				snprintf(status, sizeof(status),
				         "status: phase %.47s, last %.63s, stack free %d KB",
				         con_phase, con_last_step, stack_free / 1024);
			else
				snprintf(status, sizeof(status),
				         "status: phase %.47s, last %.63s",
				         con_phase, con_last_step);

			con_log_direct(status);
		}

		/* Once a second, whatever the engine traced goes to the card.  The
		 * game itself no longer pays a card round trip per line (see
		 * con_log_append), and this thread is where the batch leaves. */
		con_log_sync();

		if (con_progress != seen)
		{
			seen = con_progress;
			stalled = 0;
			reported = 0;
			continue;
		}

		stalled++;

		/* Five seconds is long enough that a long picture load is not
		 * mistaken for a hang, and short enough that the answer is already
		 * in the log when the console is switched off by hand -- which is
		 * what actually happens: a frozen PSP has to be powered off, and
		 * nothing written after that moment exists. */
		if (stalled < 5)
			continue;

		if (reported && (stalled % 10) != 0)
			continue;

		snprintf(msg, sizeof(msg),
		         "watchdog: %d s without progress, last step: %s, phase: %s",
		         stalled, con_last_step, con_phase);

		/* First report of this stall: put the text console on the panel.
		 *
		 * Until now this wrote into EDRAM page 0 with pspDebugScreenPrintf,
		 * and after the game's first frame that page is not what the
		 * display scans out -- so the one line that names the hang was
		 * invisible on the hardware and existed only in kabluk.log.  Taking
		 * the panel back is what makes a freeze readable on the console
		 * itself, and it is undone by the next sceGuSwapBuffers() if the
		 * engine recovers. */
		if (!reported)
		{
			con_screen_take_panel();
			pspDebugScreenPrintf("*** kabluk: the game stopped "
			                     "making progress ***\n");
			pspDebugScreenPrintf("%s\n", msg);

			/* The history is the part that turns "it froze somewhere in
			 * a picture" into a file name.  Printed once, because the
			 * later reports would scroll it off the 33 rows. */
			con_watchdog_history(1);
		}
		else
		{
			/* Later reports only update the bottom row: the panel keeps
			 * the first report and the trail above it, and the seconds
			 * counter in the corner keeps climbing so a photograph of
			 * the screen says how long it has been stuck. */
			pspDebugScreenSetXY(0, CON_ROWS - 1);
			pspDebugScreenPrintf("%.68s", msg);
			pspDebugScreenSetXY(0, con_row < CON_ROWS ? con_row : 0);
		}

		snprintf(line, sizeof(line), "*** %s", msg);
		con_log_direct(line);

		/* Already on the card before the panel goes dark again. */
		con_log_sync();

		reported = 1;
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
