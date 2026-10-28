SHELL := /bin/sh

PSPDEV ?= /Users/liu/pspdev
BUILD_DIR ?= build/client
BUILD_TYPE ?= Release
PARALLEL ?= 4
PSP_CMAKE ?= $(PSPDEV)/bin/psp-cmake
BUILD_TOOL ?= cmake
PPSSPP_PACKAGE_DIR ?= dist/PSP/GAME/WiliMini
PPSSPP_GAME_DIR ?= $(HOME)/.config/ppsspp/PSP/GAME

COMMON_CONFIG := -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

.PHONY: all configure build package ppsspp-install dependencies curl ffmpeg me assets clean

all: package

configure: dependencies
	PSPDEV="$(PSPDEV)" PATH="$(PSPDEV)/bin:$$PATH" $(PSP_CMAKE) $(COMMON_CONFIG) $(CMAKE_ARGS)

build: configure
	$(BUILD_TOOL) --build $(BUILD_DIR) --parallel $(PARALLEL)

package: configure
	$(BUILD_TOOL) --build $(BUILD_DIR) --target package --parallel $(PARALLEL)

ppsspp-install:
	@test -f "$(PPSSPP_PACKAGE_DIR)/EBOOT.PBP" || { echo "Missing package: $(PPSSPP_PACKAGE_DIR)/EBOOT.PBP; run make package first." >&2; exit 1; }
	mkdir -p "$(PPSSPP_GAME_DIR)/WiliMini"
	cp -R "$(PPSSPP_PACKAGE_DIR)/." "$(PPSSPP_GAME_DIR)/WiliMini/"
	@echo "Installed WiliMini to $(PPSSPP_GAME_DIR)/WiliMini"

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
