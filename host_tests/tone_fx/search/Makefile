# Host test of the search kernel (main/tone_fx_search.h), as its stand in
# ../model/tone_fx_search_model.cpp, against a float scan of all entries. Run from this directory:
# make test. The report is also in out/report.txt.

ROOT  := ../../..
MODEL := ../model
OUT   := out

CXX      := clang++
CXXFLAGS := -std=gnu++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -I$(ROOT)/main -I$(MODEL) \
            -I$(ROOT)/components/epd97

# The table is that of main/tone_tables.inc, which main/tone_fx_search.cpp reads.
SRCS := search_test.cpp $(ROOT)/main/tone_fx_search.cpp $(MODEL)/tone_fx_search_model.cpp
DEPS := $(SRCS) $(ROOT)/main/tone_fx_search.h $(ROOT)/main/tone_fx_search_internal.h $(ROOT)/main/tone_fx.h \
        $(ROOT)/main/tonemap.h $(ROOT)/main/tonemap_entries.inc $(MODEL)/tone_fx_model.h \
        $(wildcard $(ROOT)/main/tone_tables*) $(ROOT)/main/tone_plan.h

.PHONY: test clean

$(OUT)/search_test: $(DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

test: $(OUT)/search_test
	$(OUT)/search_test > $(OUT)/report.txt; s=$$?; cat $(OUT)/report.txt; exit $$s

clean:
	rm -rf $(OUT)
