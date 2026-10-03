.DEFAULT_GOAL := help

# DEMO_MODE: 0 = production (expiration enforcement compiled out), 1 = demo.
# FORCE_UPDATE: 1 forces the auxiliary build scripts to re-pull their sources.
# Both are exported so the build scripts see them.
DEMO_MODE ?= 0
FORCE_UPDATE ?=
export DEMO_MODE FORCE_UPDATE

# Optional path to a BrightSign cobra toolchain installer, used only by the
# fetch-sdk fallback. Normally the SDK is provisioned by ../brightsign-sdk-builder.
SDK_INSTALLER ?=

# Optional local, untracked cache location. When this repo is not checked out
# parallel to brightsign-sdk-builder (so the default ../argus-build-cache does not
# resolve), drop a `cache.env` next to this Makefile with, e.g.:
#     ARGUS_CACHE_DIR := /home/you/src/.../argus-all/argus-build-cache
# It is git-ignored and overridden by a command-line ARGUS_CACHE_DIR=...
-include cache.env

# Shared build cache (SDK + RKNN toolkit + compiled models) lives OUTSIDE this
# repo and is provisioned once per build box by the brightsign-sdk-builder repo.
# scripts/lib/cache.sh is the single source of truth. Override the location with
# ARGUS_CACHE_DIR (default ../argus-build-cache). ARGUS_CACHE_DIR is forwarded
# explicitly so a command-line override reaches $(shell ...), then exported so the
# recipe scripts (which source cache.sh themselves) resolve the same location.
CACHE_SH := ARGUS_CACHE_DIR='$(ARGUS_CACHE_DIR)' bash scripts/lib/cache.sh print
CACHE_DIR := $(shell $(CACHE_SH) cache)
SDK_DIR := $(shell $(CACHE_SH) sdk)
SDK_ENV := $(shell $(CACHE_SH) sdk-env)
TOOLKIT_DIR := $(shell $(CACHE_SH) toolkit)
MODELS_DIR := $(shell $(CACHE_SH) models)
export ARGUS_CACHE_DIR

# Pandoc options for PDF generation with Mermaid support.
PANDOC_OPTS := -F mermaid-filter --pdf-engine=xelatex \
	-V geometry:margin=1in \
	-V colorlinks=true \
	-V linkcolor=blue \
	-V urlcolor=blue \
	-V mainfont="Helvetica Neue" \
	-V sansfont="Helvetica Neue" \
	-V monofont="Menlo"

DOCS_DIR := docs
DOCS_MD := $(DOCS_DIR)/argus-api-integration-guide.md \
	$(DOCS_DIR)/mqtt-message-format.md \
	$(DOCS_DIR)/CONFIGURATION.md \
	$(DOCS_DIR)/TRACKING-EXPLAINED.md \
	$(DOCS_DIR)/INTEGRATION-MQTT.md \
	$(DOCS_DIR)/prometheus-grafana-setup.md \
	$(DOCS_DIR)/BUILD-INSTRUCTIONS.md
DOCS_PDF := $(DOCS_MD:.md=.pdf)

.PHONY: help build build-demo build-update build-demo-update build-gst-plugins \
	fetch-sdk build-models sync-models package package-demo cache-info cache-clean \
	clean clean-all run-tests test install-tools build-docs pdf

help:                ## Print available targets
	@echo "Argus Audience Measurement Extension -- build targets"
	@echo ""
	@grep -hE '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | awk 'BEGIN{FS=":.*?## "}{printf "  %-20s %s\n", $$1, $$2}'
	@echo ""
	@echo "The SDK, RKNN toolkit, and models come from the shared cache provisioned by"
	@echo "../brightsign-sdk-builder. Run 'make cache-info' to see the resolved location."

fetch-sdk:           ## Detect the cross-compile SDK in the shared cache (provisioned by ../brightsign-sdk-builder)
	bash scripts/fetch-sdk.sh $(SDK_INSTALLER)

build-models:        ## Compile the RKNN models (per SoC) into the shared cache (needs docker + the cached toolkit)
	bash scripts/build-models.sh "$(MODELS_DIR)"

sync-models: build-models  ## Copy the cached models into install/<SOC>/model for packaging
	bash scripts/sync-models.sh

# The CMake build copies install/<SOC>/model into the build dir as a post-build
# step, so models must be present in install/ before build.sh runs.
build: fetch-sdk sync-models  ## Cross-compile the extension (all SoCs; DEMO_MODE=0). Limit with SOCS="rk3588"
	bash scripts/build.sh $(SOCS)

build-demo:          ## Cross-compile with demo expiration enforcement (DEMO_MODE=1)
	$(MAKE) build DEMO_MODE=1

build-update:        ## Production build, forcing auxiliary sources to re-pull
	$(MAKE) build FORCE_UPDATE=1

build-demo-update:   ## Demo build, forcing auxiliary sources to re-pull
	$(MAKE) build DEMO_MODE=1 FORCE_UPDATE=1

build-gst-plugins:   ## (Optional) Build GStreamer MP4 plugins from the cached OE tree
	bash scripts/build-gst-isomp4-plugin.sh

package: build       ## Build all SoCs (which syncs models) and create the extension zips (production)
	./package

package-demo:        ## Same as package but with demo expiration enforcement
	$(MAKE) package DEMO_MODE=1

cache-info:          ## Show the resolved shared-cache location and what is present
	@echo "ARGUS_CACHE_DIR : $(if $(ARGUS_CACHE_DIR),$(ARGUS_CACHE_DIR) (override),(unset -> default))"
	@echo "cache   : $(CACHE_DIR)"
	@echo "sdk     : $(SDK_DIR)  [$(if $(wildcard $(SDK_ENV)),present,absent)]"
	@echo "toolkit : $(TOOLKIT_DIR)  [$(if $(wildcard $(TOOLKIT_DIR)),present,absent)]"
	@echo "models  : $(MODELS_DIR)  [$(if $(wildcard $(MODELS_DIR)/RK3588/RetinaFace.rknn),present,absent)]"

run-tests:           ## Build and run the host-side C++ unit tests (SDK-free)
	bash scripts/setup_and_run_tests.sh

test: run-tests      ## Alias for run-tests

clean:               ## Remove this repo's build artifacts (leaves the shared cache)
	rm -rf build_xt5 build_ls5 build_rk3576 build_rk3568 staging
	@# Clean install dirs but preserve model subdirectories
	@for dir in install/*/; do \
		find "$$dir" -mindepth 1 -maxdepth 1 ! -name model -exec rm -rf {} + 2>/dev/null || true; \
	done
	rm -f *.pdf docs/*.pdf
	rm -f *.zip

clean-all: clean     ## Also remove install/ and generated docs (leaves the shared cache)
	rm -rf install
	rm -f *.pdf docs/*.pdf
	rm -f *.zip
	@echo "Note: the shared build cache is left intact ($(CACHE_DIR))."
	@echo "      Run 'make cache-clean' to remove it (affects all projects)."

cache-clean:         ## Remove the ENTIRE shared build cache (SDK + toolkit + models) -- AFFECTS ALL PROJECTS
	@echo "Removing the shared cache used by every project: $(CACHE_DIR)"
	rm -rf "$(CACHE_DIR)"
	@echo "Shared cache removed. Docker images are left; remove with: docker rmi rknn_tk2 bsoe-build"

install-tools:       ## Install documentation build tools (pandoc, xelatex, mermaid-filter)
	@echo "Checking and installing documentation tools..."
	@if ! command -v pandoc >/dev/null 2>&1; then \
		echo "Installing pandoc..."; \
		if command -v brew >/dev/null 2>&1; then brew install pandoc; \
		elif command -v apt-get >/dev/null 2>&1; then sudo apt-get install -y pandoc; \
		else echo "Error: Please install pandoc manually"; exit 1; fi; \
	else echo "  pandoc: OK"; fi
	@if ! command -v xelatex >/dev/null 2>&1; then \
		echo "Installing LaTeX (this may take a while)..."; \
		if command -v brew >/dev/null 2>&1; then brew install --cask mactex-no-gui || brew install texlive; \
		elif command -v apt-get >/dev/null 2>&1; then sudo apt-get install -y texlive-xetex texlive-fonts-recommended; \
		else echo "Error: Please install LaTeX (texlive) manually"; exit 1; fi; \
	else echo "  xelatex: OK"; fi
	@if ! command -v mermaid-filter >/dev/null 2>&1; then \
		echo "Installing mermaid-filter..."; npm install -g mermaid-filter; \
	else echo "  mermaid-filter: OK"; fi
	@echo "All documentation tools installed."

build-docs: $(DOCS_PDF)  ## Build PDF documentation from markdown (Mermaid support)
	@echo "Documentation PDFs built successfully:"
	@ls -la $(DOCS_PDF)

$(DOCS_DIR)/%.pdf: $(DOCS_DIR)/%.md
	@echo "Building $@..."
	pandoc $(PANDOC_OPTS) $< -o $@

pdf: build-docs      ## Alias for build-docs
