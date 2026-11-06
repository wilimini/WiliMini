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

# wili-server (server/) is plain Go and needs neither PSPDEV nor the PSP dependencies.
# The variables are recursive so that `go` is only run by the server targets.
GO ?= go
SERVER_DIR ?= server
SERVER_OUT ?= dist/server
SERVER_VERSION ?= $(shell git rev-parse --short=7 HEAD 2>/dev/null || echo dev)
SERVER_GOOS ?= $(shell $(GO) env GOOS)
SERVER_GOARCH ?= $(shell $(GO) env GOARCH)
SERVER_EXT = $(if $(filter windows,$(SERVER_GOOS)),.exe,)
SERVER_BIN = $(SERVER_OUT)/wili-server-$(SERVER_GOOS)-$(SERVER_GOARCH)$(SERVER_EXT)
# The platforms the Server workflow publishes.
SERVER_TARGETS ?= linux/amd64 linux/arm64 darwin/arm64 darwin/amd64 windows/amd64

.PHONY: all configure build package ppsspp-install dependencies curl ffmpeg me assets clean \
	server server-test server-all

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

# Builds wili-server for SERVER_GOOS/SERVER_GOARCH (default: this machine) into
# $(SERVER_OUT)/wili-server-<os>-<arch>[.exe]. `make clean` does not touch it.
server:
	mkdir -p "$(SERVER_OUT)"
	cd "$(SERVER_DIR)" && CGO_ENABLED=0 GOOS="$(SERVER_GOOS)" GOARCH="$(SERVER_GOARCH)" \
		$(GO) build -trimpath -ldflags "-s -w -X main.version=$(SERVER_VERSION)" \
		-o "$(abspath $(SERVER_BIN))" ./cmd/wili-server
	@echo "Built $(SERVER_BIN)"

# What the Server workflow runs on every platform: vet and the tests. ffmpeg-dependent
# tests skip when ffmpeg is not installed.
server-test:
	cd "$(SERVER_DIR)" && $(GO) vet ./... && $(GO) test -count=1 ./...

server-all:
	@set -e; for target in $(SERVER_TARGETS); do \
		$(MAKE) --no-print-directory server SERVER_GOOS=$${target%/*} SERVER_GOARCH=$${target#*/}; \
	done
