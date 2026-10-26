/*
 * port/psp/keep_sce.ld -- keeps the import stub table alive when linking with
 * --gc-sections.
 *
 * pspsdk's import stubs put two things in specially named sections (see
 * src/base/pspstub.s):
 *
 *   .sceStub.text   the jump instructions the loader patches
 *   .rodata.sceNid  the NID words those jumps resolve against
 *
 * Nothing in our code refers to either one: they are data the *loader* reads,
 * reached through pointers in .lib.stub.  With -Wl,--gc-sections the linker
 * drops them, and psp-fixup-imports then fails.  It looks both sections up by
 * name (tools/psp-fixup-imports.c) and insists on a strict 1:2 size ratio,
 * because it pairs NID word i with stub words 2i and 2i+1:
 *
 *   if (g_stubtext->iSize != (g_nid->iSize * 2)) {
 *       "Error, size of text section and nid section do not match"
 *
 * Keeping only the NID half is not enough, and that was the game build's
 * failure: the NID sections survived through KEEP while the stub text of
 * imports nothing called got collected, so the sizes no longer agreed.
 * Both halves have to be kept, and both have to be collected in the same
 * order -- which they are, since each stub object contributes its two
 * sections at the same point in the link.
 *
 * PSPSDK's own linkfile.prx protects these with KEEP(), but that script is
 * only used for PRX builds.  This fragment adds the same protection to a plain
 * EBOOT link without replacing the default script: INSERT keeps our block
 * additive.
 *
 * Placement matters: the stub text goes right after .text so it stays in the
 * executable segment, not into .rodata.
 *
 * GNU ld syntax reference: "Overlay description" / "INSERT" in the ld manual.
 */

SECTIONS
{
	.sceStub.text : { KEEP (*(.sceStub.text)) }
}
INSERT AFTER .text;

SECTIONS
{
	.rodata.sceNid : { KEEP (*(.rodata.sceNid)) }
}
INSERT AFTER .rodata;
