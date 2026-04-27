# Makefile for FTRFS Technical Report v1
# Build: make
# Clean: make clean
# Hard clean: make distclean

PAPER = paper
LATEX = latexmk -lualatex -interaction=nonstopmode -file-line-error

all: $(PAPER).pdf

$(PAPER).pdf: $(PAPER).tex refs.bib sections/*.tex figures/*.tex
	$(LATEX) $(PAPER).tex

clean:
	latexmk -c

distclean:
	latexmk -C

.PHONY: all clean distclean
