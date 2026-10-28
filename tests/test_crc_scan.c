/*
 * tests/test_crc_scan.c -- the streaming progs.dat scan, checked on the build
 * machine.
 *
 * Quake/menu.c used to answer "does this progs know about bots" by reading the
 * whole file into one malloc().  On the hardware that request was
 *
 *     malloc: NULL for 17400000 bytes -- last pic: cached, phase host frame:
 *
 * -- 17.4 MB, in the menu drawn just before a map starts, on a console whose
 * whole heap block is 10 MB.  The answer is now produced by a resumable scan
 * in Quake/crc.c (CRC_ScanBegin/CRC_ScanStep/CRC_ScanFinish), fed a window per
 * frame.
 *
 * A rewrite of a check is only a fix if it gives the same answers, so this file
 * compares the scan against both things it replaces:
 *
 *   - the CRC against CRC_Block() over the whole buffer (the scan must return
 *     the same number, including for an empty file);
 *   - the whole-word search against a brute-force search with q_memmem()'s
 *     rule (byte before and byte after are not alphanumeric, or do not exist).
 *
 * and it does that for random data, split into random windows and resumed the
 * way the menu resumes it -- the interesting cases are a word that straddles a
 * window boundary, a word at the very first and very last byte, and a match
 * that is part of a longer word.
 *
 * Build and run:
 *   cc -std=gnu11 -Wall -I Quake -o /tmp/test_crc_scan \
 *      tests/test_crc_scan.c Quake/crc.c && /tmp/test_crc_scan
 * (tests/run_gameplay_tests.sh does exactly that.)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quakedef.h"
#include "crc.h"
#include "q_ctype.h"

static int failures;
static int checks;

static void check (int ok, const char *what, const char *detail)
{
	checks++;
	if (!ok)
	{
		failures++;
		printf ("FAIL: %s (%s)\n", what, detail ? detail : "");
	}
}

/* The rule q_memmem() applies to a buffer in memory: a match counts when the
 * byte in front is not alphanumeric (or there is none) and the byte behind is
 * not alphanumeric (or there is none). */
static int reference_whole_word (const byte *buf, size_t len, const char *word)
{
	size_t wl = strlen (word), i, j;

	if (!wl)
		return 0;

	for (i = 0; i + wl <= len; i++)
	{
		for (j = 0; j < wl; j++)
			if (buf[i + j] != (byte)word[j])
				break;

		if (j != wl)
			continue;

		if (i > 0 && q_isalnum (buf[i - 1]))
			continue;
		if (i + wl < len && q_isalnum (buf[i + wl]))
			continue;

		return 1;
	}

	return 0;
}

/* Feed the scan in windows whose sizes come from `sizes`, which is what the
 * menu does with its 256 KB window (and what an EOF landing anywhere does). */
static void scan_in_windows (const byte *buf, size_t len, const char *word,
                             unsigned short *crc, int *found,
                             const size_t *sizes, int nsizes)
{
	crc_scan_t scan;
	size_t off = 0;
	int w = 0;

	CRC_ScanBegin (&scan, word);

	while (off < len)
	{
		size_t n = sizes[w % nsizes];

		if (n > len - off)
			n = len - off;

		CRC_ScanStep (&scan, buf + off, n);
		off += n;
		w++;
	}

	CRC_ScanFinish (&scan);

	*crc = scan.crc;
	*found = scan.found ? 1 : 0;
}

static unsigned int rng_state = 12345;

static unsigned int rng (void)
{
	rng_state = rng_state * 1103515245u + 12345u;
	return (rng_state >> 8) & 0x7fffffff;
}

static void one_case (const byte *buf, size_t len, const char *word,
                      const size_t *sizes, int nsizes, const char *detail)
{
	unsigned short crc, ref_crc;
	int found, ref_found;

	scan_in_windows (buf, len, word, &crc, &found, sizes, nsizes);
	ref_crc = CRC_Block (buf, len);
	ref_found = reference_whole_word (buf, len, word);

	/* A scan that found the word stops there (that is the point: the answer is
	 * known), so its CRC covers only the part that was read.  The CRC is
	 * therefore only comparable when the word was not found -- which is the
	 * case the CRC is actually used for (see Bots_ScanStep in Quake/menu.c). */
	if (!found)
		check (crc == ref_crc, "the scan's CRC is CRC_Block()'s", detail);
	check (found == ref_found,
	       found ? "the scan found a word the reference did not"
	             : "the scan missed a word the reference found", detail);
}

int main (void)
{
	/* One size for the "everything at once" pass, and a spread of awkward
	 * windows for the resuming pass -- 1 byte windows make every position a
	 * boundary, which is the worst case for the state machine. */
	static const size_t whole[] = { 1u << 30 };
	static const size_t split[] = { 1, 2, 3, 5, 8, 13, 64, 4096 };
	byte *buf;
	size_t i, len;
	char detail[128];

	/* ---- the CRC alone: every length up to 300, against CRC_Block ---- */
	for (len = 0; len <= 300; len++)
	{
		buf = (byte *) malloc (len ? len : 1);
		for (i = 0; i < len; i++)
			buf[i] = (byte) rng ();

		{
			unsigned short crc;
			int found;
			size_t all = 1u << 30;

			scan_in_windows (buf, len, "crx", &crc, &found, &all, 1);
			snprintf (detail, sizeof (detail), "length %u", (unsigned) len);
			check (crc == CRC_Block (buf, len), "CRC matches for every length", detail);
			check (!found, "no marker in random data", detail);
		}

		free (buf);
	}

	/* The empty file: CRC_Block() of nothing is the initial value. */
	{
		unsigned short crc = 0;
		int found = 1;
		size_t all = 8;

		scan_in_windows ((const byte *) "", 0, "crx", &crc, &found, &all, 1);
		check (crc == 0xffff, "an empty file has the initial CRC", "empty");
		check (!found, "an empty file has no marker", "empty");
	}

	/* ---- the word: placed where a window boundary can hide it ---------- */
	{
		static const size_t places[] = { 0, 1, 2, 3, 4093, 4094, 4095, 4096, 4097, 8189 };
		static const size_t total = 8192;

		buf = (byte *) malloc (total);

		for (i = 0; i < sizeof (places) / sizeof (places[0]); i++)
		{
			size_t at = places[i];

			/* '.' is not alphanumeric, so a marker planted in it has a
			 * whole-word boundary on both sides.  The filler was 'q' in the
			 * first version of this test -- alphanumeric -- and every one of
			 * these cases then "failed", because the word really was part of
			 * a longer word.  The reference was right and the test was wrong:
			 * which is the reason it is written twice, twice differently. */
			memset (buf, '.', total);
			memcpy (buf + at, "crx", 3);

			snprintf (detail, sizeof (detail), "whole word at %u",
			          (unsigned) at);

			one_case (buf, total, "crx", whole, 1, detail);
			one_case (buf, total, "crx", split,
			          (int) (sizeof (split) / sizeof (split[0])), detail);
			check (reference_whole_word (buf, total, "crx"),
			       "the reference finds a planted whole word", detail);
		}

		/* At the very last three bytes: the word closes the file, so there is
		 * no byte behind it and it is still a whole word. */
		memset (buf, '.', total);
		memcpy (buf + total - 3, "crx", 3);
		one_case (buf, total, "crx", whole, 1, "word at the end of the file");
		one_case (buf, total, "crx", split,
		          (int) (sizeof (split) / sizeof (split[0])), "word at the end of the file");
		check (reference_whole_word (buf, total, "crx"),
		       "the reference finds a word at the end of the file", "end");

		/* Letters on both sides: part of a longer word, not a match. */
		memset (buf, '.', total);
		memcpy (buf + 100, "acrxb", 5);
		one_case (buf, total, "crx", whole, 1, "marker inside a longer word");
		one_case (buf, total, "crx", split,
		          (int) (sizeof (split) / sizeof (split[0])), "marker inside a longer word");
		check (!reference_whole_word (buf, total, "crx"),
		       "the reference rejects a marker inside a longer word", "embedded");

		/* Two occurrences, the first one glued to a letter: the second one is
		 * the answer, and it is 400 bytes later -- a window that only looked
		 * at the first would say "no". */
		memset (buf, '.', total);
		memcpy (buf + 300, "xcrx", 4);
		memcpy (buf + 700, "crx", 3);
		one_case (buf, total, "crx", split,
		          (int) (sizeof (split) / sizeof (split[0])),
		          "the second occurrence is the whole word");
		check (reference_whole_word (buf, total, "crx"),
		       "the reference finds the second occurrence", "second occurrence");

		/* A word with a border ("aa" in "aaa"): overlapping candidates, none
		 * of them a whole word. */
		{
			static const byte aaa[] = "aaa";
			one_case (aaa, 3, "aa", whole, 1, "overlapping words");
			one_case (aaa, 3, "aa", split,
			          (int) (sizeof (split) / sizeof (split[0])), "overlapping words");
			check (!reference_whole_word (aaa, 3, "aa"),
			       "the reference rejects overlapping words", "overlap");
		}

		free (buf);
	}

	/* ---- random data, random windows: both answers, many times --------- */
	{
		int round;

		for (round = 0; round < 200; round++)
		{
			size_t sizes[6];
			int k;

			len = (rng () % 9000) + 1;
			buf = (byte *) malloc (len);

			for (i = 0; i < len; i++)
			{
				/* A small alphabet, so that accidental matches and
				 * near-matches are common: 'c', 'r', 'x' and 'q'. */
				static const char alphabet[] = "cccrxq";
				buf[i] = (byte) alphabet[rng () % 5];
			}

			for (k = 0; k < 6; k++)
				sizes[k] = (rng () % 64) + 1;

			snprintf (detail, sizeof (detail), "random file of %u bytes, round %d",
			          (unsigned) len, round);
			one_case (buf, len, "crx", whole, 1, detail);
			one_case (buf, len, "crx", sizes, 6, detail);

			free (buf);
		}
	}

	/* ---- the one-shot wrapper, on a real file --------------------------- */
	{
		const char *path = "test_crc_scan.tmp";
		byte data[1500];
		FILE *f;
		unsigned short crc = 0;
		qboolean found = true;
		unsigned short ref_crc;
		int ref_found;

		for (i = 0; i < sizeof (data); i++)
			data[i] = '.';
		memcpy (data + 1000, "crx", 3);
		data[0] = 'c';
		data[1] = 'r';

		f = fopen (path, "wb");
		check (f != NULL, "the temporary file could be written", path);
		if (f)
		{
			fwrite (data, 1, sizeof (data), f);
			fclose (f);

			f = fopen (path, "rb");
			check (f != NULL, "the temporary file could be reopened", path);
			if (f)
			{
				check (CRC_ScanFile (f, "crx", &crc, &found),
				       "CRC_ScanFile reports a readable file", path);
				fclose (f);

				ref_crc = CRC_Block (data, sizeof (data));
				ref_found = reference_whole_word (data, sizeof (data), "crx");

				check (found == (ref_found != 0),
				       "CRC_ScanFile finds the same words", path);

				if (!ref_found)
					check (crc == ref_crc,
					       "CRC_ScanFile returns CRC_Block()'s CRC", path);
			}

			remove (path);
		}

		/* A file that is not open is an error, not a wrong answer. */
		check (!CRC_ScanFile (NULL, "crx", &crc, &found),
		       "CRC_ScanFile refuses a closed file", "NULL");
	}

	printf ("test_crc_scan: %d checks, %d failures\n", checks, failures);

	return failures ? 1 : 0;
}
