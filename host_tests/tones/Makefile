# Host harness for the tone dither in main/tones.cpp. Run from this directory: make run
#
# tones.cpp builds unchanged against the ESP-IDF stub headers in stub/ and the firmware's
# components/epd97/epd97.h. epd_stub.cpp defines the epd97 calls tones_draw() makes. Each aborts,
# and the harness never calls tones_draw(). FreeType from pkg-config draws the text test.
# FREETYPE=0 builds without it. IMAGE picks the tone images.

ROOT     := ../..
OUT      := out
IMAGES   := $(ROOT)/../images
IMAGE    ?= $(IMAGES)/01.png
FREETYPE ?= $(shell pkg-config --exists freetype2 && echo 1 || echo 0)

CC       := clang
CXX      := clang++
# Both lodepng.h users see the same set, so LodePNGState has one layout.
LODEPNG  := -DLODEPNG_NO_COMPILE_CPP
CFLAGS   := -std=c11 -O2 -Wall -Wextra $(LODEPNG)
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti $(LODEPNG) -DHAVE_FREETYPE=$(FREETYPE) \
            -Istub -I. -I$(ROOT)/main -I$(ROOT)/components/epd97

ifeq ($(FREETYPE),1)
CXXFLAGS += $(shell pkg-config --cflags freetype2)
LIBS     := $(shell pkg-config --libs freetype2)
endif

SRCS := tones_host.cpp epd_stub.cpp $(ROOT)/main/tones.cpp
HDRS := $(ROOT)/main/tones.h $(ROOT)/main/tonemap.h $(ROOT)/components/epd97/epd97.h $(wildcard stub/*.h) lodepng.h

# make sweep: one build per config with the dither settings of main/tones.cpp overridden, each run
# into out/sweep/<config>/, then the side by side comparison. CFG_<config> holds the overrides.
SWEEP := $(OUT)/sweep
# Every config sets all six: kernel, noise in L, noise as a share of the gap between the two
# nearest entries, blue (1) or white (0) noise, hybrid distance, share of chroma error kept.
CFG = -DTONES_DITHER_KERNEL=KERNEL_$(1) -DTONES_DITHER_NOISE_L=$(2)f -DTONES_DITHER_NOISE_REL=$(3)f \
      -DTONES_DITHER_NOISE_BLUE=$(4) -DTONES_DITHER_HYBRID_NEAR=$(5)f -DTONES_ERR_KEEP_C=$(6)f
CFG_atkinson                := $(call CFG,ATKINSON,0.0,0.0,1,0.0,1.0)
CFG_floyd_steinberg         := $(call CFG,FLOYD_STEINBERG,0.0,0.0,1,0.0,1.0)
CFG_jjn                     := $(call CFG,JJN,0.0,0.0,1,0.0,1.0)
CFG_stucki                  := $(call CFG,STUCKI,0.0,0.0,1,0.0,1.0)
CFG_atkinson_blue_02        := $(call CFG,ATKINSON,0.02,0.0,1,0.0,1.0)
CFG_floyd_st_blue_02        := $(call CFG,FLOYD_STEINBERG,0.02,0.0,1,0.0,1.0)
CFG_jjn_blue_02             := $(call CFG,JJN,0.02,0.0,1,0.0,1.0)
CFG_stucki_blue_02          := $(call CFG,STUCKI,0.02,0.0,1,0.0,1.0)
CFG_stucki_white_02         := $(call CFG,STUCKI,0.02,0.0,0,0.0,1.0)
CFG_stucki_blue_04          := $(call CFG,STUCKI,0.04,0.0,1,0.0,1.0)
CFG_stucki_blue_02_hyb01    := $(call CFG,STUCKI,0.02,0.0,1,0.01,1.0)
CFG_stucki_kc75             := $(call CFG,STUCKI,0.0,0.0,1,0.0,0.75)
CFG_stucki_kc50             := $(call CFG,STUCKI,0.0,0.0,1,0.0,0.5)
CFG_atkinson_rel25          := $(call CFG,ATKINSON,0.0,0.25,1,0.0,1.0)
CFG_floyd_st_rel25_kc75     := $(call CFG,FLOYD_STEINBERG,0.0,0.25,1,0.0,0.75)
CFG_jjn_rel25_kc75          := $(call CFG,JJN,0.0,0.25,1,0.0,0.75)
CFG_stucki_rel15_kc75       := $(call CFG,STUCKI,0.0,0.15,1,0.0,0.75)
CFG_stucki_rel25_kc75       := $(call CFG,STUCKI,0.0,0.25,1,0.0,0.75)
CFG_stucki_rel35_kc75       := $(call CFG,STUCKI,0.0,0.35,1,0.0,0.75)
CFG_stucki_wrel25_kc75      := $(call CFG,STUCKI,0.0,0.25,0,0.0,0.75)
CFG_stucki_rel25_kc50       := $(call CFG,STUCKI,0.0,0.25,1,0.0,0.5)
CFG_stucki_rel25            := $(call CFG,STUCKI,0.0,0.25,1,0.0,1.0)
CFG_stucki_rel25_kc35       := $(call CFG,STUCKI,0.0,0.25,1,0.0,0.35)
CFG_stucki_rel35_kc50       := $(call CFG,STUCKI,0.0,0.35,1,0.0,0.5)
CFG_jjn_rel25_kc50          := $(call CFG,JJN,0.0,0.25,1,0.0,0.5)
CFG_floyd_st_rel25_kc50     := $(call CFG,FLOYD_STEINBERG,0.0,0.25,1,0.0,0.5)
CFG_stucki_rel25_kc50_hyb01 := $(call CFG,STUCKI,0.0,0.25,1,0.01,0.5)
CONFIGS ?= atkinson floyd_steinberg jjn stucki atkinson_blue_02 floyd_st_blue_02 jjn_blue_02 stucki_blue_02 \
           stucki_white_02 stucki_blue_04 stucki_blue_02_hyb01 stucki_kc75 stucki_kc50 atkinson_rel25 \
           floyd_st_rel25_kc75 jjn_rel25_kc75 stucki_rel15_kc75 stucki_rel25_kc75 stucki_rel35_kc75 \
           stucki_wrel25_kc75 stucki_rel25_kc50 stucki_rel25 stucki_rel25_kc35 stucki_rel35_kc50 jjn_rel25_kc50 \
           floyd_st_rel25_kc50 stucki_rel25_kc50_hyb01
# the configs in the side by side PNGs, three across
SHOW    ?= atkinson stucki stucki_blue_02 stucki_kc50 atkinson_rel25 floyd_st_rel25_kc50 jjn_rel25_kc50 \
           stucki_rel25_kc50 stucki_rel25_kc35

.PHONY: all run sweep clean

all: $(OUT)/tones_host

$(OUT)/lodepng.o: lodepng.c lodepng.h
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c -o $@ lodepng.c

$(OUT)/tones_host: $(SRCS) $(HDRS) $(OUT)/lodepng.o
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS) $(OUT)/lodepng.o $(LIBS)

run: $(OUT)/tones_host
	$(OUT)/tones_host $(OUT) $(IMAGE) | tee $(OUT)/report.txt

$(SWEEP)/%/tones_host: $(SRCS) $(HDRS) $(OUT)/lodepng.o
	mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(CFG_$*) -o $@ $(SRCS) $(OUT)/lodepng.o $(LIBS)

sweep: $(OUT)/tones_host $(foreach c,$(CONFIGS),$(SWEEP)/$(c)/tones_host)
	for c in $(CONFIGS); do $(SWEEP)/$$c/tones_host $(SWEEP)/$$c $(IMAGE) > $(SWEEP)/$$c/report.txt || exit 1; done
	$(OUT)/tones_host --compare $(SWEEP) $(CONFIGS) > $(SWEEP)/compare.txt
	$(OUT)/tones_host --compare $(SWEEP) $(SHOW) > /dev/null
	cat $(SWEEP)/compare.txt

clean:
	rm -rf $(OUT)
