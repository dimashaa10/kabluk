# port/objs.mk -- object groups shared by every platform build of the engine.
#
# Included by Quake/Makefile (the desktop SDL2 + OpenGL build) and by
# port/psp/Makefile (the PSP build).  A platform makefile may preset any
# SYSOBJ_* / COMOBJ_* / GLOBJS variable *before* including this file -- even
# to the empty string -- and then that value wins.  Anything left unset falls
# back to the desktop build, so including this file from Quake/Makefile
# changes nothing there.
#
# Rule of thumb for the port: the engine core is always the same set of .c
# files.  What a platform swaps out is *only* the SYSOBJ_ groups (main(),
# system services, input, sound device, video/GL device, networking, CD
# audio).  No #ifdef belongs in the shared sources; the split lives here.
#
# Object names are relative to Quake/ (the engine sources live there).

# ---------------------------
# sound: codecs vs. device
# ---------------------------

# Codec objects are pure software and are shared.  A platform that ships no
# streaming music simply builds none of the decoders it does not link in.
# mp3_obj is chosen by MP3LIB in the desktop makefile; a platform that links no
# mp3 decoder leaves it empty and we must not turn that into a bare ".o".
MP3_OBJ := $(if $(mp3_obj),$(mp3_obj).o,)

MUSIC_OBJS ?= bgmusic.o \
	snd_codec.o \
	snd_flac.o \
	snd_wave.o \
	snd_vorbis.o \
	snd_opus.o \
	$(MP3_OBJ) \
	snd_mp3tag.o \
	snd_mikmod.o \
	snd_modplug.o \
	snd_xmp.o \
	snd_umx.o

# The mixer/DMA core is shared; only the output device is per platform.
COMOBJ_SND ?= snd_voip.o snd_dma.o snd_mix.o snd_mem.o $(MUSIC_OBJS)

# ---------------------------
# platform ("system") groups
# ---------------------------

SYSOBJ_SND    ?= snd_sdl.o
SYSOBJ_CDA    ?= cd_sdl.o
SYSOBJ_INPUT  ?= in_sdl.o
SYSOBJ_GL_VID ?= gl_vidsdl.o
SYSOBJ_NET    ?= net_bsd.o net_udp.o
SYSOBJ_SYS    ?= pl_linux.o sys_sdl_unix.o
SYSOBJ_MAIN   ?= main_sdl.o

# Escape hatch: extra objects a platform needs beyond the groups above.
PORT_OBJS ?=

# ---------------------------
# renderer (GL front end, shared across GL-capable platforms)
# ---------------------------

GLOBJS ?= \
	gl_refrag.o \
	gl_rlight.o \
	gl_rmain.o \
	gl_fog.o \
	gl_rmisc.o \
	r_part.o \
	r_part_fte.o \
	r_world.o \
	gl_screen.o \
	gl_sky.o \
	gl_warp.o \
	$(SYSOBJ_GL_VID) \
	gl_draw.o \
	image.o \
	gl_texmgr.o \
	gl_mesh.o \
	r_sprite.o \
	r_alias.o \
	r_brush.o \
	gl_model.o

# ---------------------------
# full link set
# ---------------------------

OBJS := strlcat.o \
	strlcpy.o \
	$(GLOBJS) \
	$(SYSOBJ_INPUT) \
	$(COMOBJ_SND) \
	$(SYSOBJ_SND) \
	$(SYSOBJ_CDA) \
	$(SYSOBJ_NET) \
	net_dgrm.o \
	net_loop.o \
	net_main.o \
	chase.o \
	cl_demo.o \
	cl_input.o \
	cl_main.o \
	cl_parse.o \
	cl_tent.o \
	console.o \
	iplog.o \
	keys.o \
	location.o \
	menu.o \
	sbar.o \
	view.o \
	wad.o \
	cmd.o \
	common.o \
	json.o \
	fs_zip.o \
	crc.o \
	cvar.o \
	cfgfile.o \
	host.o \
	host_cmd.o \
	mathlib.o \
	mdfour.o \
	pr_cmds.o \
	pr_ext.o \
	pr_edict.o \
	pr_exec.o \
	pmove.o \
	pmovetst.o \
	sv_main.o \
	sv_move.o \
	sv_phys.o \
	sv_user.o \
	world.o \
	zone.o \
	$(SYSOBJ_SYS) \
	$(SYSOBJ_MAIN) \
	$(PORT_OBJS)
