/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers

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

#ifndef _QUAKE_WAD_H
#define _QUAKE_WAD_H

//===============
//   TYPES
//===============

#define	CMP_NONE		0
#define	CMP_LZSS		1

#define	TYP_NONE		0
#define	TYP_LABEL		1

#define	TYP_LUMPY		64				// 64 + grab command number
#define	TYP_PALETTE		64
#define	TYP_QTEX		65
#define	TYP_QPIC		66
#define	TYP_SOUND		67
#define	TYP_MIPTEX		68			// WAD2 miptex
#define	TYP_WAD3_MIPTEX	67			// WAD3 miptex

#define	WAD_VERSION_2	2
#define	WAD_VERSION_3	3

#define	WADFILENAME "gfx.wad" //johnfitz -- filename is now hard-coded for honesty

typedef struct
{
	unsigned int	width, height;
	byte			data[4];			// variably sized
} qpic_t;

typedef struct
{
	char			identification[4];		// WAD2 or WAD3
	unsigned int	numlumps;
	unsigned int	infotableofs;
} wadinfo_t;

typedef struct
{
	unsigned int	filepos;
	unsigned int	disksize;
	unsigned int	size;					// uncompressed
	char			type;
	char			compression;
	char			pad1, pad2;
	char			name[16];				// must be null terminated
} lumpinfo_t;

typedef struct wadarchive_s
{
	byte			*data;
	size_t			file_size;
	unsigned int	num_lumps;
	lumpinfo_t	*lumps;
	int			version;
	unsigned int	path_id;
} wadarchive_t;

typedef struct wadmiptex_s
{
	char			name[16];
	unsigned int	width, height;
	const byte		*pixels;
	size_t			pixel_count;
	const byte		*palette;	// RGB triples, NULL for a WAD2/Quake texture
	unsigned int	palette_colors;
} wadmiptex_t;

extern unsigned int	wad_numlumps;
extern lumpinfo_t	*wad_lumps;
extern byte			*wad_base;
extern int				wad_version;

void	W_LoadWadFile (void); //johnfitz -- filename is now hard-coded for honesty
void	*W_GetLumpName (const char *name, lumpinfo_t **out_info);

qboolean WAD_ArchiveLoad (wadarchive_t *archive, const char *filename);
void	WAD_ArchiveFree (wadarchive_t *archive);
qboolean WAD_ArchiveFindMipTex (const wadarchive_t *archive, const char *name, wadmiptex_t *miptex);
qboolean WAD_ExtractMipTex (const byte *data, size_t data_size, wadmiptex_t *miptex);
qboolean WAD_ParsePalette (const byte *data, size_t data_size, size_t palette_offset,
	const byte **palette, unsigned int *palette_colors);
void	WAD_ConvertIndexedToRGBA (const byte *indices, size_t pixel_count,
	const byte *palette, unsigned int palette_colors, int transparent_index, byte *rgba);

void SwapPic (qpic_t *pic);

#endif	/* _QUAKE_WAD_H */

