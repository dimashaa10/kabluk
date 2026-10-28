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

#ifndef _QUAKE_CRC_H
#define _QUAKE_CRC_H

/* crc.h */

void CRC_Init(unsigned short *crcvalue);
void CRC_ProcessByte(unsigned short *crcvalue, byte data);
unsigned short CRC_Value(unsigned short crcvalue);
unsigned short CRC_Block (const byte *start, size_t count); //johnfitz -- texture crc

/*
 * A CRC and a whole-word search over a file that is too big to hold, done in
 * windows the caller chooses.  See the long note above CRC_ScanBegin() in
 * crc.c: menu.c asks progs.dat for exactly these two answers, and used to ask
 * for the file in one malloc() instead.
 *
 * The scan can be stopped after any window and resumed later, which is what
 * lets a 17 MB file be read a piece per frame instead of in one pause.
 */
#define CRC_SCAN_WORD_MAX 31

typedef struct
{
	unsigned short crc;      /* the number CRC_Block() would return so far  */
	int  wordlen;            /* length of the word being searched for       */
	char word [CRC_SCAN_WORD_MAX + 1];
	int  matched;            /* bytes of that word matched at the last byte */
	int  before;             /* byte in front of the match, -1 = file start */
	int  pending;            /* a full match is waiting for the byte after  */
	int  prev;               /* the last byte of the previous window        */
	qboolean found;          /* the word was found as a whole word          */
} crc_scan_t;

void CRC_ScanBegin (crc_scan_t *scan, const char *word);
int  CRC_ScanStep (crc_scan_t *scan, const byte *buf, size_t count);
int  CRC_ScanFinish (crc_scan_t *scan);

qboolean CRC_ScanFile (FILE *file, const char *word,
                       unsigned short *crc_out, qboolean *found_out);

//additional hash functions...
unsigned Com_BlockChecksum (const void *buffer, size_t length);
void Com_BlockFullChecksum (const void *buffer, size_t len, unsigned char *outbuf);

// Map CRC validation functions
qboolean CL_MapCRC_Validate(const char *bsp, unsigned server_crc_quick, unsigned server_crc_full); // woods #mapcrc

#endif	/* _QUAKE_CRC_H */

