# Host test of the sRGB decode, main/tone_fx_srgb.h. Run from this directory: make test. srgb_test
# exits 1 when a target is missed.
# The kernel runs as its stand in, ../model/tone_fx_srgb_model.cpp. The vector form's instruction
# sequence runs on the model in ../model/pie_emu.h.

ROOT  := ../../..
MODEL := ../model
OUT   := out

CXX      := clang++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -I$(ROOT)/main -I$(MODEL)

SRCS := srgb_test.cpp $(MODEL)/tone_fx_srgb_model.cpp
DEPS := $(SRCS) $(MODEL)/pie_emu.h $(ROOT)/main/tone_fx_srgb.h $(ROOT)/main/tone_fx_srgb_internal.h $(ROOT)/main/tone_fx.h \
        $(MODEL)/tone_fx_model.h

.PHONY: test clean

$(OUT)/srgb_test: $(DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

test: $(OUT)/srgb_test
	$(OUT)/srgb_test

clean:
	rm -rf $(OUT)
