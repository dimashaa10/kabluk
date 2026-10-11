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
// wad.c

#include "quakedef.h"

unsigned int	wad_numlumps;
lumpinfo_t		*wad_lumps;
byte			*wad_base = NULL;
int				wad_version;

void SwapPic (qpic_t *pic);

/*
==================
W_CleanupName

Lowercases name and pads with spaces and a terminating 0 to the length of
lumpinfo_t->name.
Used so lumpname lookups can proceed rapidly by comparing 4 chars at a time
Space padding is so names can be printed nicely in tables.
Can safely be performed in place.
==================
*/
static void W_CleanupName (const char *in, char *out)
{
	int		i;
	int		c;

	for (i=0 ; i<16 ; i++ )
	{
		c = in[i];
		if (!c)
			break;

		if (c >= 'A' && c <= 'Z')
			c += ('a' - 'A');
		out[i] = c;
	}

	for ( ; i< 16 ; i++ )
		out[i] = 0;
}

/*
====================
W_LoadWadFile
====================
*/
void W_LoadWadFile (void) //johnfitz -- filename is now hard-coded for honesty
{
	lumpinfo_t		*lump_p;
	wadinfo_t		*header;
	int				i;
	int				infotableofs;
	int				numlumps;
	int				filesize;
	const char		*filename = WADFILENAME;

	//johnfitz -- modified to use malloc
	//TODO: use cache_alloc
	if (wad_base)
		free (wad_base);
	wad_base = NULL;
	wad_lumps = NULL;
	wad_numlumps = 0;
	wad_version = 0;
	wad_base = COM_LoadMallocFile (filename, NULL);
	if (!wad_base)
		Sys_Error ("W_LoadWadFile: couldn't load %s\n\n"
			   "Basedir is: %s\n\n"
			   "Check that this has an " GAMENAME " subdirectory containing pak0.pak and pak1.pak, "
			   "or use the -basedir command-line option to specify another directory.",
			   filename, com_basedir);

	filesize = com_filesize;
	if (filesize < (int)sizeof(wadinfo_t))
	{
		Con_Printf ("Wad file %s is too small to contain a header\n", filename);
		return;
	}

	header = (wadinfo_t *)wad_base;
	if (memcmp (header->identification, "WAD2", 4) == 0)
		wad_version = WAD_VERSION_2;
	else if (memcmp (header->identification, "WAD3", 4) == 0)
		wad_version = WAD_VERSION_3;
	else
	{
		Con_Printf ("Wad file %s doesn't have a WAD2 or WAD3 id\n", filename);
		return;
	}

	numlumps = LittleLong ((int)header->numlumps);
	infotableofs = LittleLong ((int)header->infotableofs);
	if (numlumps < 0 || infotableofs < (int)sizeof(wadinfo_t) ||
		(size_t)infotableofs > (size_t)filesize ||
		(size_t)numlumps > ((size_t)filesize - (size_t)infotableofs) / sizeof(lumpinfo_t))
	{
		Con_Printf ("Wad file %s has an invalid lump directory\n", filename);
		return;
	}

	wad_numlumps = (unsigned int)numlumps;
	wad_lumps = (lumpinfo_t *)(wad_base + infotableofs);

	for (i=0, lump_p = wad_lumps ; i<(int)wad_numlumps ; i++,lump_p++)
	{
		int filepos = LittleLong ((int)lump_p->filepos);
		int disksize = LittleLong ((int)lump_p->disksize);
		int size = LittleLong ((int)lump_p->size);

		lump_p->filepos = (unsigned int)filepos;
		lump_p->disksize = (unsigned int)disksize;
		lump_p->size = (unsigned int)size;
		if (filepos < 0 || disksize < 0 || size < 0 ||
			(size_t)filepos > (size_t)filesize || (size_t)disksize > (size_t)filesize - (size_t)filepos)
		{
			Con_DWarning ("Wad file %s lump \"%.16s\" has invalid bounds\n", filename, lump_p->name);
			lump_p->filepos = 0;
			lump_p->disksize = 0;
			lump_p->size = 0;
			continue;
		}
		if (lump_p->compression == CMP_NONE)
			lump_p->size = lump_p->disksize;
		else
		{
			Con_DWarning ("Wad file %s lump \"%.16s\" uses unsupported compression\n", filename, lump_p->name);
			lump_p->size = 0;
		}

		W_CleanupName (lump_p->name, lump_p->name);	// CAUTION: in-place editing!!! The endian fixups too.
		if (lump_p->type == TYP_QPIC && lump_p->compression == CMP_NONE && lump_p->size >= sizeof(int)*2)
			SwapPic ((qpic_t *)(wad_base + lump_p->filepos));
	}
}


/*
=============
W_GetLumpinfo
=============
*/
static lumpinfo_t	*W_GetLumpinfo (const char *name)
{
	int		i;
	lumpinfo_t	*lump_p;
	char	clean[16];

	W_CleanupName (name, clean);

	for (lump_p=wad_lumps, i=0 ; i<(int)wad_numlumps ; i++,lump_p++)
	{
		if (!strncmp(clean, lump_p->name, sizeof(lump_p->name)))
			return lump_p;
	}

	return NULL;
}

void *W_GetLumpName (const char *name, lumpinfo_t **out_info)	//Spike: so caller can verify that the qpic was written properly.
{
	lumpinfo_t	*lump;

	lump = W_GetLumpinfo (name);

	if (!lump)
		return NULL; //johnfitz

	*out_info = lump;
	return (void *)(wad_base + lump->filepos);
}


static int WAD_ReadLong (const byte *data)
{
	int value;
	memcpy (&value, data, sizeof(value));
	return LittleLong (value);
}

static unsigned short WAD_ReadShort (const byte *data)
{
	short value;
	memcpy (&value, data, sizeof(value));
	return (unsigned short)LittleShort (value);
}

qboolean WAD_ParsePalette (const byte *data, size_t data_size, size_t palette_offset,
	const byte **palette, unsigned int *palette_colors)
{
	unsigned int colors;

	if (!data || !palette || !palette_colors || palette_offset > data_size ||
		data_size - palette_offset < sizeof(unsigned short))
		return false;

	colors = WAD_ReadShort (data + palette_offset);
	if (!colors || colors > 256 ||
		(size_t)colors > (data_size - palette_offset - sizeof(unsigned short)) / 3)
		return false;

	*palette = data + palette_offset + sizeof(unsigned short);
	*palette_colors = colors;
	return true;
}

void WAD_ConvertIndexedToRGBA (const byte *indices, size_t pixel_count,
	const byte *palette, unsigned int palette_colors, int transparent_index, byte *rgba)
{
	size_t i;

	if (!indices || !palette || !rgba || !palette_colors || palette_colors > 256)
		return;

	for (i = 0; i < pixel_count; i++)
	{
		unsigned int index = indices[i];
		byte *pixel = rgba + i * 4;
		if (index < palette_colors)
		{
			pixel[0] = palette[index * 3 + 0];
			pixel[1] = palette[index * 3 + 1];
			pixel[2] = palette[index * 3 + 2];
		}
		else
			pixel[0] = pixel[1] = pixel[2] = 0;
		pixel[3] = (transparent_index >= 0 && index == (unsigned int)transparent_index) ? 0 : 255;
	}
}

qboolean WAD_ExtractMipTex (const byte *data, size_t data_size, wadmiptex_t *miptex)
{
	unsigned int width, height, offsets[4];
	size_t level_size[4];
	size_t palette_offset;
	int i;

	if (!data || !miptex || data_size < 40)
		return false;

	memset (miptex, 0, sizeof(*miptex));
	memcpy (miptex->name, data, 16);
	miptex->name[15] = '\0';
	width = (unsigned int)WAD_ReadLong (data + 16);
	height = (unsigned int)WAD_ReadLong (data + 20);
	if (!width || !height || (size_t)width > (size_t)-1 / (size_t)height)
		return false;

	for (i = 0; i < 4; i++)
	{
		unsigned int mipwidth = width >> i;
		unsigned int mipheight = height >> i;
		if (!mipwidth) mipwidth = 1;
		if (!mipheight) mipheight = 1;
		if ((size_t)mipwidth > (size_t)-1 / (size_t)mipheight)
			return false;
		level_size[i] = (size_t)mipwidth * (size_t)mipheight;
		offsets[i] = (unsigned int)WAD_ReadLong (data + 24 + i * 4);
		if (offsets[i] < 40 || (size_t)offsets[i] > data_size ||
			level_size[i] > data_size - (size_t)offsets[i])
			return false;
	}

	// WAD mip levels are stored in order and do not overlap.
	for (i = 1; i < 4; i++)
		if ((size_t)offsets[i] < (size_t)offsets[i-1] + level_size[i-1])
			return false;

	miptex->width = width;
	miptex->height = height;
	miptex->pixels = data + offsets[0];
	miptex->pixel_count = level_size[0];

	palette_offset = (size_t)offsets[3] + level_size[3];
	if (!WAD_ParsePalette (data, data_size, palette_offset, &miptex->palette, &miptex->palette_colors))
	{
		// Some WAD tools leave padding after the palette; in that case it is
		// still safely identifiable at the end of a normal 256-color miptex.
		if (data_size >= 2 + 256 * 3 &&
			!WAD_ParsePalette (data, data_size, data_size - (2 + 256 * 3),
				&miptex->palette, &miptex->palette_colors))
		{
			miptex->palette = NULL;
			miptex->palette_colors = 0;
		}
	}

	return true;
}

static qboolean WAD_ArchiveParse (wadarchive_t *archive)
{
	wadinfo_t *header;
	int version, num_lumps, directory_offset;
	unsigned int i;

	if (!archive || !archive->data || archive->file_size < sizeof(wadinfo_t))
		return false;

	header = (wadinfo_t *)archive->data;
	if (!memcmp (header->identification, "WAD2", 4))
		version = WAD_VERSION_2;
	else if (!memcmp (header->identification, "WAD3", 4))
		version = WAD_VERSION_3;
	else
		return false;

	num_lumps = LittleLong ((int)header->numlumps);
	directory_offset = LittleLong ((int)header->infotableofs);
	if (num_lumps < 0 || directory_offset < (int)sizeof(wadinfo_t) ||
		(size_t)directory_offset > archive->file_size ||
		(size_t)num_lumps > (archive->file_size - (size_t)directory_offset) / sizeof(lumpinfo_t))
		return false;

	archive->version = version;
	archive->num_lumps = (unsigned int)num_lumps;
	archive->lumps = (lumpinfo_t *)calloc (archive->num_lumps ? archive->num_lumps : 1, sizeof(*archive->lumps));
	if (!archive->lumps)
		return false;

	for (i = 0; i < archive->num_lumps; i++)
	{
		lumpinfo_t raw;
		lumpinfo_t *lump = &archive->lumps[i];
		int filepos, disksize, size;
		memcpy (&raw, archive->data + directory_offset + i * sizeof(raw), sizeof(raw));
		filepos = LittleLong ((int)raw.filepos);
		disksize = LittleLong ((int)raw.disksize);
		size = LittleLong ((int)raw.size);
		if (filepos < 0 || disksize < 0 || size < 0 ||
			(size_t)filepos > archive->file_size ||
			(size_t)disksize > archive->file_size - (size_t)filepos)
			continue;

		lump->filepos = (unsigned int)filepos;
		lump->disksize = (unsigned int)disksize;
		lump->size = (unsigned int)size;
		lump->type = raw.type;
		lump->compression = raw.compression;
		lump->pad1 = raw.pad1;
		lump->pad2 = raw.pad2;
		W_CleanupName (raw.name, lump->name);
	}

	return true;
}

qboolean WAD_ArchiveLoad (wadarchive_t *archive, const char *filename)
{
	qofs_t previous_filesize;
	qofs_t filesize;

	if (!archive || !filename || !filename[0])
		return false;
	memset (archive, 0, sizeof(*archive));
	previous_filesize = com_filesize;
	archive->data = COM_LoadMallocFile (filename, &archive->path_id);
	if (!archive->data)
	{
		com_filesize = previous_filesize;
		return false;
	}
	filesize = com_filesize;
	com_filesize = previous_filesize;
	if (filesize < 0)
	{
		WAD_ArchiveFree (archive);
		return false;
	}
	archive->file_size = (size_t)filesize;
	if (!WAD_ArchiveParse (archive))
	{
		WAD_ArchiveFree (archive);
		return false;
	}
	return true;
}

void WAD_ArchiveFree (wadarchive_t *archive)
{
	if (!archive)
		return;
	free (archive->lumps);
	free (archive->data);
	memset (archive, 0, sizeof(*archive));
}

qboolean WAD_ArchiveFindMipTex (const wadarchive_t *archive, const char *name, wadmiptex_t *miptex)
{
	char clean_name[16];
	unsigned int i;
	int expected_type;

	if (!archive || !archive->data || !name || !miptex)
		return false;
	W_CleanupName (name, clean_name);
	expected_type = archive->version == WAD_VERSION_3 ? TYP_WAD3_MIPTEX : TYP_MIPTEX;

	for (i = 0; i < archive->num_lumps; i++)
	{
		const lumpinfo_t *lump = &archive->lumps[i];
		const byte *data;
		size_t size;
		if (lump->type != expected_type || lump->compression != CMP_NONE ||
			!lump->disksize || strncmp (clean_name, lump->name, sizeof(lump->name)))
			continue;
		if ((size_t)lump->filepos > archive->file_size ||
			(size_t)lump->disksize > archive->file_size - (size_t)lump->filepos)
			continue;
		data = archive->data + lump->filepos;
		size = lump->disksize;
		if (WAD_ExtractMipTex (data, size, miptex))
			return true;
	}
	return false;
}

/*
=============================================================================

automatic byte swapping

=============================================================================
*/

void SwapPic (qpic_t *pic)
{
	pic->width = LittleLong(pic->width);
	pic->height = LittleLong(pic->height);
}
