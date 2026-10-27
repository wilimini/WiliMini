SHELL := /bin/sh

PSPDEV ?= /Users/liu/pspdev
BUILD_DIR ?= build/client
BUILD_TYPE ?= Release
PARALLEL ?= 4
PSP_CMAKE ?= $(PSPDEV)/bin/psp-cmake
BUILD_TOOL ?= cmake

COMMON_CONFIG := -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

.PHONY: all configure build package dependencies curl ffmpeg me assets clean

all: package

configure: dependencies
	PSPDEV="$(PSPDEV)" PATH="$(PSPDEV)/bin:$$PATH" $(PSP_CMAKE) $(COMMON_CONFIG) $(CMAKE_ARGS)

build: configure
	$(BUILD_TOOL) --build $(BUILD_DIR) --parallel $(PARALLEL)

package: configure
	$(BUILD_TOOL) --build $(BUILD_DIR) --target package --parallel $(PARALLEL)

dependencies:
	PSPDEV="$(PSPDEV)" PATH="$(PSPDEV)/bin:$$PATH" $(PSP_CMAKE) -S . -B build/dependencies \
		-DBILI_BUILD_CLIENT=OFF $(CMAKE_ARGS)
	$(BUILD_TOOL) --build build/dependencies --target dependencies --parallel $(PARALLEL)

curl:
	PSPDEV="$(PSPDEV)" PATH="$(PSPDEV)/bin:$$PATH" $(PSP_CMAKE) -S . -B build/dependencies \
		-DBILI_BUILD_CLIENT=OFF $(CMAKE_ARGS)
	$(BUILD_TOOL) --build build/dependencies --target psp_curl --parallel $(PARALLEL)

ffmpeg:
	PSPDEV="$(PSPDEV)" PATH="$(PSPDEV)/bin:$$PATH" $(PSP_CMAKE) -S . -B build/dependencies \
		-DBILI_BUILD_CLIENT=OFF $(CMAKE_ARGS)
	$(BUILD_TOOL) --build build/dependencies --target psp_ffmpeg --parallel $(PARALLEL)

me: build
	$(BUILD_TOOL) --build $(BUILD_DIR) --target me --parallel $(PARALLEL)

assets: configure
	$(BUILD_TOOL) --build $(BUILD_DIR) --target assets --parallel $(PARALLEL)

clean:
	$(BUILD_TOOL) -E remove_directory $(BUILD_DIR)
