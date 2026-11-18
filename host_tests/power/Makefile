# Host test of main/power_logic.h. Run from this directory: make test, or make asan for the same run
# under AddressSanitizer and UndefinedBehaviorSanitizer.
#
# The test includes the header and builds it against the ESP-IDF stub header in stub/.

ROOT := ../..
OUT  := out

CXX      := clang++
CXXFLAGS := -std=gnu++17 -Wall -Wextra -fno-exceptions -fno-rtti -Istub -I$(ROOT)/main
OPT      := -O2
ASAN     := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined

DEPS := power_test.cpp $(ROOT)/main/power_logic.h $(wildcard stub/*.h)

.PHONY: test asan clean

$(OUT)/power_test: $(DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(OPT) -o $@ power_test.cpp

$(OUT)/power_asan: $(DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(ASAN) -o $@ power_test.cpp

test: $(OUT)/power_test
	$(OUT)/power_test

asan: $(OUT)/power_asan
	$(OUT)/power_asan

clean:
	rm -rf $(OUT)
