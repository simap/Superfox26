# The generator of main/tone_tables.inc, the tables of the tone code (main/tone_tables.h). Run from
# this directory:
#   make generate  writes main/tone_tables.inc, then runs the check below on it. Run it after a
#                  change to the tone table, the phase shapes, the push limits, a kernel constant or
#                  the build code here. The firmware does not build while the file is stale for the
#                  tone table or those constants (tone_tables_source.h).
#   make test      writes a fresh file to out/ and fails when it differs from main/tone_tables.inc.
#                  Then it checks that the generator refuses a table whose patterns pass the push
#                  limits (a build with every limit at 2 frames or less) and a top of the triangles that
#                  is not star shaped from the focal point (a build with the point at L 0.83), and runs
#                  tone_tables_check, which
#                  runs pass_a() of main/tones.cpp on the file's tables and compares the slot WHITE
#                  and the solid gray table, and checks the mean of the photo dither's picks. It also
#                  checks the phase 2 marks h and '|' in the expansion and the plans. Last it runs
#                  tone_graph_check on the graph of the tones (main/tone_graph.h) and the nearest mix
#                  on it (main/tone_photo.h), then again with each of its faults, where it must report
#                  a failed check. The fault runs run at the same time, and each runs every check, so
#                  the line of each fault names all the checks that catch it.
# The file is checked in, so the firmware build needs no host compiler. Both targets build the
# programs from objects, JOBS at a time, the threads of the host by default.
#
# The flags are those of the host harness (host_tests/tones). The fit of the squeeze runs in float
# with libm, so another compiler, other flags or another libm can move its parameters by a few
# counts. make test then fails, and the fresh file in out/ is the one to check in after a look at
# the change.

ROOT  := ../..
MODEL := ../tone_fx/model
OUT   := out
CXX   := clang++
OBJ   := $(OUT)/obj
ifndef JOBS
JOBS  := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
endif

CXXFLAGS := -std=gnu++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -I. -I$(ROOT)/main -I$(MODEL) \
            -I$(ROOT)/components/epd97
# EPD_B_FRAME_US sets the push limits of epd97_config.h. At 1 s they are 1 VDH, 2 VDL and 0 VDHR
# frames.
TIGHT    := -DEPD_B_FRAME_US=1000000u
# A focal point at L 0.83 on the gray axis, from which the top of the triangles is not star shaped.
FOCAL_OFF := -DTONE_GRAPH_FOCAL_AT_L=3400

FX       := srgb bracket squeeze search oklab
MAIN_HDR := $(ROOT)/main/tonemap.h $(ROOT)/main/tonemap_entries.inc $(ROOT)/main/tonemap_squeeze.h \
            $(ROOT)/main/tone_plan.h $(ROOT)/main/tone_fx.h $(ROOT)/main/tone_tables_source.h $(ROOT)/main/tone_photo.h \
            $(ROOT)/main/tone_graph.h \
            $(foreach k,$(FX),$(ROOT)/main/tone_fx_$(k).h $(ROOT)/main/tone_fx_$(k)_internal.h) \
            $(wildcard $(ROOT)/components/epd97/*.h) $(MODEL)/tone_fx_model.h

GEN_SRCS := tone_tables_gen.cpp tone_plan_expand.cpp tone_fx_squeeze_build.cpp tone_fx_bracket_build.cpp \
            tone_fx_search_build.cpp tone_photo_build.cpp tone_graph_build.cpp \
            $(foreach k,srgb oklab squeeze bracket,$(MODEL)/tone_fx_$(k)_model.cpp)
GEN_HDRS := tone_plan_expand.h tone_tables_build.h tone_graph_build.h $(MAIN_HDR)

# The check builds tones.cpp as the host harness does, with its ESP-IDF stubs and driver model.
CHECK_SRCS := tone_tables_check.cpp tone_plan_expand.cpp $(ROOT)/main/tone_plan.cpp ../tones/epd_stub.cpp \
              $(foreach k,$(FX),$(ROOT)/main/tone_fx_$(k).cpp $(MODEL)/tone_fx_$(k)_model.cpp)
CHECK_HDRS := tone_tables_build.h tone_plan_expand.h $(ROOT)/main/tones.cpp $(ROOT)/main/tones.h \
              $(ROOT)/main/tone_tables.h $(ROOT)/main/tone_tables.inc $(MAIN_HDR) ../tones/epd_stub.h \
              $(wildcard ../tones/stub/*.h)
# The check of the graph of the tones (main/tone_graph.h) reads the file through the header alone.
GRAPH_DEPS := tone_graph_check.cpp $(ROOT)/main/tone_tables.h $(ROOT)/main/tone_tables.inc $(MAIN_HDR)
GRAPH_FAULTS := 1 2 3 4 5 6 7 8 9
BINS := $(OUT)/tone_tables_gen $(OUT)/tone_tables_gen_tight $(OUT)/tone_tables_gen_focal $(OUT)/tone_tables_check \
        $(OUT)/tone_graph_check

# Each program has its own objects, as the flags differ. The sources of all of them have distinct
# names, found through vpath.
vpath %.cpp $(MODEL) $(ROOT)/main ../tones
objs = $(addprefix $(OBJ)/$(1)/,$(notdir $(2:.cpp=.o)))

# $(1) the program, $(2) its sources, $(3) its flags, $(4) the headers of its objects
define program
$(OBJ)/$(1)/%.o: %.cpp $(4)
	@mkdir -p $$(@D)
	$(CXX) $(CXXFLAGS) $(3) -c -o $$@ $$<

$(OUT)/$(1): $(call objs,$(1),$(2))
	$(CXX) -o $$@ $$^
endef

$(eval $(call program,tone_tables_gen,$(GEN_SRCS),,$(GEN_HDRS)))
$(eval $(call program,tone_tables_gen_tight,$(GEN_SRCS),$(TIGHT),$(GEN_HDRS)))
$(eval $(call program,tone_tables_gen_focal,$(GEN_SRCS),$(FOCAL_OFF),$(GEN_HDRS)))
$(eval $(call program,tone_tables_check,$(CHECK_SRCS),-I../tones/stub -I../tones,$(CHECK_HDRS)))

$(OUT)/tone_graph_check: $(GRAPH_DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -pthread -o $@ tone_graph_check.cpp

.PHONY: generate test test-run clean

generate:
	@$(MAKE) --no-print-directory -j$(JOBS) $(OUT)/tone_tables_gen
	$(OUT)/tone_tables_gen $(ROOT)/main/tone_tables.inc
	@$(MAKE) --no-print-directory -j$(JOBS) $(OUT)/tone_tables_check $(OUT)/tone_graph_check
	$(OUT)/tone_tables_check
	$(OUT)/tone_graph_check

# test builds the programs JOBS at a time, then test-run runs them.
test:
	@$(MAKE) --no-print-directory -j$(JOBS) $(BINS)
	@$(MAKE) --no-print-directory test-run

test-run: $(BINS)
	rm -f $(OUT)/tone_tables.inc $(OUT)/tone_tables_tight.inc $(OUT)/tone_tables_focal.inc
	$(OUT)/tone_tables_gen $(OUT)/tone_tables.inc
	@cmp -s $(OUT)/tone_tables.inc $(ROOT)/main/tone_tables.inc || \
	    { echo "FAIL: main/tone_tables.inc differs from a fresh run in $(OUT)/tone_tables.inc."; \
	      echo "Look at the change, then run make generate."; exit 1; }
	@echo "main/tone_tables.inc is the generator's output"
	@if $(OUT)/tone_tables_gen_tight $(OUT)/tone_tables_tight.inc; then \
	    echo "FAIL: the generator wrote a table with push limits of 2 frames or less"; exit 1; fi
	@test ! -e $(OUT)/tone_tables_tight.inc || { echo "FAIL: the refused run left a file"; exit 1; }
	@echo "the generator refuses a table over the push limits and writes nothing"
	@if $(OUT)/tone_tables_gen_focal $(OUT)/tone_tables_focal.inc; then \
	    echo "FAIL: the generator wrote a table whose top is not star shaped from the focal point"; exit 1; fi
	@test ! -e $(OUT)/tone_tables_focal.inc || { echo "FAIL: the refused run left a file"; exit 1; }
	@echo "the generator refuses a top that is not star shaped from the focal point"
	$(OUT)/tone_tables_check
	$(OUT)/tone_graph_check
	@for k in $(GRAPH_FAULTS); do \
	    $(OUT)/tone_graph_check --break $$k > $(OUT)/tone_graph_fault_$$k.txt & \
	done; wait; \
	for k in $(GRAPH_FAULTS); do \
	    grep -q '^tone_graph_check with a fault: FAIL$$' $(OUT)/tone_graph_fault_$$k.txt || \
	        { echo "FAIL: tone_graph_check does not report a failed check with fault $$k"; exit 1; }; \
	    echo "fault $$k fails:" $$(sed -n 's/^FAIL \([a-z]*\).*/\1/p; s/^\(paths\): .*FAIL.*/\1/p' \
	        $(OUT)/tone_graph_fault_$$k.txt | sort -u); \
	done
	@echo "tone_graph_check fails the graph with each of the faults $(GRAPH_FAULTS)"

clean:
	rm -rf $(OUT)
