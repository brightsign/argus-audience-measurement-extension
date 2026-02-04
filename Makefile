.PHONY: all build build-update clean clean-all install-tools build-docs pdf

# Pass FORCE_UPDATE to child processes (for Go dependency updates)
export FORCE_UPDATE

# Pandoc options for PDF generation with Mermaid support
PANDOC_OPTS := -F mermaid-filter --pdf-engine=xelatex \
	-V geometry:margin=1in \
	-V colorlinks=true \
	-V linkcolor=blue \
	-V urlcolor=blue

# Documentation files to build
DOCS_DIR := docs
DOCS_MD := $(DOCS_DIR)/argus-api-integration-guide.md \
	$(DOCS_DIR)/mqtt-message-format.md \
	$(DOCS_DIR)/CONFIGURATION.md \
	$(DOCS_DIR)/TRACKING-EXPLAINED.md \
	$(DOCS_DIR)/INTEGRATION-MQTT.md \
	$(DOCS_DIR)/INTEGRATION-PROMETHEUS.md \
	$(DOCS_DIR)/BUILD-INSTRUCTIONS.md
DOCS_PDF := $(DOCS_MD:.md=.pdf)

all:
	@echo "Targets: build, build-update, clean, clean-all, install-tools, build-docs"

# Standard build - skips Go rebuilds if unchanged
build:
	./scripts/runall.sh --auto

# Build with forced Go dependency updates (pulls latest from repos)
build-update:
	FORCE_UPDATE=1 ./scripts/runall.sh --auto

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
