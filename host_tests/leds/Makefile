# Host test of main/leds_logic.h. Run from this directory: make test, or make asan for the same run
# under AddressSanitizer and UndefinedBehaviorSanitizer.

ROOT := ../..
OUT  := out

CXX      := clang++
CXXFLAGS := -std=gnu++17 -Wall -Wextra -fno-exceptions -fno-rtti -I$(ROOT)/main
OPT      := -O2
ASAN     := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined

DEPS := leds_test.cpp $(ROOT)/main/leds_logic.h

.PHONY: test asan clean

$(OUT)/leds_test: $(DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(OPT) -o $@ leds_test.cpp

$(OUT)/leds_asan: $(DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(ASAN) -o $@ leds_test.cpp

test: $(OUT)/leds_test
	$(OUT)/leds_test

asan: $(OUT)/leds_asan
	$(OUT)/leds_asan

clean:
	rm -rf $(OUT)
