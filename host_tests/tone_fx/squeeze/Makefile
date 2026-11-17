# Host test of the squeeze kernel (main/tone_fx_squeeze.h), as its stand in
# ../model/tone_fx_squeeze_model.cpp, against the float squeeze of main/tonemap_squeeze.h. The
# parameters are those of main/tone_tables.inc, which main/tone_fx_squeeze.cpp reads. Run from this
# directory: make test

ROOT  := ../../..
MODEL := ../model
OUT   := out
CXX   := clang++

CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -I$(ROOT)/main -I$(MODEL) \
            -I$(ROOT)/components/epd97
SRCS     := squeeze_test.cpp $(ROOT)/main/tone_fx_squeeze.cpp $(MODEL)/tone_fx_squeeze_model.cpp
DEPS     := $(SRCS) $(ROOT)/main/tone_fx_squeeze.h $(ROOT)/main/tone_fx_squeeze_internal.h $(ROOT)/main/tone_fx.h \
            $(ROOT)/main/tonemap_squeeze.h $(ROOT)/main/tonemap.h $(ROOT)/main/tonemap_entries.inc \
            $(MODEL)/tone_fx_model.h $(wildcard $(ROOT)/main/tone_tables*) $(ROOT)/main/tone_plan.h

.PHONY: test clean

$(OUT)/squeeze_test: $(DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

test: $(OUT)/squeeze_test
	$(OUT)/squeeze_test > $(OUT)/report.txt; s=$$?; cat $(OUT)/report.txt; exit $$s

clean:
	rm -rf $(OUT)
