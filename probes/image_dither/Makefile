# Host probe of the dither on an RGB image, see image_dither.cpp.
# Run from this directory: make, then out/image_dither IN.png OUT.png
# Builds against the same sources and stubs as host_tests/tones.

ROOT  := ../..
TONES := $(ROOT)/host_tests/tones
MODEL := $(ROOT)/host_tests/tone_fx/model
OUT   := out

CXX      := clang++
CXXFLAGS := -std=gnu++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
            -I$(TONES)/stub -I$(TONES) -I$(ROOT)/main -I$(MODEL) -I$(ROOT)/components/epd97

FX   := srgb bracket squeeze search oklab
SRCS := image_dither.cpp $(TONES)/epd_stub.cpp $(ROOT)/main/tones.cpp $(ROOT)/main/tone_plan.cpp \
        $(foreach k,$(FX),$(ROOT)/main/tone_fx_$(k).cpp $(MODEL)/tone_fx_$(k)_model.cpp)

# lodepng.c builds as C++ here, so its names link with image_dither.cpp without a wrapper.
$(OUT)/image_dither: $(SRCS) $(TONES)/lodepng.c
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -Wno-deprecated -o $@ $(SRCS) -x c++ $(TONES)/lodepng.c

clean:
	rm -rf $(OUT)

.PHONY: clean
