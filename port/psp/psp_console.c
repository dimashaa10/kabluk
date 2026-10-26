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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "psp_console.h"

#define CON_COLS 68   /* characters per line on the 480x272 debug font */
#define CON_ROWS 33   /* visible text rows */

static int   con_ready;
static int   con_row;
static int   con_log = -1;	/* kabluk.log, so a crash still leaves evidence */
static char  con_line[CON_COLS + 1];
static int   con_len;

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

static void con_logfmt (const char *s)
{
	if (con_log >= 0 && s)
		sceIoWrite(con_log, s, (unsigned int) strlen(s));
}

static void con_flush (void)
{
	if (!con_len)
		return;

	con_line[con_len] = '\0';

	if (con_row >= CON_ROWS) {
		pspDebugScreenClear();
		con_row = 0;
	}

	pspDebugScreenSetXY(0, con_row);
	pspDebugScreenPrintf("%s\n", con_line);
	con_row++;
	con_len = 0;

	con_logfmt(con_line);
	con_logfmt("\n");
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
 * Exception handler.  Without one a fault takes the screen with it and all we
 * see is "the console turned off", which says nothing.  This one prints the
 * faulting PC, the return address and the cause register, writes the same to
 * the log, and then parks the thread so the screen stays readable.
 */


static void con_fault (PspDebugRegBlock *regs)
{
	char buf[160];

	con_ready = 0;
	con_len = 0;

	pspDebugScreenInit();
	pspDebugScreenClear();

	pspDebugScreenSetXY(0, 0);
	pspDebugScreenPrintf("*** CRASH ***\n");
	pspDebugScreenPrintf("pc       = %08x\n", (unsigned) regs->epc);
	pspDebugScreenPrintf("ra       = %08x\n", (unsigned) regs->r[31]);
	pspDebugScreenPrintf("sp       = %08x\n", (unsigned) regs->frame[0]);
	pspDebugScreenPrintf("badvaddr = %08x\n", (unsigned) regs->badvaddr);
	pspDebugScreenPrintf("cause    = %08x\n", (unsigned) regs->cause);
	pspDebugScreenPrintf("status   = %08x\n", (unsigned) regs->status);
	pspDebugScreenPrintf("a0..a3   = %08x %08x %08x %08x\n",
	                     (unsigned) regs->r[4], (unsigned) regs->r[5],
	                     (unsigned) regs->r[6], (unsigned) regs->r[7]);

	snprintf(buf, sizeof(buf),
	         "\n*** CRASH ***\npc=%08x ra=%08x sp=%08x\n"
	         "badvaddr=%08x cause=%08x status=%08x\n"
	         "a0..a3=%08x %08x %08x %08x\n",
	         (unsigned) regs->epc, (unsigned) regs->r[31],
	         (unsigned) regs->frame[0], (unsigned) regs->badvaddr,
	         (unsigned) regs->cause, (unsigned) regs->status,
	         (unsigned) regs->r[4], (unsigned) regs->r[5],
	         (unsigned) regs->r[6], (unsigned) regs->r[7]);
	con_logfmt(buf);

	for (;;)
		sceKernelDelayThread(1000000);
}

void psp_con_init (void)
{
	con_ensure();

	if (con_log < 0)
	{
		/* Next to the EBOOT first (that is where the engine looks for its
		 * paks), then the root of the stick as a fallback. */
		con_log = sceIoOpen("kabluk.log",
		                    PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
		if (con_log < 0)
			con_log = sceIoOpen("ms0:/kabluk.log",
			                    PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	}

	pspDebugInstallErrorHandler(con_fault);
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
	con_ensure();
	con_flush();

	pspDebugScreenPrintf("\n*** %s ***\n", what ? what : "FAILED");

	if (file)
		pspDebugScreenPrintf("  at %s:%d\n", file, line);
	if (func && func[0])
		pspDebugScreenPrintf("  in %s()\n", func);
	if (expr)
		pspDebugScreenPrintf("  %s\n", expr);

	pspDebugScreenPrintf("\n  (holding for 15s)\n");

	/* Give the tester time to read it before the module dies. */
	sceKernelDelayThread(15 * 1000 * 1000);
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
