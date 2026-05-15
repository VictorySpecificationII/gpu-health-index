# probe/Makefile — GPU Health Probe binary
#
# Requires: CUDA toolkit (nvcc >= 11.0), cuBLAS, NVML
#
# Targets:
#   make              — build gpu_health_probe
#   make install      — install to $(PREFIX)/bin
#   make clean        — remove build artefacts
#
# Flags:
#   DEBUG=1           — disable optimisation, add debug symbols
#   PREFIX=/usr/local — install prefix (default)
#
# Architecture targets: Ampere (sm_80), Ada Lovelace (sm_89), Hopper (sm_90).
# BF16 tensor ops require sm_80 minimum.

NVCC    := nvcc
PREFIX  ?= /usr/local
BINARY  := gpu_health_probe

NVCC_FLAGS := -std=c++17 \
              -Xcompiler -Wall,-Wextra \
              --generate-code arch=compute_80,code=sm_80 \
              --generate-code arch=compute_89,code=sm_89 \
              --generate-code arch=compute_90,code=sm_90

ifeq ($(DEBUG),1)
    NVCC_FLAGS += -g -G -O0
else
    NVCC_FLAGS += -O2
endif

LDFLAGS := -lcublas -lnvidia-ml

.PHONY: all clean install

all: $(BINARY)

$(BINARY): gpu_health_probe.cu
	$(NVCC) $(NVCC_FLAGS) -o $@ $< $(LDFLAGS)

install: $(BINARY)
	install -Dm755 $(BINARY) $(PREFIX)/bin/$(BINARY)

clean:
	rm -f $(BINARY)
