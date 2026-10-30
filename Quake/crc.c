/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
/* crc.c */

#include "quakedef.h"
#include "crc.h"
/* q_isalnum(), for the whole-word rule below: the same "C" locale test
 * q_memmem() in common.c applies to a buffer, and one that does not depend on
 * the locale the console happens to be in. */
#include "q_ctype.h"

// this is a 16 bit, non-reflected CRC using the polynomial 0x1021
// and the initial and final xor values shown below...  in other words, the
// CCITT standard CRC used by XMODEM

#define CRC_INIT_VALUE	0xffff
#define CRC_XOR_VALUE	0x0000

static unsigned short crctable[256] =
{
	0x0000,	0x1021,	0x2042,	0x3063,	0x4084,	0x50a5,	0x60c6,	0x70e7,
	0x8108,	0x9129,	0xa14a,	0xb16b,	0xc18c,	0xd1ad,	0xe1ce,	0xf1ef,
	0x1231,	0x0210,	0x3273,	0x2252,	0x52b5,	0x4294,	0x72f7,	0x62d6,
	0x9339,	0x8318,	0xb37b,	0xa35a,	0xd3bd,	0xc39c,	0xf3ff,	0xe3de,
	0x2462,	0x3443,	0x0420,	0x1401,	0x64e6,	0x74c7,	0x44a4,	0x5485,
	0xa56a,	0xb54b,	0x8528,	0x9509,	0xe5ee,	0xf5cf,	0xc5ac,	0xd58d,
	0x3653,	0x2672,	0x1611,	0x0630,	0x76d7,	0x66f6,	0x5695,	0x46b4,
	0xb75b,	0xa77a,	0x9719,	0x8738,	0xf7df,	0xe7fe,	0xd79d,	0xc7bc,
	0x48c4,	0x58e5,	0x6886,	0x78a7,	0x0840,	0x1861,	0x2802,	0x3823,
	0xc9cc,	0xd9ed,	0xe98e,	0xf9af,	0x8948,	0x9969,	0xa90a,	0xb92b,
	0x5af5,	0x4ad4,	0x7ab7,	0x6a96,	0x1a71,	0x0a50,	0x3a33,	0x2a12,
	0xdbfd,	0xcbdc,	0xfbbf,	0xeb9e,	0x9b79,	0x8b58,	0xbb3b,	0xab1a,
	0x6ca6,	0x7c87,	0x4ce4,	0x5cc5,	0x2c22,	0x3c03,	0x0c60,	0x1c41,
	0xedae,	0xfd8f,	0xcdec,	0xddcd,	0xad2a,	0xbd0b,	0x8d68,	0x9d49,
	0x7e97,	0x6eb6,	0x5ed5,	0x4ef4,	0x3e13,	0x2e32,	0x1e51,	0x0e70,
	0xff9f,	0xefbe,	0xdfdd,	0xcffc,	0xbf1b,	0xaf3a,	0x9f59,	0x8f78,
	0x9188,	0x81a9,	0xb1ca,	0xa1eb,	0xd10c,	0xc12d,	0xf14e,	0xe16f,
	0x1080,	0x00a1,	0x30c2,	0x20e3,	0x5004,	0x4025,	0x7046,	0x6067,
	0x83b9,	0x9398,	0xa3fb,	0xb3da,	0xc33d,	0xd31c,	0xe37f,	0xf35e,
	0x02b1,	0x1290,	0x22f3,	0x32d2,	0x4235,	0x5214,	0x6277,	0x7256,
	0xb5ea,	0xa5cb,	0x95a8,	0x8589,	0xf56e,	0xe54f,	0xd52c,	0xc50d,
	0x34e2,	0x24c3,	0x14a0,	0x0481,	0x7466,	0x6447,	0x5424,	0x4405,
	0xa7db,	0xb7fa,	0x8799,	0x97b8,	0xe75f,	0xf77e,	0xc71d,	0xd73c,
	0x26d3,	0x36f2,	0x0691,	0x16b0,	0x6657,	0x7676,	0x4615,	0x5634,
	0xd94c,	0xc96d,	0xf90e,	0xe92f,	0x99c8,	0x89e9,	0xb98a,	0xa9ab,
	0x5844,	0x4865,	0x7806,	0x6827,	0x18c0,	0x08e1,	0x3882,	0x28a3,
	0xcb7d,	0xdb5c,	0xeb3f,	0xfb1e,	0x8bf9,	0x9bd8,	0xabbb,	0xbb9a,
	0x4a75,	0x5a54,	0x6a37,	0x7a16,	0x0af1,	0x1ad0,	0x2ab3,	0x3a92,
	0xfd2e,	0xed0f,	0xdd6c,	0xcd4d,	0xbdaa,	0xad8b,	0x9de8,	0x8dc9,
	0x7c26,	0x6c07,	0x5c64,	0x4c45,	0x3ca2,	0x2c83,	0x1ce0,	0x0cc1,
	0xef1f,	0xff3e,	0xcf5d,	0xdf7c,	0xaf9b,	0xbfba,	0x8fd9,	0x9ff8,
	0x6e17,	0x7e36,	0x4e55,	0x5e74,	0x2e93,	0x3eb2,	0x0ed1,	0x1ef0
};

void CRC_Init(unsigned short *crcvalue)
{
	*crcvalue = CRC_INIT_VALUE;
}

void CRC_ProcessByte(unsigned short *crcvalue, byte data)
{
	*crcvalue = (*crcvalue << 8) ^ crctable[(*crcvalue >> 8) ^ data];
}

unsigned short CRC_Value(unsigned short crcvalue)
{
	return crcvalue ^ CRC_XOR_VALUE;
}

//johnfitz -- texture crc
unsigned short CRC_Block (const byte *start, size_t count)
{
	unsigned short	crc;

	CRC_Init (&crc);
	while (count--)
		crc = (crc << 8) ^ crctable[(crc >> 8) ^ *start++];

	return crc;
}

/*
================
CRC_ScanBegin / CRC_ScanStep / CRC_ScanFinish / CRC_ScanFile

A CRC of a file and a whole-word search in it, without the file in memory.

Why this exists.  HasBots() in menu.c wants two things from progs.dat: its CRC
(are these the stock Quake progs?) and whether one small word occurs in it as a
whole word (does this progs know about bots?).  It used to get both by reading
the entire file into one malloc():

	malloc: NULL for 17400000 bytes -- last pic: cached, phase host frame:

-- 17.4 MB asked of a console whose whole newlib heap block is 10 MB
(port/psp/psp_heap.c), for two passes over the bytes that are sequential by
nature.  The request could not succeed, and the menu it happened in is the one
drawn just before a map starts.

So the work is a state machine over a window of the caller's choosing, and the
caller may stop after any window: menu.c feeds it 256 KB per frame, which turns
one multi-second card read into a scan that finishes while the player is
reading the menu.  Nothing is allocated, and no size of file can fail it.

The answers are the same as the memory versions:

  * the CRC uses the same table, the same initial value and the same update as
    CRC_Block() -- and, like CRC_Block(), it does NOT apply CRC_Value();
  * "whole word" is q_memmem()'s rule in common.c: the match counts when the
    byte in front is not alphanumeric (or there is no byte in front: start of
    file) and the byte behind is not alphanumeric (or there is no byte behind:
    end of file);
  * overlapping occurrences count, so "aab" in "aaab" is found.

CRC_ScanStep() returns 1 as soon as the word is found, at which point the CRC
is only partial -- a caller that stops there must not use it.  That is fine for
menu.c, which reports "found" first.

tests/test_crc_scan.c checks all of this against CRC_Block() and a brute-force
search on random data, split at random points and resumed the way the menu
resumes it.
================
*/
void CRC_ScanBegin (crc_scan_t *scan, const char *word)
{
	size_t len;

	if (!scan)
		return;

	memset (scan, 0, sizeof(*scan));
	CRC_Init (&scan->crc);

	scan->before = -1;
	scan->prev = -1;

	/* A word longer than CRC_SCAN_WORD_MAX cannot be searched for; the only
	 * caller passes three bytes ("crx"), and a truncated word would silently
	 * match the wrong thing, so nothing is searched for at all. */
	len = word ? strlen (word) : 0;
	if (len > CRC_SCAN_WORD_MAX)
		len = 0;

	if (len)
		memcpy (scan->word, word, len);

	scan->word[len] = '\0';
	scan->wordlen = (int)len;
}

int CRC_ScanStep (crc_scan_t *scan, const byte *buf, size_t count)
{
	size_t i;
	int wl;

	if (!scan || !buf)
		return 0;

	if (!count || scan->wordlen <= 0 || scan->found)
		return scan->found ? 1 : 0;

	wl = scan->wordlen;

	for (i = 0; i < count; i++)
	{
		int c = buf[i];

		/* Every byte goes through the CRC, found or not: this is the same
		 * update CRC_Block() does, and it has to stay that way. */
		scan->crc = (unsigned short)((scan->crc << 8) ^
		                             crctable[(scan->crc >> 8) ^ c]);

		/* A full match is a whole word only if the byte behind it is not
		 * alphanumeric; that byte is this one, so it decides now. */
		if (scan->pending)
		{
			scan->pending = 0;
			if (!q_isalnum (c))
			{
				scan->found = true;
				return 1;
			}
			/* Otherwise the match was part of a longer word; the search
			 * carries on from this byte, which may start a new one. */
		}

		if (scan->matched > 0 && c == (unsigned char)scan->word[scan->matched])
			scan->matched++;
		else if (c == (unsigned char)scan->word[0])
		{
			scan->matched = 1;
			scan->before = scan->prev;	/* the byte in front of the match */
		}
		else
			scan->matched = 0;

		if (scan->matched == wl)
		{
			/* The byte in front has already been checked, once, when the
			 * match started. */
			if (scan->before < 0 || !q_isalnum (scan->before))
				scan->pending = 1;

			scan->matched = 0;
		}

		scan->prev = c;
	}

	return scan->found ? 1 : 0;
}

/* End of the data: a match still waiting for the byte behind it is a whole
 * word, because there is no byte behind it. */
int CRC_ScanFinish (crc_scan_t *scan)
{
	if (!scan)
		return 0;

	if (scan->pending)
	{
		scan->pending = 0;
		scan->found = true;
	}

	return scan->found ? 1 : 0;
}

/* The above, in one call, for a file that is already open: the whole file is
 * read in 4 KB windows.  Used by anything that can afford to block (and by the
 * tests); returns false only if the file could not be read, in which case
 * *crc_out and *found_out hold what was read. */
#define CRC_SCAN_FILE_WINDOW 4096

qboolean CRC_ScanFile (FILE *file, const char *word,
                       unsigned short *crc_out, qboolean *found_out)
{
	byte buf[CRC_SCAN_FILE_WINDOW];
	crc_scan_t scan;
	size_t n;

	if (crc_out)
		*crc_out = 0;
	if (found_out)
		*found_out = false;

	if (!file)
		return false;

	CRC_ScanBegin (&scan, word);

	while (!scan.found && (n = fread (buf, 1, sizeof(buf), file)) > 0)
		CRC_ScanStep (&scan, buf, n);

	CRC_ScanFinish (&scan);

	if (crc_out)
		*crc_out = scan.crc;
	if (found_out)
		*found_out = scan.found;

	return !ferror (file);
}

/*
 * The client half of sv_mapcrc.  It had the same shape as the server's: the
 * whole .bsp into one malloc() to answer two questions about its bytes.  On
 * the console that malloc is 17.4 MB of a 10 MB heap (the log line
 * "malloc: NULL for 17400000 bytes at 0x08949898"), so the check could not
 * run at all -- and it runs when a map is loaded, which is where the session
 * ended.  COM_BlockChecksumFile() streams the file in 4 KB pieces and returns
 * exactly the two numbers this comparison wants.
 */
qboolean CL_MapCRC_Validate(const char *bsp, unsigned server_crc_quick, unsigned server_crc_full) // woods #mapcrc
{
	unsigned local_crc_quick, local_crc_full;
	int file_size;
	double start_time, quick_time, full_time;

	Con_DPrintf("*** Two-Stage Map CRC Validation ***\n");
	Con_DPrintf("BSP file: '%s'\n", bsp);
	Con_DPrintf("Server Quick CRC: %u (0x%08x)\n", server_crc_quick, server_crc_quick);
	Con_DPrintf("Server Full CRC:  %u (0x%08x)\n", server_crc_full, server_crc_full);

	if (!COM_BlockChecksumFile(bsp, 4096, &local_crc_quick, &local_crc_full, &file_size)) {
		Con_DPrintf("ERROR: Could not read file '%s' - skipping CRC check\n", bsp);
		return true;  // let connection proceed; user will error later
	}

	start_time = Sys_DoubleTime();

	// Stage 1: Quick CRC check (first 4KB) - EARLY REJECTION ONLY
	int quick_size = (file_size > 4096) ? 4096 : file_size;
	quick_time = Sys_DoubleTime();

	Con_DPrintf("\n=== STAGE 1: QUICK CRC CHECK (Early Rejection) ===\n");
	Con_DPrintf("Quick size: %d bytes\n", quick_size);
	Con_DPrintf("Local Quick CRC:  %u (0x%08x) [%.1fms]\n", 
	          local_crc_quick, local_crc_quick, (quick_time - start_time) * 1000.0);
	Con_DPrintf("Server Quick CRC: %u (0x%08x)\n", server_crc_quick, server_crc_quick);
	Con_DPrintf("Quick Match: %s\n", (local_crc_quick == server_crc_quick) ? "YES" : "NO");

	if (local_crc_quick != server_crc_quick) {
		Con_DPrintf("Quick CRC mismatch - maps are definitely different, skipping full check\n");
		Con_DPrintf("Map CRC mismatch: quick CRC differs (%u vs %u)\n", 
		          local_crc_quick, server_crc_quick);
		Con_DPrintf("=================================================\n");
		return false;
	}

	Con_DPrintf("Quick CRC match - performing full check to confirm...\n");

	// Stage 2: Full CRC check -- the same pass above already folded it in
	full_time = Sys_DoubleTime();

	Con_DPrintf("\n=== STAGE 2: FULL CRC CHECK (Definitive) ===\n");
	Con_DPrintf("Full size: %d bytes\n", file_size);
	Con_DPrintf("Local Full CRC:  %u (0x%08x) [%.1fms]\n", 
	          local_crc_full, local_crc_full, (full_time - quick_time) * 1000.0);
	Con_DPrintf("Server Full CRC: %u (0x%08x)\n", server_crc_full, server_crc_full);
	Con_DPrintf("Full Match: %s\n", (local_crc_full == server_crc_full) ? "YES" : "NO");
	Con_DPrintf("Total validation time: %.1fms\n", (full_time - start_time) * 1000.0);

	if (local_crc_full == server_crc_full) {
		Con_DPrintf("Full CRC match - maps are identical\n");
		Con_DPrintf("==========================================\n");
		return true;
	} else {
		Con_DPrintf("Full CRC mismatch - maps differ despite quick match (false positive avoided)\n");
		Con_DPrintf("Map CRC mismatch: quick(%u vs %u - matched), full(%u vs %u - different)\n", 
		          local_crc_quick, server_crc_quick, local_crc_full, server_crc_full);
		Con_DPrintf("==========================================\n");
		return false;
	}
}
