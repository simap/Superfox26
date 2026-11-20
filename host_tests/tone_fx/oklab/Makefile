# Host test of the Oklab kernels (main/tone_fx_oklab.h) against the float code. The kernels run as
# their stand ins, ../model/tone_fx_oklab_model.cpp. Run from this directory: make test.

ROOT  := ../../..
MODEL := ../model
OUT   := out

CXX      := clang++
CXXFLAGS := -std=gnu++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -I$(ROOT)/main -I$(MODEL)

SRCS := oklab_test.cpp $(ROOT)/main/tone_fx_oklab.cpp $(MODEL)/tone_fx_oklab_model.cpp
DEPS := $(SRCS) $(ROOT)/main/tone_fx_oklab.h $(ROOT)/main/tone_fx_oklab_internal.h $(ROOT)/main/tone_fx.h \
        $(MODEL)/tone_fx_model.h

.PHONY: test clean

$(OUT)/oklab_test: $(DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

test: $(OUT)/oklab_test
	$(OUT)/oklab_test > $(OUT)/report.txt; s=$$?; cat $(OUT)/report.txt; exit $$s

clean:
	rm -rf $(OUT)
