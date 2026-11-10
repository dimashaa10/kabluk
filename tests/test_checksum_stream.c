/*
 * tests/test_checksum_stream.c -- Com_BlockChecksum() in pieces must give the
 * same number as Com_BlockChecksum() in one piece.
 *
 * Why this exists.  The map CRC (Quake/sv_main.c sv_mapcrc, Quake/crc.c
 * CL_MapCRC_Validate) asked for the whole .bsp in one malloc() -- 17.4 MB of a
 * 10 MB heap block on the console, which is the log line
 *
 *     malloc: NULL for 17400000 bytes at 0x08949898 -- last pic: cached,
 *     phase host frame: input/commands/clien
 *
 * and the end of the session at the top of the level.  COM_BlockChecksumFile()
 * reads the file in 4 KB windows and folds each window into an MD4 chain
 * (Quake/mdfour.c, Com_BlockChecksumBegin/Update/End), so the answer never
 * needs the file in memory.
 *
 * That is only allowed to be a change of *how* the bytes are read: a client
 * and a server still compare these numbers to decide that they are playing
 * the same map.  So the streaming chain is checked here against the one-piece
 * function it replaces, on random data, split at every awkward place --
 * window boundaries, one byte either side of a 64-byte block, empty input.
 *
 * Built and run by tests/run_gameplay_tests.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdfour.h"

static int checks;
static int failures;

static void check (int ok, const char *what, const char *detail)
{
	checks++;
	if (!ok)
	{
		failures++;
		printf ("FAIL: %s (%s)\n", what, detail ? detail : "");
	}
}

/* The same chain fed in pieces of `chunk` bytes. */
static unsigned streamed (const unsigned char *data, size_t len, size_t chunk)
{
	com_checksum_t c;
	size_t off = 0;

	Com_BlockChecksumBegin (&c);

	while (off < len)
	{
		size_t n = len - off;

		if (n > chunk)
			n = chunk;
		Com_BlockChecksumUpdate (&c, data + off, n);
		off += n;
	}

	return Com_BlockChecksumEnd (&c);
}

static unsigned lcg_state = 12345u;

static unsigned lcg (void)
{
	lcg_state = lcg_state * 1103515245u + 12345u;
	return (lcg_state >> 8) & 0xffffu;
}

int main (void)
{
	static const size_t chunk_sizes[] = { 1, 2, 3, 63, 64, 65, 100, 1000, 4096, 4097, 100000 };
	static const size_t lengths[] = { 0, 1, 2, 63, 64, 65, 127, 128, 129, 191, 192, 255,
	                                  4095, 4096, 4097, 8191, 8192, 8193, 65536, 70001 };
	unsigned char *data = (unsigned char *)malloc (70001);
	char detail[128];
	size_t i, k;

	check (data != NULL, "the test buffer could be allocated", "malloc");
	if (!data)
	{
		printf ("test_checksum_stream: %d checks, %d failures\n", checks, failures);
		return 1;
	}

	for (i = 0; i < sizeof (lengths) / sizeof (lengths[0]); i++)
	{
		size_t len = lengths[i];
		unsigned one_piece;

		for (k = 0; k < len; k++)
			data[k] = (unsigned char)(lcg () & 0xff);

		one_piece = Com_BlockChecksum (data, len);

		for (k = 0; k < sizeof (chunk_sizes) / sizeof (chunk_sizes[0]); k++)
		{
			size_t chunk = chunk_sizes[k];

			snprintf (detail, sizeof (detail), "%lu bytes in %lu-byte pieces",
			          (unsigned long)len, (unsigned long)chunk);
			check (streamed (data, len, chunk) == one_piece,
			       "a streamed checksum equals the one-piece one", detail);
		}

		/* A window size the caller picks at random, which is what a
		 * file read really produces. */
		snprintf (detail, sizeof (detail), "%lu bytes in random pieces",
		          (unsigned long)len);
		{
			com_checksum_t c;
			size_t off = 0;

			Com_BlockChecksumBegin (&c);
			while (off < len)
			{
				size_t n = (size_t)(lcg () % 300) + 1;

				if (n > len - off)
					n = len - off;
				Com_BlockChecksumUpdate (&c, data + off, n);
				off += n;
			}
			check (Com_BlockChecksumEnd (&c) == one_piece,
			       "a streamed checksum equals the one-piece one", detail);
		}

		/* The map CRC's "quick" value is the head of the file only. */
		{
			size_t head = len > 4096 ? 4096 : len;

			snprintf (detail, sizeof (detail), "%lu-byte head of %lu bytes",
			          (unsigned long)head, (unsigned long)len);
			check (streamed (data, head, 4096) == Com_BlockChecksum (data, head),
			       "the quick checksum covers the head of the file", detail);
		}
	}

	/* Empty input is the one case a caller can produce by accident: a file
	 * that could be opened but not read. */
	{
		com_checksum_t c;

		Com_BlockChecksumBegin (&c);
		Com_BlockChecksumUpdate (&c, data, 0);
		check (Com_BlockChecksumEnd (&c) == Com_BlockChecksum (data, 0),
		       "an empty file checksums the same either way", "0 bytes");
	}

	/* A context reused after End must start from the beginning again: the
	 * callers keep one on the stack and a stale state would be silent. */
	{
		com_checksum_t c;
		unsigned first, second;

		Com_BlockChecksumBegin (&c);
		Com_BlockChecksumUpdate (&c, "kuzbass", 7);
		first = Com_BlockChecksumEnd (&c);

		Com_BlockChecksumBegin (&c);
		Com_BlockChecksumUpdate (&c, "kuzbass", 7);
		second = Com_BlockChecksumEnd (&c);

		check (first == second, "a finished context can be started again", "kuzbass");
		check (first == Com_BlockChecksum ("kuzbass", 7),
		       "and gives the one-piece answer", "kuzbass");
	}

	free (data);

	printf ("test_checksum_stream: %d checks, %d failures\n", checks, failures);

	return failures ? 1 : 0;
}
