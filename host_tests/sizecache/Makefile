# Host test of main/sizecache.cpp and main/rtcstate.cpp. Run from this directory: make test, or make
# asan for the same run under AddressSanitizer and UndefinedBehaviorSanitizer.
#
# The test includes both source files and builds them against the ESP-IDF stub headers in stub/.

ROOT := ../..
OUT  := out

CXX      := clang++
CXXFLAGS := -std=gnu++17 -Wall -Wextra -fno-exceptions -fno-rtti -Istub -I$(ROOT)/main
OPT      := -O2
ASAN     := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined

DEPS := sizecache_test.cpp $(wildcard $(ROOT)/main/sizecache.* $(ROOT)/main/rtcstate.*) $(wildcard stub/*.h)

.PHONY: test asan clean

$(OUT)/sizecache_test: $(DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(OPT) -o $@ sizecache_test.cpp

$(OUT)/sizecache_asan: $(DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(ASAN) -o $@ sizecache_test.cpp

test: $(OUT)/sizecache_test
	$(OUT)/sizecache_test

asan: $(OUT)/sizecache_asan
	$(OUT)/sizecache_asan

clean:
	rm -rf $(OUT)
