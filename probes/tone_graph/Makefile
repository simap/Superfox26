# Host probe of the graph of the tones (main/tone_graph.h), see tone_graph.cpp. Run from this
# directory: make run. It writes out/tone_graph.png, the nearest mix over the same plane in
# out/tone_graph_near.png, and prints the figures of the graph.

ROOT  := ../..
TONES := $(ROOT)/host_tests/tones
MODEL := $(ROOT)/host_tests/tone_fx/model
OUT   := out

CXX      := clang++
CXXFLAGS := -std=gnu++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
            -I$(TONES)/stub -I$(TONES) -I$(ROOT)/main -I$(MODEL) -I$(ROOT)/components/epd97

DEPS := tone_graph.cpp $(ROOT)/main/tone_graph.h $(ROOT)/main/tone_photo.h $(ROOT)/main/tone_tables.h $(ROOT)/main/tone_tables.inc \
        $(ROOT)/main/tonemap_entries.inc $(ROOT)/main/tones.h

# lodepng.c builds as C++ here, so its names link with tone_graph.cpp without a wrapper.
$(OUT)/tone_graph: $(DEPS) $(TONES)/lodepng.c
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -Wno-deprecated -o $@ tone_graph.cpp -x c++ $(TONES)/lodepng.c

run: $(OUT)/tone_graph
	$(OUT)/tone_graph $(OUT)/tone_graph.png $(OUT)/tone_graph_near.png

clean:
	rm -rf $(OUT)

.PHONY: run clean
