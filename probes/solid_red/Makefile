# Host probe of the dither on solid sRGB fills, see solid_red.cpp. Run from this directory: make run
# Builds against the same sources and stubs as host_tests/tones.

ROOT  := ../..
TONES := $(ROOT)/host_tests/tones
MODEL := $(ROOT)/host_tests/tone_fx/model
OUT   := out

CXX      := clang++
CXXFLAGS := -std=gnu++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
            -I$(TONES)/stub -I$(TONES) -I$(ROOT)/main -I$(MODEL) -I$(ROOT)/components/epd97

FX   := srgb bracket squeeze search oklab
SRCS := solid_red.cpp $(TONES)/epd_stub.cpp $(ROOT)/main/tones.cpp $(ROOT)/main/tone_plan.cpp \
        $(foreach k,$(FX),$(ROOT)/main/tone_fx_$(k).cpp $(MODEL)/tone_fx_$(k)_model.cpp)

$(OUT)/solid_red: $(SRCS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

run: $(OUT)/solid_red
	$(OUT)/solid_red

clean:
	rm -rf $(OUT)

.PHONY: run clean
