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

LAYERS := audio_stubs chp_xbox gp_xbox gx_xbox vi_xbox xvi_backend_sys ar_xbox cache_xbox card_xbox dvd_xbox link_xbox os_xbox pad_xbox ps_xbox thread_xbox \
          xcard_backend_file xdvd_backend_file xmem_xbox xos_backend_sys xpad_backend_sdl xthread_backend_sys
SDK_C  := GXAttr GXBump GXDisplayList GXDraw GXFifo GXFrameBuf GXGeometry GXInit GXLight GXMisc GXPerf GXPixel GXTev GXTexture GXTransform texPalette fileCache OSAlloc OSArena OSStopwatch OSThread OSMutex OSSemaphore

GEN_C := mtx mtxvec mtx44 mtx44vec vec quat
GAME := $(filter-out memset_2 yz2asm,$(basename $(notdir $(wildcard $(RE4)/src/game/*.cpp))))
SRCS = $(CURDIR)/main.cpp $(addprefix $(RE4)/src/game/,$(addsuffix .cpp,$(GAME))) $(RE4)/src/xbox/native_asm.cpp $(RE4)/src/xbox/dev_stubs.cpp $(addprefix $(CURDIR)/gen/,$(addsuffix .c,$(GEN_C))) \
       $(addprefix $(RE4)/src/xbox/,$(addsuffix .cpp,$(LAYERS))) \
       $(addprefix $(RE4)/src/lib/,$(addsuffix .c,$(SDK_C)))

# what the sources of the game expect
RE4_DEFS = -Dmain=re4_main -DRE4_XBOX -DNXDK -Wno-c++11-narrowing
RE4_INC  = -I$(RE4)/include -I$(RE4)/src/lib
CFLAGS   += $(RE4_DEFS) $(RE4_INC) -w
CXXFLAGS += $(RE4_DEFS) $(RE4_INC) -w

$(CURDIR)/gen/%.c: $(RE4)/src/lib/%.c $(RE4)/tools/xbox/strip_ps.py
	@mkdir -p $(CURDIR)/gen
	python3 $(RE4)/tools/xbox/strip_ps.py $< $@

include $(NXDK_DIR)/Makefile
LDFLAGS += -errorlimit:0
LDFLAGS += -demangle:no

LDFLAGS += @$(CURDIR)/alt.inc
