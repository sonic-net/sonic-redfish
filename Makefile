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
RMC_EVENTS_DIR := $(REPO_ROOT)/rmc-events
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

.PHONY: all build clean reset setup-bmcweb copy-patches copy-rmc-events apply-patches build-bmcweb build-bridge build-bmcweb-native build-bridge-native build-in-docker help

# Default target - Build both components (via Docker)
all: build
	@echo ""
	@echo "========================================="
	@echo "All components built successfully!"
	@echo "========================================="
	@echo ""
	@echo "Build artifacts in $(SONIC_REDFISH_TARGET):"
	@ls -lh $(TARGET_DIR)/*.deb 2>/dev/null || echo "  No .deb files found"

help:
	@echo "sonic-redfish Build System (Docker-Only)"
	@echo "========================================="
	@echo ""
	@echo "Targets:"
	@echo "  all              - Build all components (Docker-only)"
	@echo "  build-bmcweb     - Build bmcweb only"
	@echo "  build-bridge     - Build sonic-dbus-bridge only"
	@echo "  clean            - Remove build artifacts (Docker-based)"
	@echo "  reset            - Complete cleanup (clean + reset bmcweb + remove Docker images)"
	@echo ""
	@echo "Variables:"
	@echo "  SONIC_CONFIG_MAKE_JOBS  - Number of parallel jobs (default: nproc)"
	@echo "  SONIC_REDFISH_TARGET    - Target directory for build artifacts (default: target/debs/trixie)"
	@echo "  BMCWEB_HEAD_COMMIT      - bmcweb commit to checkout (default: 6926d430)"
	@echo "  BMCWEB_REPO_URL         - bmcweb repository URL (default: https://github.com/openbmc/bmcweb.git)"
	@echo ""
	@echo "Examples:"
	@echo "  make -f Makefile                                    # Build with Docker (auto-clone bmcweb if needed)"
	@echo "  make -f Makefile clean                              # Clean build artifacts"
	@echo "  make -f Makefile reset                              # Complete reset"
	@echo "  make -f Makefile SONIC_CONFIG_MAKE_JOBS=4           # Build with 4 parallel jobs"
	@echo "  make -f Makefile BMCWEB_HEAD_COMMIT=abc123          # Build with specific bmcweb commit"
	@echo "  make -f Makefile SONIC_REDFISH_TARGET=output/debs   # Use custom output directory"
	@echo ""
	@echo "NOTE: This build system is Docker-only for consistency with sonic-buildimage"
	@echo "      bmcweb will be automatically cloned if not present"

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
		$(DOCKER_BUILDER_IMAGE)\
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

# Copy rmc-events source files into bmcweb tree before patches are applied.
# The integration patch (0003) wires these files into the bmcweb build but
# does not contain the file contents -- they live in rmc-events/.
copy-rmc-events: setup-bmcweb
	@echo "Copying rmc-events sources into bmcweb tree..."
	@if [ -d "$(RMC_EVENTS_DIR)" ]; then \
		cp -v $(RMC_EVENTS_DIR)/lib/*.hpp $(BMCWEB_DIR)/redfish-core/lib/ 2>/dev/null || true; \
		cp -v $(RMC_EVENTS_DIR)/include/*.hpp $(BMCWEB_DIR)/redfish-core/include/ 2>/dev/null || true; \
		cp -v $(RMC_EVENTS_DIR)/src/*.cpp $(BMCWEB_DIR)/redfish-core/src/ 2>/dev/null || true; \
		echo "  rmc-events files copied"; \
	else \
		echo "  No rmc-events directory found, skipping"; \
	fi

# Apply patches using series file
apply-patches: setup-bmcweb copy-rmc-events
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
	@mv $(REPO_ROOT)/bmcweb_*.tar.gz $(TARGET_DIR)/ 2>/dev/null || true
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

# Clean build artifacts
clean:
	@echo "========================================="
	@echo "Cleaning build artifacts..."
	@echo "========================================="
	@echo ""

	# Clean root-owned files (from Docker builds) using sudo
	@echo "Cleaning build directories..."
	@if [ -d "$(BMCWEB_DIR)/obj-"* ] || [ -d "$(BMCWEB_DIR)/subprojects" ]; then \
		echo "  Removing bmcweb build artifacts (may require sudo)..."; \
		sudo rm -rf $(BMCWEB_DIR)/obj-* 2>/dev/null || true; \
		if [ -d "$(BMCWEB_DIR)/subprojects" ]; then \
			find $(BMCWEB_DIR)/subprojects -mindepth 1 -maxdepth 1 -type d -exec sudo rm -rf {} + 2>/dev/null || true; \
		fi; \
	fi
	@if [ -d "$(BRIDGE_DIR)/obj-"* ] || [ -d "$(BRIDGE_DIR)/subprojects" ] || [ -d "$(BRIDGE_DIR)/debian/.debhelper" ]; then \
		echo "  Removing sonic-dbus-bridge build artifacts (may require sudo)..."; \
		sudo rm -rf $(BRIDGE_DIR)/obj-* $(BRIDGE_DIR)/debian/.debhelper $(BRIDGE_DIR)/debian/debhelper-build-stamp $(BRIDGE_DIR)/debian/files $(BRIDGE_DIR)/debian/sonic-dbus-bridge $(BRIDGE_DIR)/debian/*.log $(BRIDGE_DIR)/debian/*.substvars 2>/dev/null || true; \
		if [ -d "$(BRIDGE_DIR)/subprojects" ]; then \
			find $(BRIDGE_DIR)/subprojects -mindepth 1 -maxdepth 1 -type d -exec sudo rm -rf {} + 2>/dev/null || true; \
		fi; \
	fi

	# Clean host-owned files
	@echo "Cleaning package artifacts..."
	@rm -rf $(BMCWEB_DIR)/debian 2>/dev/null || true
	@rm -rf $(BRIDGE_DIR)/build 2>/dev/null || true
	@rm -f $(REPO_ROOT)/*.deb $(REPO_ROOT)/*.changes $(REPO_ROOT)/*.buildinfo $(REPO_ROOT)/*.dsc $(REPO_ROOT)/*.tar.gz 2>/dev/null || true
	@echo "  Removed package artifacts from root directory"

	# Reset bmcweb source to clean state (so patches can be reapplied)
	# git clean -fd also removes rmc-events copies (untracked files)
	@echo "Resetting bmcweb source to clean state..."
	@if [ -d "$(BMCWEB_DIR)/.git" ]; then \
		cd $(BMCWEB_DIR) && git reset --hard HEAD 2>/dev/null || true; \
		cd $(BMCWEB_DIR) && git clean -fd 2>/dev/null || true; \
		echo "  bmcweb source reset to clean state"; \
	fi

	@echo ""
	@echo "Clean completed"

# Reset - Complete cleanup including bmcweb source and Docker images
reset: clean
	@echo ""
	@echo "========================================="
	@echo "Resetting workspace to clean state..."
	@echo "========================================="
	@echo ""
	@echo "Removing Docker images..."
	@docker rmi $(DOCKER_BUILDER_IMAGE) 2>/dev/null || echo "  (Docker image not found, skipping)"
	@echo ""
	@echo "Resetting bmcweb source..."
	@if [ -d "$(BMCWEB_DIR)/.git" ]; then \
		sudo rm -rf $(BMCWEB_DIR)/debian $(BMCWEB_DIR)/obj-* 2>/dev/null || true; \
		cd $(BMCWEB_DIR) && git reset --hard HEAD 2>/dev/null || true; \
		cd $(BMCWEB_DIR) && git clean -fdx 2>/dev/null || true; \
		echo "  bmcweb source reset"; \
	else \
		echo "  bmcweb is not a git repository, skipping"; \
	fi
	@echo ""
	@echo "Removing target directory..."
	@sudo rm -rf target 2>/dev/null || true
	@echo "  Target directory removed"
	@echo ""
	@echo "========================================="
	@echo "Workspace reset complete!"
	@echo "========================================="
	@echo ""
	@echo "You can now run: make -f Makefile"

