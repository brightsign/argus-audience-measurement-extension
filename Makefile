.PHONY: all clean pdf

all:
	@echo "Targets: clean, pdf"

clean:
	rm -rf build_xt5 build_ls5 build_rk3576 build_rk3568 staging
	rm -f *.pdf docs/*.pdf

pdf:
	md2pdf *.md
	cd docs && md2pdf *.md
