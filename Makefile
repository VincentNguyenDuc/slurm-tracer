FMT_FILES := $(wildcard src/*.c) $(wildcard src/*.h) \
             $(wildcard src/*.cpp) $(wildcard src/*.hpp) \
             $(wildcard include/*.hpp) $(wildcard tests/*.cpp)

BUILD_PRESET ?= debug

.PHONY: init clean install-hooks format build run test

help:
	@echo "-----------------------------------------------------------------------"
	@echo "Usage: make [target] [BUILD_PRESET=debug|release|profile]"
	@echo "Targets:"
	@echo "  init            | Install git hooks"
	@echo "  build           | Configure and build the project"
	@echo "  run             | Build and run the main executable"
	@echo "  test            | Build and run tests"
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

run: build
	./build/$(BUILD_PRESET)/main

test: build
	ctest --test-dir build/$(BUILD_PRESET) --output-on-failure

clean:
	rm -rf build

format:
	clang-format -i $(FMT_FILES)
