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
//image.c -- image loading

#include "quakedef.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

#define LODEPNG_NO_COMPILE_DECODER
#define LODEPNG_NO_COMPILE_CPP
#define LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
#define LODEPNG_NO_COMPILE_ERROR_TEXT
#include "lodepng.h"
#include "lodepng.c"

static char loadfilename[MAX_OSPATH]; //file scope so that error messages can use it

#if defined(PLATFORM_PSP) || defined(__psp__)
/* Names the candidate file just before it is opened.  Image loading is the one
 * place where a bad file can take the console down without a message, and the
 * difference between "cherry.png" and "cherry.pcx" is the difference between
 * two completely different decoders. */
extern void psp_mark (const char *what);
#define IMG_MARK(x)	psp_mark(x)
#else
#define IMG_MARK(x)	do { } while (0)
#endif

extern cvar_t gl_load24bit_skins; // woods #loadskins

typedef struct stdio_buffer_s {
	FILE *f;
	unsigned char buffer[1024];
	int size;
	int pos;
} stdio_buffer_t;

static stdio_buffer_t *Buf_Alloc(FILE *f)
{
	stdio_buffer_t *buf = (stdio_buffer_t *) calloc(1, sizeof(stdio_buffer_t));

	/* calloc() returning NULL used to be a write through a NULL pointer, which
	 * on the PSP is the screen going out without a word. */
	if (!buf)
		Sys_Error ("Buf_Alloc: out of memory reading '%s'", loadfilename);

	buf->f = f;
	return buf;
}

static void Buf_Free(stdio_buffer_t *buf)
{
	free(buf);
}

static inline int Buf_GetC(stdio_buffer_t *buf)
{
	if (buf->pos >= buf->size)
	{
		buf->size = fread(buf->buffer, 1, sizeof(buf->buffer), buf->f);
		buf->pos = 0;
		
		if (buf->size == 0)
			return EOF;
	}

	return buf->buffer[buf->pos++];
}

/*small function to read files with stb_image - single-file image loader library.
** downloaded from: https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
** use JPEG, PNG and BMP formats for external UI images.
** */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_BMP
#ifdef LODEPNG_NO_COMPILE_DECODER
	#define STBI_ONLY_PNG
#endif
#ifndef PLATFORM_PSP
#ifndef __psp__
	/* No JPEG on the console.
	 *
	 * With a decoder list of "JPEG, PNG, BMP" (that is the order stb_image
	 * tries them in, see stbi__load_main()), every PNG first goes through the
	 * JPEG probe -- and on the PSP that probe is both expensive (it puts an
	 * 18 KB stbi__jpeg on the stack in the middle of a menu redraw) and
	 * unreliable: the menu art in the paks came back as
	 * "img: gfx/a_mmenu/mainmenu1.png FAILED: no SOI", which is the JPEG
	 * decoder saying the file does not start with FF D8 -- for a file whose
	 * first bytes are 89 50 4E 47 0D 0A 1A 0A, i.e. a perfectly good PNG.
	 *
	 * The game's own interface art is PNG, BMP, PCX and TGA, and it has no
	 * JPEG at all, so the probe is pure liability here: without it, PNG is
	 * tested first and the "no SOI" class of failure cannot happen.  A .jpg
	 * would now report "unknown image type" instead of decoding. */
	#define STBI_ONLY_JPEG
#endif
#endif
/* Third-party headers get the warnings they were written for, not ours.
 *
 * With the JPEG decoder switched off (above) stb_image still compiles one
 * helper nothing calls any more -- stbi__at_eof() is used by the JPEG, PIC,
 * PNM and GIF readers only -- so -Wall reports "defined but not used" for a
 * line of a 7000-line header we do not maintain.  Silencing that one
 * diagnostic around the include is the right level: patching the header means
 * redoing the patch at every stb_image update, and the warning itself is
 * evidence that the PNG-first configuration is the one being compiled. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
/* A decoded picture is copied into the hunk and the decoder's buffer is freed.
 * stb hands back w*h*4 bytes of RGBA; the engine wants them in the hunk because
 * that is where the rest of the loader puts image data. */
static byte *Image_STBIDone (byte *heap, int width, int height)
{
	byte *hunk;

	if (!heap)
		return NULL;

	//this is silly, but we do it for consistency.
	//frankly, most people should be using tga-inside-pk3.
	hunk = Hunk_Alloc(width * height * 4);
	memcpy(hunk, heap, width * height * 4);
	free(heap);

	return hunk;
}

/* Files larger than this are streamed instead of read into memory: the decode
 * needs its own w*h*4 buffer on top of the file, and the port only keeps about
 * ten megabytes of malloc() for pictures.  Every menu picture in this game is a
 * few kilobytes, so the two-megabyte path handles the real work and the stream
 * path stays as a safety net for something huge in a pak. */
#define IMG_STBI_READ_LIMIT (2u * 1024u * 1024u)

#if defined(PLATFORM_PSP) || defined(__psp__)
/* ---- what a picture decode does when the heap says no --------------------
 *
 * A decoded picture is w*h*4 bytes of RGBA from malloc(), and that buffer
 * competes with the picture arena's chunks for the same ten megabytes.  The
 * hardware log of build 08:12 has both numbers on it:
 *
 *     tex: 84 pictures, 2488 KB in use of ...
 *     ... tex 4022 KB at run: f 347
 *     img: gfx/a_sgl/asgl3.bmp FAILED: outofmem (14166 bytes,
 *          sig 42 4D 56 37 00 00 00 00)
 *
 * "outofmem" is stb_image's own word for "malloc() returned NULL", and the
 * file was fine: the whole 14166 bytes were read (its header declares exactly
 * 14166) and they begin with "BM".  The memory that was missing was the one
 * the decoder needed, while the engine's picture cache was still holding
 * megabytes of pictures that nothing had drawn for a long time.
 *
 * So an allocation for a picture that fails now asks the cache first
 * (psp_gl_shim_reclaim_pictures(): evict idle pictures, then hand the arena
 * chunks that became empty back to the heap) and tries once more.  Nothing is
 * retried blindly -- each line in the log names the allocation, its size, how
 * many pictures were released and what the heap looked like on both sides. */
extern int psp_gl_shim_reclaim_pictures (void);
extern unsigned int psp_heap_free (void);

#define IMG_RECLAIM_LOGS 16
static int img_reclaim_logs;

/* `size` is 0 when the failed allocation is the decoder's own (stb allocates
 * w*h*4 internally and does not tell us how big it was). */
static int Image_ReclaimForPicture (const char *what, size_t size)
{
	int released;
	unsigned int before = psp_heap_free (), after;

	released = psp_gl_shim_reclaim_pictures ();
	after = psp_heap_free ();

	if (img_reclaim_logs < IMG_RECLAIM_LOGS)
	{
		extern void psp_tracef (const char *fmt, ...);

		img_reclaim_logs++;

		if (size)
			psp_tracef ("img: %s: %d bytes failed, %d idle pictures "
			            "released, heap free %d -> %d KB",
			            what, (int)size, released,
			            (int)(before / 1024), (int)(after / 1024));
		else
			psp_tracef ("img: %s: the decoder's buffer failed, %d idle "
			            "pictures released, heap free %d -> %d KB",
			            what, released,
			            (int)(before / 1024), (int)(after / 1024));
	}

	return released;
}

/* malloc() for picture work, with the reclaim above in front of the failure. */
static void *Image_AllocPicture (const char *what, size_t size)
{
	void *p = malloc (size);

	if (!p && Image_ReclaimForPicture (what, size) > 0)
		p = malloc (size);

	return p;
}

#define IMG_ALLOC(what, size)	Image_AllocPicture ((what), (size))
#else
#define IMG_ALLOC(what, size)	malloc (size)
#endif

static byte *Image_LoadSTBI(FILE *f, int *width, int *height)
{
	int bytesPerPixel;
	byte *heap = NULL;
	unsigned char *in;
	size_t want, got = 0;

	/* One read, then decode from memory.
	 *
	 * The previous version read the first eight bytes for the log, seeked
	 * back to where it started, and handed the FILE* to
	 * stbi_load_from_file().  The hardware log says why that is not
	 * trustworthy:
	 *
	 *   img: gfx/a_spmenu/aspm2.png FAILED: bad png sig (2023 bytes,
	 *        sig 89 50 4E 47 0D 0A 1A 0A)
	 *
	 * "Not a PNG", about a file whose own first bytes -- read one line
	 * earlier from the same stream -- are the PNG signature exactly.  Two
	 * reads of one stream cannot disagree unless the second one does not
	 * start where the first one did, and the only thing between them is the
	 * seek back.  A stream that came out of a pak is a file handle positioned
	 * at an entry, not a file that begins at zero, so "seek to where I was"
	 * is one operation away from "seek to the start of something else".
	 *
	 * With the file read into memory once, the signature the log prints and
	 * the bytes stb decodes are the same bytes, and a failure is finally what
	 * it claims to be.  It also reads the file once instead of twice. */
	want = (size_t)(com_filesize > 0 ? com_filesize : 0);

	if (want > IMG_STBI_READ_LIMIT)
	{
		/* Too big to hold twice: let stb read the stream itself, as before.
		 * The position question above does not disappear, so the answer is
		 * still named, only without the signature bytes. */
		heap = stbi_load_from_file(f, width, height, &bytesPerPixel, 4);
		fclose(f);

#if defined(PLATFORM_PSP) || defined(__psp__)
		{
			extern void psp_tracef (const char *fmt, ...);

			if (heap)
				psp_tracef("img: %s ok %dx%d (%d bytes, streamed)",
				           loadfilename, *width, *height, (int)want);
			else
				psp_tracef("img: %s FAILED: %s (%d bytes, streamed)",
				           loadfilename,
				           stbi_failure_reason() ? stbi_failure_reason() : "(no reason)",
				           (int)want);
		}
#else
		(void)bytesPerPixel;
#endif

		return Image_STBIDone(heap, *width, *height);
	}

	in = want ? (unsigned char *)IMG_ALLOC(loadfilename, want) : NULL;

	if (in)
		got = fread(in, 1, want, f);

	fclose(f);

#if defined(PLATFORM_PSP) || defined(__psp__)
	{
		extern void psp_tracef (const char *fmt, ...);

		if (!in)
		{
			psp_tracef("img: %s: no %d-byte buffer to read the file into",
			           loadfilename, (int)want);
			return NULL;
		}

		if (got != want)
		{
			/* A short read is its own answer: the size the file system
			 * reported and the bytes it handed over disagree. */
			psp_tracef("img: %s: read %d of %d bytes -- file truncated or "
			           "not where the file system says it is",
			           loadfilename, (int)got, (int)want);
			free(in);
			return NULL;
		}
	}
#endif

	if (!in || !got)
	{
		free(in);
		return NULL;
	}

	heap = stbi_load_from_memory(in, (int)got, width, height, &bytesPerPixel, 4);

#if defined(PLATFORM_PSP) || defined(__psp__)
	/* Only one failure of the decoder is worth a second attempt: its own
	 * allocation coming back empty.  Everything else (a corrupt file, an
	 * unsupported format) says the same thing twice. */
	if (!heap && stbi_failure_reason () && !strcmp (stbi_failure_reason (), "outofmem"))
		if (Image_ReclaimForPicture (loadfilename, 0) > 0)
			heap = stbi_load_from_memory (in, (int)got, width, height,
			                              &bytesPerPixel, 4);
#endif

#if defined(PLATFORM_PSP) || defined(__psp__)
	{
		extern void psp_tracef (const char *fmt, ...);

		if (heap)
			psp_tracef("img: %s ok %dx%d (%d bytes)", loadfilename,
			           *width, *height, (int)got);
		else
			psp_tracef("img: %s FAILED: %s (%d bytes, sig %02X %02X %02X %02X "
			           "%02X %02X %02X %02X)", loadfilename,
			           stbi_failure_reason() ? stbi_failure_reason() : "(no reason)",
			           (int)got,
			           got > 0 ? in[0] : 0, got > 1 ? in[1] : 0,
			           got > 2 ? in[2] : 0, got > 3 ? in[3] : 0,
			           got > 4 ? in[4] : 0, got > 5 ? in[5] : 0,
			           got > 6 ? in[6] : 0, got > 7 ? in[7] : 0);
	}
#else
	(void)bytesPerPixel;
#endif

	free(in);

	return Image_STBIDone(heap, *width, *height);
}

static unsigned int Image_ReadLE32(const byte *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
		((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned short Image_ReadLE16(const byte *p)
{
	return (unsigned short)(p[0] | ((unsigned short)p[1] << 8));
}

/*
================
Image_LoadBMP

Quake UI bitmaps use palette index 255 as their transparent color. STB converts
indexed BMPs to opaque RGBA, so expand 8-bit BMPs here and retain that key.
================
*/
static byte *Image_LoadBMP(FILE *f, int *width, int *height, qboolean *malloced, qboolean quake_transparency)
{
	long file_start = ftell(f);
	byte file_header[14], dib_header[40], palette[256][4];
	unsigned int dib_size, pixel_offset, colors_used, palette_count, compression;
	int32_t bmp_width, bmp_signed_height;
	int bmp_height, top_down;
	size_t row_stride, image_size, pixel_count, output_size;
	byte *row = NULL, *rgba = NULL;
	unsigned int i;

	if (file_start < 0)
		goto fail;
	if (fread(file_header, 1, sizeof(file_header), f) != sizeof(file_header))
		goto fail;
	if (file_header[0] != 'B' || file_header[1] != 'M')
		goto fallback;
	if (fread(dib_header, 1, 4, f) != 4)
		goto fail;

	dib_size = Image_ReadLE32(dib_header);
	if (dib_size < sizeof(dib_header) || com_filesize < 14 ||
		dib_size > (unsigned int)(com_filesize - 14))
		goto fallback;
	if (fread(dib_header + 4, 1, sizeof(dib_header) - 4, f) != sizeof(dib_header) - 4)
		goto fail;

	bmp_width = (int32_t)Image_ReadLE32(dib_header + 4);
	bmp_signed_height = (int32_t)Image_ReadLE32(dib_header + 8);
	pixel_offset = Image_ReadLE32(file_header + 10);
	compression = Image_ReadLE32(dib_header + 16);
	colors_used = Image_ReadLE32(dib_header + 32);
	if (bmp_width <= 0 || bmp_signed_height == 0 || bmp_signed_height == INT32_MIN ||
		Image_ReadLE16(dib_header + 12) != 1 || Image_ReadLE16(dib_header + 14) != 8 || compression != 0)
		goto fallback;

	bmp_height = bmp_signed_height < 0 ? -bmp_signed_height : bmp_signed_height;
	top_down = bmp_signed_height < 0;
	palette_count = colors_used ? colors_used : 256;
	if (palette_count > 256 || (size_t)bmp_width > (size_t)-1 - 3)
		goto fallback;
	row_stride = ((size_t)bmp_width + 3) & ~(size_t)3;
	if ((size_t)bmp_height > (size_t)-1 / row_stride ||
		(size_t)bmp_width > (size_t)-1 / (size_t)bmp_height)
		goto fallback;
	image_size = row_stride * (size_t)bmp_height;
	pixel_count = (size_t)bmp_width * (size_t)bmp_height;
	if (pixel_count > (size_t)-1 / 4)
		goto fallback;
	output_size = pixel_count * 4;

	if (pixel_offset < 14 + dib_size + palette_count * 4 ||
		com_filesize < 0 || pixel_offset > (unsigned int)com_filesize ||
		image_size > (size_t)(com_filesize - (int)pixel_offset))
		goto fallback;

	memset(palette, 0, sizeof(palette));
	if (fseek(f, file_start + 14 + (long)dib_size, SEEK_SET) != 0)
		goto fail;
	for (i = 0; i < palette_count; i++)
	{
		byte entry[4];
		if (fread(entry, 1, sizeof(entry), f) != sizeof(entry))
			goto fail;
		palette[i][0] = entry[2];
		palette[i][1] = entry[1];
		palette[i][2] = entry[0];
		palette[i][3] = 255;
	}

	row = (byte *)IMG_ALLOC(loadfilename, row_stride);
	rgba = (byte *)IMG_ALLOC(loadfilename, output_size);
	if (!row || !rgba)
		goto fail;
	if (fseek(f, file_start + (long)pixel_offset, SEEK_SET) != 0)
		goto fail;

	for (i = 0; i < (unsigned int)bmp_height; i++)
	{
		size_t x;
		unsigned int output_y = top_down ? i : (unsigned int)bmp_height - 1 - i;
		byte *dst = rgba + (size_t)output_y * (size_t)bmp_width * 4;
		if (fread(row, 1, row_stride, f) != row_stride)
			goto fail;
		for (x = 0; x < (size_t)bmp_width; x++)
		{
			unsigned int color = row[x];
			if (color >= palette_count)
				goto fallback;
			*dst++ = palette[color][0];
			*dst++ = palette[color][1];
			*dst++ = palette[color][2];
			*dst++ = (quake_transparency && color == 255) ? 0 : 255;
		}
	}

	free(row);
	fclose(f);
	*width = bmp_width;
	*height = bmp_height;
	*malloced = true;
	return rgba;

fallback:
	free(row);
	free(rgba);
	if (file_start < 0 || fseek(f, file_start, SEEK_SET) != 0)
		goto fail;
	return Image_LoadSTBI(f, width, height);

fail:
	free(row);
	free(rgba);
	fclose(f);
	return NULL;
}

byte *Image_LoadPNG(FILE *f, int *width, int *height, qboolean *malloced)
{
#ifdef LODEPNG_NO_COMPILE_DECODER
	return Image_LoadSTBI (f, width, height);
#else
	unsigned w = 0, h = 0;
	unsigned err;
	unsigned char *out = NULL, *in;
	size_t insize = com_filesize;

	in = malloc(com_filesize);
	if (!in)
	{
		fclose(f);
		return NULL;
	}
	if (com_filesize == fread(in, 1, com_filesize, f))
	{
		err = lodepng_decode32(&out, &w, &h, in, insize);
		if (err || !out)
		{
			/* Some pictures decode and some do not, and from a
			 * screenshot the two are indistinguishable: both arrive
			 * at the texture upload as a 1x1 picture.  So the failure
			 * says what lodepng objected to, and the IHDR fields say
			 * what the file actually is -- a PNG whose header is
			 * 640x400 of 16-bit interlaced RGBA is a different
			 * problem from one that is only 40 bytes long. */
#if defined(PLATFORM_PSP) || defined(__psp__)
			{
				extern void psp_tracef (const char *fmt, ...);
				unsigned pw = 0, ph = 0, depth = 0, ctype = 0, inter = 0;

				if (insize > 29 && !memcmp(in + 12, "IHDR", 4))
				{
					pw = ((unsigned)in[16] << 24) | ((unsigned)in[17] << 16) |
					     ((unsigned)in[18] << 8) | (unsigned)in[19];
					ph = ((unsigned)in[20] << 24) | ((unsigned)in[21] << 16) |
					     ((unsigned)in[22] << 8) | (unsigned)in[23];
					depth = in[24];
					ctype = in[25];
					inter = in[28];
				}

				psp_tracef("png: %s rc=%d, %d bytes, IHDR %ux%u depth %u "
				           "ctype %u interlace %u",
				           loadfilename, (int)err, (int)insize,
				           pw, ph, depth, ctype, inter);
			}
#endif
			free(out);
			out = NULL;
		}
		else
		{
			/* lodepng hands the size back through its own w/h.  This
			 * function used to ignore them, so every PNG arrived at
			 * TexMgr_LoadImage() as a 0x0 picture: the menu logo and
			 * the menu background were loaded, uploaded as a single
			 * texel and then drawn with no area at all. */
			*width = (int)w;
			*height = (int)h;
			*malloced = true;
		}
	}
	free(in);
	fclose(f);
	return out;
#endif
}



/*spike -- start of dds loader
note that support for loading compressed mipchains is elsewhere. not all those listed here will work.
formats are dependant upon the user's hardware.
mip chains must be complete (no single-level images)
non-2d images are not supported.
dx10 dds files ARE supported (but probably won't be generated by your tools...), but bc7 is there if you want it (and the user's gpu drivers support it).
*/
typedef struct {
	unsigned int dwSize;
	unsigned int dwFlags;
	unsigned int dwFourCC;

	unsigned int bitcount;
	unsigned int redmask;
	unsigned int greenmask;
	unsigned int bluemask;
	unsigned int alphamask;
} ddspixelformat_t;

typedef struct {
	unsigned int magic;
	unsigned int dwSize;
	unsigned int dwFlags;
	unsigned int dwHeight;
	unsigned int dwWidth;
	unsigned int dwPitchOrLinearSize;
	unsigned int dwDepth;
	unsigned int dwMipMapCount;
	unsigned int dwReserved1[11];
	ddspixelformat_t ddpfPixelFormat;
	unsigned int ddsCaps[4];
	unsigned int dwReserved2;
} ddsheader_t;
typedef struct {
	unsigned int dxgiformat;
	unsigned int resourcetype; //0=unknown, 1=buffer, 2=1d, 3=2d, 4=3d
	unsigned int miscflag;	//singular... yeah. 4=cubemap.
	unsigned int arraysize;
	unsigned int miscflags2;
} dds10header_t;

static byte *Image_LoadDDS(FILE *f, int *width, int *height, enum srcformat *fmt)
{
	const char *fname = loadfilename;	//parameters as globals is a shitty practise, but I'm too lazy to fix up the other loaders too.
	int nummips;
	int mipnum;
	int datasize;
	byte *ret;
	enum srcformat encoding = SRC_EXTERNAL;

	ddsheader_t fmtheader;
	dds10header_t fmt10header;

	if (fread(&fmtheader, 1, sizeof(fmtheader), f) != sizeof(fmtheader)) // woods
		goto fail;

	if (fmtheader.magic != (('D'<<0)|('D'<<8)|('S'<<16)|(' '<<24)))
		goto fail;

	fmtheader.dwSize += sizeof(fmtheader.magic);
	if (fmtheader.dwSize != sizeof(fmtheader))
		goto fail;	//corrupt/different version
	memset(&fmt10header, 0, sizeof(fmt10header));

	fmt10header.arraysize = (fmtheader.ddsCaps[1] & 0x200)?6:1; //cubemaps need 6 faces...

	nummips = fmtheader.dwMipMapCount;
	if (nummips < 1)
		nummips = 1;

	if (!(fmtheader.ddpfPixelFormat.dwFlags & 4))
	{
#define IsPacked(bits,r,g,b,a)	fmtheader.ddpfPixelFormat.bitcount==bits&&fmtheader.ddpfPixelFormat.redmask==r&&fmtheader.ddpfPixelFormat.greenmask==g&&fmtheader.ddpfPixelFormat.bluemask==b&&fmtheader.ddpfPixelFormat.alphamask==a
		if (IsPacked(24, 0xff0000, 0x00ff00, 0x0000ff, 0))
			encoding = TexMgr_FormatForName("BGR8");
		else if (IsPacked(24, 0x000000, 0x00ff00, 0xff0000, 0))
			encoding = TexMgr_FormatForName("RGB8");
		else if (IsPacked(32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000))
			encoding = TexMgr_FormatForName("BGRA8");
		else if (IsPacked(32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000))
			encoding = TexMgr_FormatForName("RGBA8");
		else if (IsPacked(32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0))
			encoding = TexMgr_FormatForName("BGRX8");
		else if (IsPacked(32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0))
			encoding = TexMgr_FormatForName("RGBX8");
		else if (IsPacked(32, 0x000003ff, 0x000ffc00, 0x3ff00000, 0xc0000000))
			encoding = TexMgr_FormatForName("A2BGR10");
		else if (IsPacked(16, 0xf800, 0x07e0, 0x001f, 0))
			encoding = TexMgr_FormatForName("RGB565");
		else if (IsPacked(16, 0xf800, 0x07c0, 0x003e, 0x0001))
			encoding = TexMgr_FormatForName("RGBA5551");
		else if (IsPacked(16, 0x7c00, 0x03e0, 0x001f, 0x8000))
			encoding = TexMgr_FormatForName("ARGB1555");
		else if (IsPacked(16, 0xf000, 0x0f00, 0x00f0, 0x000f))
			encoding = TexMgr_FormatForName("RGBA4444");
		else if (IsPacked(16, 0x0f00, 0x00f0, 0x000f, 0xf000))
			encoding = TexMgr_FormatForName("ARGB4444");
		else if (IsPacked( 8, 0x000000ff, 0x00000000, 0x00000000, 0x00000000))
			encoding = (fmtheader.ddpfPixelFormat.dwFlags&0x20000)?TexMgr_FormatForName("L8"):TexMgr_FormatForName("R8");
		else if (IsPacked(16, 0x000000ff, 0x00000000, 0x00000000, 0x0000ff00))
			encoding = TexMgr_FormatForName("L8A8");
		if (encoding == SRC_EXTERNAL)	//used as an error code
		{
			Con_Printf("Unsupported non-fourcc dds in %s\n", fname);
			Con_Printf(" bits: %u\n", fmtheader.ddpfPixelFormat.bitcount);
			Con_Printf("  red: %08x\n", fmtheader.ddpfPixelFormat.redmask);
			Con_Printf("green: %08x\n", fmtheader.ddpfPixelFormat.greenmask);
			Con_Printf(" blue: %08x\n", fmtheader.ddpfPixelFormat.bluemask);
			Con_Printf("alpha: %08x\n", fmtheader.ddpfPixelFormat.alphamask);
			Con_Printf(" used: %08x\n", fmtheader.ddpfPixelFormat.redmask^fmtheader.ddpfPixelFormat.greenmask^fmtheader.ddpfPixelFormat.bluemask^fmtheader.ddpfPixelFormat.alphamask);
			goto fail;
		}
#undef IsPacked
	}
	else if (*(int*)&fmtheader.ddpfPixelFormat.dwFourCC == (('D'<<0)|('X'<<8)|('T'<<16)|('1'<<24)))
		encoding = TexMgr_FormatForName("BC1_RGBA");	//alpha or not? Assume yes, and let the drivers decide.
	else if (*(int*)&fmtheader.ddpfPixelFormat.dwFourCC == (('D'<<0)|('X'<<8)|('T'<<16)|('2'<<24)))	//dx3 with premultiplied alpha
	{
//		if (!(tex->flags & IF_PREMULTIPLYALPHA))
//			return false;
		encoding = TexMgr_FormatForName("BC2_RGBA");
	}
	else if (*(int*)&fmtheader.ddpfPixelFormat.dwFourCC == (('D'<<0)|('X'<<8)|('T'<<16)|('3'<<24)))
	{
//		if (tex->flags & IF_PREMULTIPLYALPHA)
//			return false;
		encoding = TexMgr_FormatForName("BC2_RGBA");
	}
	else if (*(int*)&fmtheader.ddpfPixelFormat.dwFourCC == (('D'<<0)|('X'<<8)|('T'<<16)|('4'<<24)))	//dx5 with premultiplied alpha
	{
//		if (!(tex->flags & IF_PREMULTIPLYALPHA))
//			return false;
		encoding = TexMgr_FormatForName("BC3_RGBA");
	}
	else if (*(int*)&fmtheader.ddpfPixelFormat.dwFourCC == (('D'<<0)|('X'<<8)|('T'<<16)|('5'<<24)))
	{
//		if (tex->flags & IF_PREMULTIPLYALPHA)
//			return false;
		encoding = TexMgr_FormatForName("BC3_RGBA");
	}
	else if (*(int*)&fmtheader.ddpfPixelFormat.dwFourCC == (('A'<<0)|('T'<<8)|('I'<<16)|('1'<<24))
		||   *(int*)&fmtheader.ddpfPixelFormat.dwFourCC == (('B'<<0)|('C'<<8)|('4'<<16)|('U'<<24)))
		encoding = TexMgr_FormatForName("BC4_R");
	else if (*(int*)&fmtheader.ddpfPixelFormat.dwFourCC == (('A'<<0)|('T'<<8)|('I'<<16)|('2'<<24))
		||   *(int*)&fmtheader.ddpfPixelFormat.dwFourCC == (('B'<<0)|('C'<<8)|('5'<<16)|('U'<<24)))
		encoding = TexMgr_FormatForName("BC5_RG");
	else if (*(int*)&fmtheader.ddpfPixelFormat.dwFourCC == (('B'<<0)|('C'<<8)|('4'<<16)|('S'<<24)))
		encoding = TexMgr_FormatForName("BC4_R_SNORM");
	else if (*(int*)&fmtheader.ddpfPixelFormat.dwFourCC == (('B'<<0)|('C'<<8)|('5'<<16)|('S'<<24)))
		encoding = TexMgr_FormatForName("BC5_RG_SNORM");
	else if (*(int*)&fmtheader.ddpfPixelFormat.dwFourCC == (('E'<<0)|('T'<<8)|('C'<<16)|('2'<<24)))
		encoding = TexMgr_FormatForName("ETC2_RGB");
	else if (*(int*)&fmtheader.ddpfPixelFormat.dwFourCC == (('D'<<0)|('X'<<8)|('1'<<16)|('0'<<24)))
	{
		//this has some weird extra header with dxgi format types.
		if (fread(&fmt10header, 1, sizeof(fmt10header), f) != sizeof(fmt10header)) // woods
			goto fail;
		switch(fmt10header.dxgiformat)
		{
//		case 0x0/*DXGI_FORMAT_UNKNOWN*/:				encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x1/*DXGI_FORMAT_R32G32B32A32_TYPELESS*/:	encoding = TexMgr_FormatForName("INVALID");			break;
		case 0x2/*DXGI_FORMAT_R32G32B32A32_FLOAT*/:		encoding = TexMgr_FormatForName("RGBA32F");			break;
//		case 0x3/*DXGI_FORMAT_R32G32B32A32_UINT*/:		encoding = TexMgr_FormatForName("RGBA32_UINT");		break;
//		case 0x4/*DXGI_FORMAT_R32G32B32A32_SINT*/:		encoding = TexMgr_FormatForName("RGBA32_SINT");		break;
//		case 0x5/*DXGI_FORMAT_R32G32B32_TYPELESS*/:		encoding = TexMgr_FormatForName("INVALID");			break;
		case 0x6/*DXGI_FORMAT_R32G32B32_FLOAT*/:		encoding = TexMgr_FormatForName("RGB32F");			break;
//		case 0x7/*DXGI_FORMAT_R32G32B32_UINT*/:			encoding = TexMgr_FormatForName("RGB32_UINT");		break;
//		case 0x8/*DXGI_FORMAT_R32G32B32_SINT*/:			encoding = TexMgr_FormatForName("RGB32_SINT");		break;
//		case 0x9/*DXGI_FORMAT_R16G16B16A16_TYPELESS*/:	encoding = TexMgr_FormatForName("INVALID");			break;
		case 0xa/*DXGI_FORMAT_R16G16B16A16_FLOAT*/:		encoding = TexMgr_FormatForName("RGBA16F");			break;
		case 0xb/*DXGI_FORMAT_R16G16B16A16_UNORM*/:		encoding = TexMgr_FormatForName("RGBA16");			break;
//		case 0xc/*DXGI_FORMAT_R16G16B16A16_UINT*/:		encoding = TexMgr_FormatForName("RGBA16_UINT");		break;
//		case 0xd/*DXGI_FORMAT_R16G16B16A16_SNORM*/:		encoding = TexMgr_FormatForName("RGBA16_SNORM");	break;
//		case 0xe/*DXGI_FORMAT_R16G16B16A16_SINT*/:		encoding = TexMgr_FormatForName("RGBA16_SINT");		break;
//		case 0xf/*DXGI_FORMAT_R32G32_TYPELESS*/:		encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x10/*DXGI_FORMAT_R32G32_FLOAT*/:			encoding = TexMgr_FormatForName("RG32F");			break;
//		case 0x11/*DXGI_FORMAT_R32G32_UINT*/:			encoding = TexMgr_FormatForName("RG32_UINT");		break;
//		case 0x12/*DXGI_FORMAT_R32G32_SINT*/:			encoding = TexMgr_FormatForName("RG32_SINT");		break;
//		case 0x13/*DXGI_FORMAT_R32G8X24_TYPELESS*/:		encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x14/*DXGI_FORMAT_D32_FLOAT_S8X24_UINT*/:	encoding = TexMgr_FormatForName("DEPTH32_8");		break;
//		case 0x15/*DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS*/:encoding = TexMgr_FormatForName("INVALID");		break;
//		case 0x16/*DXGI_FORMAT_X32_TYPELESS_G8X24_UINT*/:encoding = TexMgr_FormatForName("INVALID");		break;
//		case 0x17/*DXGI_FORMAT_R10G10B10A2_TYPELESS*/:	encoding = TexMgr_FormatForName("INVALID");			break;
		case 0x18/*DXGI_FORMAT_R10G10B10A2_UNORM*/:		encoding = TexMgr_FormatForName("A2BGR10");			break;
//		case 0x19/*DXGI_FORMAT_R10G10B10A2_UINT*/:		encoding = TexMgr_FormatForName("A2BGR10_UINT");	break;
		case 0x1a/*DXGI_FORMAT_R11G11B10_FLOAT*/:		encoding = TexMgr_FormatForName("B10G11R11F");		break;
//		case 0x1b/*DXGI_FORMAT_R8G8B8A8_TYPELESS*/:		encoding = TexMgr_FormatForName("INVALID");			break;
		case 0x1c/*DXGI_FORMAT_R8G8B8A8_UNORM*/:		encoding = TexMgr_FormatForName("RGBA8");			break;
		case 0x1d/*DXGI_FORMAT_R8G8B8A8_UNORM_SRGB*/:	encoding = TexMgr_FormatForName("RGBA8_SRGB");		break;
//		case 0x1e/*DXGI_FORMAT_R8G8B8A8_UINT*/:			encoding = TexMgr_FormatForName("RGBA8_UINT");		break;
//		case 0x1f/*DXGI_FORMAT_R8G8B8A8_SNORM*/:		encoding = TexMgr_FormatForName("RGBA8_SNORM");		break;
//		case 0x20/*DXGI_FORMAT_R8G8B8A8_SINT*/:			encoding = TexMgr_FormatForName("RGBA8_SINT");		break;
//		case 0x21/*DXGI_FORMAT_R16G16_TYPELESS*/:		encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x22/*DXGI_FORMAT_R16G16_FLOAT*/:			encoding = TexMgr_FormatForName("RG16F");			break;
//		case 0x23/*DXGI_FORMAT_R16G16_UNORM*/:			encoding = TexMgr_FormatForName("RG16");			break;
//		case 0x24/*DXGI_FORMAT_R16G16_UINT*/:			encoding = TexMgr_FormatForName("RG16_UINT");		break;
//		case 0x25/*DXGI_FORMAT_R16G16_SNORM*/:			encoding = TexMgr_FormatForName("RG16_SNORM");		break;
//		case 0x26/*DXGI_FORMAT_R16G16_SINT*/:			encoding = TexMgr_FormatForName("RG16_SINT");		break;
//		case 0x27/*DXGI_FORMAT_R32_TYPELESS*/:			encoding = TexMgr_FormatForName("INVALID");			break;
		case 0x28/*DXGI_FORMAT_D32_FLOAT*/:				encoding = TexMgr_FormatForName("DEPTH32");			break;
		case 0x29/*DXGI_FORMAT_R32_FLOAT*/:				encoding = TexMgr_FormatForName("R32F");			break;
//		case 0x2a/*DXGI_FORMAT_R32_UINT*/:				encoding = TexMgr_FormatForName("R32_UINT");		break;
//		case 0x2b/*DXGI_FORMAT_R32_SINT*/:				encoding = TexMgr_FormatForName("R32_SINT");		break;
//		case 0x2c/*DXGI_FORMAT_R24G8_TYPELESS*/:		encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x2d/*DXGI_FORMAT_D24_UNORM_S8_UINT*/:		encoding = TexMgr_FormatForName("DEPTH24_8");		break;
//		case 0x2e/*DXGI_FORMAT_R24_UNORM_X8_TYPELESS*/:	encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x2f/*DXGI_FORMAT_X24_TYPELESS_G8_UINT*/:	encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x30/*DXGI_FORMAT_R8G8_TYPELESS*/:			encoding = TexMgr_FormatForName("INVALID");			break;
		case 0x31/*DXGI_FORMAT_R8G8_UNORM*/:			encoding = TexMgr_FormatForName("RG8");				break;
//		case 0x32/*DXGI_FORMAT_R8G8_UINT*/:				encoding = TexMgr_FormatForName("RG8_UINT");		break;
		case 0x33/*DXGI_FORMAT_R8G8_SNORM*/:			encoding = TexMgr_FormatForName("RG8_SNORM");		break;
//		case 0x34/*DXGI_FORMAT_R8G8_SINT*/:				encoding = TexMgr_FormatForName("RG8_SINT");		break;
//		case 0x35/*DXGI_FORMAT_R16_TYPELESS*/:			encoding = TexMgr_FormatForName("INVALID");			break;
		case 0x36/*DXGI_FORMAT_R16_FLOAT*/:				encoding = TexMgr_FormatForName("R16F");			break;
		case 0x37/*DXGI_FORMAT_D16_UNORM*/:				encoding = TexMgr_FormatForName("DEPTH16");			break;
		case 0x38/*DXGI_FORMAT_R16_UNORM*/:				encoding = TexMgr_FormatForName("R16");				break;
//		case 0x39/*DXGI_FORMAT_R16_UINT*/:				encoding = TexMgr_FormatForName("R16_UINT");		break;
//		case 0x3a/*DXGI_FORMAT_R16_SNORM*/:				encoding = TexMgr_FormatForName("R16_SNORM");		break;
//		case 0x3b/*DXGI_FORMAT_R16_SINT*/:				encoding = TexMgr_FormatForName("R16_SINT");		break;
//		case 0x3c/*DXGI_FORMAT_R8_TYPELESS*/:			encoding = TexMgr_FormatForName("INVALID");			break;
		case 0x3d/*DXGI_FORMAT_R8_UNORM*/:				encoding = TexMgr_FormatForName("R8");				break;
//		case 0x3e/*DXGI_FORMAT_R8_UINT*/:				encoding = TexMgr_FormatForName("R8_UINT");			break;
		case 0x3f/*DXGI_FORMAT_R8_SNORM*/:				encoding = TexMgr_FormatForName("R8_SNORM");		break;
//		case 0x40/*DXGI_FORMAT_R8_SINT*/:				encoding = TexMgr_FormatForName("R8_SINT");			break;
		case 0x41/*DXGI_FORMAT_A8_UNORM*/:				encoding = TexMgr_FormatForName("A8");				break;
//		case 0x42/*DXGI_FORMAT_R1_UNORM*/:				encoding = TexMgr_FormatForName("R1");				break;
		case 0x43/*DXGI_FORMAT_R9G9B9E5_SHAREDEXP*/:	encoding = TexMgr_FormatForName("E5BGR9");			break;
//		case 0x44/*DXGI_FORMAT_R8G8_B8G8_UNORM*/:		encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x45/*DXGI_FORMAT_G8R8_G8B8_UNORM*/:		encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x46/*DXGI_FORMAT_BC1_TYPELESS*/:			encoding = TexMgr_FormatForName("INVALID");			break;
		case 0x47/*DXGI_FORMAT_BC1_UNORM*/:				encoding = TexMgr_FormatForName("BC1_RGBA");		break;
		case 0x48/*DXGI_FORMAT_BC1_UNORM_SRGB*/:		encoding = TexMgr_FormatForName("BC1_RGBA_SRGB");	break;
//		case 0x49/*DXGI_FORMAT_BC2_TYPELESS*/:			encoding = TexMgr_FormatForName("INVALID");			break;
		case 0x4a/*DXGI_FORMAT_BC2_UNORM*/:				encoding = TexMgr_FormatForName("BC2_RGBA");		break;
		case 0x4b/*DXGI_FORMAT_BC2_UNORM_SRGB*/:		encoding = TexMgr_FormatForName("BC2_RGBA_SRGB");	break;
//		case 0x4c/*DXGI_FORMAT_BC3_TYPELESS*/:			encoding = TexMgr_FormatForName("INVALID");			break;
		case 0x4d/*DXGI_FORMAT_BC3_UNORM*/:				encoding = TexMgr_FormatForName("BC3_RGBA");		break;
		case 0x4e/*DXGI_FORMAT_BC3_UNORM_SRGB*/:		encoding = TexMgr_FormatForName("BC3_RGBA_SRGB");	break;
//		case 0x4f/*DXGI_FORMAT_BC4_TYPELESS*/:			encoding = TexMgr_FormatForName("INVALID");			break;
		case 0x50/*DXGI_FORMAT_BC4_UNORM*/:				encoding = TexMgr_FormatForName("BC4_R");			break;
		case 0x51/*DXGI_FORMAT_BC4_SNORM*/:				encoding = TexMgr_FormatForName("BC4_R_SNORM");		break;
//		case 0x52/*DXGI_FORMAT_BC5_TYPELESS*/:			encoding = TexMgr_FormatForName("INVALID");			break;
		case 0x53/*DXGI_FORMAT_BC5_UNORM*/:				encoding = TexMgr_FormatForName("BC5_RG");			break;
		case 0x54/*DXGI_FORMAT_BC5_SNORM*/:				encoding = TexMgr_FormatForName("BC5_RG_SNORM");	break;
		case 0x55/*DXGI_FORMAT_B5G6R5_UNORM*/:			encoding = TexMgr_FormatForName("RGB565");			break;
		case 0x56/*DXGI_FORMAT_B5G5R5A1_UNORM*/:		encoding = TexMgr_FormatForName("ARGB1555");		break;
		case 0x57/*DXGI_FORMAT_B8G8R8A8_UNORM*/:		encoding = TexMgr_FormatForName("BGRA8");			break;
		case 0x58/*DXGI_FORMAT_B8G8R8X8_UNORM*/:		encoding = TexMgr_FormatForName("BGRX8");			break;
//		case 0x59/*DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM*/:encoding = TexMgr_FormatForName("INVALID");		break;
//		case 0x5a/*DXGI_FORMAT_B8G8R8A8_TYPELESS*/:		encoding = TexMgr_FormatForName("INVALID");			break;
		case 0x5b/*DXGI_FORMAT_B8G8R8A8_UNORM_SRGB*/:	encoding = TexMgr_FormatForName("BGRA8_SRGB");		break;
//		case 0x5c/*DXGI_FORMAT_B8G8R8X8_TYPELESS*/:		encoding = TexMgr_FormatForName("INVALID");			break;
		case 0x5d/*DXGI_FORMAT_B8G8R8X8_UNORM_SRGB*/:	encoding = TexMgr_FormatForName("BGRX8_SRGB");		break;
//		case 0x5e/*DXGI_FORMAT_BC6H_TYPELESS*/:			encoding = TexMgr_FormatForName("INVALID");			break;
		case 0x5f/*DXGI_FORMAT_BC6H_UF16*/:				encoding = TexMgr_FormatForName("BC6_RGB_UFLOAT");	break;
		case 0x60/*DXGI_FORMAT_BC6H_SF16*/:				encoding = TexMgr_FormatForName("BC6_RGB_SFLOAT");	break;
//		case 0x61/*DXGI_FORMAT_BC7_TYPELESS*/:			encoding = TexMgr_FormatForName("INVALID");			break;
		case 0x62/*DXGI_FORMAT_BC7_UNORM*/:				encoding = TexMgr_FormatForName("BC7_RGBA");		break;
		case 0x63/*DXGI_FORMAT_BC7_UNORM_SRGB*/:		encoding = TexMgr_FormatForName("BC7_RGBA_SRGB");	break;
//		case 0x64/*DXGI_FORMAT_AYUV*/:					encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x65/*DXGI_FORMAT_Y410*/:					encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x66/*DXGI_FORMAT_Y416*/:					encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x67/*DXGI_FORMAT_NV12*/:					encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x68/*DXGI_FORMAT_P010*/:					encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x69/*DXGI_FORMAT_P016*/:					encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x6a/*DXGI_FORMAT_420_OPAQUE*/:			encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x6b/*DXGI_FORMAT_YUY2*/:					encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x6c/*DXGI_FORMAT_Y210*/:					encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x6d/*DXGI_FORMAT_Y216*/:					encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x6e/*DXGI_FORMAT_NV11*/:					encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x6f/*DXGI_FORMAT_AI44*/:					encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x70/*DXGI_FORMAT_IA44*/:					encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x71/*DXGI_FORMAT_P8*/:					encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x72/*DXGI_FORMAT_A8P8*/:					encoding = TexMgr_FormatForName("INVALID");			break;
		case 0x73/*DXGI_FORMAT_B4G4R4A4_UNORM*/:		encoding = TexMgr_FormatForName("ARGB4444");		break;
//		case 0x82/*DXGI_FORMAT_P208*/:					encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x83/*DXGI_FORMAT_V208*/:					encoding = TexMgr_FormatForName("INVALID");			break;
//		case 0x84/*DXGI_FORMAT_V408*/:					encoding = TexMgr_FormatForName("INVALID");			break;
		case 134:	encoding = TexMgr_FormatForName("ASTC_4X4");		break;
		case 135:	encoding = TexMgr_FormatForName("ASTC_4X4_SRGB");	break;
		case 138:	encoding = TexMgr_FormatForName("ASTC_5X4");		break;
		case 139:	encoding = TexMgr_FormatForName("ASTC_5X4_SRGB");	break;
		case 142:	encoding = TexMgr_FormatForName("ASTC_5X5");		break;
		case 143:	encoding = TexMgr_FormatForName("ASTC_5X5_SRGB");	break;
		case 146:	encoding = TexMgr_FormatForName("ASTC_6X5");		break;
		case 147:	encoding = TexMgr_FormatForName("ASTC_6X5_SRGB");	break;
		case 150:	encoding = TexMgr_FormatForName("ASTC_6X6");		break;
		case 151:	encoding = TexMgr_FormatForName("ASTC_6X6_SRGB");	break;
		case 154:	encoding = TexMgr_FormatForName("ASTC_8X5");		break;
		case 155:	encoding = TexMgr_FormatForName("ASTC_8X5_SRGB");	break;
		case 158:	encoding = TexMgr_FormatForName("ASTC_8X6");		break;
		case 159:	encoding = TexMgr_FormatForName("ASTC_8X6_SRGB");	break;
		case 162:	encoding = TexMgr_FormatForName("ASTC_8X8");		break;
		case 163:	encoding = TexMgr_FormatForName("ASTC_8X8_SRGB");	break;
		case 166:	encoding = TexMgr_FormatForName("ASTC_10X5");		break;
		case 167:	encoding = TexMgr_FormatForName("ASTC_10X5_SRGB");	break;
		case 170:	encoding = TexMgr_FormatForName("ASTC_10X6");		break;
		case 171:	encoding = TexMgr_FormatForName("ASTC_10X6_SRGB");	break;
		case 174:	encoding = TexMgr_FormatForName("ASTC_10X8");		break;
		case 175:	encoding = TexMgr_FormatForName("ASTC_10X8_SRGB");	break;
		case 178:	encoding = TexMgr_FormatForName("ASTC_10X10");		break;
		case 179:	encoding = TexMgr_FormatForName("ASTC_10X10_SRGB");	break;
		case 182:	encoding = TexMgr_FormatForName("ASTC_12X10");		break;
		case 183:	encoding = TexMgr_FormatForName("ASTC_12X10_SRGB");	break;
		case 186:	encoding = TexMgr_FormatForName("ASTC_12X12");		break;
		case 187:	encoding = TexMgr_FormatForName("ASTC_12X12_SRGB");	break;

		default:
			Con_Printf("Unsupported dds10 dxgi in %s - %u\n", fname, fmt10header.dxgiformat);
			goto fail;
		}
	}
	if (encoding == SRC_EXTERNAL)	//used as an error code
	{
		Con_Printf("Unsupported encoding in %s - \"%c%c%c%c\"\n", fname,
			((char*)&fmtheader.ddpfPixelFormat.dwFourCC)[0],
			((char*)&fmtheader.ddpfPixelFormat.dwFourCC)[1],
			((char*)&fmtheader.ddpfPixelFormat.dwFourCC)[2],
			((char*)&fmtheader.ddpfPixelFormat.dwFourCC)[3]);
		goto fail;
	}

	if ((fmtheader.ddsCaps[1] & 0x200) && (fmtheader.ddsCaps[1] & 0xfc00) != 0xfc00)
		goto fail;	//cubemap without all 6 faces defined.

	if (fmtheader.ddsCaps[1] & 0x200)
	{
		if (fmt10header.arraysize % 6)	//weird number of faces.
			goto fail;

		if (fmt10header.arraysize == 6)
		{
			//layers = 6;
			goto fail;	//don't try to load cubemaps.
		}
		else
		{
			//layers = fmt10header.arraysize;
			goto fail;	//don't try to load cubemap arrays.
		}
	}
	else if (fmtheader.ddsCaps[1] & 0x200000)
	{
		if (fmt10header.arraysize != 1)	//no 2d arrays
			goto fail;
		goto fail;	//don't try to load 3d textures.
	}
	else
	{
		if (fmt10header.arraysize == 1)
			;	//yay, we can load 2d images.
		else
			goto fail;	//don't try to load 2d arrays.
		//layers = fmt10header.arraysize;
	}

	for (mipnum = 0; ; mipnum++)
	{
		int mipwidth = fmtheader.dwWidth >> mipnum;
		int mipheight = fmtheader.dwHeight >> mipnum;
		if (!mipwidth && !mipheight)
			break;
		mipwidth = q_max(1,mipwidth);	//include the 1*1 mip with non-square textures.
		mipheight = q_max(1,mipheight);
	}
	if (mipnum != nummips)
	{
		Con_Printf("%s: dds with incomplete mip chain\n", fname);
		goto fail;
	}

	datasize = TexMgr_ImageSize(fmtheader.dwWidth, fmtheader.dwHeight, encoding);
	if (!datasize)
		goto fail;	//werid/unsupported

	//just read the mipchain into a new bit of memory and return that.
	//note that layers and mips are awkward, but we don't support layers here so its just a densely packed pyramid.
	ret = (byte *) Hunk_Alloc (datasize);
	if (fread(ret, 1, datasize, f) != datasize) // woods
	{
		goto fail;
	}

	fclose(f);

	*width = fmtheader.dwWidth;
	*height = fmtheader.dwHeight;
	*fmt = encoding;
	return ret;

fail:
	fclose(f);
	return NULL;
}
/*spike -- end of dds loader*/


/*
============
Image_LoadImage

returns a pointer to hunk allocated data. either RGBA8 or a compressed mip chain.

TODO: search order: tga png jpg pcx lmp
============
*/
byte *Image_LoadImage (const char *name, int *width, int *height, enum srcformat *fmt, qboolean *malloced)
{
	FILE	*f;
	char *prefixes[3] = {"", "textures/", "textures/"};
	int i;

	const char *origname = name;

	*malloced = false;
	*fmt = SRC_RGBA;

	for (i = 0; i < sizeof(prefixes)/sizeof(prefixes[0]); i++)
	{
		if (i == 2)	//last resort...
		{
			name = COM_SkipPath(name);
			if (origname == name)
				continue;	//no point trying.
		}

		if (strstr(name, "progs/player.mdl_") && !gl_load24bit_skins.value) // woods #loadskins
			break;

		q_snprintf (loadfilename, sizeof(loadfilename), "%s%s.dds", prefixes[i], name);
		IMG_MARK(loadfilename);
		COM_FOpenFile (loadfilename, &f, NULL);
		if (f)
			return Image_LoadDDS (f, width, height, fmt);

		q_snprintf (loadfilename, sizeof(loadfilename), "%s%s.tga", prefixes[i], name);
		IMG_MARK(loadfilename);
		COM_FOpenFile (loadfilename, &f, NULL);
		if (f)
			return Image_LoadTGA (f, width, height);

		q_snprintf (loadfilename, sizeof(loadfilename), "%s%s.png", prefixes[i], name);
		IMG_MARK(loadfilename);
		COM_FOpenFile (loadfilename, &f, NULL);
		if (f)
			return Image_LoadPNG (f, width, height, malloced);

		q_snprintf (loadfilename, sizeof(loadfilename), "%s%s.bmp", prefixes[i], name);
		IMG_MARK(loadfilename);
		COM_FOpenFile (loadfilename, &f, NULL);
		if (f)
			return Image_LoadBMP (f, width, height, malloced, !strncmp(origname, "gfx/", 4));

		q_snprintf (loadfilename, sizeof(loadfilename), "%s%s.jpeg", prefixes[i], name);
		IMG_MARK(loadfilename);
		COM_FOpenFile (loadfilename, &f, NULL);
		if (f)
			return Image_LoadSTBI (f, width, height);

		q_snprintf (loadfilename, sizeof(loadfilename), "%s%s.jpg", prefixes[i], name);
		IMG_MARK(loadfilename);
		COM_FOpenFile (loadfilename, &f, NULL);
		if (f)
			return Image_LoadSTBI (f, width, height);

		q_snprintf (loadfilename, sizeof(loadfilename), "%s%s.pcx", prefixes[i], name);
		IMG_MARK(loadfilename);
		COM_FOpenFile (loadfilename, &f, NULL);
		if (f)
			return Image_LoadPCX (f, width, height);
	}

	name = origname;
	q_snprintf (loadfilename, sizeof(loadfilename), "%s%s.lmp", "", name);
	COM_FOpenFile (loadfilename, &f, NULL);
	if (f)
		return Image_LoadLMP (f, width, height, fmt);

	return NULL;
}

//==============================================================================
//
//  TGA
//
//==============================================================================

typedef struct targaheader_s {
	unsigned char 	id_length, colormap_type, image_type;
	unsigned short	colormap_index, colormap_length;
	unsigned char	colormap_size;
	unsigned short	x_origin, y_origin, width, height;
	unsigned char	pixel_size, attributes;
} targaheader_t;

#define TARGAHEADERSIZE 18		/* size on disk */

int fgetLittleShort (FILE *f)
{
	byte	b1, b2;

	b1 = fgetc(f);
	b2 = fgetc(f);

	return (short)(b1 + b2*256);
}

int fgetLittleLong (FILE *f)
{
	byte	b1, b2, b3, b4;

	b1 = fgetc(f);
	b2 = fgetc(f);
	b3 = fgetc(f);
	b4 = fgetc(f);

	return b1 + (b2<<8) + (b3<<16) + (b4<<24);
}

/*
============
Image_WriteTGA -- writes RGB or RGBA data to a TGA file

returns true if successful

TODO: support BGRA and BGR formats (since opengl can return them, and we don't have to swap)
============
*/
qboolean Image_WriteTGA (const char *name, byte *data, int width, int height, int bpp, qboolean upsidedown)
{
	int		handle, i, size, temp, bytes;
	char	pathname[MAX_OSPATH];
	byte	header[TARGAHEADERSIZE];

	Sys_mkdir (com_gamedir); //if we've switched to a nonexistant gamedir, create it now so we don't crash
	q_snprintf (pathname, sizeof(pathname), "%s/%s", com_gamedir, name);
	handle = Sys_FileOpenWrite (pathname);
	if (handle == -1)
		return false;

	Q_memset (header, 0, TARGAHEADERSIZE);
	header[2] = 2; // uncompressed type
	header[12] = width&255;
	header[13] = width>>8;
	header[14] = height&255;
	header[15] = height>>8;
	header[16] = bpp; // pixel size
	if (upsidedown)
		header[17] = 0x20; //upside-down attribute

	// swap red and blue bytes
	bytes = bpp/8;
	size = width*height*bytes;
	for (i=0; i<size; i+=bytes)
	{
		temp = data[i];
		data[i] = data[i+2];
		data[i+2] = temp;
	}

	Sys_FileWrite (handle, header, TARGAHEADERSIZE);
	Sys_FileWrite (handle, data, size);
	Sys_FileClose (handle);

	return true;
}

/*
=============
Image_LoadTGA
=============
*/
byte *Image_LoadTGA (FILE *fin, int *width, int *height)
{
	int				columns, rows, numPixels;
	byte			*pixbuf;
	int				row, column;
	byte			*targa_rgba;
	int				realrow; //johnfitz -- fix for upside-down targas
	qboolean		upside_down; //johnfitz -- fix for upside-down targas
	stdio_buffer_t	*buf;
	targaheader_t	targa_header;

	targa_header.id_length = fgetc(fin);
	targa_header.colormap_type = fgetc(fin);
	targa_header.image_type = fgetc(fin);

	targa_header.colormap_index = fgetLittleShort(fin);
	targa_header.colormap_length = fgetLittleShort(fin);
	targa_header.colormap_size = fgetc(fin);
	targa_header.x_origin = fgetLittleShort(fin);
	targa_header.y_origin = fgetLittleShort(fin);
	targa_header.width = fgetLittleShort(fin);
	targa_header.height = fgetLittleShort(fin);
	targa_header.pixel_size = fgetc(fin);
	targa_header.attributes = fgetc(fin);

	if (targa_header.image_type==1)
	{
		if (targa_header.pixel_size != 8 || targa_header.colormap_size != 24 || targa_header.colormap_length > 256)
			Sys_Error ("Image_LoadTGA: %s has an %ibit palette", loadfilename, targa_header.colormap_type);
	}
	else
	{
		if (targa_header.image_type!=2 && targa_header.image_type!=10)
			Sys_Error ("Image_LoadTGA: %s is not a type 2 or type 10 targa (%i)", loadfilename, targa_header.image_type);

		if (targa_header.colormap_type !=0 || (targa_header.pixel_size!=32 && targa_header.pixel_size!=24))
			Sys_Error ("Image_LoadTGA: %s is not a 24bit or 32bit targa", loadfilename);
	}

	columns = targa_header.width;
	rows = targa_header.height;
	numPixels = columns * rows;
	upside_down = !(targa_header.attributes & 0x20); //johnfitz -- fix for upside-down targas

	targa_rgba = (byte *) Hunk_Alloc (numPixels*4);

	if (targa_header.id_length != 0)
		fseek(fin, targa_header.id_length, SEEK_CUR);  // skip TARGA image comment

	buf = Buf_Alloc(fin);

	if (targa_header.image_type==1) // Uncompressed, paletted images
	{
		byte palette[256*4];
		int i;
		//palette data comes first
		for (i = 0; i < targa_header.colormap_length; i++)
		{	//this palette data is bgr.
			palette[i*3+2] = Buf_GetC(buf);
			palette[i*3+1] = Buf_GetC(buf);
			palette[i*3+0] = Buf_GetC(buf);
			palette[i*3+3] = 255;
		}
		for (i = targa_header.colormap_length*4; i < sizeof(palette); i++)
			palette[i] = 0;
		for(row=rows-1; row>=0; row--)
		{
			realrow = upside_down ? row : rows - 1 - row;
			pixbuf = targa_rgba + realrow*columns*4;

			for(column=0; column<columns; column++)
			{
				i = Buf_GetC(buf);
				*pixbuf++= palette[i*3+0];
				*pixbuf++= palette[i*3+1];
				*pixbuf++= palette[i*3+2];
				*pixbuf++= palette[i*3+3];
			}
		}
	}
	else if (targa_header.image_type==2) // Uncompressed, RGB images
	{
		for(row=rows-1; row>=0; row--)
		{
			//johnfitz -- fix for upside-down targas
			realrow = upside_down ? row : rows - 1 - row;
			pixbuf = targa_rgba + realrow*columns*4;
			//johnfitz
			for(column=0; column<columns; column++)
			{
				unsigned char red,green,blue,alphabyte;
				switch (targa_header.pixel_size)
				{
				case 24:
					blue = Buf_GetC(buf);
					green = Buf_GetC(buf);
					red = Buf_GetC(buf);
					*pixbuf++ = red;
					*pixbuf++ = green;
					*pixbuf++ = blue;
					*pixbuf++ = 255;
					break;
				case 32:
					blue = Buf_GetC(buf);
					green = Buf_GetC(buf);
					red = Buf_GetC(buf);
					alphabyte = Buf_GetC(buf);
					*pixbuf++ = red;
					*pixbuf++ = green;
					*pixbuf++ = blue;
					*pixbuf++ = alphabyte;
					break;
				}
			}
		}
	}
	else if (targa_header.image_type==10) // Runlength encoded RGB images
	{
		unsigned char red,green,blue,alphabyte,packetHeader,packetSize,j;
		for(row=rows-1; row>=0; row--)
		{
			//johnfitz -- fix for upside-down targas
			realrow = upside_down ? row : rows - 1 - row;
			pixbuf = targa_rgba + realrow*columns*4;
			//johnfitz
			for(column=0; column<columns; )
			{
				packetHeader=Buf_GetC(buf);
				packetSize = 1 + (packetHeader & 0x7f);
				if (packetHeader & 0x80) // run-length packet
				{
					switch (targa_header.pixel_size)
					{
					case 24:
						blue = Buf_GetC(buf);
						green = Buf_GetC(buf);
						red = Buf_GetC(buf);
						alphabyte = 255;
						break;
					case 32:
						blue = Buf_GetC(buf);
						green = Buf_GetC(buf);
						red = Buf_GetC(buf);
						alphabyte = Buf_GetC(buf);
						break;
					default: /* avoid compiler warnings */
						blue = red = green = alphabyte = 0;
					}

					for(j=0;j<packetSize;j++)
					{
						*pixbuf++=red;
						*pixbuf++=green;
						*pixbuf++=blue;
						*pixbuf++=alphabyte;
						column++;
						if (column==columns) // run spans across rows
						{
							column=0;
							if (row>0)
								row--;
							else
								goto breakOut;
							//johnfitz -- fix for upside-down targas
							realrow = upside_down ? row : rows - 1 - row;
							pixbuf = targa_rgba + realrow*columns*4;
							//johnfitz
						}
					}
				}
				else // non run-length packet
				{
					for(j=0;j<packetSize;j++)
					{
						switch (targa_header.pixel_size)
						{
						case 24:
							blue = Buf_GetC(buf);
							green = Buf_GetC(buf);
							red = Buf_GetC(buf);
							*pixbuf++ = red;
							*pixbuf++ = green;
							*pixbuf++ = blue;
							*pixbuf++ = 255;
							break;
						case 32:
							blue = Buf_GetC(buf);
							green = Buf_GetC(buf);
							red = Buf_GetC(buf);
							alphabyte = Buf_GetC(buf);
							*pixbuf++ = red;
							*pixbuf++ = green;
							*pixbuf++ = blue;
							*pixbuf++ = alphabyte;
							break;
						default: /* avoid compiler warnings */
							blue = red = green = alphabyte = 0;
						}
						column++;
						if (column==columns) // pixel packet run spans across rows
						{
							column=0;
							if (row>0)
								row--;
							else
								goto breakOut;
							//johnfitz -- fix for upside-down targas
							realrow = upside_down ? row : rows - 1 - row;
							pixbuf = targa_rgba + realrow*columns*4;
							//johnfitz
						}
					}
				}
			}
			breakOut:;
		}
	}

	Buf_Free(buf);
	fclose(fin);

	*width = (int)(targa_header.width);
	*height = (int)(targa_header.height);
	return targa_rgba;
}

//==============================================================================
//
//  PCX
//
//==============================================================================

typedef struct
{
    char			signature;
    char			version;
    char			encoding;
    char			bits_per_pixel;
    unsigned short	xmin,ymin,xmax,ymax;
    unsigned short	hdpi,vdpi;
    byte			colortable[48];
    char			reserved;
    char			color_planes;
    unsigned short	bytes_per_line;
    unsigned short	palette_type;
    char			filler[58];
} pcxheader_t;

/*
============
Image_LoadPCX
============
*/
byte *Image_LoadPCX (FILE *f, int *width, int *height)
{
	pcxheader_t	pcx;
	int			x, y, w, h, readbyte, runlength, start;
	byte		*p, *data;
	byte		palette[768];
	stdio_buffer_t  *buf;

	IMG_MARK("pcx: header");

	start = ftell (f); //save start of file (since we might be inside a pak file, SEEK_SET might not be the start of the pcx)

	if (fread(&pcx, sizeof(pcx), 1, f) != 1) // woods
		Sys_Error("Failed to read PCX header from '%s'", loadfilename);
	pcx.xmin = (unsigned short)LittleShort (pcx.xmin);
	pcx.ymin = (unsigned short)LittleShort (pcx.ymin);
	pcx.xmax = (unsigned short)LittleShort (pcx.xmax);
	pcx.ymax = (unsigned short)LittleShort (pcx.ymax);
	pcx.bytes_per_line = (unsigned short)LittleShort (pcx.bytes_per_line);

	if (pcx.signature != 0x0A)
		Sys_Error ("'%s' is not a valid PCX file", loadfilename);

	if (pcx.version != 5)
		Sys_Error ("'%s' is version %i, should be 5", loadfilename, pcx.version);

	if (pcx.encoding != 1 || pcx.bits_per_pixel != 8 || pcx.color_planes != 1)
		Sys_Error ("'%s' has wrong encoding or bit depth", loadfilename);

	w = pcx.xmax - pcx.xmin + 1;
	h = pcx.ymax - pcx.ymin + 1;

	if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
		Sys_Error ("'%s' has bad dimensions %ix%i", loadfilename, w, h);

	IMG_MARK("pcx: allocating");
	data = (byte *) Hunk_Alloc((w*h+1)*4); //+1 to allow reading padding byte on last line

	//load palette
	IMG_MARK("pcx: palette");
	fseek (f, start + com_filesize - 768, SEEK_SET);
	if (fread(palette, 1, 768, f) != 768) // woods
		Sys_Error("Failed to read PCX palette from '%s'", loadfilename);

	//back to start of image data
	fseek (f, start + sizeof(pcx), SEEK_SET);

	IMG_MARK("pcx: rows");
	buf = Buf_Alloc(f);

	for (y=0; y<h; y++)
	{
		p = data + y * w * 4;

		for (x=0; x<(pcx.bytes_per_line); ) //read the extra padding byte if necessary
		{
			readbyte = Buf_GetC(buf);

			if(readbyte >= 0xC0)
			{
				runlength = readbyte & 0x3F;
				readbyte = Buf_GetC(buf);
			}
			else
				runlength = 1;

			/* Buf_GetC() returns EOF (-1) on a truncated or corrupt file.  The
			 * palette lookup below would then read three bytes *before* the
			 * array, and on a BE/alignment-strict CPU that is the kind of
			 * crash that leaves no message. */
			if (readbyte < 0)
				readbyte = 0;

			while(runlength-- > 0 && x < pcx.bytes_per_line)
			{
				/* bytes_per_line is padded up for the row, and can be larger
				 * than the real width.  The padding pixels are not part of the
				 * image: writing them ran past the end of a buffer that was
				 * sized for w*h pixels. */
				if (x < w)
				{
					p[0] = palette[readbyte*3];
					p[1] = palette[readbyte*3+1];
					p[2] = palette[readbyte*3+2];
					p[3] = 255;
					p += 4;
				}
				x++;
			}
		}
	}

	IMG_MARK("pcx: done");
	Buf_Free(buf);
	fclose(f);

	*width = w;
	*height = h;
	return data;
}

//==============================================================================
//
//  QPIC (aka '.lmp')
//
//==============================================================================

typedef struct
{
    unsigned int width, height;
} lmpheader_t;

/*
============
Image_LoadLMP
============
*/
byte *Image_LoadLMP (FILE *f, int *width, int *height, enum srcformat *fmt)
{
	lmpheader_t	qpic;
	size_t		pix;
	void		*data;

	if (fread(&qpic, sizeof(qpic), 1, f) != 1) // woods
	{
		fclose(f);
		return NULL;
	}
	qpic.width = LittleLong (qpic.width);
	qpic.height = LittleLong (qpic.height);

	pix = qpic.width*qpic.height;

	if (com_filesize != 8+pix)
	{
		fclose(f);
		return NULL;
	}

	data = (byte *) Hunk_Alloc(pix); //+1 to allow reading padding byte on last line
	if (fread(data, 1, pix, f) != pix) // woods
	{
		fclose(f);
		return NULL;
	}
	fclose(f);

	*width = qpic.width;
	*height = qpic.height;
	*fmt = SRC_INDEXED;
	return data;
}

//==============================================================================
//
//  STB_IMAGE_WRITE
//
//==============================================================================

static byte *CopyFlipped(const byte *data, int width, int height, int bpp)
{
	int	y, rowsize;
	byte	*flipped;

	rowsize = width * (bpp / 8);
	flipped = (byte *) malloc(height * rowsize);
	if (!flipped)
		return NULL;

	for (y=0; y<height; y++)
	{
		memcpy(&flipped[y * rowsize], &data[(height - 1 - y) * rowsize], rowsize);
	}
	return flipped;
}

/*
============
Image_WriteJPG -- writes using stb_image_write

returns true if successful
============
*/
qboolean Image_WriteJPG (const char *name, byte *data, int width, int height, int bpp, int quality, qboolean upsidedown)
{
	unsigned error;
	char	pathname[MAX_OSPATH];
	byte	*flipped;
	int	bytes_per_pixel;

	if (!(bpp == 32 || bpp == 24))
		Sys_Error ("bpp not 24 or 32");

	bytes_per_pixel = bpp / 8;

	Sys_mkdir (com_gamedir); //if we've switched to a nonexistant gamedir, create it now so we don't crash
	q_snprintf (pathname, sizeof(pathname), "%s/%s", com_gamedir, name);

	if (!upsidedown)
	{
		flipped = CopyFlipped (data, width, height, bpp);
		if (!flipped)
			return false;
	}
	else
		flipped = data;

	error = stbi_write_jpg (pathname, width, height, bytes_per_pixel, flipped, quality);
	if (!upsidedown)
		free (flipped);

	return (error != 0);
}

qboolean Image_WritePNG (const char *name, byte *data, int width, int height, int bpp, qboolean upsidedown)
{
	unsigned error;
	char	pathname[MAX_OSPATH];
	byte	*flipped;
	unsigned char	*filters;
	unsigned char	*png;
	size_t		pngsize;
	LodePNGState	state;

	if (!(bpp == 32 || bpp == 24))
		Sys_Error("bpp not 24 or 32");

	Sys_mkdir (com_gamedir); //if we've switched to a nonexistant gamedir, create it now so we don't crash
	q_snprintf (pathname, sizeof(pathname), "%s/%s", com_gamedir, name);

	flipped = (!upsidedown)? CopyFlipped (data, width, height, bpp) : data;
	filters = (unsigned char *) malloc (height);
	if (!filters || !flipped)
	{
		if (!upsidedown)
		  free (flipped);
		free (filters);
		return false;
	}

// set some options for faster compression
	lodepng_state_init(&state);
	state.encoder.zlibsettings.use_lz77 = 0;
	state.encoder.auto_convert = 0;
	state.encoder.filter_strategy = LFS_PREDEFINED;
	memset(filters, 1, height); //use filter 1; see https://www.w3.org/TR/PNG-Filters.html
	state.encoder.predefined_filters = filters;

	if (bpp == 24)
	{
		state.info_raw.colortype = LCT_RGB;
		state.info_png.color.colortype = LCT_RGB;
	}
	else
	{
		state.info_raw.colortype = LCT_RGBA;
		state.info_png.color.colortype = LCT_RGBA;
	}

	error = lodepng_encode (&png, &pngsize, flipped, width, height, &state);
	if (error == 0) lodepng_save_file (png, pngsize, pathname);
#ifdef LODEPNG_COMPILE_ERROR_TEXT
	else Con_Printf("WritePNG: %s\n", lodepng_error_text (error));
#endif

	lodepng_state_cleanup (&state);
	lodepng_free (png); /* png was allocated by lodepng */
	free (filters);
	if (!upsidedown) {
	  free (flipped);
	}

	return (error == 0);
}
