/*
Copyright (C) 1997-1998  Andrew Tridgell

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

#ifndef _MDFOUR_H
#define _MDFOUR_H

#include <stddef.h>		/* size_t */

#ifndef int32
#define int32 int
#endif

#if SIZEOF_INT > 4
#define LARGE_INT32
#endif

#ifndef uint32
#define uint32 unsigned int32
#endif

struct mdfour {
	uint32 A, B, C, D;
	size_t totalN;
};

/*
 * Com_BlockChecksum() in one piece, or the same answer in pieces.
 *
 * MD4 is a chain: every 64-byte block is folded into four words, so the
 * checksum of a file never needs the file.  Com_BlockChecksum() takes a
 * pointer and a length because every caller in this engine happened to have
 * the bytes already -- and on the console that is the bug: the map, at the
 * start of every level, was read into one malloc() of 17.4 MB just to be
 * hashed (Quake/sv_main.c, Quake/crc.c, sv_mapcrc).
 *
 *     malloc: NULL for 17400000 bytes at 0x08949898 -- last pic: cached,
 *     phase host frame: input/commands/clien
 *
 * -- and the heap block these three allocators share is 10 MB
 * (port/psp/psp_heap.c), so the request could not be served, the answer was
 * NULL, and the module died at the top of the level.
 *
 * Begin/Update/End give the identical value from a 4 KB window:
 *
 *     com_checksum_t c;
 *     Com_BlockChecksumBegin (&c);
 *     while ((n = fread (buf, 1, sizeof(buf), f)) > 0)
 *             Com_BlockChecksumUpdate (&c, buf, n);
 *     value = Com_BlockChecksumEnd (&c);
 *
 * End finishes the chain, so a context is good for one answer.  Nothing is
 * allocated: the type is the four words plus one block of buffering.
 */
typedef struct {
	struct mdfour md;
	unsigned char block [64];	/* bytes not yet folded in */
	int used;			/* how many of them          */
} com_checksum_t;

void Com_BlockChecksumBegin (com_checksum_t *c);
void Com_BlockChecksumUpdate (com_checksum_t *c, const void *data, size_t len);
unsigned Com_BlockChecksumEnd (com_checksum_t *c);

/* The one-piece version, in mdfour.c and unchanged in meaning. */
unsigned Com_BlockChecksum (const void *buffer, size_t length);

#endif	/* _MDFOUR_H */
