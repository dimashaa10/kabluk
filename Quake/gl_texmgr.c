/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
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

//gl_texmgr.c -- fitzquake's texture manager. manages opengl texture images

#include "quakedef.h"

/* PSP breadcrumbs.
 *
 * psp_mark() (port/psp/psp_console.c) writes one line to the debug screen and
 * to kabluk.log, so the last mark in the file names the step that died.  The
 * macro has to be defined up here: it is used by the texture resampler and the
 * 32-bit loader, which come *before* TexMgr_LoadImage in this file. */
#if defined(PLATFORM_PSP) || defined(__psp__)
extern void psp_mark (const char *what);
#define TEX_MARK(x)	psp_mark(x)
#else
#define TEX_MARK(x)	do { } while (0)
#endif

const int	gl_solid_format = 3;
const int	gl_alpha_format = 4;

static cvar_t	gl_texturemode = { "gl_texturemode", "", CVAR_ARCHIVE };
static cvar_t	gl_texture_anisotropy = { "gl_texture_anisotropy", "1", CVAR_ARCHIVE };
cvar_t	gl_max_size = { "gl_max_size", "0", CVAR_NONE }; // woods remove static for menu
cvar_t	gl_picmip = { "gl_picmip", "0", CVAR_NONE }; // woods remove static for #f_config
cvar_t	r_fastturb = { "r_fastturb", "0", CVAR_ARCHIVE }; // woods #fastturb

void VID_ChangedRestart_f(cvar_t* var); // woods #vidrestart

static int numgltextures;
static gltexture_t* active_gltextures, * free_gltextures;
gltexture_t* notexture, * nulltexture;

plcolour_t plcolour_none = { 0 };

unsigned int d_8to24table[256];
unsigned int d_8to24table_fbright[256];
unsigned int d_8to24table_fbright_fence[256];
unsigned int d_8to24table_nobright[256];
unsigned int d_8to24table_nobright_fence[256];
unsigned int d_8to24table_conchars[256];

static void TexMgr_ColormapTexture_Free(struct gltexture_s* basetex);

static struct
{
	const char* formatname;	//full name
	const char* mipextname;	//four chars
	int internalformat;		//opengl's internal format (mostly sized formats)
	int format;				//for non-compressed formats (opengl's transcoding)
	int type;				//for non-compressed formats (opengl's transcoding)
	int blockbytes;			//bytes per block
	int blockwidth;			//width of a block (or 1 for non-block formats)
	int blockheight;		//height of a block (or 1 for non-block formats)
	qboolean* supported;	//pointer to some boolean that says whether some opengl extension is actually support or not.
} compressedformats[] =
{
	{NULL},	//SRC_INDEXED
	{NULL},	//SRC_LIGHTMAP
	{NULL},	//SRC_RGBA
	{NULL},	//SRC_EXTERNAL

	{"RGBA8",		"RGBA", GL_RGBA,GL_RGBA,GL_UNSIGNED_INT_8_8_8_8_REV,	 4, 1, 1, NULL},
	{"RGB8",		"RGB",  GL_RGB,GL_RGB,GL_UNSIGNED_BYTE,					 3, 1, 1, NULL},
#ifdef GL_RGB565	//gles3
	{"RGB565",		"565",  GL_RGB565,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,		 2, 1, 1, NULL},
#else	//gles2-compat
	{"RGB565",		"565",  GL_RGB,GL_RGB,GL_UNSIGNED_SHORT_5_6_5,		 2, 1, 1, NULL},
#endif
	{"RGBA4444",	"4444", GL_RGBA4,GL_RGBA,GL_UNSIGNED_SHORT_4_4_4_4,		 2, 1, 1, NULL},
	{"RGBA5551",	"5551", GL_RGB5_A1,GL_RGBA,GL_UNSIGNED_SHORT_5_5_5_1,	 2, 1, 1, NULL},
	{"L8",			"LUM8", GL_LUMINANCE8,GL_LUMINANCE,GL_UNSIGNED_BYTE,	 1, 1, 1, NULL},
#ifndef GL_VERSION_3_0
	#define GL_RGB9_E5                        0x8C3D
	#define GL_UNSIGNED_INT_5_9_9_9_REV       0x8C3E
#endif
	{"E5BGR9",		"EXP5", GL_RGB9_E5,GL_RGB,GL_UNSIGNED_INT_5_9_9_9_REV,	 4, 1, 1, &gl_texture_e5bgr9},
#if defined(GL_EXT_texture_compression_s3tc) || defined(GL_EXT_texture_compression_dxt1)
	{"BC1_RGBA",	"BC1",  GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,0,0,			 8, 4, 4, &gl_texture_s3tc},
#endif
#if defined(GL_EXT_texture_compression_s3tc)
	{"BC2_RGBA",	"BC2",  GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,0,0,			16, 4, 4, &gl_texture_s3tc},
	{"BC3_RGBA",	"BC3",  GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,0,0,			16, 4, 4, &gl_texture_s3tc},
#endif
#ifdef GL_VERSION_3_0
	{"BC4_R",		"BC4",  GL_COMPRESSED_RED_RGTC1,0,0,					 8, 4, 4, &gl_texture_rgtc},
	{"BC5_RG",		"BC5",  GL_COMPRESSED_RG_RGTC2,0,0,						16, 4, 4, &gl_texture_rgtc},
#elif defined(GL_EXT_texture_compression_rgtc)
	{"BC4_R",		"BC4",  GL_COMPRESSED_RED_RGTC1_EXT,0,0,				 8, 4, 4, &gl_texture_rgtc},
	{"BC5_RG",		"BC5",  GL_COMPRESSED_RED_GREEN_RGTC2_EXT,0,0,			16, 4, 4, &gl_texture_rgtc},
#endif
#ifdef GL_VERSION_4_2
	{"BC6_RGB_UFLOAT","BC6",GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT,0,0,		16, 4, 4, &gl_texture_bptc},
	{"BC7_RGBA",	"BC7",  GL_COMPRESSED_RGBA_BPTC_UNORM,0,0,				16, 4, 4, &gl_texture_bptc},
#elif defined(GL_EXT_texture_compression_bptc)
	{"BC6_RGB_UFLOAT","BC6",GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_EXT,0,0,	16, 4, 4, &gl_texture_bptc},
	{"BC7_RGBA",	"BC7",  GL_COMPRESSED_RGBA_BPTC_UNORM_EXT,0,0,			16, 4, 4, &gl_texture_bptc},
#endif
#if defined(GL_ES_VERSION_3_0) || defined(GL_VERSION_4_3)
	{"ETC1_RGB8",	"ETC1", GL_COMPRESSED_RGB8_ETC2,0,0,					 8, 4, 4, &gl_texture_etc2},
	{"ETC2_RGB8",	"ETC2", GL_COMPRESSED_RGB8_ETC2,0,0,					 8, 4, 4, &gl_texture_etc2},
	{"ETC2_RGB8A1",	"ETCP", GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2,0,0, 8, 4, 4, &gl_texture_etc2},
	{"ETC2_RGB8A8",	"ETCA", GL_COMPRESSED_RGBA8_ETC2_EAC,0,0,				16, 4, 4, &gl_texture_etc2},
#endif
#ifdef GL_ES_VERSION_3_2
	{"ASTC_4X4",	"AST4", GL_COMPRESSED_RGBA_ASTC_4x4,0,0,				16, 4, 4, &gl_texture_astc},
	{"ASTC_5X4",	"AS54", GL_COMPRESSED_RGBA_ASTC_5x4,0,0,				16, 4, 4, &gl_texture_astc},
	{"ASTC_5X5",	"AST5", GL_COMPRESSED_RGBA_ASTC_5x5,0,0,				16, 5, 5, &gl_texture_astc},
	{"ASTC_6X5",	"AS65", GL_COMPRESSED_RGBA_ASTC_6x5,0,0,				16, 4, 4, &gl_texture_astc},
	{"ASTC_6X6",	"AST6", GL_COMPRESSED_RGBA_ASTC_6x6,0,0,				16, 6, 6, &gl_texture_astc},
	{"ASTC_8X5",	"AS85", GL_COMPRESSED_RGBA_ASTC_8x5,0,0,				16, 4, 4, &gl_texture_astc},
	{"ASTC_8X6",	"AS86", GL_COMPRESSED_RGBA_ASTC_8x6,0,0,				16, 4, 4, &gl_texture_astc},
	{"ASTC_8X8",	"AST8", GL_COMPRESSED_RGBA_ASTC_8x8,0,0,				16, 8, 8, &gl_texture_astc},
	{"ASTC_10X5",	"AS05", GL_COMPRESSED_RGBA_ASTC_10x5,0,0,				16, 4, 4, &gl_texture_astc},
	{"ASTC_10X6",	"AS06", GL_COMPRESSED_RGBA_ASTC_10x6,0,0,				16, 4, 4, &gl_texture_astc},
	{"ASTC_10X8",	"AS08", GL_COMPRESSED_RGBA_ASTC_10x8,0,0,				16, 4, 4, &gl_texture_astc},
	{"ASTC_10X10",	"AST0", GL_COMPRESSED_RGBA_ASTC_10x10,0,0,				16, 8, 8, &gl_texture_astc},
	{"ASTC_12X10",	"AST0", GL_COMPRESSED_RGBA_ASTC_12x10,0,0,				16, 8, 8, &gl_texture_astc},
	{"ASTC_12X12",	"AST2", GL_COMPRESSED_RGBA_ASTC_12x12,0,0,				16, 8, 8, &gl_texture_astc},
#elif defined(GL_KHR_texture_compression_astc_ldr)
	{"ASTC_4X4",	"AST4", GL_COMPRESSED_RGBA_ASTC_4x4_KHR,0,0,			16, 4, 4, &gl_texture_astc},
	{"ASTC_5X4",	"AS54", GL_COMPRESSED_RGBA_ASTC_5x4_KHR,0,0,			16, 4, 4, &gl_texture_astc},
	{"ASTC_5X5",	"AST5", GL_COMPRESSED_RGBA_ASTC_5x5_KHR,0,0,			16, 5, 5, &gl_texture_astc},
	{"ASTC_6X5",	"AS65", GL_COMPRESSED_RGBA_ASTC_6x5_KHR,0,0,			16, 4, 4, &gl_texture_astc},
	{"ASTC_6X6",	"AST6", GL_COMPRESSED_RGBA_ASTC_6x6_KHR,0,0,			16, 6, 6, &gl_texture_astc},
	{"ASTC_8X5",	"AS85", GL_COMPRESSED_RGBA_ASTC_8x5_KHR,0,0,			16, 4, 4, &gl_texture_astc},
	{"ASTC_8X6",	"AS86", GL_COMPRESSED_RGBA_ASTC_8x6_KHR,0,0,			16, 4, 4, &gl_texture_astc},
	{"ASTC_8X8",	"AST8", GL_COMPRESSED_RGBA_ASTC_8x8_KHR,0,0,			16, 8, 8, &gl_texture_astc},
	{"ASTC_10X5",	"AS05", GL_COMPRESSED_RGBA_ASTC_10x5_KHR,0,0,			16, 4, 4, &gl_texture_astc},
	{"ASTC_10X6",	"AS06", GL_COMPRESSED_RGBA_ASTC_10x6_KHR,0,0,			16, 4, 4, &gl_texture_astc},
	{"ASTC_10X8",	"AS08", GL_COMPRESSED_RGBA_ASTC_10x8_KHR,0,0,			16, 4, 4, &gl_texture_astc},
	{"ASTC_10X10",	"AST0", GL_COMPRESSED_RGBA_ASTC_10x10_KHR,0,0,			16, 8, 8, &gl_texture_astc},
	{"ASTC_12X10",	"AST0", GL_COMPRESSED_RGBA_ASTC_12x10_KHR,0,0,			16, 8, 8, &gl_texture_astc},
	{"ASTC_12X12",	"AST2", GL_COMPRESSED_RGBA_ASTC_12x12_KHR,0,0,			16, 8, 8, &gl_texture_astc},
#endif
};

/*
================================================================================

	COMMANDS

================================================================================
*/

typedef struct
{
	int	magfilter;
	int	minfilter;
	const char* name2, * name1;
} glmode_t;
static glmode_t glmodes[] = {
	{GL_NEAREST, GL_NEAREST,				"n.n", "GL_NEAREST"},
	{GL_NEAREST, GL_NEAREST_MIPMAP_NEAREST,	"nnn", "GL_NEAREST_MIPMAP_NEAREST"},
	{GL_NEAREST, GL_NEAREST_MIPMAP_LINEAR,	"nln", "GL_NEAREST_MIPMAP_LINEAR"},
	{GL_LINEAR,  GL_LINEAR,					"l.l", "GL_LINEAR"},
	{GL_LINEAR,  GL_LINEAR_MIPMAP_NEAREST,	"lnl", "GL_LINEAR_MIPMAP_NEAREST"},
	{GL_LINEAR,  GL_LINEAR_MIPMAP_LINEAR,	"lll", "GL_LINEAR_MIPMAP_LINEAR"},

	{GL_NEAREST, GL_LINEAR,					"n.l", NULL},
	{GL_NEAREST,  GL_LINEAR_MIPMAP_NEAREST,	"nnl", NULL},
	{GL_NEAREST,  GL_LINEAR_MIPMAP_LINEAR,	"nll", NULL},
	{GL_LINEAR,  GL_NEAREST,				"l.n", NULL},
	{GL_LINEAR,  GL_NEAREST_MIPMAP_NEAREST,	"lnn", NULL},
	{GL_LINEAR,  GL_NEAREST_MIPMAP_LINEAR,	"lln", NULL},
};
#define NUM_GLMODES (int)(sizeof(glmodes)/sizeof(glmodes[0]))
static int glmode_idx = 5; /* trilinear */

int TexMgr_GetTextureMode(void)
{
	if (glmodes[glmode_idx].magfilter == GL_NEAREST)
		return 0;
	else
		return q_max(1, gl_texture_anisotropy.value);
}

/*
===============
TexMgr_DescribeTextureModes_f -- report available texturemodes
===============
*/
static void TexMgr_DescribeTextureModes_f(void)
{
	int i;

	for (i = 0; i < NUM_GLMODES; i++)
		Con_SafePrintf("   %2i: %s\n", i + 1, glmodes[i].name1 ? glmodes[i].name1 : glmodes[i].name2);

	Con_Printf("%i modes\n", i);
}

/*
===============
TexMgr_SetFilterModes
===============
*/
static void TexMgr_SetFilterModes(gltexture_t* glt)
{
	GL_Bind(glt);

	if (glt->flags & TEXPREF_NEAREST)
	{
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	}
	else if (glt->flags & TEXPREF_LINEAR)
	{
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	}
	else if (glt->flags & TEXPREF_MIPMAP)
	{
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, glmodes[glmode_idx].magfilter);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, glmodes[glmode_idx].minfilter);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, gl_texture_anisotropy.value);
	}
	else
	{
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, glmodes[glmode_idx].magfilter);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, glmodes[glmode_idx].magfilter);
	}

	if (glt->flags & TEXPREF_CLAMP) // woods iw clamp
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
}

/*
===============
TexMgr_TextureMode_Completion_f -- woods #iwtabcomplete
===============
*/
static void TexMgr_TextureMode_Completion_f(cvar_t* cvar, const char* partial)
{
	int i;

	for (i = 0; i < NUM_GLMODES; i++)
	{
		if (glmodes[i].name1 == NULL)
			Con_AddToTabList(glmodes[i].name2, partial, NULL, NULL); // #demolistsort add arg
		else
			Con_AddToTabList(glmodes[i].name1, partial, NULL, NULL); // #demolistsort add arg
	}
}

/*
===============
TexMgr_TextureMode_f -- called when gl_texturemode changes
===============
*/
static void TexMgr_TextureMode_f(cvar_t* var)
{
	gltexture_t* glt;
	int i;

	for (i = 0; i < NUM_GLMODES; i++)
	{
		if ((glmodes[i].name1 && !Q_strcmp(glmodes[i].name1, gl_texturemode.string)) ||
			(glmodes[i].name2 && !Q_strcmp(glmodes[i].name2, gl_texturemode.string)))
		{
			if (glmode_idx != i)
			{
				glmode_idx = i;
				for (glt = active_gltextures; glt; glt = glt->next)
					TexMgr_SetFilterModes(glt);
				Sbar_Changed(); //sbar graphics need to be redrawn with new filter mode
				//FIXME: warpimages need to be redrawn, too.
			}
			return;
		}
	}

	for (i = 0; i < NUM_GLMODES; i++)
	{
		if ((glmodes[i].name1 && !q_strcasecmp(glmodes[i].name1, gl_texturemode.string)) ||
			(glmodes[i].name2 && !q_strcasecmp(glmodes[i].name2, gl_texturemode.string)))
		{
			Cvar_SetQuick(&gl_texturemode, glmodes[i].name1 ? glmodes[i].name1 : glmodes[i].name2);
			return;
		}
	}

	i = atoi(gl_texturemode.string);
	if (i >= 1 && i <= NUM_GLMODES)
	{
		Cvar_SetQuick(&gl_texturemode, glmodes[i - 1].name1);
		return;
	}

	Con_Printf("\"%s\" is not a valid texturemode\n", gl_texturemode.string);
	Cvar_SetQuick(&gl_texturemode, glmodes[glmode_idx].name1 ? glmodes[glmode_idx].name1 : glmodes[glmode_idx].name2);
}

/*
===============
TexMgr_Anisotropy_f -- called when gl_texture_anisotropy changes
===============
*/
static void TexMgr_Anisotropy_f(cvar_t* var)
{
	if (gl_texture_anisotropy.value < 1)
	{
		Cvar_SetQuick(&gl_texture_anisotropy, "1");
	}
	else if (gl_texture_anisotropy.value > gl_max_anisotropy)
	{
		Cvar_SetValueQuick(&gl_texture_anisotropy, gl_max_anisotropy);
	}
	else
	{
		gltexture_t* glt;
		for (glt = active_gltextures; glt; glt = glt->next)
		{
			/*  TexMgr_SetFilterModes (glt);*/
			if (glt->flags & TEXPREF_MIPMAP) {
				GL_Bind(glt);
				glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, glmodes[glmode_idx].magfilter);
				glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, glmodes[glmode_idx].minfilter);
				glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, gl_texture_anisotropy.value);
			}
		}
	}
}

/*
===============
CompleteImageList -- woods -- tab completion for imagelist/imagedump -- iw
===============
*/
qboolean CompleteImageList(const char* partial, void* unused)
{
	gltexture_t* glt;

	for (glt = active_gltextures; glt; glt = glt->next)
		if (Con_Match(glt->name, partial))
			Con_AddToTabList(glt->name, partial, NULL, NULL); // #demolistsort add arg

	return true;
}

/*
===============
TexMgr_Imagelist_f -- report loaded textures
===============
*/
static void TexMgr_Imagelist_f(void)
{
	float mb;
	float texels = 0;
	int count = 0;
	const char* filter = NULL; // woods add filter (ironwail)
	gltexture_t* glt; // woods add filter (ironwail)

	if (Cmd_Argc() >= 2)
		filter = Cmd_Argv(1);

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		char buf[MAX_QPATH]; // woods add filter (ironwail)

		if (filter) // woods add filter (ironwail)
		{
			if (!q_strcasestr(glt->name, filter))
				continue;
			COM_TintSubstring(glt->name, filter, buf, sizeof(buf));
		}
		else
		{
			q_strlcpy(buf, glt->name, sizeof(buf));
		}

		Con_SafePrintf("   %4i x%4i %s\n", glt->width, glt->height, buf); // woods add filter (ironwail)
		if (glt->flags & TEXPREF_MIPMAP)
			texels += glt->width * glt->height * 4.0f / 3.0f;
		else
			texels += (glt->width * glt->height);
		count++;
	}

	mb = texels * (Cvar_VariableValue("vid_bpp") / 8.0f) / 0x100000;
	if (filter)
		Con_Printf("%i/%i textures containing '%s': %.1lf mpixels %1.1lf megabytes\n", // woods add filter (ironwail)
			count, numgltextures, filter, texels * 1e-6, mb);
	else
		Con_Printf("%i textures %.1lf mpixels %1.1lf megabytes\n",
			numgltextures, texels * 1e-6, mb);
}

/*
===============
TexMgr_Imagedump_f -- dump all current textures to TGA files
===============
*/
static void TexMgr_Imagedump_f(void)
{
	char tganame[MAX_OSPATH], tempname[MAX_OSPATH], dirname[MAX_OSPATH];
	const char* filter = NULL; // woods add filter (ironwail)
	int count = 0; // woods add filter (ironwail)
	gltexture_t* glt;
	byte* buffer;
	char* c;

	if (Cmd_Argc() >= 2) // woods add filter (ironwail)
		filter = Cmd_Argv(1); // woods add filter (ironwail)

	//create directory
	q_snprintf(dirname, sizeof(dirname), "%s/imagedump", com_gamedir);
	Sys_mkdir(dirname);

	//loop through textures
	for (glt = active_gltextures; glt; glt = glt->next)
	{

		if (filter && !q_strcasestr(glt->name, filter)) // woods add filter (ironwail)
			continue; // woods add filter (ironwail)

		q_strlcpy(tempname, glt->name, sizeof(tempname));
		while ((c = strchr(tempname, ':'))) *c = '_';
		while ((c = strchr(tempname, '/'))) *c = '_';
		while ((c = strchr(tempname, '*'))) *c = '_';
		q_snprintf(tganame, sizeof(tganame), "imagedump/%s.tga", tempname);

		GL_Bind(glt);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);/* for widths that aren't a multiple of 4 */

		if (glt->flags & TEXPREF_ALPHA)
		{
			buffer = (byte*)malloc(glt->width * glt->height * 4);
			glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, buffer);
			Image_WriteTGA(tganame, buffer, glt->width, glt->height, 32, true);
		}
		else
		{
			buffer = (byte*)malloc(glt->width * glt->height * 3);
			glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, buffer);
			Image_WriteTGA(tganame, buffer, glt->width, glt->height, 24, true);
		}
		free(buffer);
		count++; // woods add filter (ironwail)
	}

	if (filter)
	{
		if (cl_contentfilter.value) // woods #contentfilter
			Con_Printf("dumped %i textures containing '%s' to %s/imagedump.\n", count, filter, COM_SkipPath(com_gamedir));
		else
			Con_Printf("dumped %i textures containing '%s' to %s\n", count, filter, dirname);
	}
	else if (cl_contentfilter.value) // woods #contentfilter
		Con_Printf("dumped %i textures to %s/imagedump.\n", count, COM_SkipPath(com_gamedir));
	else
		Con_Printf("dumped %i textures to %s\n", count, dirname);
}

/*
===============
TexMgr_FrameUsage -- report texture memory usage for this frame
===============
*/
float TexMgr_FrameUsage(void)
{
	float mb;
	float texels = 0;
	gltexture_t* glt;

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		if (glt->visframe == r_framecount)
		{
			if (glt->flags & TEXPREF_MIPMAP)
				texels += glt->width * glt->height * 4.0f / 3.0f;
			else
				texels += (glt->width * glt->height);
		}
	}

	mb = texels * (Cvar_VariableValue("vid_bpp") / 8.0f) / 0x100000;
	return mb;
}

/*
================================================================================

	TEXTURE MANAGER

================================================================================
*/

/*
================
TexMgr_FindTexture
================
*/
gltexture_t* TexMgr_FindTexture(qmodel_t* owner, const char* name)
{
	gltexture_t* glt;

	if (name)
	{
		for (glt = active_gltextures; glt; glt = glt->next)
		{
			if (glt->owner == owner && !strcmp(glt->name, name))
				return glt;
		}
	}

	return NULL;
}

/*
================
TexMgr_NewTexture
================
*/
gltexture_t* TexMgr_NewTexture(void)
{
	gltexture_t* glt;

	if (!free_gltextures)
	{
		int i, newtexturecount = 64;
		free_gltextures = (gltexture_t*)malloc(newtexturecount * sizeof(gltexture_t));
		/* Without this check a failed malloc is a write through a NULL
		 * pointer, which on the PSP means the console goes out silently. */
		if (!free_gltextures)
			Sys_Error ("TexMgr_NewTexture: out of memory for %i textures",
			           newtexturecount);
		for (i = 0; i < newtexturecount - 1; i++)
			free_gltextures[i].next = &free_gltextures[i + 1];
		free_gltextures[i].next = NULL;
	}

	glt = free_gltextures;
	free_gltextures = glt->next;
	glt->next = active_gltextures;
	active_gltextures = glt;

	glGenTextures(1, &glt->texnum);
	numgltextures++;
	return glt;
}

static void GL_DeleteTexture(gltexture_t* texture);

//ericw -- workaround for preventing TexMgr_FreeTexture during TexMgr_ReloadImages
static qboolean in_reload_images;

/*
================
TexMgr_FreeTexture
================
*/
void TexMgr_FreeTexture(gltexture_t* kill)
{
	gltexture_t* glt;

	if (in_reload_images)
		return;

	if (kill == NULL)
	{
		Con_Printf("TexMgr_FreeTexture: NULL texture\n");
		return;
	}

	if (kill->flags & TEXPREF_COLOURMAPPED)
		TexMgr_ColormapTexture_Free(kill);

	if (active_gltextures == kill)
	{
		active_gltextures = kill->next;
		kill->next = free_gltextures;
		free_gltextures = kill;

		GL_DeleteTexture(kill);
		numgltextures--;
		return;
	}

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		if (glt->next == kill)
		{
			glt->next = kill->next;
			kill->next = free_gltextures;
			free_gltextures = kill;

			GL_DeleteTexture(kill);
			numgltextures--;
			return;
		}
	}

	Con_Printf("TexMgr_FreeTexture: not found\n");
}

/*
================
TexMgr_FreeTextures

compares each bit in "flags" to the one in glt->flags only if that bit is active in "mask"
================
*/
void TexMgr_FreeTextures(unsigned int flags, unsigned int mask)
{
	gltexture_t* glt, * next;

	for (glt = active_gltextures; glt; glt = next)
	{
		next = glt->next;
		if ((glt->flags & mask) == (flags & mask))
			TexMgr_FreeTexture(glt);
	}
}

/*
================
TexMgr_FreeTexturesForOwner
================
*/
void TexMgr_FreeTexturesForOwner(qmodel_t* owner)
{
	gltexture_t* glt, * next;

	for (glt = active_gltextures; glt; glt = next)
	{
		next = glt->next;
		if (glt && glt->owner == owner)
			TexMgr_FreeTexture(glt);
	}
}

/*
================
TexMgr_DeleteTextureObjects
================
*/
void TexMgr_DeleteTextureObjects(void)
{
	gltexture_t* glt;

	TexMgr_ColormapTexture_Free(NULL);

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		GL_DeleteTexture(glt);
	}
}

/*
================================================================================

	INIT

================================================================================
*/

/*
=================
TexMgr_LoadPalette -- johnfitz -- was VID_SetPalette, moved here, renamed, rewritten
=================
*/
void TexMgr_LoadPalette(void)
{
	byte* pal, * src, * dst;
	int i, mark;
	FILE* f;

	COM_FOpenFile("gfx/palette.lmp", &f, NULL);
	if (!f)
		Sys_Error("Couldn't load gfx/palette.lmp");

	mark = Hunk_LowMark();
	pal = (byte*)Hunk_Alloc(768);
	if (fread(pal, 1, 768, f) != 768) // woods
	{
		fclose(f);
		Sys_Error("Couldn't read gfx/palette.lmp");
	}
	fclose(f);

	//standard palette, 255 is transparent
	dst = (byte*)d_8to24table;
	src = pal;
	for (i = 0; i < 256; i++)
	{
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = 255;
	}
	((byte*)&d_8to24table[255])[3] = 0;

	//fullbright palette, 0-223 are black (for additive blending)
	src = pal + 224 * 3;
	dst = (byte*)&d_8to24table_fbright[224];
	for (i = 224; i < 256; i++)
	{
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = 255;
	}
	for (i = 0; i < 224; i++)
	{
		dst = (byte*)&d_8to24table_fbright[i];
		dst[3] = 255;
		dst[2] = dst[1] = dst[0] = 0;
	}

	//nobright palette, 224-255 are black (for additive blending)
	dst = (byte*)d_8to24table_nobright;
	src = pal;
	for (i = 0; i < 256; i++)
	{
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = *src++;
		*dst++ = 255;
	}
	for (i = 224; i < 256; i++)
	{
		dst = (byte*)&d_8to24table_nobright[i];
		dst[3] = 255;
		dst[2] = dst[1] = dst[0] = 0;
	}

	//fullbright palette, for fence textures
	memcpy(d_8to24table_fbright_fence, d_8to24table_fbright, 256 * 4);
	d_8to24table_fbright_fence[255] = 0; // Alpha of zero.

	//nobright palette, for fence textures
	memcpy(d_8to24table_nobright_fence, d_8to24table_nobright, 256 * 4);
	d_8to24table_nobright_fence[255] = 0; // Alpha of zero.

	//conchars palette, 0 and 255 are transparent
	memcpy(d_8to24table_conchars, d_8to24table, 256 * 4);
	((byte*)&d_8to24table_conchars[0])[3] = 0;

	Hunk_FreeToLowMark(mark);
}

/*
================
TexMgr_NewGame
================
*/
void TexMgr_NewGame(void)
{
	TexMgr_ColormapTexture_Free(NULL);

	/* Everything below is about to stop existing, and the picture cache in
	 * gl_draw.c is holding raw pointers into this list.  Tell it first:
	 * otherwise it hands those pointers back to TexMgr_FreeTexture() later,
	 * which either answers "not found" or -- when the struct has already
	 * been reused by a different texture -- deletes a live one.  The log of
	 * that is fifty "TexMgr_FreeTexture: not found" lines in a row followed
	 * by picture memory collapsing and every later PNG failing to decode. */
	Draw_ForgetTextures();

	TexMgr_FreeTextures(0, TEXPREF_PERSIST); //deletes all textures where TEXPREF_PERSIST is unset
	TexMgr_LoadPalette();
}

/*
===============
TexMgr_Anisotropy_Completion_f -- woods #iwtabcomplete
===============
*/
static void TexMgr_Anisotropy_Completion_f(cvar_t* cvar, const char* partial)
{
	Con_AddToTabList("1", partial, "no filtering - bilinear", NULL);
	Con_AddToTabList("2", partial, "2x - basic clarity", NULL);
	Con_AddToTabList("4", partial, "4x - moderate clarity", NULL);
	Con_AddToTabList("8", partial, "8x - good balance", NULL);
	Con_AddToTabList("16", partial, "16x - highest quality", NULL);

	return;
}

/*
================
TexMgr_Init

must be called before any texture loading
================
*/
void TexMgr_Init(void)
{
	int i;
	static byte notexture_data[16] = { 159,91,83,255,0,0,0,255,0,0,0,255,159,91,83,255 }; //black and pink checker
	static byte nulltexture_data[16] = { 127,191,255,255,0,0,0,255,0,0,0,255,127,191,255,255 }; //black and blue checker

	// init texture list
	int initialtexturecount = 256;
	free_gltextures = (gltexture_t*)Hunk_AllocName(initialtexturecount * sizeof(gltexture_t), "gltextures");
	active_gltextures = NULL;
	for (i = 0; i < initialtexturecount - 1; i++)
		free_gltextures[i].next = &free_gltextures[i + 1];
	free_gltextures[i].next = NULL;
	numgltextures = 0;

	// palette
	TexMgr_LoadPalette();

	Cvar_RegisterVariable(&gl_max_size);
	Cvar_SetCallback(&gl_max_size, VID_ChangedRestart_f); // woods #vidrestart
	Cvar_RegisterVariable(&gl_picmip);
	Cvar_SetCallback(&gl_picmip, VID_ChangedRestart_f); // woods #vidrestart
	Cvar_RegisterVariable(&r_fastturb); // woods #fastturb
	Cvar_RegisterVariable(&gl_texture_anisotropy);
	Cvar_SetCallback(&gl_texture_anisotropy, &TexMgr_Anisotropy_f);
	Cvar_SetCompletion(&gl_texture_anisotropy, &TexMgr_Anisotropy_Completion_f); // woods #iwtabcomplete
	gl_texturemode.string = glmodes[glmode_idx].name1 ? glmodes[glmode_idx].name1 : glmodes[glmode_idx].name2;
	Cvar_RegisterVariable(&gl_texturemode);
	Cvar_SetCallback(&gl_texturemode, &TexMgr_TextureMode_f);
	Cvar_SetCompletion(&gl_texturemode, &TexMgr_TextureMode_Completion_f); // woods #iwtabcomplete
	Cmd_AddCommand("gl_describetexturemodes", &TexMgr_DescribeTextureModes_f);
	Cmd_AddCommand("imagelist", &TexMgr_Imagelist_f);
	Cmd_AddCommand("imagedump", &TexMgr_Imagedump_f);

	// load notexture images
	notexture = TexMgr_LoadImage(NULL, "notexture", 2, 2, SRC_RGBA, notexture_data, "", (src_offset_t)notexture_data, TEXPREF_NEAREST | TEXPREF_PERSIST | TEXPREF_NOPICMIP);
	nulltexture = TexMgr_LoadImage(NULL, "nulltexture", 2, 2, SRC_RGBA, nulltexture_data, "", (src_offset_t)nulltexture_data, TEXPREF_NEAREST | TEXPREF_PERSIST | TEXPREF_NOPICMIP);

	//have to assign these here becuase Mod_Init is called before TexMgr_Init
	r_notexture_mip->gltexture = r_notexture_mip2->gltexture = notexture;
}

/*
================================================================================

	IMAGE LOADING

================================================================================
*/

/*
================
TexMgr_Pad -- return smallest power of two greater than or equal to s
================
*/
int TexMgr_Pad(int s)
{
	int i;
	for (i = 1; i < s; i <<= 1)
		;
	return i;
}

/*
===============
TexMgr_SafeTextureSize -- return a size with hardware and user prefs in mind
===============
*/
int TexMgr_SafeTextureSize(int s)
{
	int p = abs((int)gl_max_size.value); // woods add abs
	if (!gl_texture_NPOT)
		s = TexMgr_Pad(s);
	if (p > 0) {
		p = TexMgr_Pad(p);
		if (p < s) s = p;
	}
	if (s > gl_hardware_maxsize)
		s = gl_hardware_maxsize;
	return s;
}

/*
===============
TexMgr_SafeTextureSize2 -- // woods #gl_max_size
===============
*/
int TexMgr_SafeTextureSize2(int s)
{
	int p = 0;
	if (!gl_texture_NPOT)
		s = TexMgr_Pad(s);
	if (p > 0) {
		p = TexMgr_Pad(p);
		if (p < s) s = p;
	}
	if (s > gl_hardware_maxsize)
		s = gl_hardware_maxsize;
	return s;
}

/*
===============
TexMgr_SafeTextureSize3 -- // woods #fastturb
===============
*/
int TexMgr_SafeTextureSize3(int s)
{
	int p = 1;
	if (!gl_texture_NPOT)
		s = TexMgr_Pad(s);
	if (p > 0) {
		p = TexMgr_Pad(p);
		if (p < s) s = p;
	}
	if (s > gl_hardware_maxsize)
		s = gl_hardware_maxsize;
	return s;
}

/*
================
TexMgr_PadConditional -- only pad if a texture of that size would be padded. (used for tex coords)
================
*/
int TexMgr_PadConditional(int s)
{
	if (s < TexMgr_SafeTextureSize(s))
		return TexMgr_Pad(s);
	else
		return s;
}

/*
================
TexMgr_MipMapW
================
*/
static unsigned* TexMgr_MipMapW(unsigned* data, int width, int height)
{
	int	i, size;
	byte* out, * in;

	out = in = (byte*)data;
	size = (width * height) >> 1;

	for (i = 0; i < size; i++, out += 4, in += 8)
	{
		out[0] = (in[0] + in[4]) >> 1;
		out[1] = (in[1] + in[5]) >> 1;
		out[2] = (in[2] + in[6]) >> 1;
		out[3] = (in[3] + in[7]) >> 1;
	}

	return data;
}

/*
================
TexMgr_MipMapH
================
*/
static unsigned* TexMgr_MipMapH(unsigned* data, int width, int height)
{
	int	i, j;
	byte* out, * in;

	out = in = (byte*)data;
	height >>= 1;
	width <<= 2;

	for (i = 0; i < height; i++, in += width)
	{
		for (j = 0; j < width; j += 4, out += 4, in += 4)
		{
			out[0] = (in[0] + in[width + 0]) >> 1;
			out[1] = (in[1] + in[width + 1]) >> 1;
			out[2] = (in[2] + in[width + 2]) >> 1;
			out[3] = (in[3] + in[width + 3]) >> 1;
		}
	}

	return data;
}

/*
================
TexMgr_ResampleTexture -- bilinear resample
================
*/
static unsigned* TexMgr_ResampleTexture(unsigned* in, int inwidth, int inheight, qboolean alpha)
{
	byte* nwpx, * nepx, * swpx, * sepx, * dest;
	unsigned xfrac, yfrac, x, y, modx, mody, imodx, imody, injump, outjump;
	unsigned* out;
	int i, j, outwidth, outheight;

	/* A zero dimension must never reach the arithmetic below: xfrac/yfrac
	 * divide by (outwidth - 1), and Pad(0) is 1, so the divisor would be 0 --
	 * on the PSP that is a trap exception, not an infinity. */
	if (inwidth < 1 || inheight < 1)
		return in;

	if (inwidth == TexMgr_Pad(inwidth) && inheight == TexMgr_Pad(inheight))
		return in;

	outwidth = TexMgr_Pad(inwidth);
	outheight = TexMgr_Pad(inheight);
	out = (unsigned*)Hunk_Alloc(outwidth * outheight * 4);

	/* A one-pixel dimension makes the divisor zero, and a division by zero on
	 * the PSP is a trap exception, not an infinity: the module dies with no
	 * message.  A single row or column needs no interpolation anyway. */
	xfrac = (outwidth > 1) ? (unsigned)(((inwidth - 1) << 16) / (outwidth - 1)) : 0;
	yfrac = (outheight > 1) ? (unsigned)(((inheight - 1) << 16) / (outheight - 1)) : 0;
	y = outjump = 0;

	for (i = 0; i < outheight; i++)
	{
		mody = (y >> 8) & 0xFF;
		imody = 256 - mody;
		injump = (y >> 16) * inwidth;
		x = 0;

		for (j = 0; j < outwidth; j++)
		{
			unsigned ix = x >> 16;
			unsigned iy = injump / (unsigned)inwidth;
			unsigned ix1, iy1;

			modx = (x >> 8) & 0xFF;
			imodx = 256 - modx;

			/* Clamp the four neighbours inside the source image.
			 *
			 * The routine was written for *downscaling*, where (x>>16) never
			 * reaches the last column.  Upscaling happens for every non
			 * power-of-two picture, and there nwpx on the bottom row points
			 * one full row past the end of the buffer, swpx two rows past it.
			 * On a desktop that reads whatever is next in the heap; on the PSP
			 * those bytes can be past the end of the block we own. */
			if (ix > (unsigned)(inwidth - 1))
				ix = inwidth - 1;
			if (iy > (unsigned)(inheight - 1))
				iy = inheight - 1;

			ix1 = (ix + 1 < (unsigned)inwidth) ? ix + 1 : ix;
			iy1 = (iy + 1 < (unsigned)inheight) ? iy + 1 : iy;

			nwpx = (byte*)(in + iy * inwidth + ix);
			nepx = (byte*)(in + iy * inwidth + ix1);
			swpx = (byte*)(in + iy1 * inwidth + ix);
			sepx = (byte*)(in + iy1 * inwidth + ix1);

			dest = (byte*)(out + outjump + j);

			dest[0] = (nwpx[0] * imodx * imody + nepx[0] * modx * imody + swpx[0] * imodx * mody + sepx[0] * modx * mody) >> 16;
			dest[1] = (nwpx[1] * imodx * imody + nepx[1] * modx * imody + swpx[1] * imodx * mody + sepx[1] * modx * mody) >> 16;
			dest[2] = (nwpx[2] * imodx * imody + nepx[2] * modx * imody + swpx[2] * imodx * mody + sepx[2] * modx * mody) >> 16;
			if (alpha)
				dest[3] = (nwpx[3] * imodx * imody + nepx[3] * modx * imody + swpx[3] * imodx * mody + sepx[3] * modx * mody) >> 16;
			else
				dest[3] = 255;

			x += xfrac;
		}
		outjump += outwidth;
		y += yfrac;
	}

	return out;
}

/*
===============
TexMgr_AlphaEdgeFix

eliminate pink edges on sprites, etc.
operates in place on 32bit data

spike -- small note that would be better to use premultiplied alpha to completely eliminate these skirts without the possibility of misbehaving.
===============
*/
static void TexMgr_AlphaEdgeFix(byte* data, int width, int height)
{
	int	i, j, n = 0, b, c[3] = { 0,0,0 },
		lastrow, thisrow, nextrow,
		lastpix, thispix, nextpix;
	byte* dest = data;

	for (i = 0; i < height; i++)
	{
		lastrow = width * 4 * ((i == 0) ? height - 1 : i - 1);
		thisrow = width * 4 * i;
		nextrow = width * 4 * ((i == height - 1) ? 0 : i + 1);

		for (j = 0; j < width; j++, dest += 4)
		{
			if (dest[3]) //not transparent
				continue;

			lastpix = 4 * ((j == 0) ? width - 1 : j - 1);
			thispix = 4 * j;
			nextpix = 4 * ((j == width - 1) ? 0 : j + 1);

			b = lastrow + lastpix; if (data[b + 3]) { c[0] += data[b]; c[1] += data[b + 1]; c[2] += data[b + 2]; n++; }
			b = thisrow + lastpix; if (data[b + 3]) { c[0] += data[b]; c[1] += data[b + 1]; c[2] += data[b + 2]; n++; }
			b = nextrow + lastpix; if (data[b + 3]) { c[0] += data[b]; c[1] += data[b + 1]; c[2] += data[b + 2]; n++; }
			b = lastrow + thispix; if (data[b + 3]) { c[0] += data[b]; c[1] += data[b + 1]; c[2] += data[b + 2]; n++; }
			b = nextrow + thispix; if (data[b + 3]) { c[0] += data[b]; c[1] += data[b + 1]; c[2] += data[b + 2]; n++; }
			b = lastrow + nextpix; if (data[b + 3]) { c[0] += data[b]; c[1] += data[b + 1]; c[2] += data[b + 2]; n++; }
			b = thisrow + nextpix; if (data[b + 3]) { c[0] += data[b]; c[1] += data[b + 1]; c[2] += data[b + 2]; n++; }
			b = nextrow + nextpix; if (data[b + 3]) { c[0] += data[b]; c[1] += data[b + 1]; c[2] += data[b + 2]; n++; }

			//average all non-transparent neighbors
			if (n)
			{
				dest[0] = (byte)(c[0] / n);
				dest[1] = (byte)(c[1] / n);
				dest[2] = (byte)(c[2] / n);

				n = c[0] = c[1] = c[2] = 0;
			}
		}
	}
}

/*
===============
TexMgr_PadEdgeFixW -- special case of AlphaEdgeFix for textures that only need it because they were padded

operates in place on 32bit data, and expects unpadded height and width values
===============
*/
static void TexMgr_PadEdgeFixW(byte* data, int width, int height)
{
	byte* src, * dst;
	int i, padw, padh;

	padw = TexMgr_PadConditional(width);
	padh = TexMgr_PadConditional(height);

	//copy last full column to first empty column, leaving alpha byte at zero
	src = data + (width - 1) * 4;
	for (i = 0; i < padh; i++)
	{
		src[4] = src[0];
		src[5] = src[1];
		src[6] = src[2];
		src += padw * 4;
	}

	//copy first full column to last empty column, leaving alpha byte at zero
	src = data;
	dst = data + (padw - 1) * 4;
	for (i = 0; i < padh; i++)
	{
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = src[2];
		src += padw * 4;
		dst += padw * 4;
	}
}

/*
===============
TexMgr_PadEdgeFixH -- special case of AlphaEdgeFix for textures that only need it because they were padded

operates in place on 32bit data, and expects unpadded height and width values
===============
*/
static void TexMgr_PadEdgeFixH(byte* data, int width, int height)
{
	byte* src, * dst;
	int i, padw, padh;

	padw = TexMgr_PadConditional(width);
	padh = TexMgr_PadConditional(height);

	//copy last full row to first empty row, leaving alpha byte at zero
	dst = data + height * padw * 4;
	src = dst - padw * 4;
	for (i = 0; i < padw; i++)
	{
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = src[2];
		src += 4;
		dst += 4;
	}

	//copy first full row to last empty row, leaving alpha byte at zero
	dst = data + (padh - 1) * padw * 4;
	src = data;
	for (i = 0; i < padw; i++)
	{
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = src[2];
		src += 4;
		dst += 4;
	}
}

/*
================
TexMgr_8to32
================
*/
static unsigned* TexMgr_8to32(byte* in, int pixels, unsigned int* usepal)
{
	int i;
	unsigned* out, * data;

	out = data = (unsigned*)Hunk_Alloc(pixels * 4);

	for (i = 0; i < pixels; i++)
		*out++ = usepal[*in++];

	return data;
}

/*
================
TexMgr_PadImageW -- return image with width padded up to power-of-two dimentions
================
*/
static byte* TexMgr_PadImageW(byte* in, int width, int height, byte padbyte)
{
	int i, j, outwidth;
	byte* out, * data;

	if (width == TexMgr_Pad(width))
		return in;

	outwidth = TexMgr_Pad(width);

	out = data = (byte*)Hunk_Alloc(outwidth * height);

	for (i = 0; i < height; i++)
	{
		for (j = 0; j < width; j++)
			*out++ = *in++;
		for (; j < outwidth; j++)
			*out++ = padbyte;
	}

	return data;
}

/*
================
TexMgr_PadImageH -- return image with height padded up to power-of-two dimentions
================
*/
static byte* TexMgr_PadImageH(byte* in, int width, int height, byte padbyte)
{
	int i, srcpix, dstpix;
	byte* data, * out;

	if (height == TexMgr_Pad(height))
		return in;

	srcpix = width * height;
	dstpix = width * TexMgr_Pad(height);

	out = data = (byte*)Hunk_Alloc(dstpix);

	for (i = 0; i < srcpix; i++)
		*out++ = *in++;
	for (; i < dstpix; i++)
		*out++ = padbyte;

	return data;
}

static byte* TexMgr_PreMultiply32(byte* in, size_t width, size_t height)
{
	size_t pixels = width * height;
	byte* out = (byte*)Hunk_Alloc(pixels * 4);
	byte* result = out;
	while (pixels-- > 0)
	{
		out[0] = (in[0] * in[3]) >> 8;
		out[1] = (in[1] * in[3]) >> 8;
		out[2] = (in[2] * in[3]) >> 8;
		out[3] = in[3];
		in += 4;
		out += 4;
	}
	return result;
}

/*
================
TexMgr_LoadImage32 -- handles 32bit source data
================
*/
static void TexMgr_LoadImage32(gltexture_t* glt, unsigned* data)
{
	int	internalformat, miplevel, mipwidth, mipheight, picmip;
	char mapname[MAX_QPATH]; // woods #gl_max_size

	//do this before any rescaling
	if (glt->flags & TEXPREF_PREMULTIPLY)
		data = (unsigned*)TexMgr_PreMultiply32((byte*)data, glt->width, glt->height);

	TEX_MARK("tex32: resample");

	if (!gl_texture_NPOT)
	{
		// resample up
		data = TexMgr_ResampleTexture(data, glt->width, glt->height, glt->flags & TEXPREF_ALPHA);
		glt->width = TexMgr_Pad(glt->width);
		glt->height = TexMgr_Pad(glt->height);
	}

	// mipmap down

	picmip = (glt->flags & TEXPREF_NOPICMIP) ? 0 : q_max((int)gl_picmip.value, 0);

	if (gl_max_size.value) // woods #gl_max_size only apply to map bsp wall/floors
	{
		COM_FileBase(glt->name, mapname, sizeof(mapname));

		if (!strstr(glt->name, "maps") || isSpecialMap(mapname) || strstr(glt->name, "*") || strstr(glt->name, "sky"))
		{
			mipwidth = TexMgr_SafeTextureSize2(glt->width >> picmip);
			mipheight = TexMgr_SafeTextureSize2(glt->height >> picmip);
		}
		else
		{
			if (gl_max_size.value < 0) // woods - grayscale for negative values
			{
				for (unsigned int i = 0; i < (glt->width * glt->height); i++)
				{
					unsigned char* pixel = (unsigned char*)&data[i];
					// Calculate luminance
					float grey = 0.3f * pixel[0] + 0.6f * pixel[1] + 0.1f * pixel[2];
					// Set RGB to luminance (maintain alpha)
					pixel[0] = pixel[1] = pixel[2] = (unsigned char)grey;
				}
			}

			mipwidth = TexMgr_SafeTextureSize(glt->width >> picmip);
			mipheight = TexMgr_SafeTextureSize(glt->height >> picmip);
		}
	}
	else
	{
		mipwidth = TexMgr_SafeTextureSize(glt->width >> picmip);
		mipheight = TexMgr_SafeTextureSize(glt->height >> picmip);
	}

	if (strstr(glt->name, "*") && r_fastturb.value) // woods #fastturb
	{
		mipwidth = TexMgr_SafeTextureSize3(glt->width >> picmip);
		mipheight = TexMgr_SafeTextureSize3(glt->height >> picmip);
	}

	TEX_MARK("tex32: mipmap");

	while ((int)glt->width > mipwidth)
	{
		TexMgr_MipMapW(data, glt->width, glt->height);
		glt->width >>= 1;
		if (glt->flags & TEXPREF_ALPHA)
			TexMgr_AlphaEdgeFix((byte*)data, glt->width, glt->height);
	}
	while ((int)glt->height > mipheight)
	{
		TexMgr_MipMapH(data, glt->width, glt->height);
		glt->height >>= 1;
		if (glt->flags & TEXPREF_ALPHA)
			TexMgr_AlphaEdgeFix((byte*)data, glt->width, glt->height);
	}

	// upload
	/* The size goes into the mark: if a picture dies here, its dimensions are
	 * the first thing to check (a padded 1xN or a giant one explains a lot). */
	TEX_MARK(va("tex32: upload %dx%d", glt->width, glt->height));
	TEX_MARK("tex32: bind");
	GL_Bind(glt);
	internalformat = (glt->flags & TEXPREF_ALPHA) ? gl_alpha_format : gl_solid_format;
	TEX_MARK("tex32: image2d");
	glTexImage2D(GL_TEXTURE_2D, 0, internalformat, glt->width, glt->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

	// upload mipmaps
	if (glt->flags & TEXPREF_MIPMAP && !(glt->flags & TEXPREF_WARPIMAGE)) // warp image mipmaps are generated later
	{
		TEX_MARK("tex32: mipchain");
		mipwidth = glt->width;
		mipheight = glt->height;

		for (miplevel = 1; mipwidth > 1 || mipheight > 1; miplevel++)
		{
			if (mipwidth > 1)
			{
				TexMgr_MipMapW(data, mipwidth, mipheight);
				mipwidth >>= 1;
			}
			if (mipheight > 1)
			{
				TexMgr_MipMapH(data, mipwidth, mipheight);
				mipheight >>= 1;
			}
			glTexImage2D(GL_TEXTURE_2D, miplevel, internalformat, mipwidth, mipheight, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
		}
	}

	// set filter modes
	TEX_MARK("tex32: filter");
	TexMgr_SetFilterModes(glt);
	TEX_MARK("tex32: done");
}

void TexMgr_BlockSize(enum srcformat format, int* bytes, int* width, int* height)
{
	*width = 1;
	*height = 1;
	switch (format)
	{
	case SRC_RGBA:
		*bytes = 4;
		break;
	case SRC_LIGHTMAP:
		*bytes = lightmap_bytes;
		break;
	case SRC_INDEXED:
		*bytes = 1;
		break;
	case SRC_EXTERNAL:
		*bytes = 0;
		break;
	default:
		*bytes = compressedformats[format].blockbytes;
		*width = compressedformats[format].blockwidth;
		*height = compressedformats[format].blockheight;
		break;
	}
}
size_t TexMgr_ImageSize(int width, int height, enum srcformat format)
{
	int	miplevel, mipwidth, mipheight;
	size_t mipbytes = 0, blockbytes;
	unsigned int blockwidth, blockheight;
	switch (format)
	{
	case SRC_RGBA:
		return width * height * 4;
	case SRC_LIGHTMAP:
		return width * height * lightmap_bytes;
	case SRC_INDEXED:
		return width * height;
	case SRC_EXTERNAL:	//panic
		Con_Printf("TexMgr_ImageCompressedSize called for SRC_EXTERNAL\n");
		return 0;
	default:
		//a compressed format with multiple mip levels in it
		blockbytes = compressedformats[format].blockbytes;
		blockwidth = compressedformats[format].blockwidth;
		blockheight = compressedformats[format].blockheight;
		for (miplevel = 0; ; miplevel++)
		{
			mipwidth = width >> miplevel;
			mipheight = height >> miplevel;
			if (!mipwidth && !mipheight)
				break;
			mipwidth = q_max(1, mipwidth);	//include the 1*1 mip with non-square textures.
			mipheight = q_max(1, mipheight);
			mipbytes += blockbytes * ((mipwidth + blockwidth - 1) / blockwidth) * ((mipheight + blockheight - 1) / blockheight);
		}
		return mipbytes;
	}
}
enum srcformat TexMgr_FormatForName(const char* code)
{
	size_t i;
	for (i = 0; i < sizeof(compressedformats) / sizeof(compressedformats[0]); i++)
	{
		if (!compressedformats[i].formatname)
			continue;
		if (!q_strcasecmp(code, compressedformats[i].formatname))
			return i;
	}
	return SRC_EXTERNAL;
}
enum srcformat TexMgr_FormatForCode(const char* code)
{
	size_t i;
	for (i = 0; i < sizeof(compressedformats) / sizeof(compressedformats[0]); i++)
	{
		if (!compressedformats[i].mipextname)
			continue;
		if (!q_strncasecmp(code, compressedformats[i].mipextname, 4))
			return i;
	}
	return SRC_EXTERNAL;
}
static void TexMgr_LoadImageCompressed(gltexture_t* glt, byte* data)
{
	int	internalformat, format, type, miplevel, mipwidth, mipheight, picmip;
	size_t mipbytes, blockbytes;
	unsigned int blockwidth, blockheight;

	internalformat = compressedformats[glt->source_format].internalformat;
	format = compressedformats[glt->source_format].format;
	type = compressedformats[glt->source_format].type;
	blockbytes = compressedformats[glt->source_format].blockbytes;
	blockwidth = compressedformats[glt->source_format].blockwidth;
	blockheight = compressedformats[glt->source_format].blockheight;

	//no premultiply support.
	//no npot fallback support

	// mipmap down
	picmip = ((glt->flags & TEXPREF_NOPICMIP) || !(glt->flags & TEXPREF_MIPMAP)) ? 0 : q_max((int)gl_picmip.value, 0);

	//make sure the picmip level is not bigger than the number of mips that we have available...
	while (picmip && (!(glt->width >> picmip) || !(glt->height >> picmip)))
		picmip--;

	if (type && blockbytes < 4)
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);	//makes stuff work more reliably, if slower.

	//upload each mip level in turn.
	GL_Bind(glt);
	for (miplevel = 0; ; miplevel++)
	{
		mipwidth = glt->width >> miplevel;
		mipheight = glt->height >> miplevel;
		if (!mipwidth && !mipheight)
			break;
		mipwidth = q_max(1, mipwidth);	//include the 1*1 mip with non-square textures.
		mipheight = q_max(1, mipheight);
		mipbytes = blockbytes * ((mipwidth + blockwidth - 1) / blockwidth) * ((mipheight + blockheight - 1) / blockheight);
		if (miplevel - picmip >= 0)
		{
			if (type)
				glTexImage2D(GL_TEXTURE_2D, miplevel - picmip, internalformat, mipwidth, mipheight, 0, format, type, data);
			else
				GL_CompressedTexImage2D(GL_TEXTURE_2D, miplevel - picmip, internalformat, mipwidth, mipheight, 0, mipbytes, data);
		}
		data += mipbytes;

		if (!(glt->flags & TEXPREF_MIPMAP))
			break;
	}

	if (type && blockbytes < 4)
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);	//back to opengl's default.

	// set filter modes
	TexMgr_SetFilterModes(glt);
}

#if defined(PLATFORM_PSP) || defined(__psp__)
/*
================
TexMgr_Downscale8 -- halve 8-bit texels until both sides fit maxdim

The GE cannot sample more than 512 texels in either direction.  pspsdk
src/gu/sceGuTexImage.c keeps width and height as 4-bit logarithms
(getExp(height) << 8 | getExp(width)), so anything above 512 either loses a bit
of its size or, at 1024 and up, is masked to zero and encoded as 0xFFFF --
"sample a 32768x32768 texture".  port/psp/gl_shim.c refuses those pictures
instead of drawing garbage (SHIM_TEX_MAX), but by then the engine has already
paid for them.

Round 70 showed what "paid for them" costs.  Entering the map needed

    Hunk_Alloc 4194336 bytes failed: 19456 KB total, 18128 KB used, 1327 KB free

and 4194336 is 32 bytes of hunk header plus exactly 4 MB: one 1024x1024 miptex
expanded by TexMgr_8to32() at four bytes a texel, into a hunk that also holds
the zone, the console buffer, progs/player.mdl and cl_entities.  The texture
could never have been drawn either -- 1024 is past what the GE addresses.

Interstice-ProQuake (MotoLegacy/Interstice-ProQuake) does the opposite of this
whole pipeline on the same hardware: source/psp/gu_draw.cpp GL_LoadTexture()
rounds the picture up to a power of two, halves it when it is over 800
(r_tex_scale_down), caps the mip chain at 16 texels and uploads it as
GU_PSM_T8 -- one byte a texel, the map's own palette in a CLUT -- straight into
VRAM.  There is no 32-bit copy at any point, so a big miptex costs its own size
and nothing else.

So the reduction is done here, on the indexed texels, *before* TexMgr_8to32()
turns them into four bytes each.  The copy is one byte a texel (256 KB at
512x512, against the megabyte the 32-bit version needed), and afterwards the
largest 32-bit scratch this file can ask the hunk for is 512*512*4 = 1 MB.

It is a copy and not an edit in place, because the same miptex is loaded twice
when it has fullbright pixels (gl_model.c loads it once with TEXPREF_NOBRIGHT
and once with TEXPREF_FULLBRIGHT from the same `(byte *)(tx+1)`), and the second
load has no way to know the first one changed the bytes under it.

Nearest-neighbour, not averaging: these are palette *indices*, and the mean of
index 3 and index 200 is not a colour.  Averaging them is how a downscaled
texture comes out looking like noise.  Interstice subsamples for the same
reason.
================
*/
#define TEX_PSP_MAX 512

static byte *TexMgr_Downscale8 (byte *data, unsigned int *width, unsigned int *height, int maxdim)
{
	unsigned int w = *width, h = *height;
	unsigned int f = 1, nw, nh, x, y;
	byte *out;

	if (!w || !h)
		return data;

	/* How many halvings are needed to fit both sides, done in one pass: the
	 * sample step is 2^n, so every destination texel reads one source texel
	 * and nothing else. */
	while (w / f > (unsigned int)maxdim || h / f > (unsigned int)maxdim)
	{
		if (f > (1u << 16))
			break;
		f <<= 1;
	}

	nw = w / f;
	nh = h / f;
	if (nw < 1) nw = 1;
	if (nh < 1) nh = 1;

	if (nw == w && nh == h)
		return data;		/* already small enough */

	out = (byte *)Hunk_AllocName ((int)(nw * nh), "tex_scale");
	if (!out)
		return data;		/* no room: upload it as it came */

	for (y = 0; y < nh; y++)
	{
		const byte *src = data + (size_t)(y * f) * w;
		byte *dst = out + (size_t)y * nw;

		for (x = 0; x < nw; x++)
			dst[x] = src[x * f];
	}

	*width = nw;
	*height = nh;
	return out;
}
#endif

/*
================
TexMgr_LoadImage8 -- handles 8bit source data, then passes it to LoadImage32
================
*/
static void TexMgr_LoadImage8(gltexture_t* glt, byte* data)
{
	extern cvar_t gl_fullbrights;
	qboolean padw = false, padh = false;
	byte padbyte;
	unsigned int* usepal, translation[256];
	int i;

	// HACK HACK HACK -- taken from tomazquake
	if (strstr(glt->name, "shot1sid") &&
		glt->width == 32 && glt->height == 32 &&
		CRC_Block(data, 1024) == 65393)
	{
		// This texture in b_shell1.bsp has some of the first 32 pixels painted white.
		// They are invisible in software, but look really ugly in GL. So we just copy
		// 32 pixels from the bottom to make it look nice.
		memcpy(data, data + 32 * 31, 32);
	}

#if defined(PLATFORM_PSP) || defined(__psp__)
	/* ---- keep the 32-bit copy inside what the GE can address -----------
	 * See TexMgr_Downscale8 above for why this is here and not in the
	 * upload path.  gl_hardware_maxsize (512 on the PSP, vid_psp.c) clamps
	 * the *result* of the conversion, which is one texture too late: the
	 * 4 MB block has been taken by then.
	 *
	 * The gltexture stops being reloadable from its original source:
	 * TexMgr_ReloadImage() would read source_width*source_height bytes out
	 * of the lump and colour a smaller picture with them, which is not the
	 * same picture.  World textures are never reloaded (the player skins
	 * are, and those are 320x200 at most), but saying so costs one line and
	 * prevents a silently scrambled picture if that ever changes. */
	if (glt->width > TEX_PSP_MAX || glt->height > TEX_PSP_MAX)
	{
		extern void psp_tracef (const char *fmt, ...);
		unsigned int ow = glt->width, oh = glt->height;

		data = TexMgr_Downscale8 (data, &glt->width, &glt->height, TEX_PSP_MAX);
		glt->source_width = glt->width;
		glt->source_height = glt->height;
		glt->source_file[0] = 0;
		glt->source_offset = 0;

		psp_tracef ("tex: %s is %dx%d, the GE takes %d -- scaled to %dx%d",
		            glt->name, (int)ow, (int)oh, TEX_PSP_MAX,
		            (int)glt->width, (int)glt->height);
	}
#endif

	// detect false alpha cases
	if (glt->flags & TEXPREF_ALPHA && !(glt->flags & TEXPREF_CONCHARS))
	{
		for (i = 0; i < (int)(glt->width * glt->height); i++)
			if (data[i] == 255) //transparent index
				break;
		if (i == (int)(glt->width * glt->height))
			glt->flags -= TEXPREF_ALPHA;
	}

	// choose palette and padbyte
	if (glt->flags & TEXPREF_FULLBRIGHT)
	{
		if (glt->flags & TEXPREF_ALPHA)
			usepal = d_8to24table_fbright_fence;
		else
			usepal = d_8to24table_fbright;
		padbyte = 0;
	}
	else if (glt->flags & TEXPREF_NOBRIGHT && gl_fullbrights.value)
	{
		if (glt->flags & TEXPREF_ALPHA)
			usepal = d_8to24table_nobright_fence;
		else
			usepal = d_8to24table_nobright;
		padbyte = 0;
	}
	else if (glt->flags & TEXPREF_CONCHARS)
	{
		usepal = d_8to24table_conchars;
		padbyte = 0;
	}
	else
	{
		usepal = d_8to24table;
		padbyte = 255;
	}

	if (glt->shirt.type || glt->pants.type)
	{
		int shirt, pants, m;
		//create new translation table
		for (i = 0; i < 256; i++)
			translation[i] = usepal[i];

		if (glt->shirt.type == 2)
		{
			for (i = 0; i < 16; i++)
			{
				m = i | (i << 4);
				((byte*)&translation[TOP_RANGE + i])[0] = (m * glt->shirt.rgb[0]) >> 8;
				((byte*)&translation[TOP_RANGE + i])[1] = (m * glt->shirt.rgb[1]) >> 8;
				((byte*)&translation[TOP_RANGE + i])[2] = (m * glt->shirt.rgb[2]) >> 8;
				((byte*)&translation[TOP_RANGE + i])[3] = 255;
			}
		}
		else if (glt->shirt.type == 1)
		{
			shirt = glt->shirt.basic * 16;
			if (shirt < 128)
			{
				for (i = 0; i < 16; i++)
					translation[TOP_RANGE + i] = usepal[shirt + i];
			}
			else
			{
				for (i = 0; i < 16; i++)
					translation[TOP_RANGE + i] = usepal[shirt + 15 - i];
			}
		}

		if (glt->pants.type == 2)
		{
			for (i = 0; i < 16; i++)
			{
				m = i | (i << 4);
				((byte*)&translation[BOTTOM_RANGE + i])[0] = (m * glt->pants.rgb[0]) >> 8;
				((byte*)&translation[BOTTOM_RANGE + i])[1] = (m * glt->pants.rgb[1]) >> 8;
				((byte*)&translation[BOTTOM_RANGE + i])[2] = (m * glt->pants.rgb[2]) >> 8;
				((byte*)&translation[BOTTOM_RANGE + i])[3] = 255;
			}
		}
		else if (glt->pants.type == 1)
		{
			pants = glt->pants.basic * 16;
			if (pants < 128)
			{
				for (i = 0; i < 16; i++)
					translation[BOTTOM_RANGE + i] = usepal[pants + i];
			}
			else
			{
				for (i = 0; i < 16; i++)
					translation[BOTTOM_RANGE + i] = usepal[pants + 15 - i];
			}
		}

		usepal = translation;
	}

	// pad each dimention, but only if it's not going to be downsampled later
	if (glt->flags & TEXPREF_PAD)
	{
		if ((int)glt->width < TexMgr_SafeTextureSize(glt->width))
		{
			data = TexMgr_PadImageW(data, glt->width, glt->height, padbyte);
			glt->width = TexMgr_Pad(glt->width);
			padw = true;
		}
		if ((int)glt->height < TexMgr_SafeTextureSize(glt->height))
		{
			data = TexMgr_PadImageH(data, glt->width, glt->height, padbyte);
			glt->height = TexMgr_Pad(glt->height);
			padh = true;
		}
	}

	// convert to 32bit
	data = (byte*)TexMgr_8to32(data, glt->width * glt->height, usepal);

	// fix edges
	if ((glt->flags & TEXPREF_ALPHA) && !(glt->flags & TEXPREF_PREMULTIPLY))
		TexMgr_AlphaEdgeFix(data, glt->width, glt->height);
	else
	{
		if (padw)
			TexMgr_PadEdgeFixW(data, glt->source_width, glt->source_height);
		if (padh)
			TexMgr_PadEdgeFixH(data, glt->source_width, glt->source_height);
	}

	// upload it
	TexMgr_LoadImage32(glt, (unsigned*)data);
}

/*
================
TexMgr_LoadLightmap -- handles lightmap data
================
*/
static void TexMgr_LoadLightmap(gltexture_t* glt, byte* data)
{
#if defined(PLATFORM_PSP) || defined(__psp__)
	/* A map without its lightmaps is a black map, and the picture pool is
	 * always full by the time a map is entered (gl_shim.c: the round-73 log
	 * has it at 6131 KB of 6144 with 12 KB free).  Say that this upload is a
	 * lightmap, so the shim lets it past the picture cap.  Cleared again
	 * below, so no ordinary picture inherits the exemption. */
	{
		extern void psp_gl_shim_priority_texture (int on);

		psp_gl_shim_priority_texture (1);
	}
#endif
	// upload it
	GL_Bind(glt);
	if (gl_lightmap_format == GL_RGB9_E5)
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB9_E5, glt->width, glt->height, 0, GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV, data);
	else
		glTexImage2D(GL_TEXTURE_2D, 0, lightmap_bytes, glt->width, glt->height, 0, gl_lightmap_format, GL_UNSIGNED_BYTE, data);

#if defined(PLATFORM_PSP) || defined(__psp__)
	{
		extern void psp_gl_shim_priority_texture (int on);

		psp_gl_shim_priority_texture (0);
	}
#endif

	// set filter modes
	TexMgr_SetFilterModes(glt);
}

/*
================
TexMgr_LoadImage -- the one entry point for loading all textures
================
*/
gltexture_t* TexMgr_LoadImage(qmodel_t* owner, const char* name, int width, int height, enum srcformat format,
	byte* data, const char* source_file, src_offset_t source_offset, unsigned flags)
{
	{
		/* The prefix matters: it says whether the hang is before the texture
		 * path (the wad lookup, "num_0") or inside it ("tex num_0"). */
		char texmark[64];
		snprintf(texmark, sizeof(texmark), "tex %s", name ? name : "?");
		TEX_MARK(texmark);
	}
	unsigned short crc;
	gltexture_t* glt = NULL;
	int mark;
	qboolean malloced = false;
	enum srcformat fmt = format;

	if (isDedicated)
		return NULL;

	// cache check
	if (format == SRC_EXTERNAL || format == SRC_LIGHTMAP)
		crc = 0;
	else
		crc = CRC_Block(data, TexMgr_ImageSize(width, height, format));
	if ((flags & TEXPREF_OVERWRITE) && (glt = TexMgr_FindTexture(owner, name)))
	{
		if (glt->source_crc == crc)
			return glt;
	}

	if (format == SRC_EXTERNAL)
	{
		data = Image_LoadImage(source_file, &width, &height, &fmt, &malloced); //simple file
		if (!data && (flags & TEXPREF_ALLOWMISSING))
			return NULL;	//don't allocate anything.
	}

	if (!glt)
		glt = TexMgr_NewTexture();

	// copy data
	glt->owner = owner;
	q_strlcpy(glt->name, name, sizeof(glt->name));
	glt->width = width;
	glt->height = height;
	glt->flags = flags;
	glt->shirt.type = 0;
	glt->pants.type = 0;
	q_strlcpy(glt->source_file, source_file, sizeof(glt->source_file));
	glt->source_offset = source_offset;
	glt->source_format = format;
	glt->source_width = width;
	glt->source_height = height;
	glt->source_crc = crc;

	TEX_MARK("tex: convert");

	//upload it
	mark = Hunk_LowMark();

	switch (glt->source_format)
	{
	case SRC_INDEXED:
		TexMgr_LoadImage8(glt, data);
		break;
	case SRC_LIGHTMAP:
		TexMgr_LoadLightmap(glt, data);
		break;
	case SRC_EXTERNAL:
		if (!data)
		{
			glt->source_width = glt->source_height = 1;
			glt->width = glt->source_width;
			glt->height = glt->source_height;
			TexMgr_LoadImage8(glt, (byte*)"\x07");

			glt->source_width = glt->source_height = 0;
			glt->width = glt->source_width;
			glt->height = glt->source_height;
		}
		else
		{
			glt->width = glt->source_width;
			glt->height = glt->source_height;
			if (fmt == SRC_RGBA)
				TexMgr_LoadImage32(glt, (unsigned*)data);
			else if (fmt == SRC_INDEXED)
				TexMgr_LoadImage8(glt, data);
			else
				TexMgr_LoadImageCompressed(glt, data);
			if (malloced)
				free(data);
		}
		break;
	case SRC_RGBA:
		TexMgr_LoadImage32(glt, (unsigned*)data);
		break;
	default:
		TexMgr_LoadImageCompressed(glt, data);
		break;
	}

	TEX_MARK("tex: free hunk");
	Hunk_FreeToLowMark(mark);

	TEX_MARK("tex: done");
	return glt;
}

/*
================================================================================

	COLORMAPPING AND TEXTURE RELOADING

================================================================================
*/

/*
================
TexMgr_ReloadImage -- reloads a texture, and colormaps it if needed
================
*/
void TexMgr_ReloadImage(gltexture_t* glt, plcolour_t shirt, plcolour_t pants)
{
	byte* data = NULL;
	int	mark, size;
	qboolean malloced = false;
	enum srcformat fmt = glt->source_format;
	//
	// get source data
	//
	mark = Hunk_LowMark();

	if (glt->source_file[0] && glt->source_offset)
	{	//lump inside file
		FILE* f;
		COM_FOpenFile(glt->source_file, &f, NULL);
		if (!f) goto invalid;
		fseek(f, glt->source_offset, SEEK_CUR);

		size = TexMgr_ImageSize(glt->source_width, glt->source_height, glt->source_format);
		data = (byte*)Hunk_Alloc(size);
		if (fread(data, 1, size, f) != size) // woods
		{
			fclose(f);
			goto invalid;
		}
		fclose(f);
	}
	else if (glt->source_file[0] && !glt->source_offset)
	{
		data = Image_LoadImage(glt->source_file, (int*)&glt->source_width, (int*)&glt->source_height, &fmt, &malloced); //simple file
	}
	else if (!glt->source_file[0] && glt->source_offset)
	{
		data = (byte*)glt->source_offset; //image in memory
	}
	if (!data) {
	invalid:	/*Con_Printf("TexMgr_ReloadImage: invalid source for %s\n", glt->name);*/ // woods disable 
		Hunk_FreeToLowMark(mark);
		return;
	}

	glt->width = glt->source_width;
	glt->height = glt->source_height;
	//
	// apply shirt and pants colors
	//
	// if shirt and pants are -1,-1, use existing shirt and pants colors
	// if existing shirt and pants colors are -1,-1, don't bother colormapping
	if (shirt.type || pants.type)
	{
		if (fmt == SRC_INDEXED)
		{
			glt->shirt = shirt;
			glt->pants = pants;
		}/*
		else
			Con_Printf ("TexMgr_ReloadImage: can't colormap a non SRC_INDEXED texture: %s\n", glt->name);*/
	}

	//
	// upload it
	//
	switch (fmt)
	{
	case SRC_INDEXED:
		TexMgr_LoadImage8(glt, data);
		break;
	case SRC_LIGHTMAP:
		TexMgr_LoadLightmap(glt, data);
		break;
	case SRC_EXTERNAL:
	case SRC_RGBA:
		TexMgr_LoadImage32(glt, (unsigned*)data);
		break;
	default:
		TexMgr_LoadImageCompressed(glt, data);
		break;
	}

	if (malloced)
		free(data);
	Hunk_FreeToLowMark(mark);
}

/*
================
TexMgr_ReloadImages -- reloads all texture images. called only by vid_restart
================
*/
void TexMgr_ReloadImages(void)
{
	gltexture_t* glt;

	// ericw -- tricky bug: if the hunk is almost full, an allocation in TexMgr_ReloadImage
	// triggers cache items to be freed, which calls back into TexMgr to free the
	// texture. If this frees 'glt' in the loop below, the active_gltextures
	// list gets corrupted.
	// A test case is jam3_tronyn.bsp with -heapsize 65536, and do several mode
	// switches/fullscreen toggles
	// 2015-09-04 -- Cache_Flush workaround was causing issues (http://sourceforge.net/p/quakespasm/bugs/10/)
	// switching to a boolean flag.
	in_reload_images = true;

	TexMgr_ColormapTexture_Free(NULL);	//just flush colourmapped cache instead of reloading them all unecessarily.

	for (glt = active_gltextures; glt; glt = glt->next)
	{
		glGenTextures(1, &glt->texnum);
		TexMgr_ReloadImage(glt, plcolour_none, plcolour_none);
	}

	in_reload_images = false;
}

/*
================
TexMgr_ReloadNobrightImages -- reloads all texture that were loaded with the nobright palette.  called when gl_fullbrights changes
================
*/
void TexMgr_ReloadNobrightImages(void)
{
	gltexture_t* glt;

	for (glt = active_gltextures; glt; glt = glt->next)
		if (glt->flags & TEXPREF_NOBRIGHT)
			TexMgr_ReloadImage(glt, plcolour_none, plcolour_none);
}


static struct
{
	struct gltexture_s* basetex;
	struct gltexture_s* coloured;
	plcolour_t upper;
	plcolour_t lower;
	double usetime;
} colourmappedtexture[MAX_SCOREBOARD * 2];
static size_t numcolourmappedtextures;
static void TexMgr_ColormapTexture_Free(struct gltexture_s* basetex)
{
	int i;
	if (basetex)
	{	//kill a single one.
		basetex->flags &= ~TEXPREF_COLOURMAPPED;

		for (i = 0; i < numcolourmappedtextures; i++)
		{
			if (colourmappedtexture[i].basetex == basetex)
			{
				TexMgr_FreeTexture(colourmappedtexture[i].coloured);
				colourmappedtexture[i].basetex = NULL;
				colourmappedtexture[i].usetime = FLT_MIN;
				colourmappedtexture[i].coloured = NULL;
				//may be multiple combinations of the same texture.
			}
		}
	}
	else
	{	//kill em all
		for (i = 0; i < numcolourmappedtextures; i++)
		{
			if (colourmappedtexture[i].coloured)
				TexMgr_FreeTexture(colourmappedtexture[i].coloured);
			colourmappedtexture[i].coloured = NULL;
			colourmappedtexture[i].basetex = NULL;
		}
		numcolourmappedtextures = 0;
	}
}
struct gltexture_s* TexMgr_ColormapTexture(struct gltexture_s* basetex, plcolour_t lower, plcolour_t upper)
{
	int oldest;
	float otime;
	int i;
	struct gltexture_s* glt;
	if (!basetex)
		return NULL;
	for (i = 0; i < numcolourmappedtextures; i++)
	{
		if (colourmappedtexture[i].basetex == basetex && colourmappedtexture[i].upper.key == upper.key && colourmappedtexture[i].lower.key == lower.key)
		{
			colourmappedtexture[i].usetime = realtime;
			return colourmappedtexture[i].coloured;
		}
	}
	if (numcolourmappedtextures < countof(colourmappedtexture))
		oldest = numcolourmappedtextures++;	//just use a new one
	else
	{
		otime = colourmappedtexture[oldest = 0].usetime;
		for (i = 1; i < countof(colourmappedtexture); i++)
		{
			if (otime > colourmappedtexture[i].usetime)
				otime = colourmappedtexture[oldest = i].usetime;
		}
	}
	if (colourmappedtexture[oldest].coloured)
		TexMgr_FreeTexture(colourmappedtexture[oldest].coloured);	//was previously used...

	colourmappedtexture[oldest].basetex = basetex;
	colourmappedtexture[oldest].upper = upper;
	colourmappedtexture[oldest].lower = lower;

	//create the new texture from the existing one
	colourmappedtexture[oldest].coloured = glt = TexMgr_NewTexture();
	glt->owner = NULL; //don't get clobbered by mistake
	q_strlcpy(glt->name, basetex->name, sizeof(glt->name));
	glt->width = basetex->width;
	glt->height = basetex->height;
	glt->flags = basetex->flags | TEXPREF_OVERWRITE;
	glt->shirt = upper;
	glt->pants = lower;
	q_strlcpy(glt->source_file, basetex->source_file, sizeof(glt->source_file));
	glt->source_offset = basetex->source_offset;
	glt->source_format = basetex->source_format;
	glt->source_width = basetex->source_width;
	glt->source_height = basetex->source_height;
	glt->source_crc = basetex->source_crc + 1;	//something wrong so we don't get found so easily...

	//and now reload it so it gets the proper colours.
	TexMgr_ReloadImage(glt, upper, lower);

	basetex->flags |= TEXPREF_COLOURMAPPED;	//so we clean up other textures spawned from it too.

	colourmappedtexture[oldest].usetime = realtime;
	return colourmappedtexture[oldest].coloured;
}


/*
================================================================================

	TEXTURE BINDING / TEXTURE UNIT SWITCHING

================================================================================
*/

#define TEXTURE_BINDING_UNITS 8
static GLuint	currenttexture[TEXTURE_BINDING_UNITS] = {
	GL_UNUSED_TEXTURE, GL_UNUSED_TEXTURE, GL_UNUSED_TEXTURE, GL_UNUSED_TEXTURE,
	GL_UNUSED_TEXTURE, GL_UNUSED_TEXTURE, GL_UNUSED_TEXTURE, GL_UNUSED_TEXTURE
}; // cached texture bindings for the units used by the renderer
static GLenum	currenttarget = GL_TEXTURE0_ARB;
qboolean	mtexenabled = false;

/*
================
GL_SelectTexture -- johnfitz -- rewritten
================
*/
void GL_SelectTexture(GLenum target)
{
	if (target == currenttarget)
		return;

	GL_SelectTextureFunc(target);
	currenttarget = target;
}

/*
================
GL_DisableMultitexture -- selects texture unit 0
================
*/
void GL_DisableMultitexture(void)
{
	if (mtexenabled)
	{
		glDisable(GL_TEXTURE_2D);
		GL_SelectTexture(GL_TEXTURE0_ARB);
		mtexenabled = false;
	}
}

/*
================
GL_EnableMultitexture -- selects texture unit 1
================
*/
void GL_EnableMultitexture(void)
{
	if (gl_mtexable)
	{
		GL_SelectTexture(GL_TEXTURE1_ARB);
		glEnable(GL_TEXTURE_2D);
		mtexenabled = true;
	}
}

/*
================
GL_Bind -- johnfitz -- heavy revision
================
*/
void GL_Bind(gltexture_t* texture)
{
	if (!texture)
		texture = nulltexture;

	if (texture->texnum != currenttexture[currenttarget - GL_TEXTURE0_ARB])
	{
		currenttexture[currenttarget - GL_TEXTURE0_ARB] = texture->texnum;
		glBindTexture(GL_TEXTURE_2D, texture->texnum);
		texture->visframe = r_framecount;
	}
}

/*
================
GL_DeleteTexture -- ericw

Wrapper around glDeleteTextures that also clears the given texture number
from our per-TMU cached texture binding table.
================
*/
static void GL_DeleteTexture(gltexture_t* texture)
{
	int i;
	glDeleteTextures(1, &texture->texnum);

	for (i = 0; i < countof(currenttexture); i++)
		if (texture->texnum == currenttexture[i]) currenttexture[i] = GL_UNUSED_TEXTURE;

	texture->texnum = 0;
}

/*
================
GL_ClearBindings -- ericw

Invalidates cached bindings, so the next GL_Bind calls for each TMU will
make real glBindTexture calls.
Call this after changing the binding outside of GL_Bind.
================
*/
void GL_ClearBindings(void)
{
	int i;
	for (i = 0; i < countof(currenttexture); i++)
	{
		currenttexture[i] = GL_UNUSED_TEXTURE;
	}
}