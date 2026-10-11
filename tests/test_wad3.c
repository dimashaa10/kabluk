#include "quakedef.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_WIDTH 16
#define TEST_HEIGHT 16
#define TEST_MIPTEX_HEADER 40
#define TEST_MIP0_OFFSET TEST_MIPTEX_HEADER
#define TEST_MIP1_OFFSET (TEST_MIP0_OFFSET + 16 * 16)
#define TEST_MIP2_OFFSET (TEST_MIP1_OFFSET + 8 * 8)
#define TEST_MIP3_OFFSET (TEST_MIP2_OFFSET + 4 * 4)
#define TEST_PALETTE_OFFSET (TEST_MIP3_OFFSET + 2 * 2)
#define TEST_TEXTURE_SIZE (TEST_PALETTE_OFFSET + 2 + 256 * 3)
#define TEST_WAD_DIRECTORY_OFFSET (12 + TEST_TEXTURE_SIZE)
#define TEST_WAD_SIZE (TEST_WAD_DIRECTORY_OFFSET + 32)
#define TEST_WAD2_DIRECTORY_OFFSET (12 + TEST_PALETTE_OFFSET)
#define TEST_WAD2_SIZE (TEST_WAD2_DIRECTORY_OFFSET + 32)

static byte test_wad[TEST_WAD_SIZE];
static qofs_t test_file_size;
qofs_t com_filesize;

static short TestLittleShort(short value)
{
	const unsigned short one = 1;
	if (*(const byte *)&one)
		return value;
	return (short)((unsigned short)value >> 8 | (unsigned short)value << 8);
}

static int TestLittleLong(int value)
{
	const unsigned int one = 1;
	if (*(const byte *)&one)
		return value;
	return (int)(((unsigned int)value >> 24) |
		(((unsigned int)value >> 8) & 0x0000ff00) |
		(((unsigned int)value << 8) & 0x00ff0000) |
		((unsigned int)value << 24));
}

short (*LittleShort)(short) = TestLittleShort;
int (*LittleLong)(int) = TestLittleLong;

static void WriteLittleLong(byte *out, unsigned int value)
{
	out[0] = (byte)(value >> 0);
	out[1] = (byte)(value >> 8);
	out[2] = (byte)(value >> 16);
	out[3] = (byte)(value >> 24);
}

static byte *TestLoadMallocFile(const char *path, unsigned int *path_id)
{
	byte *copy;
	if (strcmp(path, "fixture.wad"))
		return NULL;
	copy = (byte *)malloc((size_t)test_file_size + 1);
	if (!copy)
		return NULL;
	memcpy(copy, test_wad, (size_t)test_file_size);
	copy[test_file_size] = 0;
	com_filesize = test_file_size;
	if (path_id)
		*path_id = 1;
	return copy;
}

byte *COM_LoadMallocFile(const char *path, unsigned int *path_id)
{
	return TestLoadMallocFile(path, path_id);
}

static void BuildMipTex(byte *texture)
{
	unsigned int i;
	memcpy(texture, "abcdefghijklmnop", 16);
	WriteLittleLong(texture + 16, TEST_WIDTH);
	WriteLittleLong(texture + 20, TEST_HEIGHT);
	WriteLittleLong(texture + 24, TEST_MIP0_OFFSET);
	WriteLittleLong(texture + 28, TEST_MIP1_OFFSET);
	WriteLittleLong(texture + 32, TEST_MIP2_OFFSET);
	WriteLittleLong(texture + 36, TEST_MIP3_OFFSET);

	for (i = 0; i < 256; i++)
		texture[TEST_MIP0_OFFSET + i] = (byte)i;
	for (i = 0; i < 64; i++)
		texture[TEST_MIP1_OFFSET + i] = (byte)i;
	for (i = 0; i < 16; i++)
		texture[TEST_MIP2_OFFSET + i] = (byte)i;
	for (i = 0; i < 4; i++)
		texture[TEST_MIP3_OFFSET + i] = (byte)i;

	texture[TEST_PALETTE_OFFSET + 0] = 0;
	texture[TEST_PALETTE_OFFSET + 1] = 1; // 256 palette entries
	for (i = 0; i < 256; i++)
	{
		texture[TEST_PALETTE_OFFSET + 2 + i * 3 + 0] = (byte)i;
		texture[TEST_PALETTE_OFFSET + 2 + i * 3 + 1] = (byte)(255 - i);
		texture[TEST_PALETTE_OFFSET + 2 + i * 3 + 2] = (byte)(i ^ 0x55);
	}
}

static void BuildWad3(void)
{
	byte *entry;
	memset(test_wad, 0, sizeof(test_wad));
	memcpy(test_wad, "WAD3", 4);
	WriteLittleLong(test_wad + 4, 1);
	WriteLittleLong(test_wad + 8, TEST_WAD_DIRECTORY_OFFSET);
	BuildMipTex(test_wad + 12);

	entry = test_wad + TEST_WAD_DIRECTORY_OFFSET;
	WriteLittleLong(entry + 0, 12);
	WriteLittleLong(entry + 4, TEST_TEXTURE_SIZE);
	WriteLittleLong(entry + 8, TEST_TEXTURE_SIZE);
	entry[12] = TYP_WAD3_MIPTEX;
	entry[13] = CMP_NONE;
	memcpy(entry + 16, "abcdefghijklmnop", 16);
	test_file_size = TEST_WAD_SIZE;
}

static void BuildWad2(void)
{
	byte *entry;
	memset(test_wad, 0, sizeof(test_wad));
	memcpy(test_wad, "WAD2", 4);
	WriteLittleLong(test_wad + 4, 1);
	WriteLittleLong(test_wad + 8, TEST_WAD2_DIRECTORY_OFFSET);
	BuildMipTex(test_wad + 12);

	entry = test_wad + TEST_WAD2_DIRECTORY_OFFSET;
	WriteLittleLong(entry + 0, 12);
	WriteLittleLong(entry + 4, TEST_PALETTE_OFFSET);
	WriteLittleLong(entry + 8, TEST_PALETTE_OFFSET);
	entry[12] = TYP_MIPTEX;
	entry[13] = CMP_NONE;
	memcpy(entry + 16, "abcdefghijklmnop", 16);
	test_file_size = TEST_WAD2_SIZE;
}

int main(void)
{
	wadarchive_t archive;
	wadmiptex_t miptex;
	byte rgba[8];
	byte indices[2] = { 1, 255 };
	byte qpic[8 + 4 + 2 + 6];
	const byte *qpic_palette = NULL;
	unsigned int qpic_colors = 0;

	BuildWad3();
	com_filesize = 1234;
	assert(WAD_ArchiveLoad(&archive, "fixture.wad"));
	assert(com_filesize == 1234); // archive loading must not clobber the caller's file size
	assert(archive.version == WAD_VERSION_3);
	assert(archive.num_lumps == 1);
	assert(WAD_ArchiveFindMipTex(&archive, "ABCDEFGHIJKLMNOP", &miptex));
	assert(miptex.width == TEST_WIDTH && miptex.height == TEST_HEIGHT);
	assert(miptex.pixel_count == TEST_WIDTH * TEST_HEIGHT);
	assert(miptex.palette && miptex.palette_colors == 256);
	assert(miptex.pixels[1] == 1);

	WAD_ConvertIndexedToRGBA(indices, 2, miptex.palette, miptex.palette_colors, 255, rgba);
	assert(rgba[0] == 1 && rgba[1] == 254 && rgba[2] == (byte)(1 ^ 0x55) && rgba[3] == 255);
	assert(rgba[4] == 255 && rgba[5] == 0 && rgba[6] == (byte)(255 ^ 0x55) && rgba[7] == 0);

	memset(qpic, 0, sizeof(qpic));
	WriteLittleLong(qpic + 0, 2);
	WriteLittleLong(qpic + 4, 2);
	qpic[12] = 2; // two RGB colors, following the QPIC's four indexed pixels
	qpic[14] = 10;
	qpic[15] = 20;
	qpic[16] = 30;
	qpic[17] = 40;
	qpic[18] = 50;
	qpic[19] = 60;
	assert(WAD_ParsePalette(qpic, sizeof(qpic), 8 + 4, &qpic_palette, &qpic_colors));
	assert(qpic_colors == 2 && qpic_palette[0] == 10 && qpic_palette[5] == 60);

	WAD_ArchiveFree(&archive);
	assert(!archive.data && !archive.lumps);

	BuildWad2();
	com_filesize = 4321;
	assert(WAD_ArchiveLoad(&archive, "fixture.wad"));
	assert(com_filesize == 4321);
	assert(archive.version == WAD_VERSION_2);
	assert(WAD_ArchiveFindMipTex(&archive, "ABCDEFGHIJKLMNOP", &miptex));
	assert(miptex.pixel_count == TEST_WIDTH * TEST_HEIGHT);
	assert(!miptex.palette && miptex.palette_colors == 0);
	WAD_ArchiveFree(&archive);

	puts("PASS: WAD2 compatibility and WAD3 archive, palettes, QPIC parsing, and RGBA conversion.");
	return 0;
}
