.PHONY: all build clean clean-all pdf

all:
	@echo "Targets: build, clean, clean-all, pdf"

build:
	./scripts/runall.sh --auto

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
