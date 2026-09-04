FMT_FILES := $(wildcard src/*.c) $(wildcard src/*.h) \
             $(wildcard src/*.cpp) $(wildcard src/*.hpp) \
             $(wildcard include/*.h) $(wildcard include/*.hpp) \
             $(wildcard bpf/*.c) $(wildcard bpf/*.h) \
             $(wildcard tests/*.cpp)

BUILD_PRESET ?= debug

.PHONY: help init clean install-hooks format build run test vmlinux

help:
	@echo "-----------------------------------------------------------------------"
	@echo "Usage: make [target] [BUILD_PRESET=debug|release|profile]"
	@echo "Targets:"
	@echo "  init            | Install git hooks"
	@echo "  build           | Configure and build the project (BPF objects + daemon)"
	@echo "  run             | Build and run slurm-tracer (needs CAP_BPF/CAP_PERFMON)"
	@echo "  test            | Build and run tests"
	@echo "  vmlinux         | Dump the running kernel's BTF to build/vmlinux.h"
	@echo "  clean           | Remove build artifacts"
	@echo "  format          | Format C/C++ files"
	@echo "-----------------------------------------------------------------------"

init:
	$(MAKE) install-hooks

install-hooks:
	git config core.hooksPath .githooks
	@echo "Git hooks installed (.githooks/pre-commit)"

build:
	cmake --preset $(BUILD_PRESET)
	cmake --build --preset $(BUILD_PRESET)

# Loading BPF programs needs CAP_BPF + CAP_PERFMON; run as root outside a
# privileged container, or grant the caps on the binary.
run: build
	./build/$(BUILD_PRESET)/slurm-tracer

test: build
	ctest --test-dir build/$(BUILD_PRESET) --output-on-failure

vmlinux:
	@mkdir -p build
	bpftool btf dump file /sys/kernel/btf/vmlinux format c > build/vmlinux.h
	@echo "Wrote build/vmlinux.h"

clean:
	rm -rf build

format:
	clang-format -i $(FMT_FILES)
