# Host test of main/browser_logic.h. Run from this directory: make test, or make asan for the same run
# under AddressSanitizer and UndefinedBehaviorSanitizer.
#
# The test includes the header and builds it against the ESP-IDF stub header of host_tests/power/stub.

ROOT := ../..
OUT  := out

CXX      := clang++
CXXFLAGS := -std=gnu++17 -Wall -Wextra -fno-exceptions -fno-rtti -I../power/stub -I$(ROOT)/main
OPT      := -O2
ASAN     := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined

DEPS := browser_power_test.cpp $(ROOT)/main/browser_logic.h $(ROOT)/main/power_logic.h $(wildcard ../power/stub/*.h)

.PHONY: test asan clean

$(OUT)/browser_power_test: $(DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(OPT) -o $@ browser_power_test.cpp

$(OUT)/browser_power_asan: $(DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(ASAN) -o $@ browser_power_test.cpp

test: $(OUT)/browser_power_test
	$(OUT)/browser_power_test

asan: $(OUT)/browser_power_asan
	$(OUT)/browser_power_asan

clean:
	rm -rf $(OUT)
