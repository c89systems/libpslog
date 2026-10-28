SHELL := bash
.DEFAULT_GOAL := help
MAKEFLAGS += --no-builtin-rules
.NOTPARALLEL:

DEBUG_PRESET := debug
HOST_PRESET := host
VALGRIND_PRESET := valgrind
FUZZ_PRESET := fuzz
COVERAGE_PRESET := coverage
# Select installed osxcross even when its cached host MIG still needs preparing.
RELEASE_BUILD_PRESETS := \
	x86_64-linux-gnu-release \
	x86_64-linux-musl-release \
	aarch64-linux-gnu-release \
	aarch64-linux-musl-release \
	armhf-linux-gnu-release \
	armhf-linux-musl-release \
	$(shell scripts/cpkt-toolchains.sh discover arm64-apple-darwin | grep -q '^prefix=.' && printf '%s' arm64-apple-darwin-release)

CROSS_RELEASE_PRESETS := \
	aarch64-linux-gnu-release \
	aarch64-linux-musl-release \
	armhf-linux-gnu-release \
	armhf-linux-musl-release
LUA_RELEASE_VERSION := $(shell ./lua/scripts/release_version.sh)
LUA_DIST_DIR := $(CURDIR)/dist
LUA_RELEASE_ROCKSPEC := $(LUA_DIST_DIR)/lua-pslog-$(LUA_RELEASE_VERSION)-1.rockspec
LUA_RELEASE_PACK_DIR := $(CURDIR)/build/lua-release-pack
LUA_RELEASE_STAGE_NAME := lua-pslog-$(LUA_RELEASE_VERSION)
LUA_RELEASE_STAGE_DIR := $(LUA_RELEASE_PACK_DIR)/$(LUA_RELEASE_STAGE_NAME)
LUA_RELEASE_MANIFEST := lua/RELEASE_MANIFEST.in
LUA_RELEASE_STAGE_SCRIPT := lua/scripts/stage_release_sources.sh
LUA_RELEASE_STAGE_INPUTS := \
	$(LUA_RELEASE_MANIFEST) \
	$(LUA_RELEASE_STAGE_SCRIPT) \
	LICENSE \
	include/pslog.h \
	include/pslog_lua.h \
	lua/README.md \
	lua/lua-pslog.rockspec.in \
	lua/pslog/init.lua \
	lua/scripts/render_release_rockspec.sh \
	lua/scripts/release_version.sh \
	lua/src/pslog_lua.c
LUA_RELEASE_PACK_SOURCE_TAR := $(LUA_RELEASE_PACK_DIR)/$(LUA_RELEASE_STAGE_NAME).tar
LUA_RELEASE_PACK_SOURCE_TARBALL := $(LUA_RELEASE_PACK_SOURCE_TAR).gz
LUA_RELEASE_SOURCE_TARBALL := $(LUA_DIST_DIR)/$(LUA_RELEASE_STAGE_NAME).tar.gz
LUA_RELEASE_PACK_ROCKSPEC := $(LUA_RELEASE_PACK_DIR)/lua-pslog-$(LUA_RELEASE_VERSION)-1.rockspec
LUA_RELEASE_ROCK := $(LUA_DIST_DIR)/lua-pslog-$(LUA_RELEASE_VERSION)-1.src.rock
LUA_ROCK_TREE := build/luarocks
LUA_ROCKSPEC := $(LUA_ROCK_TREE)/lua-pslog-$(LUA_RELEASE_VERSION)-1.rockspec
LUA_ROCK_STAMP := $(LUA_ROCK_TREE)/.installed.stamp
LUA_ROCK_BUILD_LOCK := $(LUA_ROCK_TREE)/.build.lock.d
LUA_ROCKS ?= luarocks
# Resolve optional LuaRocks state only for Lua targets.  Immediate $(shell ...)
# expansion would make every C-only Make command depend on LuaRocks.
LUA_STAGED_ROOT := build/lua-host
LUA_STAGED_INCLUDE_DIR := $(LUA_STAGED_ROOT)/include
LUA_STAGED_LIB_DIR := $(LUA_STAGED_ROOT)/lib
LUA_STAGED_LUA_DEPS := $(LUA_STAGED_ROOT)/.staged.stamp
LUA_SDK_PREFIX := $(CURDIR)/build/lua-sdk
LUA_SDK_INCLUDE_DIR := $(LUA_SDK_PREFIX)/include
LUA_SDK_LIB_DIR := $(LUA_SDK_PREFIX)/lib
LUA_SDK_STAMP := $(LUA_SDK_PREFIX)/.installed.stamp
LUA_LOCAL_LIBDIR := $(LUA_SDK_LIB_DIR)
LUA_ROCK_SOURCES := \
	lua/scripts/build_local_rock.sh \
	lua/lua-pslog.rockspec.in \
	lua/scripts/check_binding_boundary.sh \
	lua/scripts/render_release_rockspec.sh \
	lua/scripts/release_version.sh \
	lua/pslog/init.lua \
	lua/src/pslog_lua.c \
	include/pslog_lua.h \
	include/pslog.h
GO_PRODUCTION_DATASET := gobencher/benchmark/production_data_generated.go
GO_PRODUCTION_DATASET_SOURCE := bench/bench_production_dataset.c
GO_PRODUCTION_DATASET_TOOL := bench/gen_go_production_dataset.c
GO_CKVFMT_WRAPPERS := gobencher/benchmark/cpslog_kvfmt_generated.go
HOST_GENERATED_VERSION_HEADER := $(CURDIR)/build/host/generated/include/pslog_version.h
HOST_C_COMPILER = $(shell sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' build/host/CMakeCache.txt | head -n 1)
HOST_CXX_COMPILER = $(shell cxx=$$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' build/host/CMakeCache.txt | head -n 1); override=$$(sed -n 's/^PSLOG_ALLOW_HOST_COMPILER:[^=]*=//p' build/host/CMakeCache.txt | tail -n 1); if [ -n "$$cxx" ]; then printf '%s\n' "$$cxx"; elif [ "$$(uname -s)" = Darwin ] || [ "$$override" = 1 ] || [ "$$override" = ON ] || [ "$$override" = TRUE ] || [ "$$override" = YES ]; then command -v c++ 2>/dev/null || true; fi)
HOST_ELF_LINKER_FLAGS = $(shell sed -n 's/^PSLOG_NONSHIPPED_ELF_LINKER_FLAGS:STRING=//p' build/$(HOST_PRESET)/CMakeCache.txt 2>/dev/null | tail -n 1)

BENCH_ITERS ?= 200000
FUZZ_TIME ?= 30
FUZZ_LONG_TIME ?= 300
GO_BENCH_COUNT ?= 1
ELEVATORPITCH_ARGS ?=
TIMED := ./scripts/run_timed.sh
RELEASE_TIMING_FILE := $(CURDIR)/build/release-timings.tsv

.PHONY: \
	help \
	build \
	build-debug \
	build-host \
	build-release \
	format \
	format-check \
	test \
	test-debug \
	test-host \
	test-all \
	deps-debug \
	deps-release \
	deps-cross \
	clangd \
	valgrind \
	coverage \
	fuzz \
	fuzz-smoke \
	fuzz-long \
	bench \
	bench-gate \
	benchmarks-c \
	benchmarks-gobencher \
	benchmarks-go \
	benchmarks-all \
	benchmarks \
	elevatorpitch \
	gobencher-tests \
	prepare-gobencher-data \
	perf-gate \
	bench-freeze-baseline \
	cross-build \
	cross-test \
	test-cross \
	package \
	package-source \
	package-source-smoke \
	package-single-header \
	package-checksums \
	package-verify \
	verify-release-archives \
	verify-release-privacy \
	lifecycle-version-contract \
	release-matrix \
	finalize-slice \
	release-pipeline \
	prerelease \
	prerelease-hardening \
	print-release-version \
	release-lua-artifacts \
	lua-env \
	lua-rock \
	lua-test \
	release \
	clean \
	clean-dist \
	FORCE

help:
	@printf '%s\n' \
		'make build           Configure and build the debug preset.' \
		'make deps-debug      Provision the pinned native Bootlin collection (normally automatic).' \
		'make deps-release    Provision all pinned Linux release collections (normally automatic).' \
		'make deps-cross      Provision the pinned non-host Linux collections (normally automatic).' \
		'make build-debug     Alias for make build.' \
		'make build-host      Configure and build the local development build.' \
		'make build-release   Configure and build the full shipped release build matrix.' \
		'make format          Run clang-format on repo C/C header sources.' \
		'make format-check    Check formatting without modifying source.' \
		'make clangd          Check native debug sources with host clangd.' \
		'make test            Run the debug C test suite.' \
		'make test-debug      Alias for make test.' \
		'make test-host       Build and run the local CTest suite.' \
		'make test-all        Run C tests, native hardening, Go gobencher tests, and the Go-vs-C perf gate.' \
		'make valgrind        Run the native Valgrind memory-check gate.' \
		'make coverage        Run the coverage preset and generate coverage-report.' \
		'make fuzz            Run a bounded native AFL++ fuzz job.' \
		'make fuzz-smoke      Run a short fuzz smoke pass.' \
		'make fuzz-long       Run a longer native AFL++ fuzz job (FUZZ_LONG_TIME=$(FUZZ_LONG_TIME)).' \
		'make bench           Alias for make benchmarks.' \
		'make bench-gate      Alias for make perf-gate.' \
		'make benchmarks-c    Run the pure C benchmark matrix (BENCH_ITERS=$(BENCH_ITERS)).' \
		'make benchmarks-gobencher Run the full gobencher benchmark suite.' \
		'make benchmarks-all  Run both C and gobencher benchmark suites.' \
		'make benchmarks      Alias for make benchmarks-all.' \
		'make elevatorpitch   Run the live Go/C/Lua comparison chart.' \
		'make gobencher-tests Run all Go gobencher tests.' \
		'make perf-gate       Compare performance against this host’s baseline.' \
		'make bench-freeze-baseline  Intentionally capture this host’s C/Lua baseline.' \
		'make cross-build     Build all non-host cross release presets.' \
		'make cross-test      Test all non-host cross release presets.' \
		'make test-cross      Alias for make cross-test.' \
		'make package         Build host C package artifacts, Lua artifacts, and checksums.' \
		'make package-source  Build the source release archive.' \
		'make package-source-smoke Verify the source archive builds outside git.' \
		'make package-single-header Build the single-header release artifact.' \
		'make package-checksums Generate the release checksum manifest.' \
		'make package-verify  Run package archive and release privacy verification.' \
		'make verify-release-archives Alias for make package-verify.' \
		'make verify-release-privacy Run checksum-manifest privacy/relocatability gate.' \
		'make lifecycle-version-contract Verify exact lightweight Git-tag version resolution.' \
		'make release-matrix  Build/test/package the release target matrix.' \
		'make finalize-slice  Run formatting and focused local verification.' \
		'make prerelease      Run deterministic local pre-release checks.' \
		'make release         Clean, run prerelease proof, and write build/release-timings.tsv.' \
		'make prerelease-hardening Run prerelease plus the long AFL++ fuzz pass.' \
		'make print-release-version Print the version used by release artifacts.' \
		'make release-lua-artifacts Build the standalone Lua release artifacts.' \
		'make lua-rock        Install a local C SDK and build the Lua module into build/luarocks.' \
		'make lua-env         Print shell exports for the repo-local Lua rock.' \
		'make lua-test        Run the Lua binding smoke tests.' \
		'make prepare-gobencher-data Regenerate deterministic Go benchmark inputs.' \
		'make clean           Remove build/, dist/, and local .cache/ generated state.' \
		'make clean-dist      Remove dist/ release artifacts.'

build:
	./scripts/configure_cmake.sh --preset $(DEBUG_PRESET)
	cmake --build --preset $(DEBUG_PRESET)

deps-debug:
	./scripts/cpkt-toolchains.sh ensure x86_64-linux-gnu

deps-release:
	./scripts/cpkt-toolchains.sh ensure all

deps-cross:
	@set -e; for target in aarch64-linux-gnu aarch64-linux-musl armhf-linux-gnu armhf-linux-musl; do \
		./scripts/cpkt-toolchains.sh ensure "$$target"; \
	done

build-debug: build

build-host:
	./scripts/configure_cmake.sh --preset $(HOST_PRESET)
	cmake --build --preset $(HOST_PRESET)

build-release:
	@set -e; for preset in $(RELEASE_BUILD_PRESETS); do \
		./scripts/configure_cmake.sh --preset "$$preset"; \
		cmake --build --preset "$$preset"; \
	done

format:
	./scripts/configure_cmake.sh --preset $(DEBUG_PRESET)
	cmake --build --preset format

format-check:
	./scripts/configure_cmake.sh --preset $(DEBUG_PRESET)
	cmake --build --preset format-check

clangd: build
	./scripts/clangd_check.sh

test: build
	ctest --preset $(DEBUG_PRESET) --output-on-failure

test-debug: test

test-host: build-host
	ctest --preset $(HOST_PRESET) --output-on-failure

gobencher-tests: build-host lua-rock $(GO_PRODUCTION_DATASET) $(GO_CKVFMT_WRAPPERS)
	cd gobencher && CC="$(HOST_C_COMPILER)" CXX="$(HOST_CXX_COMPILER)" "$(CURDIR)/scripts/local-go.sh" test -a ./...

bench-freeze-baseline: build-host lua-rock $(GO_PRODUCTION_DATASET) $(GO_CKVFMT_WRAPPERS)
	CC="$(HOST_C_COMPILER)" CXX="$(HOST_CXX_COMPILER)" ./bench/run_rebaseline.sh

perf-gate: build-host lua-rock $(GO_PRODUCTION_DATASET) $(GO_CKVFMT_WRAPPERS)
	CC="$(HOST_C_COMPILER)" CXX="$(HOST_CXX_COMPILER)" ./bench/run_perf_gate.sh

test-all:
	$(TIMED) test $(MAKE) test
	$(TIMED) valgrind $(MAKE) valgrind
	$(TIMED) fuzz-smoke $(MAKE) fuzz-smoke
	$(TIMED) cross-test $(MAKE) cross-test
	$(TIMED) gobencher-tests $(MAKE) gobencher-tests
	$(TIMED) perf-gate $(MAKE) perf-gate

valgrind:
	./scripts/configure_cmake.sh --preset $(VALGRIND_PRESET)
	cmake --build --preset $(VALGRIND_PRESET)
	valgrind --leak-check=full --track-origins=yes --error-exitcode=1 \
		./build/$(VALGRIND_PRESET)/pslog_valgrind_facade_tests

coverage:
	./scripts/configure_cmake.sh --preset $(COVERAGE_PRESET)
	cmake --build --preset $(COVERAGE_PRESET)
	cmake --build --preset coverage-report

fuzz:
	./scripts/fuzz.sh run $(FUZZ_TIME)

fuzz-smoke:
	./scripts/fuzz.sh smoke 5

fuzz-long:
	./scripts/fuzz.sh long $(FUZZ_LONG_TIME)

benchmarks-c: build-host
	./build/host/pslog_bench $(BENCH_ITERS) all

benchmarks-gobencher: build-host lua-rock $(GO_PRODUCTION_DATASET) $(GO_CKVFMT_WRAPPERS)
	cd gobencher && CC="$(HOST_C_COMPILER)" CXX="$(HOST_CXX_COMPILER)" "$(CURDIR)/scripts/local-go.sh" test ./benchmark -run '^$$' -bench . -benchmem -count=$(GO_BENCH_COUNT)

benchmarks-go: benchmarks-gobencher

benchmarks-all: benchmarks-c benchmarks-gobencher

benchmarks: benchmarks-all

bench: benchmarks

bench-gate: perf-gate

elevatorpitch: build-host lua-rock $(GO_PRODUCTION_DATASET) $(GO_CKVFMT_WRAPPERS)
	eval "$$($(LUA_ROCKS) path --tree $(LUA_ROCK_TREE))" && cd gobencher && CC="$(HOST_C_COMPILER)" CXX="$(HOST_CXX_COMPILER)" "$(CURDIR)/scripts/local-go.sh" run ./cmd/elevatorpitch $(ELEVATORPITCH_ARGS)

cross-build:
	@set -e; for preset in $(CROSS_RELEASE_PRESETS); do \
		./scripts/configure_cmake.sh --preset "$$preset"; \
		cmake --build --preset "$$preset"; \
	done

cross-test: cross-build
	@set -e; for preset in $(CROSS_RELEASE_PRESETS); do \
		ctest --preset "$$preset" --output-on-failure; \
	done

test-cross: cross-test

package:
	./scripts/configure_cmake.sh --preset $(HOST_PRESET)
	cmake --build build/$(HOST_PRESET) --target package-clean-dist
	cmake --build build/$(HOST_PRESET) --target package-archive
	cmake --build build/$(HOST_PRESET) --target package-single-header
	cmake --build build/$(HOST_PRESET) --target package-source
	$(MAKE) release-lua-artifacts
	cmake --build build/$(HOST_PRESET) --target package-checksums

package-source:
	./scripts/configure_cmake.sh --preset $(HOST_PRESET)
	cmake --build build/$(HOST_PRESET) --target package-source

package-source-smoke:
	./scripts/configure_cmake.sh --preset $(HOST_PRESET)
	cmake -DPSLOG_ROOT=$(CURDIR) -DPSLOG_BINARY_DIR=$(CURDIR)/build/$(HOST_PRESET) -DPSLOG_VERSION=$(LUA_RELEASE_VERSION) -DPSLOG_TOOLCHAIN_RELATIVE=cmake/toolchains/linux-x86_64-gnu.cmake -P tests/source_archive_smoke_test.cmake

package-single-header:
	./scripts/configure_cmake.sh --preset $(HOST_PRESET)
	cmake --build build/$(HOST_PRESET) --target package-single-header

package-checksums:
	./scripts/configure_cmake.sh --preset $(HOST_PRESET)
	cmake --build build/$(HOST_PRESET) --target package-checksums

package-verify:
	./scripts/configure_cmake.sh --preset $(HOST_PRESET)
	ctest --test-dir build/$(HOST_PRESET) -R '^(package_archives_test|release_privacy_gate_test)$$' --output-on-failure --stop-on-failure --no-tests=error
	cmake -DPSLOG_ROOT=$(CURDIR) -DPSLOG_BINARY_DIR=$(CURDIR)/build/$(HOST_PRESET) -DPSLOG_VERSION=$(LUA_RELEASE_VERSION) -DPSLOG_TOOLCHAIN_RELATIVE=cmake/toolchains/linux-x86_64-gnu.cmake -P tests/source_archive_smoke_test.cmake
	cmake --build build/$(HOST_PRESET) --target package-privacy-gate

verify-release-archives: package-verify

verify-release-privacy:
	./scripts/configure_cmake.sh --preset $(HOST_PRESET)
	./scripts/verify_release_privacy.sh --build-dir build/$(HOST_PRESET) --target-id x86_64-linux-gnu

release-matrix:
	./scripts/run_linux_release_matrix.sh

lifecycle-version-contract:
	./scripts/lifecycle_version_contract.sh

finalize-slice: format clangd build-host
	./scripts/configure_cmake.sh --preset $(HOST_PRESET)
	ctest --test-dir build/$(HOST_PRESET) -R '^(pslog_tests|pslog_single_header_tests|example_integration_test|public_symbol_visibility_test|darwin_linker_route_test)$$' --output-on-failure --stop-on-failure --no-tests=error

release-pipeline:
	$(TIMED) format-check $(MAKE) format-check
	$(TIMED) test $(MAKE) test
	$(TIMED) valgrind $(MAKE) valgrind
	$(TIMED) fuzz-smoke $(MAKE) fuzz-smoke
	$(TIMED) gobencher-tests $(MAKE) gobencher-tests
	$(TIMED) lua-test $(MAKE) lua-test
	$(TIMED) perf-gate $(MAKE) perf-gate
	$(TIMED) release-matrix $(MAKE) release-matrix

prerelease: release-pipeline

prerelease-hardening: prerelease fuzz-long

print-release-version:
	@./lua/scripts/release_version.sh

release-lua-artifacts: $(LUA_RELEASE_ROCKSPEC) $(LUA_RELEASE_SOURCE_TARBALL) $(LUA_RELEASE_ROCK)

$(LUA_RELEASE_PACK_DIR):
	mkdir -p "$(LUA_RELEASE_PACK_DIR)"

$(LUA_DIST_DIR):
	mkdir -p "$(LUA_DIST_DIR)"

$(LUA_RELEASE_STAGE_DIR): $(LUA_RELEASE_STAGE_INPUTS) $(LUA_SDK_STAMP) | $(LUA_RELEASE_PACK_DIR)
	rm -rf "$(LUA_RELEASE_STAGE_DIR)"
	bash "$(LUA_RELEASE_STAGE_SCRIPT)" "$(CURDIR)" "$(LUA_RELEASE_STAGE_DIR)" "$(LUA_RELEASE_VERSION)" "$(LUA_SDK_INCLUDE_DIR)/pslog_version.h"

$(LUA_RELEASE_ROCKSPEC): $(LUA_ROCK_SOURCES) Makefile | $(LUA_DIST_DIR)
	./lua/scripts/render_release_rockspec.sh "$(LUA_RELEASE_VERSION)" "$(LUA_RELEASE_ROCKSPEC)"

$(LUA_RELEASE_PACK_ROCKSPEC): Makefile $(LUA_RELEASE_STAGE_DIR)
	cd "$(LUA_RELEASE_STAGE_DIR)" && ./lua/scripts/render_release_rockspec.sh "$(LUA_RELEASE_VERSION)" "../$(notdir $(LUA_RELEASE_PACK_ROCKSPEC))" "file://$(notdir $(LUA_RELEASE_PACK_SOURCE_TARBALL))" ""

$(LUA_RELEASE_SOURCE_TARBALL): $(LUA_RELEASE_STAGE_DIR) | $(LUA_DIST_DIR)
	rm -f "$(LUA_RELEASE_PACK_SOURCE_TAR)" "$(LUA_RELEASE_PACK_SOURCE_TARBALL)"
	cd "$(LUA_RELEASE_PACK_DIR)" && tar -cf "$(notdir $(LUA_RELEASE_PACK_SOURCE_TAR))" --format=gnu --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner --mode=u=rwX,go=rX "$(LUA_RELEASE_STAGE_NAME)"
	gzip -n -9 -f "$(LUA_RELEASE_PACK_SOURCE_TAR)"
	cp "$(LUA_RELEASE_PACK_SOURCE_TARBALL)" "$(LUA_RELEASE_SOURCE_TARBALL)"

$(LUA_RELEASE_ROCK): $(LUA_RELEASE_PACK_ROCKSPEC) $(LUA_RELEASE_ROCKSPEC) $(LUA_RELEASE_SOURCE_TARBALL)
	chmod 644 "$(LUA_RELEASE_PACK_ROCKSPEC)" "$(LUA_RELEASE_PACK_SOURCE_TARBALL)"
	TZ=UTC touch -t 198001010000 "$(LUA_RELEASE_PACK_ROCKSPEC)" "$(LUA_RELEASE_PACK_SOURCE_TARBALL)"
	rm -f "$(LUA_RELEASE_PACK_DIR)/$(notdir $(LUA_RELEASE_ROCK))"
	cd "$(LUA_RELEASE_PACK_DIR)" && TZ=UTC zip -X -9 "$(notdir $(LUA_RELEASE_ROCK))" "$(notdir $(LUA_RELEASE_PACK_ROCKSPEC))" "$(notdir $(LUA_RELEASE_PACK_SOURCE_TARBALL))"
	@set -eu; verify_dir="$$(mktemp -d "$(CURDIR)/build/lua-rock-verify.XXXXXX")"; \
	trap 'rm -rf "$$verify_dir"' EXIT; \
	cd "$$verify_dir" && "$(LUA_ROCKS)" unpack "$(LUA_RELEASE_PACK_DIR)/$(notdir $(LUA_RELEASE_ROCK))"
	mv "$(LUA_RELEASE_PACK_DIR)/$(notdir $(LUA_RELEASE_ROCK))" "$(LUA_RELEASE_ROCK)"
	rm -rf "$(LUA_RELEASE_PACK_DIR)"

lua-rock: $(LUA_ROCK_STAMP)

lua-env:
	@printf '%s\n' 'eval "$$($(LUA_ROCKS) path --tree $(LUA_ROCK_TREE))"'

prepare-gobencher-data: lua-rock $(GO_PRODUCTION_DATASET) $(GO_CKVFMT_WRAPPERS)

$(LUA_ROCKSPEC): $(LUA_ROCK_SOURCES)
	mkdir -p "$(LUA_ROCK_TREE)"
	./lua/scripts/render_release_rockspec.sh "$(LUA_RELEASE_VERSION)" "$(LUA_ROCKSPEC)" "git+file://$(CURDIR)"

$(LUA_STAGED_LUA_DEPS): FORCE
	./scripts/configure_cmake.sh --source cmake/lua --build build/lua-runtime --target host -- -DCMAKE_BUILD_TYPE=Release
	cmake --build build/lua-runtime
	cmake --install build/lua-runtime --prefix "$(CURDIR)/$(LUA_STAGED_ROOT)"
	touch "$@"

$(LUA_SDK_STAMP): build-host
	rm -rf "$(LUA_SDK_PREFIX)"
	cmake --install build/$(HOST_PRESET) --prefix "$(LUA_SDK_PREFIX)"
	test -f "$(LUA_SDK_INCLUDE_DIR)/pslog.h"
	test -f "$(LUA_SDK_INCLUDE_DIR)/pslog_version.h"
	{ test -e "$(LUA_SDK_LIB_DIR)/libpslog.so" || test -e "$(LUA_SDK_LIB_DIR)/libpslog.dylib"; }
	touch "$@"

$(LUA_ROCK_STAMP): $(LUA_ROCKSPEC) $(LUA_ROCK_SOURCES) $(LUA_STAGED_LUA_DEPS) $(LUA_SDK_STAMP)
	./scripts/with_lock.sh "$(LUA_ROCK_BUILD_LOCK)" bash lua/scripts/build_local_rock.sh "$(LUA_ROCKS)" "$(CURDIR)/$(LUA_ROCKSPEC)" "$(CURDIR)/$(LUA_ROCK_TREE)" "$(HOST_C_COMPILER)"
	touch "$(LUA_ROCK_STAMP)"

lua-test: lua-rock
	./lua/scripts/check_binding_boundary.sh "$(LUA_ROCK_TREE)"
	CC="$(HOST_C_COMPILER)" CXX="$(HOST_CXX_COMPILER)" ./lua/scripts/run_interop_embedder_test.sh "$(CURDIR)/build/$(HOST_PRESET)" "$(LUA_RELEASE_VERSION)" "$(LUA_ROCK_TREE)" "$(LUA_SDK_PREFIX)"
	eval "$$($(LUA_ROCKS) path --tree $(LUA_ROCK_TREE))" && "$(CURDIR)/build/lua-runtime/pslog_lua" lua/tests/test_pslog.lua

$(GO_PRODUCTION_DATASET): $(HOST_GENERATED_VERSION_HEADER) $(GO_PRODUCTION_DATASET_TOOL) $(GO_PRODUCTION_DATASET_SOURCE)
	@set -eu; tmp_bin="$$(mktemp "$(CURDIR)/build/.gen_go_production_dataset.XXXXXX")"; tmp_output="$$(mktemp "$(CURDIR)/build/.gen_go_production_dataset_output.XXXXXX")"; \
	trap 'rm -f "$$tmp_bin" "$$tmp_output"' EXIT; \
	rm -f "$$tmp_bin"; \
		"$(HOST_C_COMPILER)" -std=c99 -O2 -I"$(CURDIR)" -I"$(CURDIR)/include" -I"$(CURDIR)/build/host/generated/include" "$(GO_PRODUCTION_DATASET_TOOL)" "$(GO_PRODUCTION_DATASET_SOURCE)" $(foreach flag,$(HOST_ELF_LINKER_FLAGS),'$(flag)') -o "$$tmp_bin"; \
		"$$tmp_bin" >"$$tmp_output"; \
	mv "$$tmp_output" "$(GO_PRODUCTION_DATASET)"

$(GO_CKVFMT_WRAPPERS): $(GO_PRODUCTION_DATASET) $(LUA_SDK_STAMP) gobencher/cmd/gen_ckvfmt_wrappers/main.go gobencher/benchmark/cpslog_kvfmt.go
	cd gobencher/benchmark && CC="$(HOST_C_COMPILER)" CXX="$(HOST_CXX_COMPILER)" "$(CURDIR)/scripts/local-go.sh" run ../cmd/gen_ckvfmt_wrappers

release:
	$(MAKE) lifecycle-version-contract
	PKT_TIMING_FILE="$(RELEASE_TIMING_FILE)" $(TIMED) release-clean $(MAKE) clean
	PKT_TIMING_FILE="$(RELEASE_TIMING_FILE)" $(TIMED) release-pipeline $(MAKE) release-pipeline
	@printf 'Release timings: %s\n' "$(RELEASE_TIMING_FILE)"

clean:
	./scripts/clean.sh

clean-dist:
	./scripts/clean.sh --dist-only
