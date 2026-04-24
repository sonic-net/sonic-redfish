# Makefile for sonic-redfish
.ONESHELL:
SHELL = /bin/bash
.SHELLFLAGS += -e

# Build configuration
CONFIGURED_ARCH ?= amd64
SONIC_CONFIG_MAKE_JOBS ?= $(shell nproc)

# Source configuration
BMCWEB_HEAD_COMMIT ?= 6926d430
BMCWEB_REPO_URL ?= https://github.com/openbmc/bmcweb.git

# Target directory for build artifacts
SONIC_REDFISH_TARGET ?= target/debs/trixie

# Directories
REPO_ROOT := $(shell pwd)
BMCWEB_DIR := $(REPO_ROOT)/bmcweb
BRIDGE_DIR := $(REPO_ROOT)/sonic-dbus-bridge
PATCHES_DIR := $(REPO_ROOT)/patches
SCRIPTS_DIR := $(REPO_ROOT)/scripts
BUILD_DIR := $(REPO_ROOT)/build
TARGET_DIR := $(REPO_ROOT)/$(SONIC_REDFISH_TARGET)
SERIES_FILE := $(PATCHES_DIR)/series
DEBIAN_DIR := $(BMCWEB_DIR)/debian

# Build artifacts
BMCWEB_BINARY := $(BMCWEB_DIR)/build/bmcweb
BRIDGE_BINARY := $(BRIDGE_DIR)/build/sonic-dbus-bridge

# Docker configuration
DOCKER_BUILDER_IMAGE := sonic-redfish-builder:latest
DOCKERFILE_BUILD := $(BUILD_DIR)/Dockerfile.build

# Main targets
MAIN_TARGET := $(BMCWEB_BINARY)
DERIVED_TARGETS := $(BRIDGE_BINARY)

.PHONY: all build clean reset setup-bmcweb copy-patches apply-patches build-bmcweb build-bridge build-bmcweb-native build-bridge-native build-in-docker help

# Default target - Build both components (via Docker)
all: build
	@echo ""
	@echo "========================================="
	@echo "All components built successfully!"
	@echo "========================================="
	@echo ""
	@echo "Build artifacts in $(SONIC_REDFISH_TARGET):"
	@ls -lh $(TARGET_DIR)/*.deb 2>/dev/null || echo "  No .deb files found"

define HELP_TEXT
sonic-redfish Build System (Docker-only)
=========================================

Targets:
  make                  Build bmcweb + sonic-dbus-bridge (default)
  make build-bmcweb     Build bmcweb only
  make build-bridge     Build sonic-dbus-bridge only
  make clean            Remove build artifacts; reset bmcweb source to pristine (keeps dir)
  make reset            Full wipe: delete bmcweb dir, remove Docker image, clear target/
  make help             Show this help

Variables (override with `make VAR=value`):
  SONIC_CONFIG_MAKE_JOBS  Parallel build jobs     [default: $$(nproc)]
  SONIC_REDFISH_TARGET    Output directory        [default: target/debs/trixie]
  BMCWEB_HEAD_COMMIT      bmcweb commit to build  [default: 6926d430]
  BMCWEB_REPO_URL         bmcweb git URL          [default: openbmc/bmcweb on GitHub]

Examples:
  make                             Build everything (clones bmcweb if missing)
  make SONIC_CONFIG_MAKE_JOBS=4    Build with 4 parallel jobs
  make BMCWEB_HEAD_COMMIT=abc123   Build against a specific bmcweb commit
endef
export HELP_TEXT

help:
	@echo "$$HELP_TEXT"

# Build target - Always Docker
build: $(DOCKERFILE_BUILD)
	@echo "========================================="
	@echo "Building sonic-redfish (Docker-only mode)"
	@echo "========================================="
	@echo ""

	# Build Docker image
	@echo "Building Docker build environment..."
	docker build -t $(DOCKER_BUILDER_IMAGE) -f $(DOCKERFILE_BUILD) $(BUILD_DIR)
	@echo "  Build environment ready"
	@echo ""

	# Run build inside Docker
	@echo "Running build inside Docker container..."
	docker run --rm \
		-v "$(REPO_ROOT):/workspace" \
		-w /workspace \
		-e SONIC_CONFIG_MAKE_JOBS=$(SONIC_CONFIG_MAKE_JOBS) \
		$(DOCKER_BUILDER_IMAGE) \
		bash -c "\
			set -e; \
			git config --global --add safe.directory /workspace; \
			git config --global --add safe.directory /workspace/bmcweb; \
			make -f Makefile build-in-docker; \
		"

	@echo ""
	@echo "========================================="
	@echo "Build completed successfully!"
	@echo "========================================="
	@echo ""
	@echo "Build artifacts in $(SONIC_REDFISH_TARGET):"
	@ls -lh $(TARGET_DIR)/*.deb 2>/dev/null || echo "  No .deb files found"

# Build inside Docker (called from Docker container)
# Note: sdbusplus is pre-installed in the Docker image
build-in-docker: setup-bmcweb apply-patches build-bridge-native build-bmcweb-native
	@echo "  Build inside Docker completed"

# Setup bmcweb source
setup-bmcweb:
	@echo "Checking bmcweb source..."
	@if [ ! -d "$(BMCWEB_DIR)" ]; then \
		echo "  bmcweb directory not found, cloning from $(BMCWEB_REPO_URL)..."; \
		git clone $(BMCWEB_REPO_URL) $(BMCWEB_DIR); \
		echo "  Checking out commit $(BMCWEB_HEAD_COMMIT)..."; \
		cd $(BMCWEB_DIR) && git checkout $(BMCWEB_HEAD_COMMIT); \
		echo "  bmcweb cloned and checked out to $(BMCWEB_HEAD_COMMIT)"; \
	elif [ -d "$(BMCWEB_DIR)/.git" ]; then \
		cd $(BMCWEB_DIR) && \
		current_commit=$$(git rev-parse --short HEAD 2>/dev/null || echo "unknown"); \
		if ! git diff --quiet 2>/dev/null; then \
			echo "  bmcweb has local changes (patches applied), ready"; \
		else \
			echo "  bmcweb source is clean at commit $$current_commit, ready for patches"; \
		fi; \
	else \
		echo "  bmcweb source directory ready"; \
	fi
	@echo "  bmcweb ready"

# Copy patches to debian/ directory 
copy-patches: $(SERIES_FILE)
	@echo "Copying patches to debian/ directory ..."
	@# Note: Patches will create debian/ directory, so we only copy series file after patches are applied
	@echo "  Patches will be applied from $(PATCHES_DIR)"

# Apply patches using series file
apply-patches: setup-bmcweb
	@echo "Applying patches from series file..."
	@if [ ! -d "$(BMCWEB_DIR)" ]; then \
		echo "Error: bmcweb directory not found"; \
		exit 1; \
	fi

	@cd $(BMCWEB_DIR) && \
	if git diff --quiet 2>/dev/null; then \
		echo "  Applying patches from $(PATCHES_DIR)/series..."; \
		while IFS= read -r patch || [ -n "$$patch" ]; do \
			patch=$$(echo "$$patch" | sed 's/#.*//;s/^[[:space:]]*//;s/[[:space:]]*$$//'); \
			[ -z "$$patch" ] && continue; \
			echo "  Applying: $$patch"; \
			if [ -f "$(PATCHES_DIR)/$$patch" ]; then \
				git apply "$(PATCHES_DIR)/$$patch" || { echo "Error applying $$patch"; exit 1; }; \
			else \
				echo "Error: Patch file not found: $$patch"; \
				exit 1; \
			fi; \
		done < $(PATCHES_DIR)/series; \
		echo "  All patches applied successfully"; \
	else \
		echo "  Patches already applied (bmcweb has local changes)"; \
	fi

# Build bmcweb Debian package
# Dependencies: clean → setup-bmcweb → apply-patches → build-bmcweb
build-bmcweb: clean setup-bmcweb apply-patches
	@echo "========================================="
	@echo "Building bmcweb Debian package"
	@echo "========================================="
	@echo ""

	# Build Docker image if needed
	@echo "Ensuring Docker build environment..."
	@docker build -t $(DOCKER_BUILDER_IMAGE) -f $(DOCKERFILE_BUILD) $(BUILD_DIR) 2>/dev/null || true
	@echo ""

	# Run dpkg-buildpackage inside Docker
	@echo "Building bmcweb Debian package inside Docker..."
	@mkdir -p $(TARGET_DIR)
	@docker run --rm \
		-v "$(REPO_ROOT):/workspace" \
		-w /workspace/bmcweb \
		$(DOCKER_BUILDER_IMAGE) \
		bash -c "dpkg-buildpackage -us -uc -j$(SONIC_CONFIG_MAKE_JOBS)"
	@echo ""

	# Copy all build artifacts to target directory
	@echo "Collecting build artifacts to $(SONIC_REDFISH_TARGET)..."
	@mv $(REPO_ROOT)/bmcweb_*.deb $(TARGET_DIR)/ 2>/dev/null || true
	@mv $(REPO_ROOT)/bmcweb-dbg_*.deb $(TARGET_DIR)/ 2>/dev/null || true
	@mv $(REPO_ROOT)/bmcweb_*.changes $(TARGET_DIR)/ 2>/dev/null || true
	@mv $(REPO_ROOT)/bmcweb_*.buildinfo $(TARGET_DIR)/ 2>/dev/null || true
	@mv $(REPO_ROOT)/bmcweb_*.dsc $(TARGET_DIR)/ 2>/dev/null || true
	@echo ""
	@echo "========================================="
	@echo "bmcweb build complete!"
	@echo "========================================="
	@echo "Build artifacts in $(SONIC_REDFISH_TARGET):"
	@ls -lh $(TARGET_DIR)/bmcweb* 2>/dev/null || echo "No artifacts found"
	@echo ""

# Build sonic-dbus-bridge Debian package
# Dependencies: clean → build-bridge
build-bridge: clean
	@echo "========================================="
	@echo "Building sonic-dbus-bridge Debian package"
	@echo "========================================="
	@echo ""

	# Build Docker image if needed
	@echo "Ensuring Docker build environment..."
	@docker build -t $(DOCKER_BUILDER_IMAGE) -f $(DOCKERFILE_BUILD) $(BUILD_DIR) 2>/dev/null || true
	@echo ""

	# Build .deb package inside Docker
	@echo "Building sonic-dbus-bridge .deb package in Docker..."
	@docker run --rm \
		-v "$(REPO_ROOT):/workspace" \
		-w /workspace \
		-e SONIC_CONFIG_MAKE_JOBS=$(SONIC_CONFIG_MAKE_JOBS) \
		$(DOCKER_BUILDER_IMAGE) \
		bash -c "\
			set -e; \
			git config --global --add safe.directory /workspace; \
			git config --global --add safe.directory /workspace/bmcweb; \
			echo 'Installing Debian packaging tools and build dependencies...'; \
			apt-get update -qq; \
			apt-get install -y -qq debhelper devscripts build-essential fakeroot dpkg-dev libboost-dev meson; \
			echo 'Building sonic-dbus-bridge package...'; \
			cd /workspace/sonic-dbus-bridge; \
			dpkg-buildpackage -us -uc -b -j$(SONIC_CONFIG_MAKE_JOBS); \
			echo 'Package built successfully'; \
		"

	# Copy all build artifacts to target directory
	@echo ""
	@echo "Collecting build artifacts to $(SONIC_REDFISH_TARGET)..."
	@mkdir -p $(TARGET_DIR)
	@mv $(REPO_ROOT)/sonic-dbus-bridge_*.deb $(TARGET_DIR)/ 2>/dev/null || true
	@mv $(REPO_ROOT)/sonic-dbus-bridge-dbgsym_*.deb $(TARGET_DIR)/ 2>/dev/null || true
	@mv $(REPO_ROOT)/sonic-dbus-bridge_*.changes $(TARGET_DIR)/ 2>/dev/null || true
	@mv $(REPO_ROOT)/sonic-dbus-bridge_*.buildinfo $(TARGET_DIR)/ 2>/dev/null || true
	@mv $(REPO_ROOT)/sonic-dbus-bridge_*.dsc $(TARGET_DIR)/ 2>/dev/null || true
	@echo ""
	@echo "========================================="
	@echo "sonic-dbus-bridge package build complete!"
	@echo "========================================="
	@echo ""
	@echo "Build artifacts in $(SONIC_REDFISH_TARGET):"
	@ls -lh $(TARGET_DIR)/sonic-dbus-bridge* 2>/dev/null || echo "  No artifacts found"

# Build bmcweb natively (inside Docker container, no nested Docker)
build-bmcweb-native:
	@echo "========================================="
	@echo "Building bmcweb Debian package (native)"
	@echo "========================================="
	@echo ""

	# Build directly with dpkg-buildpackage (no Docker)
	@echo "Building bmcweb package..."
	@cd $(BMCWEB_DIR) && dpkg-buildpackage -us -uc -j$(SONIC_CONFIG_MAKE_JOBS)
	@echo ""

	# Copy all build artifacts to target directory
	@echo "Collecting build artifacts to $(SONIC_REDFISH_TARGET)..."
	@mkdir -p $(TARGET_DIR)
	@mv $(REPO_ROOT)/bmcweb_*.deb $(TARGET_DIR)/ 2>/dev/null || true
	@mv $(REPO_ROOT)/bmcweb-dbg_*.deb $(TARGET_DIR)/ 2>/dev/null || true
	@mv $(REPO_ROOT)/bmcweb_*.changes $(TARGET_DIR)/ 2>/dev/null || true
	@mv $(REPO_ROOT)/bmcweb_*.buildinfo $(TARGET_DIR)/ 2>/dev/null || true
	@mv $(REPO_ROOT)/bmcweb_*.dsc $(TARGET_DIR)/ 2>/dev/null || true
	@echo ""
	@echo "========================================="
	@echo "bmcweb build complete!"
	@echo "========================================="
	@echo "Build artifacts in $(SONIC_REDFISH_TARGET):"
	@ls -lh $(TARGET_DIR)/bmcweb* 2>/dev/null || echo "No artifacts found"
	@echo ""

# Build sonic-dbus-bridge natively (inside Docker container, no nested Docker)
build-bridge-native:
	@echo "========================================="
	@echo "Building sonic-dbus-bridge Debian package (native)"
	@echo "========================================="
	@echo ""

	# Build directly with dpkg-buildpackage (no Docker)
	@echo "Building sonic-dbus-bridge package..."
	@cd $(BRIDGE_DIR) && dpkg-buildpackage -us -uc -b -j$(SONIC_CONFIG_MAKE_JOBS)
	@echo ""

	# Copy all build artifacts to target directory
	@echo "Collecting build artifacts to $(SONIC_REDFISH_TARGET)..."
	@mkdir -p $(TARGET_DIR)
	@mv $(REPO_ROOT)/sonic-dbus-bridge_*.deb $(TARGET_DIR)/ 2>/dev/null || true
	@mv $(REPO_ROOT)/sonic-dbus-bridge-dbgsym_*.deb $(TARGET_DIR)/ 2>/dev/null || true
	@mv $(REPO_ROOT)/sonic-dbus-bridge_*.changes $(TARGET_DIR)/ 2>/dev/null || true
	@mv $(REPO_ROOT)/sonic-dbus-bridge_*.buildinfo $(TARGET_DIR)/ 2>/dev/null || true
	@mv $(REPO_ROOT)/sonic-dbus-bridge_*.dsc $(TARGET_DIR)/ 2>/dev/null || true
	@echo ""
	@echo "========================================="
	@echo "sonic-dbus-bridge package build complete!"
	@echo "========================================="
	@echo ""
	@echo "Build artifacts in $(SONIC_REDFISH_TARGET):"
	@ls -lh $(TARGET_DIR)/sonic-dbus-bridge* 2>/dev/null || echo "  No artifacts found"

# ========================================
# sonic-buildimage Integration Targets
# ========================================
# These targets are called by the sonic-buildimage build system
BMCWEB = bmcweb_$(SONIC_REDFISH_VERSION)_$(CONFIGURED_ARCH).deb
BMCWEB_DBG = bmcweb-dbg_$(SONIC_REDFISH_VERSION)_$(CONFIGURED_ARCH).deb

# Main bmcweb package target for sonic-buildimage
$(addprefix $(DEST)/, $(BMCWEB)): $(DEST)/% : setup-bmcweb apply-patches
	# Build bmcweb package using dpkg-buildpackage
	pushd $(BMCWEB_DIR)

ifeq ($(CROSS_BUILD_ENVIRON), y)
	dpkg-buildpackage -b -us -uc -a$(CONFIGURED_ARCH) -Pcross,nocheck -j$(SONIC_CONFIG_MAKE_JOBS)
else
	dpkg-buildpackage -b -us -uc -j$(SONIC_CONFIG_MAKE_JOBS)
endif
	popd

ifneq ($(DEST),)
	mv $(BMCWEB) $(BMCWEB_DBG) $(DEST)/
endif

# Derived package (debug symbols) depends on main package
$(addprefix $(DEST)/, $(BMCWEB_DBG)): $(DEST)/% : $(DEST)/$(BMCWEB)

# Clean - Remove all build artifacts; keeps bmcweb source dir but reverts it
#         to pristine tracked state (tracked files reset, untracked + ignored
#         files wiped — obj-*, debian/, subprojects/<wraps>, etc.).
clean:
	@echo "========================================="
	@echo "Cleaning build artifacts..."
	@echo "========================================="
	@echo ""

	# Docker builds leave root-owned files inside bmcweb/ and sonic-dbus-bridge/.
	# Reclaim ownership first so subsequent rm / git operations don't fail.
	@echo "Reclaiming ownership of build trees (sudo)..."
	@if [ -d "$(BMCWEB_DIR)" ]; then sudo chown -R $$(id -u):$$(id -g) $(BMCWEB_DIR) 2>/dev/null || true; fi
	@sudo chown -R $$(id -u):$$(id -g) $(BRIDGE_DIR) 2>/dev/null || true

	# Wipe bmcweb build state completely. With git, a hard reset + clean -fdx
	# is exhaustive: reverts patched files and removes obj-*, debian/,
	# subprojects/<wrapped-clones>, and anything else untracked or ignored.
	@if [ -d "$(BMCWEB_DIR)/.git" ]; then \
		echo "Resetting bmcweb source tree..."; \
		cd $(BMCWEB_DIR) && git reset --hard HEAD && git clean -ffdx; \
		echo "  bmcweb source is now completely clean"; \
	elif [ -d "$(BMCWEB_DIR)" ]; then \
		echo "Removing bmcweb build artifacts (no .git present)..."; \
		rm -rf $(BMCWEB_DIR)/obj-* $(BMCWEB_DIR)/debian 2>/dev/null || true; \
		if [ -d "$(BMCWEB_DIR)/subprojects" ]; then \
			find $(BMCWEB_DIR)/subprojects -mindepth 1 -maxdepth 1 -type d -exec rm -rf {} + 2>/dev/null || true; \
		fi; \
	fi

	# Clean sonic-dbus-bridge build artifacts
	@echo "Cleaning sonic-dbus-bridge build artifacts..."
	@rm -rf \
		$(BRIDGE_DIR)/obj-* \
		$(BRIDGE_DIR)/build \
		$(BRIDGE_DIR)/debian/.debhelper \
		$(BRIDGE_DIR)/debian/tmp \
		$(BRIDGE_DIR)/debian/sonic-dbus-bridge \
		$(BRIDGE_DIR)/debian/debhelper-build-stamp \
		$(BRIDGE_DIR)/debian/files \
		$(BRIDGE_DIR)/debian/*.log \
		$(BRIDGE_DIR)/debian/*.substvars 2>/dev/null || true
	@if [ -d "$(BRIDGE_DIR)/subprojects" ]; then \
		find $(BRIDGE_DIR)/subprojects -mindepth 1 -maxdepth 1 -type d -exec rm -rf {} + 2>/dev/null || true; \
	fi

	# Package artifacts: repo root strays + target/ directory
	@echo "Removing package artifacts..."
	@rm -f $(REPO_ROOT)/*.deb $(REPO_ROOT)/*.changes $(REPO_ROOT)/*.buildinfo $(REPO_ROOT)/*.dsc $(REPO_ROOT)/*.tar.gz 2>/dev/null || true
	@sudo rm -rf $(REPO_ROOT)/target 2>/dev/null || rm -rf $(REPO_ROOT)/target 2>/dev/null || true

	@echo ""
	@echo "Clean completed"

# Reset - Complete cleanup: wipes everything clean does PLUS deletes the
#         bmcweb source directory and removes the Docker builder image.
#         Next build will re-clone bmcweb and rebuild the Docker image.
reset:
	@echo "========================================="
	@echo "Resetting workspace completely..."
	@echo "========================================="
	@echo ""

	# Nuke bmcweb directory entirely (root-owned files inside need sudo)
	@echo "Removing bmcweb source directory..."
	@sudo rm -rf $(BMCWEB_DIR) 2>/dev/null || rm -rf $(BMCWEB_DIR) 2>/dev/null || true
	@echo "  bmcweb directory removed"
	@echo ""

	# Clean sonic-dbus-bridge build artifacts (source tree stays)
	@echo "Cleaning sonic-dbus-bridge build artifacts..."
	@sudo chown -R $$(id -u):$$(id -g) $(BRIDGE_DIR) 2>/dev/null || true
	@rm -rf \
		$(BRIDGE_DIR)/obj-* \
		$(BRIDGE_DIR)/build \
		$(BRIDGE_DIR)/debian/.debhelper \
		$(BRIDGE_DIR)/debian/tmp \
		$(BRIDGE_DIR)/debian/sonic-dbus-bridge \
		$(BRIDGE_DIR)/debian/debhelper-build-stamp \
		$(BRIDGE_DIR)/debian/files \
		$(BRIDGE_DIR)/debian/*.log \
		$(BRIDGE_DIR)/debian/*.substvars 2>/dev/null || true
	@if [ -d "$(BRIDGE_DIR)/subprojects" ]; then \
		find $(BRIDGE_DIR)/subprojects -mindepth 1 -maxdepth 1 -type d -exec rm -rf {} + 2>/dev/null || true; \
	fi
	@echo ""

	# Remove Docker builder image
	@echo "Removing Docker builder image..."
	@docker rmi -f $(DOCKER_BUILDER_IMAGE) 2>/dev/null || echo "  (image not found, skipping)"
	@echo ""

	# Remove target dir and root-level package artifacts
	@echo "Removing target directory and package artifacts..."
	@sudo rm -rf $(REPO_ROOT)/target 2>/dev/null || rm -rf $(REPO_ROOT)/target 2>/dev/null || true
	@rm -f $(REPO_ROOT)/*.deb $(REPO_ROOT)/*.changes $(REPO_ROOT)/*.buildinfo $(REPO_ROOT)/*.dsc $(REPO_ROOT)/*.tar.gz 2>/dev/null || true

	@echo ""
	@echo "========================================="
	@echo "Workspace reset complete!"
	@echo "========================================="
	@echo ""
	@echo "Run 'make' to rebuild from scratch (bmcweb will be re-cloned)."

