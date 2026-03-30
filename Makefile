.PHONY: all build build-demo build-update build-demo-update clean clean-all install-tools build-docs pdf

# Pass FORCE_UPDATE and DEMO_MODE to child processes
export FORCE_UPDATE
export DEMO_MODE

# Pandoc options for PDF generation with Mermaid support
# Uses Helvetica (sans-serif) for clean, readable output
PANDOC_OPTS := -F mermaid-filter --pdf-engine=xelatex \
	-V geometry:margin=1in \
	-V colorlinks=true \
	-V linkcolor=blue \
	-V urlcolor=blue \
	-V mainfont="Helvetica Neue" \
	-V sansfont="Helvetica Neue" \
	-V monofont="Menlo"

# Documentation files to build
DOCS_DIR := docs
DOCS_MD := $(DOCS_DIR)/argus-api-integration-guide.md \
	$(DOCS_DIR)/mqtt-message-format.md \
	$(DOCS_DIR)/CONFIGURATION.md \
	$(DOCS_DIR)/TRACKING-EXPLAINED.md \
	$(DOCS_DIR)/INTEGRATION-MQTT.md \
	$(DOCS_DIR)/prometheus-grafana-setup.md \
	$(DOCS_DIR)/BUILD-INSTRUCTIONS.md
DOCS_PDF := $(DOCS_MD:.md=.pdf)

all:
	@echo "Build targets:"
	@echo "  build             Build extension"
	@echo "  build-update      Build extension with forced Go dependency updates"
	@echo ""
	@echo "Demo targets (adds expiration date enforcement):"
	@echo "  build-demo        Build demo extension with expiration date"
	@echo "  build-demo-update Build demo extension with forced Go dependency updates"
	@echo ""
	@echo "Other targets: clean, clean-all, install-tools, build-docs"

# Default build: production extension (DEMO_MODE_ENABLED=OFF)
build:
	DEMO_MODE=0 ./scripts/runall.sh --auto

# Demo build: adds expiration date enforcement (DEMO_MODE_ENABLED=ON)
build-demo:
	DEMO_MODE=1 ./scripts/runall.sh --auto

# Production build with forced Go dependency updates
build-update:
	DEMO_MODE=0 FORCE_UPDATE=1 ./scripts/runall.sh --auto

# Demo build with forced Go dependency updates
build-demo-update:
	DEMO_MODE=1 FORCE_UPDATE=1 ./scripts/runall.sh --auto

clean:
	rm -rf build_xt5 build_ls5 build_rk3576 build_rk3568 staging
	@# Clean install dirs but preserve model subdirectories
	@for dir in install/*/; do \
		find "$$dir" -mindepth 1 -maxdepth 1 ! -name model -exec rm -rf {} + 2>/dev/null || true; \
	done
	rm -f *.pdf docs/*.pdf

clean-all: clean
	rm -rf install
	rm -f *.pdf docs/*.pdf

# Install documentation build tools
install-tools:
	@echo "Checking and installing documentation tools..."
	@# Check for pandoc
	@if ! command -v pandoc >/dev/null 2>&1; then \
		echo "Installing pandoc..."; \
		if command -v brew >/dev/null 2>&1; then \
			brew install pandoc; \
		elif command -v apt-get >/dev/null 2>&1; then \
			sudo apt-get install -y pandoc; \
		else \
			echo "Error: Please install pandoc manually"; \
			exit 1; \
		fi; \
	else \
		echo "  pandoc: OK"; \
	fi
	@# Check for xelatex (LaTeX)
	@if ! command -v xelatex >/dev/null 2>&1; then \
		echo "Installing LaTeX (this may take a while)..."; \
		if command -v brew >/dev/null 2>&1; then \
			brew install --cask mactex-no-gui || brew install texlive; \
		elif command -v apt-get >/dev/null 2>&1; then \
			sudo apt-get install -y texlive-xetex texlive-fonts-recommended; \
		else \
			echo "Error: Please install LaTeX (texlive) manually"; \
			exit 1; \
		fi; \
	else \
		echo "  xelatex: OK"; \
	fi
	@# Check for mermaid-filter (npm)
	@if ! command -v mermaid-filter >/dev/null 2>&1; then \
		echo "Installing mermaid-filter..."; \
		npm install -g mermaid-filter; \
	else \
		echo "  mermaid-filter: OK"; \
	fi
	@echo "All documentation tools installed."

# Build PDF documentation from markdown with Mermaid support
build-docs: $(DOCS_PDF)
	@echo "Documentation PDFs built successfully:"
	@ls -la $(DOCS_PDF)

# Pattern rule for converting .md to .pdf
$(DOCS_DIR)/%.pdf: $(DOCS_DIR)/%.md
	@echo "Building $@..."
	pandoc $(PANDOC_OPTS) $< -o $@

# Legacy target - now uses pandoc
pdf: build-docs
