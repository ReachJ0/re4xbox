# nxdk build of RE4 for the original Xbox.
#   source /path/to/nxdk/bin/activate   (or: export NXDK_DIR=/path/to/nxdk PATH=$NXDK_DIR/bin:$PATH)
#   make -C xbox            -> xbox/bin/default.xbe (+ iso)
#
# Stage 1 (this file): the platform layers (src/xbox) plus the SDK files they reuse unchanged, and a smoke test
# (main.cpp). The game itself is added module by module in later stages (see ../docs/xbox.md).
XBE_TITLE = RE4
NXDK_DIR ?= $(CURDIR)/../../nxdk
RE4 := $(abspath $(CURDIR)/..)
NXDK_CXX = y
NXDK_SDL = y

LAYERS := audio_stubs chp_xbox gp_xbox xraster gx_xbox vi_xbox xvi_backend_sys ar_xbox cache_xbox card_xbox dvd_xbox link_xbox os_xbox pad_xbox ps_xbox thread_xbox \
          xcard_backend_file xcrash_sys xsnd_endian xtex_endian xarc_endian xmes_endian xdataext xidtex_endian xdvd_backend_file xmem_xbox xos_backend_sys xpad_backend_sdl xthread_backend_sys
SDK_C  := GXAttr GXBump GXDisplayList GXDraw GXFifo GXFrameBuf GXGeometry GXInit GXLight GXMisc GXPerf GXPixel GXTev GXTexture GXTransform texPalette fileCache OSAlloc OSArena OSStopwatch OSThread OSMutex OSSemaphore

GEN_C := mtx mtxvec mtx44 mtx44vec vec quat

# build identifier shown by the development HUD (short git hash, + when the tree has uncommitted changes); the file is only
# rewritten when it changes, so its object is rebuilt exactly then
$(shell mkdir -p $(CURDIR)/gen; printf 'const char XBuildId[] = "%s";\n' "$$(git -C $(RE4) rev-parse --short HEAD 2>/dev/null || echo nogit)$$(git -C $(RE4) diff --quiet HEAD 2>/dev/null || echo +)" > $(CURDIR)/gen/xbuildid.c.new; cmp -s $(CURDIR)/gen/xbuildid.c.new $(CURDIR)/gen/xbuildid.c || mv $(CURDIR)/gen/xbuildid.c.new $(CURDIR)/gen/xbuildid.c; rm -f $(CURDIR)/gen/xbuildid.c.new)
GAME := $(filter-out memset_2 yz2asm,$(basename $(notdir $(wildcard $(RE4)/src/game/*.cpp))))
SRCS = $(CURDIR)/main.cpp $(addprefix $(RE4)/src/game/,$(addsuffix .cpp,$(GAME))) $(RE4)/src/xbox/native_asm.cpp $(RE4)/src/xbox/dev_stubs.cpp $(addprefix $(CURDIR)/gen/,$(addsuffix .c,$(GEN_C))) $(CURDIR)/gen/xbuildid.c \
       $(addprefix $(RE4)/src/xbox/,$(addsuffix .cpp,$(LAYERS))) \
       $(addprefix $(RE4)/src/lib/,$(addsuffix .c,$(SDK_C)))

# what the sources of the game expect. They apply to OUR objects only: a global CFLAGS would also reach nxdk's own
# libraries (its crt0 would then call re4_main instead of main) when they are built in the same make run.
RE4_DEFS = -Dmain=re4_main -DRE4_XBOX -DNXDK -Wno-c++11-narrowing $(if $(RUN_GAME),-DRE4_RUN_GAME) $(if $(DEBUGSCREEN),-DRE4_DEBUGSCREEN)
RE4_INC  = -I$(RE4)/include -I$(RE4)/src/lib
RE4_OBJS = $(addsuffix .obj,$(basename $(SRCS)))
$(RE4_OBJS): CFLAGS   += $(RE4_DEFS) $(RE4_INC) -w
$(RE4_OBJS): CXXFLAGS += $(RE4_DEFS) $(RE4_INC) -w
# nxdk builds without -O (and with the x87 FPU); the software rasteriser is hot code of ours, so it gets optimisation and SSE floats.
$(RE4)/src/xbox/xraster.obj: CXXFLAGS += -O2 -msse -mfpmath=sse

$(CURDIR)/gen/%.c: $(RE4)/src/lib/%.c $(RE4)/tools/xbox/strip_ps.py
	@mkdir -p $(CURDIR)/gen
	python3 $(RE4)/tools/xbox/strip_ps.py $< $@

include $(NXDK_DIR)/Makefile
LDFLAGS += -errorlimit:0
LDFLAGS += -demangle:no
LDFLAGS += -map:$(CURDIR)/main.map

LDFLAGS += @$(CURDIR)/alt.inc
