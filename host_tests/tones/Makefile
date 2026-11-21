# Host test of the tone dither and the frame play of main/tones.cpp, see tones_host.cpp. Run from
# this directory: make test. tones_host exits 1 when a check fails, so make test fails.
#
# tones.cpp, tone_plan.cpp and the tone_fx kernels build unchanged from main/ against the ESP-IDF
# stub headers in stub/ and the firmware's components/epd97 headers. The kernels run as their stand
# ins from host_tests/tone_fx/model, which give the bits of the vector kernels the device runs.
# epd_stub.cpp is a model of the epd97 calls tones_draw() makes. It plays no panel and checks every
# pull against the plan. FreeType from pkg-config draws the text image. FREETYPE=0 builds without it.
# TONES_FONT is the face of the text image, the sans face of the font pack by default.
# probes/image_dither, probes/solid_red and host_tests/tone_tables build epd_stub.cpp and stub/ from
# here, and probes/image_dither builds lodepng.c.

ROOT     := ../..
MODEL    := ../tone_fx/model
OUT      := out
OBJ      := $(OUT)/obj
# make test builds the objects JOBS at a time, the threads of the host by default.
ifndef JOBS
JOBS     := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
endif
FREETYPE ?= $(shell pkg-config --exists freetype2 && echo 1 || echo 0)
TONES_FONT ?= $(ROOT)/data/fonts/sans.ttf
export TONES_FONT

CXX      := clang++
CXXFLAGS := -std=gnu++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -DHAVE_FREETYPE=$(FREETYPE) \
            -Istub -I. -I$(ROOT)/main -I$(MODEL) -I$(ROOT)/components/epd97

ifeq ($(FREETYPE),1)
CXXFLAGS += $(shell pkg-config --cflags freetype2)
LIBS     := $(shell pkg-config --libs freetype2)
endif

# The kernels: the tables from main/ (main/tone_tables.inc), the stand ins from $(MODEL).
FX   := srgb bracket squeeze search oklab
SRCS := tones_host.cpp epd_stub.cpp $(ROOT)/main/tones.cpp $(ROOT)/main/tone_plan.cpp \
        $(foreach k,$(FX),$(ROOT)/main/tone_fx_$(k).cpp $(MODEL)/tone_fx_$(k)_model.cpp)
HDRS := $(ROOT)/main/tones.h $(ROOT)/main/tonemap.h $(ROOT)/main/tonemap_entries.inc \
        $(ROOT)/main/tonemap_squeeze.h $(ROOT)/main/tone_plan.h $(ROOT)/main/tone_fx.h $(MODEL)/tone_fx_model.h \
        $(ROOT)/main/tone_photo.h \
        $(wildcard $(ROOT)/main/tone_tables*) \
        $(foreach k,$(FX),$(ROOT)/main/tone_fx_$(k).h $(ROOT)/main/tone_fx_$(k)_internal.h) \
        $(wildcard $(ROOT)/components/epd97/*.h) $(wildcard stub/*.h) epd_stub.h

# The sources have distinct names, found through vpath.
vpath %.cpp $(ROOT)/main $(MODEL)
OBJS := $(addprefix $(OBJ)/,$(notdir $(SRCS:.cpp=.o)))

.PHONY: test clean

$(OBJ)/%.o: %.cpp $(HDRS)
	@mkdir -p $(OBJ)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OUT)/tones_host: $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LIBS)

test:
	@$(MAKE) --no-print-directory -j$(JOBS) $(OUT)/tones_host
	$(OUT)/tones_host

clean:
	rm -rf $(OUT)
