/*
 * port/psp/keep_sce.ld -- keeps the NID table alive when linking with
 * --gc-sections.
 *
 * pspsdk's import stubs put the resolved NID words in a section called
 * .rodata.sceNid (see src/base/pspstub.s).  Nothing in our code ever refers
 * to that section: it is data the *loader* reads, reached through a pointer in
 * .lib.stub.  With -Wl,--gc-sections the linker therefore drops it, and then
 * psp-fixup-imports stops with "no nid section found" -- it looks the section
 * up by name:
 *
 *   tools/psp-fixup-imports.c:  strcmp(name, PRX_NID_SECT) == 0 -> g_nid
 *
 * PSPSDK's own linkfile.prx protects the section with KEEP(), but that script
 * is only used for PRX builds.  This fragment adds the same protection to a
 * plain EBOOT link without replacing the default script: INSERT AFTER keeps
 * our block additive.
 *
 * GNU ld syntax reference: "Overlay description" / "INSERT" in the ld manual.
 */

SECTIONS
{
	.rodata.sceNid : { KEEP (*(.rodata.sceNid)) }
}
INSERT AFTER .rodata;
