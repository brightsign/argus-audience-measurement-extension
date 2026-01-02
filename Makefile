.PHONY: all build build-update clean clean-all pdf

# Pass FORCE_UPDATE to child processes (for Go dependency updates)
export FORCE_UPDATE

all:
	@echo "Targets: build, build-update, clean, clean-all, pdf"

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

pdf:
	md2pdf *.md
	cd docs && md2pdf *.md
