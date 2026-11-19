# The generator of main/tone_tables.inc, the tables of the tone code (main/tone_tables.h). Run from
# this directory:
#   make generate  writes main/tone_tables.inc, then runs the check below on it. Run it after a
#                  change to the tone table, the phase shapes, the push limits, a kernel constant or
#                  the build code here. The firmware does not build while the file is stale for the
#                  tone table or those constants (tone_tables_source.h).
#   make test      writes a fresh file to out/ and fails when it differs from main/tone_tables.inc.
#                  Then it checks that the generator refuses a table whose patterns pass the push
#                  limits (a build with every limit at 2 frames or less), and runs tone_tables_check, which
#                  runs pass_a() of main/tones.cpp on the file's tables and compares the slot WHITE
#                  and the solid gray table, and checks the mean of the photo dither's picks.
# The file is checked in, so the firmware build needs no host compiler.
#
# The flags are those of the host harness (host_tests/tones). The fit of the squeeze runs in float
# with libm, so another compiler, other flags or another libm can move its parameters by a few
# counts. make test then fails, and the fresh file in out/ is the one to check in after a look at
# the change.

ROOT  := ../..
MODEL := ../tone_fx/model
OUT   := out
CXX   := clang++

CXXFLAGS := -std=gnu++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -I. -I$(ROOT)/main -I$(MODEL) \
            -I$(ROOT)/components/epd97
# EPD_B_FRAME_US sets the push limits of epd97_config.h. At 1 s they are 1 VDH, 2 VDL and 0 VDHR
# frames.
TIGHT    := -DEPD_B_FRAME_US=1000000u

FX       := srgb bracket squeeze search oklab
MAIN_HDR := $(ROOT)/main/tonemap.h $(ROOT)/main/tonemap_entries.inc $(ROOT)/main/tonemap_squeeze.h \
            $(ROOT)/main/tone_plan.h $(ROOT)/main/tone_fx.h $(ROOT)/main/tone_tables_source.h $(ROOT)/main/tone_photo.h \
            $(foreach k,$(FX),$(ROOT)/main/tone_fx_$(k).h $(ROOT)/main/tone_fx_$(k)_internal.h) \
            $(wildcard $(ROOT)/components/epd97/*.h) $(MODEL)/tone_fx_model.h

GEN_SRCS := tone_tables_gen.cpp tone_plan_expand.cpp tone_fx_squeeze_build.cpp tone_fx_bracket_build.cpp \
            tone_fx_search_build.cpp tone_photo_build.cpp $(foreach k,srgb oklab squeeze bracket,$(MODEL)/tone_fx_$(k)_model.cpp)
GEN_DEPS := $(GEN_SRCS) tone_plan_expand.h tone_tables_build.h $(MAIN_HDR)

# The check builds tones.cpp as the host harness does, with its ESP-IDF stubs and driver model.
CHECK_SRCS := tone_tables_check.cpp $(ROOT)/main/tone_plan.cpp ../tones/epd_stub.cpp \
              $(foreach k,$(FX),$(ROOT)/main/tone_fx_$(k).cpp $(MODEL)/tone_fx_$(k)_model.cpp)
CHECK_DEPS := $(CHECK_SRCS) tone_tables_build.h $(ROOT)/main/tones.cpp $(ROOT)/main/tones.h \
              $(ROOT)/main/tone_tables.h $(ROOT)/main/tone_tables.inc $(MAIN_HDR) ../tones/epd_stub.h \
              $(wildcard ../tones/stub/*.h)

.PHONY: generate test clean

$(OUT)/tone_tables_gen: $(GEN_DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ $(GEN_SRCS)

$(OUT)/tone_tables_gen_tight: $(GEN_DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(TIGHT) -o $@ $(GEN_SRCS)

$(OUT)/tone_tables_check: $(CHECK_DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -I../tones/stub -I../tones -o $@ $(CHECK_SRCS)

generate: $(OUT)/tone_tables_gen
	$(OUT)/tone_tables_gen $(ROOT)/main/tone_tables.inc
	$(MAKE) $(OUT)/tone_tables_check
	$(OUT)/tone_tables_check

test: $(OUT)/tone_tables_gen $(OUT)/tone_tables_gen_tight $(OUT)/tone_tables_check
	rm -f $(OUT)/tone_tables.inc $(OUT)/tone_tables_tight.inc
	$(OUT)/tone_tables_gen $(OUT)/tone_tables.inc
	@cmp -s $(OUT)/tone_tables.inc $(ROOT)/main/tone_tables.inc || \
	    { echo "FAIL: main/tone_tables.inc differs from a fresh run in $(OUT)/tone_tables.inc."; \
	      echo "Look at the change, then run make generate."; exit 1; }
	@echo "main/tone_tables.inc is the generator's output"
	@if $(OUT)/tone_tables_gen_tight $(OUT)/tone_tables_tight.inc; then \
	    echo "FAIL: the generator wrote a table with push limits of 2 frames or less"; exit 1; fi
	@test ! -e $(OUT)/tone_tables_tight.inc || { echo "FAIL: the refused run left a file"; exit 1; }
	@echo "the generator refuses a table over the push limits and writes nothing"
	$(OUT)/tone_tables_check

clean:
	rm -rf $(OUT)
