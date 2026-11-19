# Host test of main/image_class.h. Run from this directory: make test, or make asan for the same run
# under AddressSanitizer and UndefinedBehaviorSanitizer.

ROOT := ../..
OUT  := out

CXX      := clang++
CXXFLAGS := -std=gnu++17 -Wall -Wextra -fno-exceptions -fno-rtti -I$(ROOT)/main
OPT      := -O2
ASAN     := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined

DEPS := image_class_test.cpp $(ROOT)/main/image_class.h $(ROOT)/main/image.h

.PHONY: test asan clean

$(OUT)/image_class_test: $(DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(OPT) -o $@ image_class_test.cpp

$(OUT)/image_class_asan: $(DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(ASAN) -o $@ image_class_test.cpp

test: $(OUT)/image_class_test
	$(OUT)/image_class_test

asan: $(OUT)/image_class_asan
	$(OUT)/image_class_asan

clean:
	rm -rf $(OUT)
