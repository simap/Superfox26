# Host harness for the tone dither in main/tones.cpp. Run from this directory: make run
#
# tones.cpp and tone_plan.cpp build unchanged against the ESP-IDF stub headers in stub/ and the
# firmware's components/epd97 headers. epd_stub.cpp is a model of the epd97 calls tones_draw()
# makes: it plays no panel and checks every pull against the plan. FreeType from pkg-config draws
# the text test. FREETYPE=0 builds without it. The tests generate their own images. IMAGE adds
# 960 px wide 8-bit gray plus alpha PNGs as more tests, none by default. TONES_FONT is the face of
# the text test, the sans face of the font pack by default.

ROOT     := ../..
MODEL    := ../tone_fx/model
OUT      := out
IMAGE    ?=
FREETYPE ?= $(shell pkg-config --exists freetype2 && echo 1 || echo 0)
TONES_FONT ?= $(ROOT)/data/fonts/sans.ttf
export TONES_FONT

CC       := clang
CXX      := clang++
# Both lodepng.h users see the same set, so LodePNGState has one layout.
LODEPNG  := -DLODEPNG_NO_COMPILE_CPP
CFLAGS   := -std=c11 -O2 -Wall -Wextra $(LODEPNG)
# TONES_FLOAT_REF builds the float dither of main/tones_float_ref.inc beside the fixed point one.
CXXFLAGS := -std=gnu++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti $(LODEPNG) -DHAVE_FREETYPE=$(FREETYPE) \
            -DTONES_FLOAT_REF=1 -Istub -I. -I$(ROOT)/main -I$(MODEL) -I$(ROOT)/components/epd97

ifeq ($(FREETYPE),1)
CXXFLAGS += $(shell pkg-config --cflags freetype2)
LIBS     := $(shell pkg-config --libs freetype2)
endif

# The kernels: the tables from main/ (main/tone_tables.inc), the stand ins from $(MODEL).
FX   := srgb bracket squeeze search oklab
SRCS := tones_host.cpp epd_stub.cpp $(ROOT)/main/tones.cpp $(ROOT)/main/tone_plan.cpp \
        $(foreach k,$(FX),$(ROOT)/main/tone_fx_$(k).cpp $(MODEL)/tone_fx_$(k)_model.cpp)
HDRS := $(ROOT)/main/tones.h $(ROOT)/main/tones_float_ref.inc $(ROOT)/main/tonemap.h $(ROOT)/main/tonemap_entries.inc \
        $(ROOT)/main/tonemap_squeeze.h $(ROOT)/main/tone_plan.h $(ROOT)/main/tone_fx.h $(MODEL)/tone_fx_model.h \
        $(wildcard $(ROOT)/main/tone_tables*) \
        $(foreach k,$(FX),$(ROOT)/main/tone_fx_$(k).h $(ROOT)/main/tone_fx_$(k)_internal.h) \
        $(wildcard $(ROOT)/components/epd97/*.h) $(wildcard stub/*.h) epd_stub.h lodepng.h

.PHONY: all run clean

all: $(OUT)/tones_host

$(OUT)/lodepng.o: lodepng.c lodepng.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $@ lodepng.c

$(OUT)/tones_host: $(SRCS) $(HDRS) $(OUT)/lodepng.o
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(OUT)/lodepng.o $(LIBS)

run: $(OUT)/tones_host
	$(OUT)/tones_host $(OUT) $(IMAGE) | tee $(OUT)/report.txt

clean:
	rm -rf $(OUT)
