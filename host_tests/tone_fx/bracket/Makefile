# Host test of the bracket kernel (main/tone_fx_bracket.h). Run from this directory: make test. It
# exits 1 when the kernel's stand in (../model/tone_fx_bracket_model.cpp) differs from bracket() of
# main/tones.cpp at any L or the vector sequence differs from the stand in. The step table is that
# of main/tone_tables.inc, which main/tone_fx_bracket.cpp reads. The vector form's instruction
# sequence runs on the model in pie_emu.h.

ROOT  := ../../..
MODEL := ../model
OUT   := out

CXX      := clang++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -I$(ROOT)/main -I$(MODEL) \
            -I$(ROOT)/components/epd97

SRCS := bracket_test.cpp $(ROOT)/main/tone_fx_bracket.cpp $(MODEL)/tone_fx_bracket_model.cpp
DEPS := $(SRCS) pie_emu.h $(ROOT)/main/tone_fx_bracket.h $(ROOT)/main/tone_fx_bracket_internal.h \
        $(ROOT)/main/tone_fx.h $(ROOT)/main/tonemap.h $(ROOT)/main/tonemap_entries.inc $(MODEL)/tone_fx_model.h \
        $(wildcard $(ROOT)/main/tone_tables*) $(ROOT)/main/tone_plan.h

.PHONY: test clean

$(OUT)/bracket_test: $(DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

test: $(OUT)/bracket_test
	$(OUT)/bracket_test

clean:
	rm -rf $(OUT)
