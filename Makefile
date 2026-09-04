# Recursive on purpose: sources live in nested directories (src/core,
# src/plugins/probes/<name>, ...), and a non-recursive wildcard would silently
# stop matching them. The pre-commit hook runs `make format`, so a miss here
# means formatting quietly stops being enforced rather than failing loudly.
FMT_DIRS := src tests
FMT_FILES := $(shell find $(FMT_DIRS) \
                 \( -name '*.c' -o -name '*.h' \
                 -o -name '*.cpp' -o -name '*.hpp' \) \
                 -not -path '*/third_party/*' 2>/dev/null)

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
