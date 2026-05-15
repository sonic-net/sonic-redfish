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

.PHONY: all build clean reset setup-bmcweb copy-patches apply-patches build-bmcweb build-bridge build-bmcweb-native build-bridge-native build-in-docker test unit-test help

# Recipes in this Makefile share Docker images and the target/ directory, so
# the top-level prereq chain (build → unit-test → test) must run sequentially.
.NOTPARALLEL:

# Default target - Build both components and gate completion on unit + integration tests.
# Prereqs run left-to-right; any failure aborts the whole invocation.
all: build unit-test test
	@echo ""
	@echo "========================================="
	@echo "Build and tests completed!"
	@echo "========================================="
	@echo ""
	@echo "Build artifacts in $(SONIC_REDFISH_TARGET):"
	@ls -lh $(TARGET_DIR)/*.deb 2>/dev/null || echo "  No .deb files found"

define HELP_TEXT
sonic-redfish Build System (Docker-only)
=========================================

Targets:
  make                  Build + run unit tests + run integration tests (fails on any test failure)
  make all              Alias for `make` (build + unit-test + test)
  make build            Build bmcweb + sonic-dbus-bridge .debs only (no tests)
  make build-bmcweb     Build bmcweb only
  make build-bridge     Build sonic-dbus-bridge only
  make test             Run Redfish API integration tests (requires prior build)
  make unit-test        Run C++ unit tests (gtest)
  make clean            Remove build artifacts; reset bmcweb source to pristine (keeps dir)
  make reset            Full wipe: delete bmcweb dir, remove Docker image, clear target/
  make clean-debug      Remove debug test container
  make help             Show this help

Variables (override with `make VAR=value`):
  SONIC_CONFIG_MAKE_JOBS  Parallel build jobs     [default: $$(nproc)]
  SONIC_REDFISH_TARGET    Output directory        [default: target/debs/trixie]
  BMCWEB_HEAD_COMMIT      bmcweb commit to build  [default: 6926d430]
  BMCWEB_REPO_URL         bmcweb git URL          [default: openbmc/bmcweb on GitHub]
  NODELETE                Keep test container alive for debugging (use with test)

Examples:
  make                             Build everything (clones bmcweb if missing)
  make all                         Build + run tests
  make test NODELETE=1             Run tests and keep container for debugging
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

# ========================================
# Integration Tests
# ========================================
# Builds a test container with redis + dbus + bridge + bmcweb,
# seeds Redis with test data, and runs pytest against the live Redfish API.

DOCKER_TEST_IMAGE := sonic-redfish-test:latest
DOCKERFILE_TEST := $(BUILD_DIR)/Dockerfile.test

test: $(DOCKERFILE_TEST)
	@echo "========================================="
	@echo "Running Redfish integration tests"
	@echo "========================================="
	@echo ""

	@# Clean up any leftover debug container from previous runs
	@docker rm -f sonic-redfish-test-debug 2>/dev/null || true

	@# Verify .deb artifacts exist
	@if [ ! -f "$(TARGET_DIR)/bmcweb_"*.deb ] || [ ! -f "$(TARGET_DIR)/sonic-dbus-bridge_"*.deb ]; then \
		echo "Error: .deb packages not found in $(TARGET_DIR)/"; \
		echo "Run 'make build' first."; \
		exit 1; \
	fi

	@sudo chown -R $$USER:$$USER $(REPO_ROOT)/bmcweb/subprojects/packagecache
	@echo "Building test container..."
	docker build -t $(DOCKER_TEST_IMAGE) -f $(DOCKERFILE_TEST) $(REPO_ROOT)
	@echo ""

	@echo "Running tests..."
ifdef NODELETE
	@echo ">>> NODELETE=1: Container will be kept for debugging <<<"
	@echo ""
	@# Run container with tail -f /dev/null to keep it alive after tests
	@docker run -d --cap-add SYS_ADMIN -v /run/dbus:/run/dbus --name sonic-redfish-test-debug $(DOCKER_TEST_IMAGE) \
		bash -c 'bash tests/redfish-api/start_services.sh && python3 -u -m pytest tests/redfish-api/ -v --tb=short 2>&1 | python3 -u scripts/format_pytest_output.py; echo "Tests completed. Container staying alive for debugging..."; tail -f /dev/null' \
		> /tmp/sonic-test-container-id.txt
	@CONTAINER_ID=$$(cat /tmp/sonic-test-container-id.txt); \
	echo "Container ID: $$CONTAINER_ID"; \
	echo "Container Name: sonic-redfish-test-debug"; \
	echo ""; \
	echo "Streaming logs (Ctrl+C to stop watching, container will keep running)..."; \
	echo ""; \
	docker logs -f $$CONTAINER_ID 2>&1 || true; \
	echo ""; \
	echo "========================================="; \
	echo "Container kept for debugging"; \
	echo "========================================="; \
	echo "Container ID:   $$CONTAINER_ID"; \
	echo "Container Name: sonic-redfish-test-debug"; \
	echo ""; \
	echo "Services are still running inside the container!"; \
	echo ""; \
	echo "To inspect: docker exec -it sonic-redfish-test-debug bash"; \
	echo "            docker exec -it $$CONTAINER_ID bash"; \
	echo ""; \
	echo "To view logs: docker logs sonic-redfish-test-debug"; \
	echo "              docker logs -f sonic-redfish-test-debug"; \
	echo ""; \
	echo "To remove:  make clean-debug"; \
	echo "            docker rm -f sonic-redfish-test-debug"; \
	echo "========================================="; \
	rm -f /tmp/sonic-test-container-id.txt
else
	docker run --rm --cap-add SYS_ADMIN -v /run/dbus:/run/dbus $(DOCKER_TEST_IMAGE)
	@echo ""
	@echo "========================================="
	@echo "Tests completed!"
	@echo "========================================="
	@# Clean up any debug containers after normal test run
	@docker rm -f sonic-redfish-test-debug 2>/dev/null || true
endif

# ========================================
# Unit Tests (C++ / gtest)
# ========================================
# Dumb-and-direct: each tests/unit-tests/<foo>_test.cpp is compiled together
# with sonic-dbus-bridge/src/<foo>.cpp and linked against gtest. Runs inside
# the builder container -- no services, no privileged mode, no new image.
# If libgtest-dev isn't present in the builder image, it's installed on demand.

UNIT_TEST_DIR := $(REPO_ROOT)/tests/unit-tests

unit-test:
	@echo "========================================="
	@echo "Running C++ unit tests (gtest)"
	@echo "========================================="
	@if ! ls $(UNIT_TEST_DIR)/*_test.cpp >/dev/null 2>&1; then \
		echo "No *_test.cpp files in $(UNIT_TEST_DIR)/"; \
		exit 0; \
	fi
	@bash -c 'set -o pipefail; docker run --rm \
		-v $(REPO_ROOT):/workspace \
		-w /workspace \
		$(DOCKER_BUILDER_IMAGE) \
		bash -c "set -e; \
			if [ ! -f /usr/src/googletest/googletest/src/gtest-all.cc ]; then \
				apt-get update -qq && apt-get install -y -qq libgtest-dev >/dev/null; \
			fi; \
			mkdir -p /tmp/ut; \
			failed=0; \
			for t in tests/unit-tests/*_test.cpp; do \
				base=\$$(basename \$$t _test.cpp); \
				src=sonic-dbus-bridge/src/\$$base.cpp; \
				if [ ! -f \$$src ]; then continue; fi; \
				g++ -std=c++20 -Wall -Wextra -g -O0 -pthread \
					-I sonic-dbus-bridge/include \
					-I /usr/src/googletest/googletest \
					-I /usr/src/googletest/googletest/include \
					\$$t \$$src \
					/usr/src/googletest/googletest/src/gtest-all.cc \
					/usr/src/googletest/googletest/src/gtest_main.cc \
					-o /tmp/ut/\$$base || { failed=1; continue; }; \
				stdbuf -oL -eL /tmp/ut/\$$base || failed=1; \
			done; \
			exit \$$failed" \
		| python3 -u $(REPO_ROOT)/scripts/format_gtest_output.py'
	@echo ""
	@echo "========================================="
	@echo "Unit tests completed"
	@echo "========================================="

# Clean up debug container
clean-debug:
	@echo "Removing debug container..."
	@docker rm -f sonic-redfish-test-debug 2>/dev/null && echo "Debug container removed" || echo "No debug container found"
	@rm -f /tmp/sonic-test-container-id.txt

# Clean build artifacts
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

	@echo "Removing Docker images..."
	@docker rmi $(DOCKER_BUILDER_IMAGE) 2>/dev/null || echo "  (Builder image not found, skipping)"
	@docker rmi $(DOCKER_TEST_IMAGE) 2>/dev/null || echo "  (Test image not found, skipping)"
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

