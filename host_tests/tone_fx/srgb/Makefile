# Host test of the sRGB decode, main/tone_fx_srgb.h. Run from this directory:
#   make test   builds and runs srgb_test, which exits 1 when a target is missed.
#   make fit    builds and runs fit, which prints the curve constants of
#               main/tone_fx_srgb_internal.h and the exact table of ../model/tone_fx_srgb_model.cpp.
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

.PHONY: test fit clean

$(OUT)/srgb_test: $(DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

$(OUT)/fit: fit.cpp tone_fx_srgb_model.h
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ fit.cpp

test: $(OUT)/srgb_test
	$(OUT)/srgb_test

fit: $(OUT)/fit
	$(OUT)/fit

clean:
	rm -rf $(OUT)
