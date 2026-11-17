# Host test of the Oklab kernels (main/tone_fx_oklab.h) against the float code. The kernels run as
# their stand ins, ../model/tone_fx_oklab_model.cpp. Run from this directory: make test. make fit
# rebuilds and runs the fit of the cube root constants, whose output goes into
# main/tone_fx_oklab_internal.h by hand. make sched prints the stall estimate of the vector blocks
# (sched_check.py) from the object of the probe build, so build probes/oklab first.

ROOT  := ../../..
MODEL := ../model
OUT   := out

CXX      := clang++
CXXFLAGS := -std=gnu++17 -O2 -Wall -Wextra -fno-exceptions -fno-rtti -I$(ROOT)/main -I$(MODEL)

SRCS := oklab_test.cpp $(ROOT)/main/tone_fx_oklab.cpp $(MODEL)/tone_fx_oklab_model.cpp
DEPS := $(SRCS) $(ROOT)/main/tone_fx_oklab.h $(ROOT)/main/tone_fx_oklab_internal.h $(ROOT)/main/tone_fx.h \
        $(MODEL)/tone_fx_model.h

PROBE_OBJ := $(shell find $(ROOT)/probes/oklab/build -name tone_fx_oklab.cpp.obj 2>/dev/null | head -1)
OBJDUMP   := $(lastword $(sort $(wildcard $(HOME)/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump)))

.PHONY: test fit sched clean

$(OUT)/oklab_test: $(DEPS)
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

$(OUT)/fit_cbrt: fit_cbrt.cpp
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ fit_cbrt.cpp

test: $(OUT)/oklab_test
	$(OUT)/oklab_test > $(OUT)/report.txt; s=$$?; cat $(OUT)/report.txt; exit $$s

fit: $(OUT)/fit_cbrt
	$(OUT)/fit_cbrt

sched:
	@test -n "$(PROBE_OBJ)" || { echo "no probe build: run idf.py build in probes/oklab"; exit 1; }
	mkdir -p $(OUT)
	$(OBJDUMP) -d $(PROBE_OBJ) > $(OUT)/tone_fx_oklab.dis
	python3 sched_check.py $(OUT)/tone_fx_oklab.dis tone_fx_

clean:
	rm -rf $(OUT)
